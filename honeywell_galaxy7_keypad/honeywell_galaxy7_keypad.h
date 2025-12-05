#pragma once

#include "esphome/core/component.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/text_sensor/text_sensor.h"  // 👈 add this
#include <string>                                        // 👈 and this
#include <vector>
#include <cstdint>
#include <utility>

namespace esphome {
namespace honeywell_galaxy7_keypad {

enum LastCmd : uint8_t {
  CMD_NONE = 0,
  CMD_POLL_00,
  CMD_ACTIVITY_19,
  CMD_SCREEN_07,
  CMD_BEEP_0C,
  CMD_BACKLIGHT_0D,
};

class HoneywellGalaxy7Keypad : public uart::UARTDevice, public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  uint8_t galaxy_checksum(const std::vector<uint8_t> &data);
  void send_frame(const std::vector<uint8_t> &payload);

  // API service: write data to RS485
  void api_write_rs485(const std::string &data);
  void set_display_text(const std::string &text);
  void set_display_text_nobl(const std::string &text);

  // Optional controls exposed to automations
  void set_backlight_timeout(uint32_t timeout_ms) { this->backlight_timeout_ms_ = timeout_ms; }
  void set_beep_enabled(bool enabled, uint8_t beep_period = 0x00, uint8_t quiet_period = 0x00);

  // Call this whenever we successfully parse a frame from the panel
  void on_panel_frame_received_(const std::vector<uint8_t> &frame);
  bool is_panel_online() const { return this->panel_online_; }

  // Optional RX text sensor hook
  void set_rx_text_sensor(text_sensor::TextSensor *sens) { this->rx_sens_ = sens; }
  void set_device_id(uint8_t id) { this->device_id_ = id; }

 protected:
  void parse_display_text_();
  void queue_screen_push_();
  void handle_reply_for_cmd_(const std::vector<uint8_t> &bytes);
  void handle_keypress_(const std::string &key_name, bool tamper);
  std::pair<std::string, bool> decode_key_and_tamper_(uint8_t code);
  void update_tamper_state_(bool new_tamper, const std::string &context);
  std::vector<uint8_t> build_screen_frame_();
  void bump_backlight_(const char *reason);

  std::string display_text_{"ESP-HOME|Initializing"};
  std::string display_line1_{"ESP-HOME"};
  std::string display_line2_{"Initializing"};
  std::string input_buffer_;
  text_sensor::TextSensor *rx_sens_{nullptr};

  uint32_t last_init_poll_{0};
  uint32_t last_activity_poll_{0};
  uint32_t last_screen_push_{0};
  uint32_t last_tx_time_{0};
  uint32_t last_key_ts_{0};
  uint32_t backlight_off_at_{0};

  bool awaiting_reply_{false};
  LastCmd last_cmd_{CMD_NONE};
  bool sent_second_init_{false};
  bool needs_button_ack_{false};
  bool screen_dirty_{true};
  bool beep_set_{false};
  bool backlight_on_{false};
  bool backlight_cmd_pending_{false};
  bool backlight_target_on_{false};
  bool in_tamper_{false};

  std::string last_key_name_;
  bool last_key_tamper_{false};

  uint32_t backlight_timeout_ms_{15000};
  uint8_t ack_toggle_{0x02};  
  uint8_t device_id_{0x20};
  uint8_t beep_mode_{0x00};
  uint8_t beep_period_{0x00};
  uint8_t beep_quiet_period_{0x00};
  uint8_t screen_seq_flag_{0x00};

  bool need_reinit_after_f2_{false};
  bool key_ack_pending_{false};       // NEW: we owe an ACK to a key
  uint8_t ack_pending_code_{0x00};    // NEW: raw F4 code for that key

  std::vector<uint8_t> rx_buf_;

  uint32_t last_panel_rx_ms_{0};
  bool panel_online_{false};
};

}  // namespace honeywell_galaxy7_keypad
}  // namespace esphome
