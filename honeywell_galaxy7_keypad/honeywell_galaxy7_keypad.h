#pragma once

#include "esphome/core/component.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/text_sensor/text_sensor.h"  // 👈 add this
#include <string>                                        // 👈 and this
#include <vector>

namespace esphome {
namespace honeywell_galaxy7_keypad {

enum LastCmd : uint8_t {
  CMD_NONE = 0,
  CMD_POLL_00,
  CMD_ACTIVITY_19,
  CMD_SCREEN_07,
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

  // Optional RX text sensor hook
  void set_rx_text_sensor(text_sensor::TextSensor *sens) { this->rx_sens_ = sens; }
  void set_device_id(uint8_t id) { this->device_id_ = id; }

 protected:
  std::string display_text_{"ESP-HOME|Initializing"};
  text_sensor::TextSensor *rx_sens_{nullptr};

  uint32_t last_init_poll_{0};
  uint32_t last_activity_poll_{0};
  uint32_t last_screen_push_{0};

  bool awaiting_screen_reply_{false};
  LastCmd last_cmd_{CMD_NONE};
  
  bool sent_second_init_{false};
  uint8_t ack_toggle_{0};
  uint8_t device_id_{0x20};
};

}  // namespace honeywell_galaxy7_keypad
}  // namespace esphome
