#include "mbus.h"
#include "wmbus_frame_codec.h"

#include "esphome/core/log.h"

namespace esphome::mbus {

static const char *const TAG = "mbus";

void MBusComponent::setup() {
#ifdef USE_MBUS_CC1101
  if (this->radio_cc1101_ != nullptr) {
    this->radio_cc1101_->register_listener(this);
  }
#endif
#ifdef USE_MBUS_SX126X
  if (this->radio_sx126x_ != nullptr) {
    this->radio_sx126x_->register_listener(this);
  }
#endif
#ifdef USE_MBUS_SX127X
  if (this->radio_sx127x_ != nullptr) {
    this->radio_sx127x_->register_listener(this);
  }
#endif
#ifdef USE_MBUS_WIRED_UART
  if (this->parent_ != nullptr) {
    this->setup_wired_uart_();
  }
#endif
  if (this->has_encryption_key_) {
    this->import_psa_key_();
  }
}

void MBusComponent::register_record_listener(MBusRecordListener *listener) {
  const std::string key = listener->match_key();
  for (auto *existing : this->record_listeners_) {
    if (existing->match_key() == key) {
      ESP_LOGW(TAG, "Ignoring duplicate M-Bus listener config: %s", key.c_str());
      return;
    }
  }
  this->record_listeners_.push_back(listener);
}

void MBusComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "M-Bus:");
  ESP_LOGCONFIG(TAG, "  ID: %s", this->config_id_.c_str());
#ifdef USE_MBUS_CC1101
  if (this->radio_cc1101_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Radio: CC1101");
  }
#endif
#ifdef USE_MBUS_SX126X
  if (this->radio_sx126x_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Radio: SX126x");
  }
#endif
#ifdef USE_MBUS_SX127X
  if (this->radio_sx127x_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Radio: SX127x");
  }
#endif
#ifdef USE_MBUS_WIRED_UART
  if (this->parent_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Source: Wired UART");
    ESP_LOGCONFIG(TAG, "  Update interval: %ums", static_cast<unsigned>(this->wired_update_interval_));
  }
#endif
  if (this->has_meter_id_) {
    ESP_LOGCONFIG(TAG, "  Meter ID: 0x%08X", static_cast<unsigned>(this->meter_id_));
  }
  if (this->has_secondary_address_) {
    ESP_LOGCONFIG(TAG, "  Secondary address: 0x%016llX", static_cast<unsigned long long>(this->secondary_address_));
  }
  if (this->has_encryption_key_) {
    ESP_LOGCONFIG(TAG, "  Encryption key: configured");
  }
  ESP_LOGCONFIG(TAG, "  Dump records: %s", this->dump_records_enabled_ ? "YES" : "NO");
}

#ifdef USE_MBUS_CC1101
void MBusComponent::on_packet(const std::vector<uint8_t> &packet, float freq_offset, float rssi, uint8_t lqi) {
  // CC1101 callback: keep it short, defer parsing to loop().
  MBusReceiveMeta meta;
  meta.has_rssi = true;
  meta.rssi = rssi;
  meta.has_freq_offset = true;
  meta.freq_offset = freq_offset;
  meta.has_lqi = true;
  meta.lqi = lqi;
  this->enqueue_packet_(packet, meta);
}
#endif

#if defined(USE_MBUS_SX126X) || defined(USE_MBUS_SX127X)
void MBusComponent::on_packet(const std::vector<uint8_t> &packet, float rssi, float snr) {
  // SX126x/SX127x callback: no frequency offset or LQI available.
  MBusReceiveMeta meta;
  meta.has_rssi = true;
  meta.rssi = rssi;
  meta.has_snr = true;
  meta.snr = snr;
  this->enqueue_packet_(packet, meta);
}
#endif

void MBusComponent::enqueue_packet_(const std::vector<uint8_t> &packet, const MBusReceiveMeta &meta) {
  if (packet.empty()) {
    return;
  }
  if (this->pending_packets_.size() >= MAX_PENDING_PACKETS) {
    ESP_LOGW(TAG, "Packet queue full, dropping oldest packet");
    this->pending_packets_.pop_front();
  }
  this->pending_packets_.push_back(PendingRadioPacket{packet, meta});
}

void MBusComponent::loop() {
#ifdef USE_MBUS_WIRED_UART
  if (this->parent_ != nullptr) {
    this->loop_wired_uart_();
  }
#endif
  if (this->pending_packets_.empty()) {
    return;
  }
  PendingRadioPacket pending = std::move(this->pending_packets_.front());
  this->pending_packets_.pop_front();
  this->process_radio_packet_(pending);
}

void MBusComponent::process_radio_packet_(const PendingRadioPacket &pending) {
  const std::vector<uint8_t> &packet = pending.packet;
  if (packet.empty()) {
    return;
  }

  if (packet[0] == wmbus_frame_codec::WMBUS_MODE_C_PREFIX) {
    this->parse_c_mode_(packet, pending.meta);
  } else {
    this->parse_t_mode_(packet, pending.meta);
  }
}

}  // namespace esphome::mbus
