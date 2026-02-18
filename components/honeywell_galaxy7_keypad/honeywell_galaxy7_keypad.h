#pragma once

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/core/helpers.h"
#include "../galaxybus/galaxybus.h"
#include <string>
#include <utility>
#include <vector>

namespace esphome {
namespace honeywell_galaxy7_keypad {

class HoneywellBeepSwitch;

enum LastCmd : uint8_t {
  CMD_NONE = 0,
  CMD_POLL_00,
  CMD_SCREEN_07,
  CMD_ACTIVITY_19,
  CMD_BEEP_0C,
  CMD_BACKLIGHT_0D,
  CMD_BEEP_ONESHOT,
  CMD_PROX_POLL,
};

class HoneywellGalaxy7Keypad : public galaxybus::GalaxyBusClient, public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  uint8_t get_bus_address() const override { return this->device_id_; }
  void on_bus_frame(const std::vector<uint8_t> &frame) override;
  void on_bus_error(galaxybus::BusError err) override;

  void set_bus(galaxybus::GalaxyBus *bus) { this->bus_ = bus; }
  void set_display_text(const std::string &text);
  void set_display_text_nobl(const std::string &text);
  void set_backlight_timeout(uint32_t timeout_ms) { this->backlight_timeout_ms_ = timeout_ms; }
  void set_beep_enabled(bool enabled, uint8_t beep_period = 0x00, uint8_t quiet_period = 0x00);
  void enable_prox_polling(bool enabled) { this->prox_enabled_ = enabled; }

  bool is_panel_online() const { return this->panel_online_; }

  void set_code_text_sensor(text_sensor::TextSensor *sens) { this->code_sens_ = sens; }
  void set_rx_text_sensor(text_sensor::TextSensor *sens) { this->rx_debug_sens_ = sens; }
  void set_tamper_binary_sensor(binary_sensor::BinarySensor *sens) { this->tamper_sens_ = sens; }
  void set_panel_online_binary_sensor(binary_sensor::BinarySensor *sens) { this->panel_online_sens_ = sens; }
  void set_page_sensor(sensor::Sensor *sens) { this->page_sensor_ = sens; }

  void set_beep_switch(HoneywellBeepSwitch *sw) { this->beep_switch_ = sw; }

  void set_device_id(uint8_t id) { this->device_id_ = id; this->prox_id_ = this->compute_prox_id_(); }
  void handle_beep_switch_state_(bool state);
  uint8_t compute_prox_id_() const;

 private:
  galaxybus::GalaxyBus *bus_{nullptr};

  void parse_display_text_();
  void queue_screen_push_();
  void bump_backlight_(const char *reason);
  std::vector<uint8_t> build_screen_frame_();
  void handle_reply_for_cmd_(const std::vector<uint8_t> &bytes);
  void handle_keypress_(const std::string &key_name, bool tamper);
  std::pair<std::string, bool> decode_key_and_tamper_(uint8_t code);
  void update_tamper_state_(bool new_tamper, const std::string &context);
  void update_panel_online_state_(bool online);
  void publish_code_(const std::string &code);
  void publish_page_();
  void sync_beep_switch_state_();
  void beep_once_();
  uint8_t galaxy_checksum_(const std::vector<uint8_t> &data);
  void send_frame(const std::vector<uint8_t> &payload, LastCmd cmd);

  // Entities
  text_sensor::TextSensor *code_sens_{nullptr};
  text_sensor::TextSensor *rx_debug_sens_{nullptr};
  binary_sensor::BinarySensor *tamper_sens_{nullptr};
  binary_sensor::BinarySensor *panel_online_sens_{nullptr};
  HoneywellBeepSwitch *beep_switch_{nullptr};

  // Pages
  int page_index_{0};
  sensor::Sensor *page_sensor_{nullptr};

  // State
  std::string display_text_{"ESP-HOME|Initializing"};
  std::string display_line1_{"ESP-HOME"};
  std::string display_line2_{"Initializing"};
  std::string input_buffer_;

  uint32_t last_init_poll_{0};
  uint32_t last_activity_poll_{0};
  uint32_t last_screen_push_{0};
  uint32_t last_key_ts_{0};
  uint32_t backlight_off_at_{0};
  uint32_t last_panel_rx_ms_{0};
  uint32_t last_prox_poll_{0};
  uint32_t last_prox_event_ms_{0};

  bool awaiting_reply_{false};
  LastCmd last_cmd_{CMD_NONE};
  bool sent_second_init_{false};
  bool screen_dirty_{true};
  bool beep_set_{false};
  bool backlight_on_{false};
  bool backlight_cmd_pending_{false};
  bool backlight_target_on_{false};
  bool in_tamper_{false};
  bool panel_online_{false};
  bool prox_enabled_{false};
  bool pending_prox_beep_{false};

  std::string last_key_name_;
  std::string last_prox_payload_;
  bool last_key_tamper_{false};

  uint32_t backlight_timeout_ms_{15000};
  uint8_t ack_toggle_{0x02};
  uint8_t device_id_{0x20};
  uint8_t prox_id_{0x91};
  uint8_t beep_mode_{0x00};
  uint8_t beep_period_{0x00};
  uint8_t beep_quiet_period_{0x00};
  uint8_t screen_seq_flag_{0x00};

  bool need_reinit_after_f2_{false};
  bool key_ack_pending_{false};
  uint8_t ack_pending_code_{0x00};
  bool needs_button_ack_{false};
};

class HoneywellBeepSwitch : public switch_::Switch {
 public:
  explicit HoneywellBeepSwitch(HoneywellGalaxy7Keypad *parent) : parent_(parent) {}

 protected:
  void write_state(bool state) override;
  HoneywellGalaxy7Keypad *parent_;
};

}  // namespace honeywell_galaxy7_keypad
}  // namespace esphome
