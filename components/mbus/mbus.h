#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"

#ifdef USE_MBUS_CC1101
#include "esphome/components/cc1101/cc1101.h"
#endif
#ifdef USE_MBUS_SX126X
#include "esphome/components/sx126x/sx126x.h"
#endif
#ifdef USE_MBUS_SX127X
#include "esphome/components/sx127x/sx127x.h"
#endif
#ifdef USE_MBUS_WIRED_UART
#include "esphome/components/uart/uart.h"
#endif

#include <psa/crypto.h>

#include <array>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

namespace esphome::mbus {

enum class WMBusFrameFormat : uint8_t { UNKNOWN, A, B };

#ifdef USE_MBUS_WIRED_UART
enum class MBusWiredUartState : uint8_t {
  IDLE,
  START,
  SEND_RESET_1,
  WAIT_RESET_1,
  SEND_RESET_2,
  WAIT_RESET_2,
  PURGE_RX,
  SEND_SELECT_SECONDARY,
  WAIT_SELECT_ACK,
  WAIT_SELECT_SETTLE,
  SEND_REQ_UD2,
  WAIT_RESPONSE_HEADER,
  WAIT_RESPONSE_BODY,
  DONE,
  RETRY,
  FAILED,
};
#endif

struct MBusReceiveMeta {
  bool has_rssi{false};
  float rssi{0.0f};
  bool has_snr{false};
  float snr{0.0f};
  bool has_freq_offset{false};
  float freq_offset{0.0f};
  bool has_lqi{false};
  uint8_t lqi{0};
};

struct PendingRadioPacket {
  std::vector<uint8_t> packet;
  MBusReceiveMeta meta;
};

struct MBusApplicationFrame {
  uint8_t ci{0};
  const uint8_t *data{nullptr};
  size_t length{0};
  bool has_control{false};
  uint8_t control{0};
  bool has_address{false};
  uint8_t address{0};
  bool has_meter_address{false};
  std::array<uint8_t, 8> meter_address{};
  MBusReceiveMeta meta;
};

enum class MBusRecordFunction : uint8_t { INSTANTANEOUS, MAXIMUM, MINIMUM, VALUE_DURING_ERROR };

// Shared by the record dump log (records.cpp) and every listener's dump_config() (sensor/
// binary_sensor/text_sensor), so the function name text only has one definition.
const char *mbus_record_function_to_string(uint8_t function);

struct MBusRecord {
  size_t pos{0};
  uint8_t dif{0};
  uint8_t vif_raw{0};
  uint8_t vif{0};
  bool has_vif_ext{false};
  uint16_t vif_ext{0};
  std::vector<uint8_t> dife;
  std::vector<uint8_t> vife;
  std::string custom_vif;
  uint32_t storage{0};
  uint32_t tariff{0};
  uint32_t subunit{0};
  MBusRecordFunction function{MBusRecordFunction::INSTANTANEOUS};
  const char *data_type{nullptr};
  std::string raw;
  std::string flags;
  std::string decoded;
  bool has_raw_value{false};
  int64_t raw_value{0};
  double scale{1.0};
  const char *unit{"unknown"};
  bool has_decoded_value{false};
  double decoded_value{0.0};
  bool has_value{false};
  double value{0.0};
};

class MBusRecordListener {
 public:
  virtual ~MBusRecordListener() = default;
  virtual bool matches(const MBusRecord &record) const = 0;
  virtual std::string match_key() const = 0;
  virtual void publish_record(const MBusRecord &record) = 0;
};

class MBusRecordMatcher {
 public:
  void set_dif(uint8_t dif) { this->dif_ = dif; }
  void set_vif(uint8_t vif) { this->vif_ = vif; }
  void set_vif_ext(uint16_t vif_ext) {
    this->vif_ext_ = vif_ext;
    this->has_vif_ext_ = true;
  }
  void set_storage(uint32_t storage) {
    this->storage_ = storage;
    this->has_storage_ = true;
  }
  void set_tariff(uint32_t tariff) {
    this->tariff_ = tariff;
    this->has_tariff_ = true;
  }
  void set_subunit(uint32_t subunit) {
    this->subunit_ = subunit;
    this->has_subunit_ = true;
  }
  void set_function(uint8_t function) { this->function_ = function; }
  void set_vife(std::initializer_list<uint8_t> vife) {
    this->vife_.clear();
    this->vife_.reserve(vife.size());
    for (uint8_t item : vife) {
      this->vife_.push_back(item);
    }
    this->has_vife_ = true;
  }
  bool matches_record(const MBusRecord &record) const {
    if (record.dif != this->dif_ || record.vif != this->vif_) {
      return false;
    }
    if (static_cast<uint8_t>(record.function) != this->function_) {
      return false;
    }
    if (this->has_vif_ext_ && (!record.has_vif_ext || record.vif_ext != this->vif_ext_)) {
      return false;
    }
    if (this->has_storage_ && record.storage != this->storage_) {
      return false;
    }
    if (this->has_tariff_ && record.tariff != this->tariff_) {
      return false;
    }
    if (this->has_subunit_ && record.subunit != this->subunit_) {
      return false;
    }
    if (this->has_vife_ && record.vife != this->vife_) {
      return false;
    }
    return true;
  }
  std::string match_key_base() const {
    char dif[8];
    char vif[8];
    std::snprintf(dif, sizeof(dif), "0x%02X", this->dif_);
    std::snprintf(vif, sizeof(vif), "0x%02X", this->vif_);
    std::string key = "dif=";
    key += dif;
    key += ":vif=";
    key += vif;
    key += ":function=";
    key += std::to_string(this->function_);
    if (this->has_vif_ext_) {
      char vif_ext[12];
      std::snprintf(vif_ext, sizeof(vif_ext), "0x%04X", this->vif_ext_);
      key += ":vif_ext=";
      key += vif_ext;
    }
    if (this->has_storage_) {
      key += ":storage=";
      key += std::to_string(this->storage_);
    }
    if (this->has_tariff_) {
      key += ":tariff=";
      key += std::to_string(this->tariff_);
    }
    if (this->has_subunit_) {
      key += ":subunit=";
      key += std::to_string(this->subunit_);
    }
    if (this->has_vife_) {
      key += ":vife=";
      for (size_t i = 0; i < this->vife_.size(); i++) {
        char item[8];
        std::snprintf(item, sizeof(item), "0x%02X", this->vife_[i]);
        if (i > 0) {
          key += ",";
        }
        key += item;
      }
    }
    return key;
  }
  // Shared dump_config() body for sensor/binary_sensor/text_sensor: DIF/VIF/VIF-extension/
  // function/storage/tariff/subunit/VIFE. Callers log their own LOG_SENSOR/LOG_BINARY_SENSOR/
  // LOG_TEXT_SENSOR header (and any platform-specific fields, e.g. binary_sensor's bit) first.
  void dump_config_matcher(const char *tag) const;

 protected:
  uint8_t dif_{0};
  uint8_t vif_{0};
  uint16_t vif_ext_{0};
  uint32_t storage_{0};
  uint32_t tariff_{0};
  uint32_t subunit_{0};
  uint8_t function_{0};
  std::vector<uint8_t> vife_;
  bool has_vif_ext_{false};
  bool has_storage_{false};
  bool has_tariff_{false};
  bool has_subunit_{false};
  bool has_vife_{false};
};

class MBusComponent : public Component
#ifdef USE_MBUS_CC1101
    ,
                      public cc1101::CC1101Listener
#endif
#ifdef USE_MBUS_SX126X
    ,
                      public sx126x::SX126xListener
#endif
#ifdef USE_MBUS_SX127X
    ,
                      public sx127x::SX127xListener
#endif
#ifdef USE_MBUS_WIRED_UART
    ,
                      public uart::UARTDevice
#endif
{
 public:
#ifdef USE_MBUS_CC1101
  void set_radio_cc1101(cc1101::CC1101Component *radio) { this->radio_cc1101_ = radio; }
#endif
#ifdef USE_MBUS_SX126X
  void set_radio_sx126x(sx126x::SX126x *radio) { this->radio_sx126x_ = radio; }
#endif
#ifdef USE_MBUS_SX127X
  void set_radio_sx127x(sx127x::SX127x *radio) { this->radio_sx127x_ = radio; }
#endif
#ifdef USE_MBUS_WIRED_UART
  void set_wired_uart(uart::UARTComponent *uart) { this->set_uart_parent(uart); }
  void set_wired_update_interval(uint32_t update_interval) { this->wired_update_interval_ = update_interval; }
#endif
  void set_config_id(const std::string &config_id) { this->config_id_ = config_id; }
  void set_secondary_address(uint64_t secondary_address) {
    this->secondary_address_ = secondary_address;
    this->has_secondary_address_ = true;
  }
  void set_meter_id(uint32_t meter_id) {
    this->meter_id_ = meter_id;
    this->has_meter_id_ = true;
  }
  void set_dump_records(bool dump_records) { this->dump_records_enabled_ = dump_records; }
  void register_record_listener(MBusRecordListener *listener);
  void set_encryption_key(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3, uint8_t b4, uint8_t b5, uint8_t b6,
                          uint8_t b7, uint8_t b8, uint8_t b9, uint8_t b10, uint8_t b11, uint8_t b12, uint8_t b13,
                          uint8_t b14, uint8_t b15) {
    this->encryption_key_ = {b0, b1, b2, b3, b4, b5, b6, b7, b8, b9, b10, b11, b12, b13, b14, b15};
    this->has_encryption_key_ = true;
  }

  void setup() override;
  void loop() override;
  void dump_config() override;
  // Must run after any bus/radio this instance depends on: the UART bus is setup_priority::BUS (1000),
  // SX126x/SX127x are setup_priority::PROCESSOR (400), but CC1101 does not override the Component default
  // (setup_priority::DATA, 600) and would otherwise tie with this component, leaving registration order
  // to component declaration order instead of a guaranteed priority.
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

#ifdef USE_MBUS_CC1101
  // CC1101 provides frequency offset and link quality indicator.
  void on_packet(const std::vector<uint8_t> &packet, float freq_offset, float rssi, uint8_t lqi) override;
#endif
#if defined(USE_MBUS_SX126X) || defined(USE_MBUS_SX127X)
  // SX126x and SX127x provide RSSI and SNR.
  void on_packet(const std::vector<uint8_t> &packet, float rssi, float snr) override;
#endif

 protected:
#ifdef USE_MBUS_CC1101
  cc1101::CC1101Component *radio_cc1101_{nullptr};
#endif
#ifdef USE_MBUS_SX126X
  sx126x::SX126x *radio_sx126x_{nullptr};
#endif
#ifdef USE_MBUS_SX127X
  sx127x::SX127x *radio_sx127x_{nullptr};
#endif
  std::string config_id_;
  std::array<uint8_t, 16> encryption_key_{};
  uint32_t meter_id_{0};
  uint64_t secondary_address_{0};
  bool has_encryption_key_{false};
  bool has_meter_id_{false};
  bool has_secondary_address_{false};
  bool dump_records_enabled_{false};
  std::vector<MBusRecordListener *> record_listeners_;

  // Imported once in setup() from encryption_key_ and reused for every decrypt, instead of importing/
  // destroying a PSA key for every received telegram.
  bool has_psa_key_{false};
  mbedtls_svc_key_id_t psa_key_id_{};

#ifdef USE_MBUS_WIRED_UART
  MBusWiredUartState wired_state_{MBusWiredUartState::IDLE};
  uint8_t wired_retry_count_{0};
  uint32_t wired_timer_{0};
  uint32_t wired_timeout_short_{0};
  uint32_t wired_timeout_long_{0};
  uint32_t wired_update_interval_{60000};
  uint32_t wired_next_update_{0};
  std::vector<uint8_t> wired_rx_buffer_;
  size_t wired_expected_length_{0};
  bool wired_update_due_{false};
  static constexpr uint8_t WIRED_MAX_RETRIES = 3;
#endif

  std::deque<PendingRadioPacket> pending_packets_;
  static constexpr size_t MAX_PENDING_PACKETS = 3;

  // The radio callbacks only enqueue; the heavy parsing runs in loop().
  void enqueue_packet_(const std::vector<uint8_t> &packet, const MBusReceiveMeta &meta);
  void process_radio_packet_(const PendingRadioPacket &pending);

  void parse_c_mode_(const std::vector<uint8_t> &packet, const MBusReceiveMeta &meta);
  void parse_t_mode_(const std::vector<uint8_t> &packet, const MBusReceiveMeta &meta);
  void handle_wmbus_link_frame_(const std::vector<uint8_t> &frame, const MBusReceiveMeta &meta);
  void handle_application_frame_(const MBusApplicationFrame &frame);
  void handle_short_application_frame_(const MBusApplicationFrame &frame);
  bool handle_wired_link_frame_(const std::vector<uint8_t> &frame, const MBusReceiveMeta &meta = {});
  void import_psa_key_();
  bool decrypt_mode_5_(const MBusApplicationFrame &frame, size_t payload_offset, size_t encrypted_length,
                       uint8_t access_number, std::vector<uint8_t> *out) const;
  void parse_records_(const std::vector<uint8_t> &payload,
                      const std::function<void(const MBusRecord &)> &callback) const;
  void process_records_(const std::vector<uint8_t> &payload) const;
  void dump_records_(const std::vector<uint8_t> &payload) const;
  void publish_record_(const MBusRecord &record) const;
#ifdef USE_MBUS_WIRED_UART
  void setup_wired_uart_();
  void loop_wired_uart_();
  bool wired_timeout_elapsed_(uint32_t now, uint32_t timeout) const;
  void retry_wired_uart_(const char *reason);
  void reset_wired_transaction_();
  std::array<uint8_t, 5> build_wired_snd_nke_frame_() const;
  std::array<uint8_t, 5> build_wired_req_ud2_frame_() const;
  std::array<uint8_t, 17> build_wired_select_secondary_frame_() const;
#endif
  size_t packet_len_frame_a_(uint8_t l_field) const;
  size_t packet_len_frame_b_(uint8_t l_field) const;
  void manufacturer_to_string_(uint16_t manufacturer, char *out) const;
};

}  // namespace esphome::mbus
