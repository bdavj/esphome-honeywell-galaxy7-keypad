#include "honeywell_galaxy7_keypad.h"
#include "esphome/core/log.h"
#include <iomanip>
#include <sstream>

namespace esphome {
namespace honeywell_galaxy7_keypad {

static const char *TAG = "honeywell_galaxy7_keypad.component";

constexpr uint32_t INIT_POLL_SECOND_MS      = 200;
constexpr uint32_t INIT_POLL_INTERVAL_MS    = 1000;
constexpr uint32_t SCREEN_PUSH_INTERVAL_MS  = 1000;
constexpr uint32_t ACTIVITY_POLL_INTERVAL_MS = 50;

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
}

void HoneywellGalaxy7Keypad::set_display_text(const std::string &text) {
  if (text.empty()) {
    this->display_text_ = "ESP-HOME|Initializing";
  } else {
    this->display_text_ = text;
  }
  ESP_LOGD(TAG, "Display text set to: %s", this->display_text_.c_str());
}

void HoneywellGalaxy7Keypad::setup() {
  ESP_LOGI(TAG, "Honeywell Galaxy keypad setup starting");

  // First-stage poll (00/0E) to mimic the panel startup sequence.
  this->last_init_poll_ = millis();
  this->send_frame({this->device_id_, 0x00, 0x0E});
}

void HoneywellGalaxy7Keypad::loop() {
  uint32_t now = millis();

  // 1) Init / status polls (00/0F)
  if (!this->sent_second_init_ && (now - this->last_init_poll_ > INIT_POLL_SECOND_MS)) {
    this->send_frame({this->device_id_, 0x00, 0x0F});  // 20 00 0F D9 style
    this->sent_second_init_ = true;
    this->last_init_poll_ = now;
    this->last_cmd_ = CMD_POLL_00;
  } else if (now - this->last_init_poll_ > INIT_POLL_INTERVAL_MS) {
    this->last_init_poll_ = now;
    this->send_frame({this->device_id_, 0x00, 0x0F});
    this->last_cmd_ = CMD_POLL_00;
  }

  // 1b) Periodic screen test after init, every 1s
  if (this->sent_second_init_ && (now - this->last_screen_push_ > SCREEN_PUSH_INTERVAL_MS)) {
    this->last_screen_push_ = now;

    // Split the message into two lines around the first pipe
    std::string line1;
    std::string line2;
    size_t sep = this->display_text_.find('|');
    if (sep != std::string::npos) {
      line1 = this->display_text_.substr(0, sep);
      line2 = this->display_text_.substr(sep + 1);
    } else {
      line1 = this->display_text_;
      line2.clear();
    }

    std::vector<uint8_t> screen = {this->device_id_, 0x07, 0xA1, 0x17};

    screen.insert(screen.end(), line1.begin(), line1.end());
    screen.push_back(0x02);  // second line
    screen.insert(screen.end(), line2.begin(), line2.end());
    screen.push_back(0x07);  // like panel

    // log full frame w/ checksum
    std::vector<uint8_t> frame = screen;
    frame.push_back(this->galaxy_checksum(screen));
    ESP_LOGI(TAG, "Sending screen frame (%d bytes): %s",
             (int) frame.size(), bytes_to_hex(frame).c_str());

    // send once
    this->send_frame(screen);
    this->awaiting_screen_reply_ = true;
    this->last_cmd_ = CMD_SCREEN_07;
  }

  // 2) Activity poll
  if (now - this->last_activity_poll_ > ACTIVITY_POLL_INTERVAL_MS) {
    this->last_activity_poll_ = now;
    this->send_frame({this->device_id_, 0x19, 0x01});
    this->last_cmd_ = CMD_ACTIVITY_19;
  }

  // ---- RX ----
  std::vector<uint8_t> bytes;
  while (this->available()) {
    uint8_t b;
    this->read_byte(&b);
    bytes.push_back(b);
  }
  if (bytes.empty())
    return;

  if (this->awaiting_screen_reply_) {
    ESP_LOGI(TAG, "Screen reply frame (%d bytes): %s",
             (int) bytes.size(), bytes_to_hex(bytes).c_str());
    this->awaiting_screen_reply_ = false;
  }

  if (bytes[0] != 0x11)
    return;

  if (bytes.size() >= 2) {
    uint8_t type = bytes[1];

    // 11 FF 08 00 64 28 – status for 00 poll
    if (type == 0xFF && bytes.size() == 6) {
      return;
    }

    // 11 FE BA – activity, no key
    if (type == 0xFE && bytes.size() == 3) {
      return;
    }

    // 11 F4 .. .. – either key OR screen ACK
    if (type == 0xF4 && bytes.size() == 4) {
      uint8_t code = bytes[2];
      uint8_t cs   = bytes[3];

      std::vector<uint8_t> chk = {0x11, 0xF4, code};
      uint8_t expected = this->galaxy_checksum(chk);
      if (expected != cs) {
        // probably framing / partial frame – ignore
        ESP_LOGW(TAG, "Discarding F4 frame with bad checksum: %s",
                 bytes_to_hex(bytes).c_str());
        return;
      }

      // Screen ACK: last cmd was 07, code = 0x7F
      if (this->last_cmd_ == CMD_SCREEN_07 && code == 0x7F) {
        ESP_LOGI(TAG, "Screen ACK: %s", bytes_to_hex(bytes).c_str());
        return;
      }

      // Keypress: last cmd was 19 and code in 0x00–0x0F
      if (this->last_cmd_ == CMD_ACTIVITY_19 && code <= 0x0F) {
        bool tamper = (code & 0x40) != 0;

        // ACK back to keypad
        this->send_frame({this->device_id_, 0x0B, this->ack_toggle_});
        this->ack_toggle_ = (this->ack_toggle_ == 0x00) ? 0x02 : 0x00;

        char buf[16];
        snprintf(buf, sizeof(buf), "\\x11\\xF4\\x%02X\\x%02X", code, cs);
        if (this->rx_sens_ != nullptr) {
          this->rx_sens_->publish_state(buf);
        }

        return;
      }

      // other 11 F4 variants – ignore
      return;
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

}  // namespace honeywell_galaxy7_keypad
}  // namespace esphome
