#include "galaxybus.h"
#include "esphome/core/log.h"

namespace esphome {
namespace galaxybus {

static const char *TAG = "galaxybus";

uint8_t GalaxyBus::galaxy_checksum_(const std::vector<uint8_t> &data) {
  // Same as HoneywellGalaxy7Keypad::galaxy_checksum
  uint32_t temp = 0xAA;
  for (auto b : data) {
    temp += b;
  }
  return ((temp >> 24) & 0xFF)
       + ((temp >> 16) & 0xFF)
       + ((temp >> 8)  & 0xFF)
       + ( temp        & 0xFF);
}

void GalaxyBus::register_client(GalaxyBusClient *client) {
  if (client == nullptr)
    return;
  this->clients_.push_back(client);
}

bool GalaxyBus::enqueue_frame(GalaxyBusClient *client, const std::vector<uint8_t> &payload) {
  if (client == nullptr || payload.empty())
    return false;

  if (this->active_.client == client) {
    ESP_LOGW(TAG, "Client already has an active transaction, dropping enqueue");
    return false;
  }

  for (const auto &q : this->queue_) {
    if (q.client == client) {
      ESP_LOGW(TAG, "Client already queued, dropping enqueue");
      return false;
    }
  }

  if (this->queue_.size() >= this->max_queue_depth_) {
    ESP_LOGW(TAG, "GalaxyBus queue full (depth=%zu), dropping", this->max_queue_depth_);
    return false;
  }

  PendingTx tx;
  tx.client = client;
  tx.frame = payload;
  tx.frame.push_back(this->galaxy_checksum_(payload));
  this->queue_.push_back(std::move(tx));
  return true;
}

void GalaxyBus::write_raw(const std::vector<uint8_t> &bytes) {
  if (bytes.empty())
    return;
  this->write_array(bytes.data(), bytes.size());
  this->flush();
}

void GalaxyBus::setup() {
  ESP_LOGI(TAG, "Setting up GalaxyBus");
}

void GalaxyBus::loop() {
  uint32_t now = millis();

  if (this->active_.client != nullptr && (now - this->active_.sent_at) > this->reply_timeout_ms_) {
    ESP_LOGW(TAG, "Reply timeout (%ums) waiting for client %p", this->reply_timeout_ms_, this->active_.client);
    this->finish_active_(BusError::TIMEOUT);
  }

  while (this->available()) {
    uint8_t b;
    this->read_byte(&b);
    this->process_rx_byte_(b);
  }

  if (!this->rx_buf_.empty() && (now - this->last_rx_byte_ms_) >= this->inter_frame_gap_ms_) {
    this->process_rx_frame_();
  }

  if (this->active_.client == nullptr && !this->queue_.empty()) {
    PendingTx next = this->queue_.front();
    this->queue_.erase(this->queue_.begin());
    this->start_tx_(next);
  }
}

void GalaxyBus::dump_config() {
  ESP_LOGCONFIG(TAG, "GalaxyBus");
  ESP_LOGCONFIG(TAG, "  Reply timeout: %u ms", this->reply_timeout_ms_);
  ESP_LOGCONFIG(TAG, "  Inter-frame gap: %u ms", this->inter_frame_gap_ms_);
}

void GalaxyBus::set_de_(bool on) { (void) on; }

void GalaxyBus::start_tx_(const PendingTx &tx) {
  this->active_ = {tx.client, millis(), tx.frame};
  this->rx_buf_.clear();
  this->write_array(tx.frame.data(), tx.frame.size());
  this->flush();
}

void GalaxyBus::finish_active_(BusError err) {
  if (this->active_.client != nullptr && err != BusError::OK) {
    this->active_.client->on_bus_error(err);
  }
  this->active_.client = nullptr;
  this->active_.frame.clear();
  this->rx_buf_.clear();
}

void GalaxyBus::process_rx_byte_(uint8_t b) {
  this->rx_buf_.push_back(b);
  this->last_rx_byte_ms_ = millis();
}

void GalaxyBus::process_rx_frame_() {
  if (this->rx_buf_.empty())
    return;

  auto frame = this->rx_buf_;
  this->rx_buf_.clear();

  if (this->active_.client == nullptr) {
    ESP_LOGW(TAG, "RX frame but no active client: size=%zu", frame.size());
    return;
  }

  if (frame.size() >= 2) {
    std::vector<uint8_t> without_checksum(frame.begin(), frame.end() - 1);
    uint8_t expected = this->galaxy_checksum_(without_checksum);
    if (expected != frame.back()) {
      ESP_LOGW(TAG, "Checksum mismatch exp=%02X got=%02X", expected, frame.back());
      this->finish_active_(BusError::CHECKSUM);
      return;
    }
  }

  this->active_.client->on_bus_frame(frame);
  this->finish_active_(BusError::OK);
}

}  // namespace galaxybus
}  // namespace esphome
