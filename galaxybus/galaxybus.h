#pragma once

#include "esphome/core/component.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/hal.h"
#include <cstdint>
#include <cstddef>
#include <vector>

namespace esphome {
namespace galaxybus {

enum class BusError : uint8_t {
  OK = 0,
  TIMEOUT,
  CHECKSUM,
  COLLISION,
  UNKNOWN,
};

class GalaxyBusClient {
 public:
  virtual uint8_t get_bus_address() const = 0;
  virtual void on_bus_frame(const std::vector<uint8_t> &frame) = 0;
  virtual void on_bus_error(BusError err) = 0;
  virtual ~GalaxyBusClient() = default;
};

class GalaxyBus : public uart::UARTDevice, public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  void register_client(GalaxyBusClient *client);

  // Queue a frame to send to the client->get_bus_address() destination.
  // Payload should exclude the checksum; GalaxyBus appends it.
  bool enqueue_frame(GalaxyBusClient *client, const std::vector<uint8_t> &payload);

  // Optional raw write helper for debugging.
  void write_raw(const std::vector<uint8_t> &bytes);

  void set_reply_timeout_ms(uint32_t ms) { this->reply_timeout_ms_ = ms; }
  void set_inter_frame_gap_ms(uint32_t ms) { this->inter_frame_gap_ms_ = ms; }
  void set_max_queue_depth(size_t depth) { this->max_queue_depth_ = depth; }

 protected:
  uint8_t galaxy_checksum_(const std::vector<uint8_t> &data);

  struct PendingTx {
    GalaxyBusClient *client{nullptr};
    std::vector<uint8_t> frame;
  };

  struct ActiveTx {
    GalaxyBusClient *client{nullptr};
    uint32_t sent_at{0};
    std::vector<uint8_t> frame;
  };

  std::vector<GalaxyBusClient *> clients_;
  std::vector<PendingTx> queue_;
  ActiveTx active_{};

  std::vector<uint8_t> rx_buf_;
  uint32_t last_rx_byte_ms_{0};

  uint32_t reply_timeout_ms_{100};
  uint32_t inter_frame_gap_ms_{10};
  size_t max_queue_depth_{8};

  void set_de_(bool on);
  void start_tx_(const PendingTx &tx);
  void finish_active_(BusError err);

  void process_rx_byte_(uint8_t b);
  void process_rx_frame_();
};

}  // namespace galaxybus
}  // namespace esphome
