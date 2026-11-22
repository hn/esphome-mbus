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

#include "crypto.h"
#include "wired_frame_codec.h"

#include <array>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

namespace esphome::mbus {

// Hex dumps in log lines are cut off after this many bytes
static constexpr size_t MBUS_LOG_HEX_MAX_BYTES = 64;

enum class WMBusFrameFormat : uint8_t { WMBUS_FRAME_FORMAT_UNKNOWN, WMBUS_FRAME_FORMAT_A, WMBUS_FRAME_FORMAT_B };

#ifdef USE_MBUS_WIRED_UART
enum class MBusWiredOperation : uint8_t {
  // SND_NKE, selection if needed, REQ_UD2 until the meter has no more datagrams
  MBUS_WIRED_OPERATION_READOUT,
  MBUS_WIRED_OPERATION_SEND_NKE,
  MBUS_WIRED_OPERATION_REQUEST_UD2,
  MBUS_WIRED_OPERATION_SEND_UD,
};

enum class MBusWiredUartState : uint8_t {
  MBUS_WIRED_UART_STATE_IDLE,
  MBUS_WIRED_UART_STATE_START,
  MBUS_WIRED_UART_STATE_WAKEUP_SEND,
  MBUS_WIRED_UART_STATE_WAKEUP_DRAIN,
  MBUS_WIRED_UART_STATE_WAKEUP_PAUSE,
  MBUS_WIRED_UART_STATE_SEND_NKE,
  MBUS_WIRED_UART_STATE_WAIT_NKE,
  MBUS_WIRED_UART_STATE_WAIT_GAP,
  MBUS_WIRED_UART_STATE_SEND_SELECT_SECONDARY,
  MBUS_WIRED_UART_STATE_WAIT_SELECT_ACK,
  MBUS_WIRED_UART_STATE_WAIT_SELECT_SETTLE,
  MBUS_WIRED_UART_STATE_SEND_DATAGRAM,
  MBUS_WIRED_UART_STATE_WAIT_DATAGRAM,
  MBUS_WIRED_UART_STATE_WAIT_QUIET,
};
#endif

// Result of a datagram sent with send_nke(), request_ud2() or send_ud()
enum class MBusResult : uint8_t {
  MBUS_RESULT_OK,
  // Another exchange is running on this instance or on its UART
  MBUS_RESULT_BUSY,
  MBUS_RESULT_NO_RESPONSE,
  MBUS_RESULT_INVALID_RESPONSE,
  // The datagram cannot be sent: too much data, or the instance has no wired UART
  MBUS_RESULT_INVALID_REQUEST,
};

const LogString *mbus_result_to_str(MBusResult result);

// Answer to a datagram. data is only valid while the callback runs.
struct MBusResponse {
  // Single character acknowledge; ci and data are not set then
  bool ack{false};
  uint8_t ci{0};
  // Application data behind the header: behind the long header with CI 72h and 76h, behind the short header with
  // 7Ah, everything behind the CI field else
  std::span<const uint8_t> data{};
};

using mbus_response_callback_t = void (*)(void *context, MBusResult result, const MBusResponse &response);

// The part of application data that follows the manufacturer data header (DIF 0Fh or 1Fh); empty without one
std::span<const uint8_t> manufacturer_data(std::span<const uint8_t> data);

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

enum class MBusRecordFunction : uint8_t {
  MBUS_RECORD_FUNCTION_INSTANTANEOUS,
  MBUS_RECORD_FUNCTION_MAXIMUM,
  MBUS_RECORD_FUNCTION_MINIMUM,
  MBUS_RECORD_FUNCTION_VALUE_DURING_ERROR,
};

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
  MBusRecordFunction function{MBusRecordFunction::MBUS_RECORD_FUNCTION_INSTANTANEOUS};
  const char *data_type{nullptr};
  const uint8_t *raw_data{nullptr};  // points into the payload being parsed
  size_t raw_length{0};
  std::string flags;
  std::string decoded;
  bool has_raw_value{false};
  int64_t raw_value{0};
  double scale{1.0};
  const LogString *unit{nullptr};
  bool has_decoded_value{false};
  double decoded_value{0.0};
  bool has_value{false};
  double value{0.0};
};

class MBusRecordListener {
 public:
  virtual ~MBusRecordListener() = default;
  virtual bool matches(const MBusRecord &record) const = 0;
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

// Radio instances only listen; their update interval stays at the PollingComponent default "never".
class MBusComponent : public PollingComponent
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
  // A datagram and up to two repetitions (EN 1434-3:2008, Annex D.2.1)
  static constexpr uint8_t WIRED_MAX_ATTEMPTS = 3;

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
  void set_wakeup_after_idle(uint32_t wakeup_after_idle) {
    this->wired_wakeup_after_idle_ = wakeup_after_idle;
    this->wired_wakeup_enabled_ = true;
  }
  void set_max_telegrams(uint8_t max_telegrams) { this->wired_max_telegrams_ = max_telegrams; }
  void set_primary_address(uint8_t primary_address) { this->wired_address_ = primary_address; }
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

  // One datagram to the meter of this instance, wired instances only. The return value tells whether it was
  // started; the callback gets the outcome. With a secondary address the selection is sent first.
  // max_retries: repetitions of the datagram if the answer is missing or invalid.
  // SND_NKE; resets the frame count bits
  MBusResult send_nke(uint8_t max_retries = WIRED_MAX_ATTEMPTS - 1, mbus_response_callback_t callback = nullptr,
                      void *context = nullptr);
  // REQ_UD2 without SND_NKE in front; does not read on at DIF 1Fh, the next call gets the next datagram
  MBusResult request_ud2(uint8_t max_retries = WIRED_MAX_ATTEMPTS - 1, mbus_response_callback_t callback = nullptr,
                         void *context = nullptr);
  // SND_UD; the answer is an acknowledge or RSP_UD. Repeat only what is harmless to do twice.
  MBusResult send_ud(uint8_t ci, const uint8_t *data, size_t length, uint8_t max_retries = 0,
                     mbus_response_callback_t callback = nullptr, void *context = nullptr);

  void setup() override;
  void loop() override;
  void update() override;
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
  // Status byte of the last long header, to log changes only
  uint8_t last_status_{0};
  // Set by the record parser on DIF 1Fh: the meter has more datagrams (EN 13757-3:2018, 6.5)
  bool more_records_follow_{false};
  std::vector<MBusRecordListener *> record_listeners_;

  // Set up once in setup() from encryption_key_ and reused for every telegram
  Aes128CbcDecryptor decryptor_;

#ifdef USE_MBUS_WIRED_UART
  MBusWiredUartState wired_state_{MBusWiredUartState::MBUS_WIRED_UART_STATE_IDLE};
  MBusWiredUartState wired_gap_next_state_{MBusWiredUartState::MBUS_WIRED_UART_STATE_IDLE};
  MBusWiredUartState wired_retry_state_{MBusWiredUartState::MBUS_WIRED_UART_STATE_IDLE};
  // FCB of the next REQ_UD2 and of the next SND_UD: one per direction (EN 13757-2:2018, 5.7.7)
  bool wired_fcb_req_{true};
  bool wired_fcb_snd_{true};
  MBusWiredOperation wired_operation_{MBusWiredOperation::MBUS_WIRED_OPERATION_READOUT};
  uint8_t wired_max_attempts_{WIRED_MAX_ATTEMPTS};
  MBusResult wired_failure_result_{MBusResult::MBUS_RESULT_NO_RESPONSE};
  mbus_response_callback_t wired_callback_{nullptr};
  void *wired_callback_context_{nullptr};
  // SND_UD datagram, kept for its repetitions
  std::array<uint8_t, wired_frame_codec::LONG_FRAME_MAX_SIZE> wired_tx_buffer_{};
  size_t wired_tx_size_{0};
  // Address of the link layer datagrams: the primary address, or 253 for a meter selected by its secondary
  // address
  uint8_t wired_address_{wired_frame_codec::ADDRESS_SELECTED};
  uint8_t wired_nke_count_{0};
  // Wake-up of the optical interface: sent before a datagram if the line was idle for longer than this
  uint32_t wired_wakeup_after_idle_{0};
  // End of the last byte sent or received
  uint32_t wired_last_activity_{0};
  // Start of the wake-up on the line and what is left to send of it
  uint32_t wired_wakeup_start_{0};
  uint32_t wired_wakeup_sent_{0};
  uint32_t wired_wakeup_remaining_{0};
  MBusWiredUartState wired_wakeup_next_state_{MBusWiredUartState::MBUS_WIRED_UART_STATE_IDLE};
  bool wired_wakeup_enabled_{false};
  // Set at boot and after a missing answer: the meter is taken to be asleep whatever the idle time says
  bool wired_wakeup_forced_{true};
  uint8_t wired_max_telegrams_{8};
  uint8_t wired_telegram_count_{0};
  // Instances with a wired UART, chained to find out whether the bus of this one is in use
  MBusComponent *wired_next_{nullptr};
  uint8_t wired_retry_count_{0};
  uint32_t wired_timer_{0};
  uint32_t wired_timeout_{0};
  wired_frame_codec::FrameReceiver wired_receiver_;
  const char *wired_failure_reason_{""};
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
  void log_status_change_(uint8_t status);
  void handle_wired_link_frame_(const wired_frame_codec::Frame &frame);
  void init_decryptor_();
  bool decrypt_mode_5_(const MBusApplicationFrame &frame, size_t payload_offset, size_t encrypted_length,
                       uint8_t access_number, std::vector<uint8_t> *out);
  // Calls callback(const MBusRecord &) for every record; defined and only used in records.cpp
  template<typename F> void parse_records_(const std::vector<uint8_t> &payload, F &&callback);
  void process_records_(const std::vector<uint8_t> &payload);
  void dump_records_(const std::vector<uint8_t> &payload);
  bool is_wired_() const {
#ifdef USE_MBUS_WIRED_UART
    return this->parent_ != nullptr;
#else
    return false;
#endif
  }
  void publish_record_(const MBusRecord &record) const;
#ifdef USE_MBUS_WIRED_UART
  void setup_wired_uart_();
  void loop_wired_uart_();
  void start_wired_readout_();
  bool wired_timeout_elapsed_(uint32_t now) const;
  void drain_wired_rx_();
  void send_wired_(const uint8_t *data, size_t size, size_t max_response_size, uint32_t now);
  void send_wired_short_(uint8_t control, size_t max_response_size, uint32_t now);
  void wired_begin_exchange_(MBusWiredUartState send_state, uint8_t attempts);
  wired_frame_codec::ReceiveResult receive_wired_();
  void retry_wired_uart_(const char *reason, MBusResult result, uint32_t now);
  void finish_wired_transaction_(MBusResult result);
  MBusResult start_wired_operation_(MBusWiredOperation operation, uint8_t attempts, mbus_response_callback_t callback,
                                    void *context);
  MBusResult wired_available_() const;
  bool wired_uart_busy_() const;
  bool wired_wakeup_(MBusWiredUartState next_state, uint32_t now);
  void set_wired_parity_(uart::UARTParityOptions parity);
  void wired_gap_(MBusWiredUartState next_state, uint32_t now);
#endif
  size_t packet_len_frame_a_(uint8_t l_field) const;
  size_t packet_len_frame_b_(uint8_t l_field) const;
  void manufacturer_to_string_(uint16_t manufacturer, char *out) const;
};

}  // namespace esphome::mbus
