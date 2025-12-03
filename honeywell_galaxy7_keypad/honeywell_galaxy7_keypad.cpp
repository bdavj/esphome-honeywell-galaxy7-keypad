#include "honeywell_galaxy7_keypad.h"
#include "esphome/core/log.h"
#include <cstring>
#include <iomanip>
#include <sstream>

namespace esphome {
namespace honeywell_galaxy7_keypad {

static const char *TAG = "honeywell_galaxy7_keypad.component";

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

void HoneywellGalaxy7Keypad::setup() {
  ESP_LOGI(TAG, "Honeywell Galaxy keypad setup starting");

  // First-stage poll (00/0E) to mimic the panel startup sequence.
  this->last_init_poll_ = millis();
  this->send_frame({this->device_id_, 0x00, 0x0E});
}

void HoneywellGalaxy7Keypad::loop() {
  uint32_t now = millis();

  // 1) Two-stage "init" / status polls (00/0E then 00/0F)
  if (!this->sent_second_init_ && (now - this->last_init_poll_ > 200)) {
    // Stage 2 poll seen on real panels: 20 00 0F D9
    this->send_frame({this->device_id_, 0x00, 0x0F});
    this->sent_second_init_ = true;
    this->last_init_poll_ = now;
  } else if (now - this->last_init_poll_ > 1000) {  // every 1s after init
    this->last_init_poll_ = now;
    this->send_frame({this->device_id_, 0x00, 0x0F});
  }

  // 1b) Periodic screen test after init, every 1s
  if (this->sent_second_init_ && (now - this->last_screen_push_ > 1000)) {
    this->last_screen_push_ = now;

    // id, cmd=07, flags=A1, subcmd=17 (reset display)
    std::vector<uint8_t> screen = {this->device_id_, 0x07, 0xA1, 0x17};

    // optional: pad first line a bit like the ADT frame does
    // screen.insert(screen.end(), 6, 0x20);  // 6 spaces

    const char *msg = "HELLO";
    screen.insert(screen.end(), msg, msg + strlen(msg));

    // move to second line (0x40)
    screen.push_back(0x02);

    const char *msg2 = "TEST";
    screen.insert(screen.end(), msg2, msg2 + strlen(msg2));

    // trailing 0x07 like your real frame
    screen.push_back(0x07);

    // now either just:
    this->send_frame(screen);

    // or if you want to log explicitly including checksum:
    uint8_t cs = this->galaxy_checksum(screen);
    std::vector<uint8_t> frame = screen;
    frame.push_back(cs);

    char buf[128];
    int pos = 0;
    for (auto b : frame) pos += sprintf(buf + pos, "%02X ", b);
    ESP_LOGI(TAG, "Sending screen frame (%d bytes): %s", (int)frame.size(), buf);

    this->send_frame(screen);  // send_frame will append the same cs
    ESP_LOGI(TAG, "Sending screen frame (%d bytes): %s", (int) frame.size(), bytes_to_hex(frame).c_str());
    this->write_array(frame.data(), frame.size());
    this->awaiting_screen_reply_ = true;
  }

  // 2) Frequent activity poll (19) to catch keypresses
  if (now - this->last_activity_poll_ > 50) {  // ~20 Hz
    this->last_activity_poll_ = now;
    // Burton example: 10 19 01 D4 → so {id, 0x19, 0x01}
    this->send_frame({this->device_id_, 0x19, 0x01});
  }

  // ---- RX: grab whatever we got this cycle ----
  std::vector<uint8_t> bytes;
  while (this->available()) {
    uint8_t b;
    this->read_byte(&b);
    bytes.push_back(b);
  }

  if (bytes.empty())
    return;

  if (this->awaiting_screen_reply_) {
    ESP_LOGI(TAG, "Screen reply frame (%d bytes): %s", (int) bytes.size(), bytes_to_hex(bytes).c_str());
    this->awaiting_screen_reply_ = false;
  }

  // We don't want to spam logs with every poll reply, so keep this very light
  // ESP_LOGD(TAG, "Raw frame len=%d", (int)bytes.size());

  // All keypad → panel frames start with 0x11 in your captures
  if (bytes[0] != 0x11) {
    // Not a keypad frame we care about
    return;
  }

  if (bytes.size() >= 2) {
    uint8_t type = bytes[1];

    // 11 FF 08 00 64 28  → initial/status response to 0x00 poll
    if (type == 0xFF && bytes.size() == 6) {
      // Known-good "I'm alive" frame – ignore for HA
      // ESP_LOGD(TAG, "Status frame (00 poll), ignoring");
      return;
    }

    // 11 FE BA → activity poll (19) with NO key pressed
    if (type == 0xFE && bytes.size() == 3) {
      // No key pressed – also ignore
      // ESP_LOGD(TAG, "Activity no-key frame, ignoring");
      return;
    }

    // 11 F4 <key> <csum> → activity poll (19) WITH a key
    if (type == 0xF4 && bytes.size() == 4) {
      uint8_t key_code = bytes[2];
      uint8_t cs = bytes[3];

      // Optional: verify checksum of [11, F4, key_code] matches cs
      std::vector<uint8_t> chk_bytes = {0x11, 0xF4, key_code};
      uint8_t expected = this->galaxy_checksum(chk_bytes);
      if (expected != cs) {
        ESP_LOGW(TAG, "Key frame bad checksum: got %02X, expected %02X; frame: %s", cs, expected,
                 bytes_to_hex(bytes).c_str());
        return;
      }

      bool tamper = (key_code & 0x40) != 0;
      if (tamper) {
        //ESP_LOGI(TAG, "Tamper active (key_code=0x%02X)", key_code);
      } else {
        //ESP_LOGI(TAG, "Keypress frame: key_code=0x%02X", key_code);
      }

      // Acknowledge the key/tamper so the keypad does not resend the same event.
      this->send_frame({this->device_id_, 0x0B, this->ack_toggle_});
      this->ack_toggle_ = (this->ack_toggle_ == 0x00) ? 0x02 : 0x00;

      // Build short hex string for HA
      char buf[16];
      snprintf(buf, sizeof(buf), "\\x11\\xF4\\x%02X\\x%02X", key_code, cs);

      if (this->rx_sens_ != nullptr) {
        this->rx_sens_->publish_state(buf);
      }

      return;
    }
  }

  // Anything else: ignore by default
  // ESP_LOGD(TAG, "Unhandled frame type, ignoring");
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

// uint8_t payload[] = { device_id, cmd, arg };  // whatever bytes, no checksum
// uint8_t cs = galaxy_checksum(payload, sizeof(payload));
// uint8_t frame[sizeof(payload) + 1];
// memcpy(frame, payload, sizeof(payload));
// frame[sizeof(payload)] = cs;
// this->write_array(frame, sizeof(frame));
