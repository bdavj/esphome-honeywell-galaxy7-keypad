#include "honeywell_galaxy7_keypad.h"
#include "esphome/core/log.h"
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace esphome {
namespace honeywell_galaxy7_keypad {

static const char *TAG = "honeywell_galaxy7_keypad.component";

constexpr uint8_t KEYPAD_ADDR = 0x11;
constexpr uint32_t INIT_POLL_SECOND_MS       = 5000;
constexpr uint32_t INIT_POLL_INTERVAL_MS     = 5000;
constexpr uint32_t SCREEN_PUSH_INTERVAL_MS   = 25000;
constexpr uint32_t ACTIVITY_POLL_INTERVAL_MS = 150;
constexpr uint32_t REPLY_WAIT_MS             = 100;
constexpr uint32_t KEY_DEDUPE_WINDOW_MS      = 100;  // treat repeats within 0.7s as duplicate

static const uint32_t PANEL_OFFLINE_TIMEOUT_MS = 300;  // 10s – tweak as you like


// Convert a vector of bytes to "AA BB CC" string for logging.
static std::string bytes_to_hex(const std::vector<uint8_t> &data) {
  std::ostringstream oss;
  for (size_t i = 0; i < data.size(); i++) {
    if (i != 0)
      oss << " ";
    oss << std::uppercase << std::hex;
    oss.width(2);
    oss.fill('0');
    oss << static_cast<int>(data[i]);
  }
  return oss.str();
}

uint8_t HoneywellGalaxy7Keypad::galaxy_checksum(const std::vector<uint8_t> &data) {
  uint32_t temp = 0xAA;
  for (auto b : data) {
    temp += b;
  }
  return ((temp >> 24) & 0xFF) + ((temp >> 16) & 0xFF) + ((temp >> 8) & 0xFF) + (temp & 0xFF);
}

void HoneywellGalaxy7Keypad::send_frame(const std::vector<uint8_t> &payload) {
  // Copy + append checksum
  std::vector<uint8_t> frame(payload);
  frame.push_back(galaxy_checksum(payload));
  this->write_array(frame.data(), frame.size());
  ESP_LOGV(TAG, "TX (%d bytes): %s", (int) frame.size(), bytes_to_hex(frame).c_str());
}

void HoneywellGalaxy7Keypad::set_display_text(const std::string &text) {
  if (text.empty()) {
    this->display_text_ = "ESP-HOME|Initializing";
  } else {
    this->display_text_ = text;
  }
  this->parse_display_text_();
  this->screen_dirty_ = true;
  this->bump_backlight_("display update");
  ESP_LOGD(TAG, "Display text set to: %s", this->display_text_.c_str());
}

void HoneywellGalaxy7Keypad::set_display_text_nobl(const std::string &text) {
  if (text.empty()) {
    this->display_text_ = "ESP-HOME|Initializing";
  } else {
    this->display_text_ = text;
  }
  this->parse_display_text_();
  this->screen_dirty_ = true;
  ESP_LOGD(TAG, "Display text set to: %s", this->display_text_.c_str());
}


void HoneywellGalaxy7Keypad::set_beep_enabled(bool enabled, uint8_t beep_period, uint8_t quiet_period) {
  this->beep_mode_ = enabled ? 0x01 : 0x00;
  this->beep_period_ = beep_period;
  this->beep_quiet_period_ = quiet_period;
  // Re-run beep command at next idle slot
  this->beep_set_ = false;
}

void HoneywellGalaxy7Keypad::parse_display_text_() {
  auto pipe = this->display_text_.find('|');
  if (pipe != std::string::npos) {
    this->display_line1_ = this->display_text_.substr(0, pipe);
    this->display_line2_ = this->display_text_.substr(pipe + 1);
  } else {
    this->display_line1_ = this->display_text_;
    this->display_line2_.clear();
  }
}

void HoneywellGalaxy7Keypad::queue_screen_push_() {
  this->screen_dirty_ = true;
  this->last_screen_push_ = millis();
}

void HoneywellGalaxy7Keypad::bump_backlight_(const char *reason) {
  uint32_t now = millis();
  this->backlight_off_at_ = now + this->backlight_timeout_ms_;

  // Only send a "backlight ON" command if it's currently off
  if (!this->backlight_on_) {
    this->backlight_target_on_ = true;
    this->backlight_cmd_pending_ = true;
    ESP_LOGV(TAG, "Backlight bump (%s) -> ON until %u ms", reason, this->backlight_off_at_);
  } else {
    ESP_LOGV(TAG, "Backlight bump (%s) -> already ON, extend until %u ms", reason, this->backlight_off_at_);
  }
}

std::vector<uint8_t> HoneywellGalaxy7Keypad::build_screen_frame_() {
  // Base flags from examples
  uint8_t modifier = 0x01;

  // Screen sequence bit (0x80) – flip *every* new screen
  this->screen_seq_flag_ = (this->screen_seq_flag_ == 0x00) ? 0x80 : 0x00;
  modifier |= this->screen_seq_flag_;

  // If this screen is also acknowledging a keypress
  if (this->needs_button_ack_) {
    modifier |= 0x10;              // "this screen acks a key"
    modifier |= this->ack_toggle_; // 0x00 or 0x02

    ESP_LOGVV(TAG, "Building screen frame with button ACK: flags=%02X", modifier);

    // Prepare next ack value
    this->ack_toggle_ = (this->ack_toggle_ == 0x00) ? 0x02 : 0x00;
    this->needs_button_ack_ = false;
  }

  std::vector<uint8_t> frame = {this->device_id_, 0x07, modifier, 0x17};

  std::string line2 = this->input_buffer_.empty()
                          ? this->display_line2_
                          : std::string(std::min<size_t>(this->input_buffer_.size(), 16), '*');

  std::string line1 = this->display_line1_;

  // Make sure both lines are exactly 16 chars (pad with spaces)
  if (line1.size() > 16) line1.resize(16);
  else line1.resize(16, ' ');

  if (line2.size() > 16) line2.resize(16);
  else line2.resize(16, ' ');

  // 0x17 reset already set cursor to 0x00, so write top line
  frame.insert(frame.end(), line1.begin(), line1.end());

  // Move cursor to bottom line, write it
  frame.push_back(0x02);  // cursor -> 0x40
  frame.insert(frame.end(), line2.begin(), line2.end());

  // Hide cursor
  frame.push_back(0x07);

  return frame;
}

void HoneywellGalaxy7Keypad::setup() {
  ESP_LOGI(TAG, "Honeywell Galaxy keypad setup starting");

  this->parse_display_text_();

  // Prime backlight timer
  this->backlight_off_at_ = millis() + this->backlight_timeout_ms_;
  this->backlight_target_on_ = false;
  this->backlight_cmd_pending_ = false;
  this->backlight_on_ = false;

  // First-stage poll (00/0E) to mimic the panel startup sequence.
  uint32_t now = millis();
  this->last_init_poll_ = now;
  this->send_frame({this->device_id_, 0x00, 0x0E});
  this->last_cmd_ = CMD_POLL_00;
  this->awaiting_reply_ = true;
  this->last_tx_time_ = now;
  this->last_activity_poll_ = now;
  this->rx_buf_.clear();
  ESP_LOGI(TAG, "Honeywell Galaxy keypad setup STARTED");
}

void HoneywellGalaxy7Keypad::loop() {
  uint32_t now = millis();

  if (this->panel_online_ && (now - this->last_panel_rx_ms_ > PANEL_OFFLINE_TIMEOUT_MS)) {
    this->panel_online_ = false;
    ESP_LOGW(TAG, "Panel timeout, marking offline");
  }

  // 1) If idle, choose exactly one thing to send (mirrors Python loop)
  if (!this->awaiting_reply_) {
    LastCmd cmd_to_send = CMD_NONE;

    // Highest priority: recover from a bad screen by doing a re-init poll
    if (this->need_reinit_after_f2_) {
      ESP_LOGW(TAG, "Performing re-init after F2");

      // Send the same 00 0F you use as init/status poll
      this->send_frame({this->device_id_, 0x00, 0x0F});
      this->last_cmd_       = CMD_POLL_00;
      this->awaiting_reply_ = true;
      this->last_tx_time_   = now;
      this->last_init_poll_ = now;

      // Reset our “known-good” protocol state
      this->ack_toggle_      = 0x02;  // first ACK after re-init = uses 0x02 bit
      this->screen_seq_flag_ = 0x00;  // so next screen sets 0x80
      this->need_reinit_after_f2_ = false;

      this->rx_buf_.clear();
      this->screen_dirty_ = true;

      return;  // don't consider any other commands this loop
    }

    // a) second init poll (00 0F) once after delay
    if (!this->sent_second_init_ && (now - this->last_init_poll_) >= INIT_POLL_SECOND_MS) {
      ESP_LOGVV(TAG, "Sending second init poll");
      cmd_to_send = CMD_POLL_00;
    }

    // d) screen update ASAP when dirty
    else if (this->sent_second_init_ && this->screen_dirty_) {
      ESP_LOGV(TAG, "Sending screen update");
      cmd_to_send = CMD_SCREEN_07;
    }

    // b) periodic 00 0F every 5s (or before screen retries)
    else if ((now - this->last_init_poll_) >= INIT_POLL_INTERVAL_MS) {
      cmd_to_send = CMD_POLL_00;
      this->ack_toggle_ = 0x02;  // reset ACK toggle on new poll
      this->screen_seq_flag_ = 0x00; // reset screen seq on new poll
    }
    // c) one-time beep disable once init is done
    else if (this->sent_second_init_ && !this->beep_set_) {
      cmd_to_send = CMD_BEEP_0C;
    }

    // e) backlight command if pending
    else if (this->backlight_cmd_pending_) {
      ESP_LOGVV(TAG, "Sending backlight command");
      cmd_to_send = CMD_BACKLIGHT_0D;
    }
    // f) activity poll
    else if ((now - this->last_activity_poll_) >= ACTIVITY_POLL_INTERVAL_MS) {
      ESP_LOGVV(TAG, "Sending activity poll");
      cmd_to_send = CMD_ACTIVITY_19;
    }

    if (cmd_to_send != CMD_NONE) {
      switch (cmd_to_send) {
        case CMD_POLL_00:
          if (!this->sent_second_init_) {
            this->send_frame({this->device_id_, 0x00, 0x0F});
            this->sent_second_init_ = true;
          } else {
            this->send_frame({this->device_id_, 0x00, 0x0F});
          }
          this->last_init_poll_ = now;
          break;

        case CMD_SCREEN_07: {
          std::vector<uint8_t> screen = this->build_screen_frame_();

          this->send_frame(screen);
          this->last_screen_push_ = now;
          this->screen_dirty_ = false;
          this->bump_backlight_("screen push");
          break;
        }

        case CMD_ACTIVITY_19:
          ESP_LOGV(TAG, "Sending ACTIVITY poll: 20 19 01");
          this->send_frame({this->device_id_, 0x19, 0x01});
          this->last_activity_poll_ = now;
          break;

        case CMD_BEEP_0C:
        ESP_LOGV(TAG, "Sending BEEP command: 0C %02X %02X %02X",
                this->beep_mode_, this->beep_period_, this->beep_quiet_period_);
          this->send_frame({this->device_id_, 0x0C, this->beep_mode_, this->beep_period_, this->beep_quiet_period_});
          this->beep_set_ = true;
          break;

        case CMD_BACKLIGHT_0D: {
          uint8_t val = this->backlight_target_on_ ? 0x01 : 0x00;
          this->send_frame({this->device_id_, 0x0D, val});
          this->backlight_on_ = this->backlight_target_on_;
          this->backlight_cmd_pending_ = false;
          break;
        }

        default:
          break;
      }

      this->last_cmd_ = cmd_to_send;
      this->awaiting_reply_ = true;
      this->last_tx_time_ = now;
      this->rx_buf_.clear();
    }
  }

  // ---- RX accumulate ----
  while (this->available()) {
    uint8_t b;
    this->read_byte(&b);
    this->rx_buf_.push_back(b);
  }

  // Process reply after wait window
  if (this->awaiting_reply_ && (now - this->last_tx_time_) >= REPLY_WAIT_MS) {
    if (!this->rx_buf_.empty()) {
      this->handle_reply_for_cmd_(this->rx_buf_);
    }
    this->rx_buf_.clear();
    this->awaiting_reply_ = false;
  }

  // Backlight timeout handling
  if (this->backlight_on_ && now >= this->backlight_off_at_) {
    this->backlight_target_on_ = false;
    this->backlight_cmd_pending_ = true;

    //Clear * in buffer and refresh screen
    if (!this->input_buffer_.empty()) {
      this->input_buffer_.clear();
      this->screen_dirty_ = true;
      ESP_LOGI(TAG, "Backlight timeout cleared input buffer");
    }
  }
}

void HoneywellGalaxy7Keypad::api_write_rs485(const std::string &data) {
  // Add newline so it shows nice on USB dongle / terminal
  std::string out = data;
  out += "\n";

  this->write_array(reinterpret_cast<const uint8_t *>(out.data()), out.size());
  ESP_LOGI(TAG, "Wrote via API: %s", data.c_str());
}

void HoneywellGalaxy7Keypad::dump_config() { ESP_LOGCONFIG(TAG, "Honeywell Galaxy 7 Keypad"); }

std::pair<std::string, bool> HoneywellGalaxy7Keypad::decode_key_and_tamper_(uint8_t code) {
  if (code == 0x7F) {
    return {"", true};
  }

  bool tamper = (code & 0x40) != 0;
  uint8_t key_code = code & 0x0F;
  switch (key_code) {
    case 0x00:
      return {"0", tamper};
    case 0x01:
      return {"1", tamper};
    case 0x02:
      return {"2", tamper};
    case 0x03:
      return {"3", tamper};
    case 0x04:
      return {"4", tamper};
    case 0x05:
      return {"5", tamper};
    case 0x06:
      return {"6", tamper};
    case 0x07:
      return {"7", tamper};
    case 0x08:
      return {"8", tamper};
    case 0x09:
      return {"9", tamper};
    case 0x0A:
      return {"B", tamper};
    case 0x0B:
      return {"A", tamper};
    case 0x0C:
      return {"ENT", tamper};
    case 0x0D:
      return {"ESC", tamper};
    case 0x0E:
      return {"*", tamper};
    case 0x0F:
      return {"#", tamper};
    default:
      return {"", tamper};
  }
}

void HoneywellGalaxy7Keypad::update_tamper_state_(bool new_tamper, const std::string &context) {
  if (new_tamper == this->in_tamper_)
    return;
  this->in_tamper_ = new_tamper;
  ESP_LOGI(TAG, "[TAMPER] %s: %s", context.c_str(), new_tamper ? "ON" : "OFF");
}

void HoneywellGalaxy7Keypad::handle_keypress_(const std::string &key_name, bool tamper) {
  this->bump_backlight_("keypress");

  // ESC clears input buffer
  if (key_name == "ESC") {
    if (!this->input_buffer_.empty()) {
      this->input_buffer_.clear();
      this->screen_dirty_ = true;
      ESP_LOGI(TAG, "Keypad input cleared (ESC)");
    }
    this->screen_dirty_ = true;
    return;
  }

  // ENTER submits to HA + clears
  if (key_name == "ENT") {
    if (!this->input_buffer_.empty()) {
      std::string code = this->input_buffer_;
      this->input_buffer_.clear();
      this->screen_dirty_ = true;
      ESP_LOGI(TAG, "Code entered: %s", code.c_str());
      if (this->rx_sens_ != nullptr) {
        this->rx_sens_->publish_state(code);

      // Clear it shortly after so the next identical code still fires
      this->set_timeout("clear_rx_sens", 200, [this]() {
        if (this->rx_sens_ != nullptr) {
          this->rx_sens_->publish_state("");
        }
      });
      }
    } else {
      ESP_LOGI(TAG, "ENT pressed with no buffered digits");
      this->screen_dirty_ = true;  // force a screen push to carry the ACK
    }
    return;
  }

  // Only buffer printable digits/letters/symbols; display as stars
  if (key_name.size() == 1 &&
      (std::isdigit(static_cast<unsigned char>(key_name[0])) || key_name[0] == '*' || key_name[0] == '#')) {
    this->input_buffer_.push_back(key_name[0]);
    this->screen_dirty_ = true;
  } else if (key_name == "A" || key_name == "B") {
    this->input_buffer_.push_back(key_name[0]);
    this->screen_dirty_ = true;
  }
}

void HoneywellGalaxy7Keypad::handle_reply_for_cmd_(const std::vector<uint8_t> &bytes) {
  if (bytes.empty() || bytes[0] != KEYPAD_ADDR)
    return;

  
  uint32_t now = millis();
  this->last_panel_rx_ms_ = now;

  if (!this->panel_online_) {
    this->panel_online_ = true;
    ESP_LOGI(TAG, "Panel came online, sending init / clearing beep");
    // This is where you do your “proper” init sequence:
    beep_set_ = false;
    screen_dirty_ = true;
  }

  uint8_t type = (bytes.size() >= 2) ? bytes[1] : 0x00;

  // Activity poll: 11 FE BA => no key/tamper change
  if (this->last_cmd_ == CMD_ACTIVITY_19 && type == 0xFE && bytes.size() >= 3 && bytes[2] == 0xBA) {
    return;
  }

  // Screen write: F2 => bad frame, we still owe an ACK for the same key.
  // Re-init the protocol state, then resend a clean 07 with the same ACK flags.
  if (this->last_cmd_ == CMD_SCREEN_07 && type == 0xF2) {
    ESP_LOGW(TAG, "Keypad rejected frame (F2), scheduling re-init: %s",
            bytes_to_hex(bytes).c_str());

    // Do NOT clear key_ack_pending_ / ack_pending_code_ here:
    // keypad still thinks that key is un-acked.
    // We just force a re-init and another 07 to carry that ACK.
    this->need_reinit_after_f2_ = true;
    this->screen_dirty_         = true;  // resend the same logical screen

    return;
  }


  // Screen write: FE BA => busy/OK
  if (this->last_cmd_ == CMD_SCREEN_07 && type == 0xFE && bytes.size() >= 3 && bytes[2] == 0xBA) {
    this->update_tamper_state_(false, "Cleared after screen FE BA");
    ESP_LOGV(TAG, "Screen OK FE BA: %s", bytes_to_hex(bytes).c_str());
    return;
  }

  // Beep/backlight ack: FE BA
  if ((this->last_cmd_ == CMD_BEEP_0C || this->last_cmd_ == CMD_BACKLIGHT_0D) && type == 0xFE && bytes.size() >= 3 &&
      bytes[2] == 0xBA) {
    ESP_LOGV(TAG, "Command ack FE BA: %s", bytes_to_hex(bytes).c_str());
    return;
  }

   // --- F4 shared handler ---
  if (type == 0xF4 && bytes.size() == 4) {
    uint8_t code = bytes[2];
    uint8_t cs   = bytes[3];

    // Validate checksum: {KEYPAD_ADDR, 0xF4, code}
    uint8_t expected = this->galaxy_checksum({KEYPAD_ADDR, 0xF4, code});
    if (expected != cs) {
      ESP_LOGW(TAG, "Bad checksum for F4: %s", bytes_to_hex(bytes).c_str());
      return;
    }

    auto decoded = this->decode_key_and_tamper_(code);
    std::string key_name = decoded.first;
    bool tamper = decoded.second;
    bool tamper_only = key_name.empty() && tamper;

    this->update_tamper_state_(tamper, "From F4");

    // --- SCREEN context: screen ACK / tamper after 07 ---
if (this->last_cmd_ == CMD_SCREEN_07) {
  if (code == 0x7F) {
    ESP_LOGI(TAG, "Screen ACK (tamper=%d): %s",
             tamper ? 1 : 0, bytes_to_hex(bytes).c_str());

    // We finally got confirmation that our ACKed key has been seen.
    this->key_ack_pending_  = false;
    this->ack_pending_code_ = 0x00;
  } else {
    ESP_LOGV(TAG, "Screen reply key=%s%s %s",
             key_name.c_str(),
             tamper ? " [TAMPER]" : "",
             bytes_to_hex(bytes).c_str());
  }
  return;
}


    // --- F4 after ACTIVITY poll: keypress / tamper events ---
    if (this->last_cmd_ == CMD_ACTIVITY_19) {
      // Tamper-only (0x7F) – no key; track tamper, no ACK
      if (tamper_only) {
        ESP_LOGV(TAG, "Tamper-only event: %s", bytes_to_hex(bytes).c_str());
        return;
      }

      // Safety: if it's neither key nor tamper, bail
      if (key_name.empty() && !tamper) {
        ESP_LOGW(TAG, "F4 unknown code=0x%02X: %s", code, bytes_to_hex(bytes).c_str());
        return;
      }

      uint32_t now_ms = millis();
      bool duplicate_time = (key_name == this->last_key_name_) &&
                            (tamper == this->last_key_tamper_) &&
                            (now_ms - this->last_key_ts_ <= KEY_DEDUPE_WINDOW_MS);

      // NEW: duplicate because keypad is re-sending the same key while
      // we're still waiting for an ACK to land.
      bool duplicate_ack = this->key_ack_pending_ && (code == this->ack_pending_code_);

      if (!duplicate_time && !duplicate_ack) {
        ESP_LOGI(TAG, "Key=%s%s %s",
                key_name.c_str(),
                tamper ? " [TAMPER]" : "",
                bytes_to_hex(bytes).c_str());

        this->last_key_name_   = key_name;
        this->last_key_tamper_ = tamper;
        this->last_key_ts_     = now_ms;

        // Only treat non-duplicates as "real" keypresses
        this->handle_keypress_(key_name, tamper);
      } else {
        ESP_LOGV(TAG, "Duplicate key=%s%s (time_dup=%d, ack_dup=%d) ignored %s",
                key_name.c_str(),
                tamper ? " [TAMPER]" : "",
                duplicate_time ? 1 : 0,
                duplicate_ack ? 1 : 0,
                bytes_to_hex(bytes).c_str());
      }

      // Always schedule a screen ACK for any F4 key event (even if duplicate),
      // so the keypad eventually sees an acknowledgement and stops repeating.
      this->needs_button_ack_  = true;
      this->screen_dirty_      = true;
      this->key_ack_pending_   = true;
      this->ack_pending_code_  = code;

      return;
    }


    // --- F4 after other commands (00 poll, beep, backlight, etc.) ---
    if (tamper_only) {
      ESP_LOGV(TAG, "F4 OTHER tamper-only after cmd=%d: %s",
               this->last_cmd_,
               bytes_to_hex(bytes).c_str());
    } else {
      ESP_LOGV(TAG, "F4 OTHER after cmd=%d key=%s%s %s",
               this->last_cmd_,
               key_name.c_str(),
               tamper ? " [TAMPER]" : "",
               bytes_to_hex(bytes).c_str());
    }
    return;
  }
}

}  // namespace honeywell_galaxy7_keypad
}  // namespace esphome
