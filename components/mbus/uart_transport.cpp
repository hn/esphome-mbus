#include "mbus.h"

#ifdef USE_MBUS_WIRED_UART
#include "wired_frame_codec.h"

#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <algorithm>
#endif

namespace esphome::mbus {

#ifdef USE_MBUS_WIRED_UART
ESPHOME_LOG_TAG(TAG, "mbus");

// SND_NKE to a primary address is repeated up to two times if the acknowledge is missing (EN 1434-3:2008,
// Annex D.2.1). A selected meter gets two without waiting for an acknowledge: only a meter that is still
// selected answers at address 253 (EN 13757-7:2018, 8.4).
static constexpr uint8_t WIRED_NKE_MAX_PRIMARY = 3;
static constexpr uint8_t WIRED_NKE_COUNT_SELECTED = 2;

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static MBusComponent *wired_instances = nullptr;
#endif

// Wired M-Bus UART transport: bus reset, secondary selection, request/response
// timing and RX assembly. wired_link.cpp validates the received wired frame;
// application.cpp handles CI dispatch, application headers, decryption and records.

#ifdef USE_MBUS_WIRED_UART
void MBusComponent::setup_wired_uart_() {
  this->wired_state_ = MBusWiredUartState::MBUS_WIRED_UART_STATE_IDLE;
  this->wired_retry_count_ = 0;
  this->wired_timer_ = 0;
  this->wired_timeout_ = 0;
  this->wired_receiver_.reset();
  this->wired_next_ = wired_instances;
  wired_instances = this;
  ESP_LOGD(TAG, "Wired UART: state machine initialized");
}

bool MBusComponent::wired_timeout_elapsed_(uint32_t now) const {
  return now - this->wired_timer_ >= this->wired_timeout_;
}

void MBusComponent::drain_wired_rx_() {
  uint8_t discard[32];
  size_t available;
  while ((available = this->available()) > 0) {
    if (!this->read_array(discard, std::min(available, sizeof(discard)))) {
      return;
    }
  }
}

void MBusComponent::send_wired_(const uint8_t *data, size_t size, size_t max_response_size, uint32_t now) {
  // Whatever is left of an earlier exchange must not be taken for the answer
  this->drain_wired_rx_();
  this->wired_receiver_.reset();
  this->wired_frame_reported_ = false;
  this->write_array(data, size);
  // The answer time counts from the end of the datagram (EN 13757-2:2018, 5.7.6), which leaves the UART
  // size * 11 bit times after this call. loop() sees the last byte up to one loop interval late.
  this->wired_timer_ = now;
  this->wired_timeout_ =
      wired_frame_codec::response_timeout_ms(size, max_response_size, this->parent_->get_baud_rate()) +
      App.get_loop_interval();
  this->wired_last_activity_ =
      now + wired_frame_codec::bits_to_ms(size * wired_frame_codec::BITS_PER_BYTE, this->parent_->get_baud_rate());
}

void MBusComponent::set_wired_parity_(uart::UARTParityOptions parity) {
  this->parent_->set_parity(parity);
#if defined(USE_ESP32) || defined(USE_ESP8266)
  this->parent_->load_settings(false);
#endif
}

// Starts the wake-up if one is due and returns true then; next_state is where the exchange goes on afterwards.
bool MBusComponent::wired_wakeup_(MBusWiredUartState next_state, uint32_t now) {
  if (!this->wired_wakeup_enabled_) {
    return false;
  }
  // The last activity may lie a few ms ahead: it is the end of a datagram on the line
  const int32_t idle = static_cast<int32_t>(now - this->wired_last_activity_);
  if (!this->wired_wakeup_forced_ && idle < static_cast<int32_t>(this->wired_wakeup_after_idle_)) {
    return false;
  }
  ESP_LOGD(TAG, "Wired UART: sending wake-up");
  this->wired_wakeup_forced_ = false;
  this->drain_wired_rx_();
  this->set_wired_parity_(uart::UART_CONFIG_PARITY_NONE);
  this->wired_wakeup_remaining_ = wired_frame_codec::wakeup_byte_count(this->parent_->get_baud_rate());
  this->wired_wakeup_sent_ = 0;
  this->wired_wakeup_start_ = now;
  this->wired_timer_ = now;
  this->wired_wakeup_next_state_ = next_state;
  this->wired_state_ = MBusWiredUartState::MBUS_WIRED_UART_STATE_WAKEUP_SEND;
  return true;
}

// The one place that puts the control field of a short frame together
void MBusComponent::send_wired_short_(uint8_t control, size_t max_response_size, uint32_t now) {
  if (control == wired_frame_codec::CONTROL_SND_NKE) {
    // SND_NKE clears the "last FCB" bits of the slave (EN 13757-2:2018, 5.7.7). The datagrams after it carry a
    // set FCB, so the slave cannot take them for repetitions.
    this->wired_fcb_req_ = true;
    this->wired_fcb_snd_ = true;
  } else if (control == wired_frame_codec::CONTROL_REQ_UD2) {
    control = wired_frame_codec::control_with_fcb(control, this->wired_fcb_req_);
  }
  const auto frame = wired_frame_codec::build_short_frame(control, this->wired_address_);
  this->send_wired_(frame.data(), frame.size(), max_response_size, now);
}

// A datagram that is repeated if its answer is missing or invalid, by default up to two times (EN 1434-3:2008,
// Annex D.2.1). The repetition is the same datagram, with the same FCB.
void MBusComponent::wired_begin_exchange_(MBusWiredUartState send_state, uint8_t attempts) {
  this->wired_retry_count_ = attempts;
  this->wired_retry_state_ = send_state;
}

wired_frame_codec::ReceiveResult MBusComponent::receive_wired_() {
  // The UART buffer may be smaller than a long frame, so take what is there on every pass
  size_t available;
  while ((available = this->available()) > 0) {
    const size_t count = std::min(available, this->wired_receiver_.wanted());
    if (count == 0 || !this->read_array(this->wired_receiver_.tail(), count)) {
      break;
    }
    this->wired_receiver_.advance(count);
    this->wired_last_activity_ = App.get_loop_component_start_time();
  }
  return this->wired_receiver_.result();
}

void MBusComponent::report_wired_frame_(MBusResult result, uint8_t ci, std::span<const uint8_t> data) {
  if (this->wired_frame_reported_) {
    return;
  }
  this->wired_frame_reported_ = true;
  this->report_received_(result, ci,
                         std::span<const uint8_t>(this->wired_receiver_.data(), this->wired_receiver_.frame().size),
                         data, MBusReceiveMeta{});
}

void MBusComponent::report_wired_link_frame_() {
  const wired_frame_codec::Frame &frame = this->wired_receiver_.frame();
  if (frame.type == wired_frame_codec::FrameType::FRAME_TYPE_ACK) {
    this->report_wired_frame_(MBusResult::MBUS_RESULT_ACK, 0, {});
  } else {
    this->report_wired_frame_(MBusResult::MBUS_RESULT_INVALID_RESPONSE, frame.ci, {});
  }
}

bool MBusComponent::wired_records_expected_() const {
  return this->wired_operation_ == MBusWiredOperation::MBUS_WIRED_OPERATION_READOUT ||
         (this->wired_operation_ == MBusWiredOperation::MBUS_WIRED_OPERATION_REQUEST_UD2 && this->wired_parse_records_);
}

void MBusComponent::retry_wired_uart_(const char *reason, MBusResult result, uint32_t now) {
  if (this->wired_receiver_.size() > 0) {
    const size_t size = std::min(this->wired_receiver_.size(), MBUS_LOG_HEX_MAX_BYTES);
    char hex[format_hex_pretty_size(MBUS_LOG_HEX_MAX_BYTES)];
    ESP_LOGD(TAG, "Wired UART: %s, bytes=%u %s", reason, static_cast<unsigned>(this->wired_receiver_.size()),
             format_hex_pretty_to(hex, this->wired_receiver_.data(), size));
  } else {
    ESP_LOGD(TAG, "Wired UART: %s", reason);
  }
  this->wired_failure_reason_ = reason;
  this->wired_failure_result_ = result;
  // A meter that does not answer may have fallen asleep. A missing acknowledge of SND_NKE says nothing: some
  // meters never acknowledge it at the test address.
  const bool nke = this->wired_state_ == MBusWiredUartState::MBUS_WIRED_UART_STATE_WAIT_DATAGRAM &&
                   this->wired_operation_ == MBusWiredOperation::MBUS_WIRED_OPERATION_SEND_NKE;
  if (result == MBusResult::MBUS_RESULT_NO_RESPONSE && !nke) {
    this->wired_wakeup_forced_ = true;
  }
  // A slave may still be sending: the next attempt starts when the line has been quiet for one answer time
  this->wired_timer_ = now;
  this->wired_timeout_ =
      wired_frame_codec::response_timeout_ms(0, 0, this->parent_->get_baud_rate()) + App.get_loop_interval();
  this->wired_state_ = MBusWiredUartState::MBUS_WIRED_UART_STATE_WAIT_QUIET;
}

// Ends the exchange and reports it. The callback may start the next datagram, so nothing touches the state
// after it; the answer stays in the receiver until then.
void MBusComponent::finish_wired_transaction_(MBusResult result, const MBusApplicationResult *answer) {
  const mbus_response_callback_t callback = this->wired_callback_;
  void *context = this->wired_callback_context_;
  this->wired_callback_ = nullptr;
  this->wired_state_ = MBusWiredUartState::MBUS_WIRED_UART_STATE_IDLE;
  if (this->wired_operation_ == MBusWiredOperation::MBUS_WIRED_OPERATION_READOUT) {
    if (result == MBusResult::MBUS_RESULT_OK) {
      this->status_clear_warning();
    } else {
      this->status_set_warning();
    }
    return;
  }
  if (callback == nullptr) {
    return;
  }
  MBusResponse response;
  // An acknowledge or an answer the application layer has seen is still in the receiver
  if (answer != nullptr || result == MBusResult::MBUS_RESULT_ACK) {
    response.x = std::span<const uint8_t>(this->wired_receiver_.data(), this->wired_receiver_.frame().size);
  }
  if (answer != nullptr) {
    response.ci = answer->ci;
    response.data = answer->data;
  }
  callback(context, result, response);
}

MBusResult MBusComponent::wired_available_() const {
  if (this->parent_ == nullptr) {
    ESP_LOGW(TAG, "Datagrams can only be sent on an instance with wired_uart_id");
    return MBusResult::MBUS_RESULT_INVALID_REQUEST;
  }
  if (this->wired_state_ != MBusWiredUartState::MBUS_WIRED_UART_STATE_IDLE || this->wired_uart_busy_()) {
    return MBusResult::MBUS_RESULT_BUSY;
  }
  return MBusResult::MBUS_RESULT_OK;
}

MBusResult MBusComponent::start_wired_operation_(MBusWiredOperation operation, uint8_t attempts,
                                                 mbus_response_callback_t callback, void *context) {
  const MBusResult available = this->wired_available_();
  if (available != MBusResult::MBUS_RESULT_OK) {
    return available;
  }
  this->wired_operation_ = operation;
  this->wired_max_attempts_ = attempts;
  this->wired_callback_ = callback;
  this->wired_callback_context_ = context;
  // A meter read by its secondary address is selected first: SND_NKE to 253 ends a selection, and another
  // instance may have selected a different meter since (EN 13757-7:2018, 8.4)
  MBusWiredUartState first_state = MBusWiredUartState::MBUS_WIRED_UART_STATE_SEND_DATAGRAM;
  if (this->has_secondary_address_) {
    first_state = MBusWiredUartState::MBUS_WIRED_UART_STATE_SEND_SELECT_SECONDARY;
    attempts = WIRED_MAX_ATTEMPTS;
  }
  this->wired_begin_exchange_(first_state, attempts);
  // Keeps the idle time if this follows an answer directly
  this->wired_gap_(first_state, millis());
  return MBusResult::MBUS_RESULT_OK;
}

// Several instances may share one UART (several meters on one bus); only one of them may talk at a time.
bool MBusComponent::wired_uart_busy_() const {
  for (const MBusComponent *other = wired_instances; other != nullptr; other = other->wired_next_) {
    if (other != this && other->parent_ == this->parent_ &&
        other->wired_state_ != MBusWiredUartState::MBUS_WIRED_UART_STATE_IDLE &&
        other->wired_state_ != MBusWiredUartState::MBUS_WIRED_UART_STATE_START) {
      return true;
    }
  }
  return false;
}

// The line has to be idle for 11 bit times between two datagrams (EN 13757-2:2018, 5.5.3)
void MBusComponent::wired_gap_(MBusWiredUartState next_state, uint32_t now) {
  this->wired_timer_ = now;
  this->wired_timeout_ =
      wired_frame_codec::bits_to_ms(wired_frame_codec::BITS_PER_BYTE, this->parent_->get_baud_rate());
  this->wired_gap_next_state_ = next_state;
  this->wired_state_ = MBusWiredUartState::MBUS_WIRED_UART_STATE_WAIT_GAP;
}

void MBusComponent::start_wired_readout_() {
  if (this->wired_state_ != MBusWiredUartState::MBUS_WIRED_UART_STATE_IDLE) {
    ESP_LOGW(TAG, "Wired UART: update skipped, the previous exchange is still running");
    return;
  }
  this->wired_operation_ = MBusWiredOperation::MBUS_WIRED_OPERATION_READOUT;
  this->wired_max_attempts_ = WIRED_MAX_ATTEMPTS;
  this->wired_callback_ = nullptr;
  this->wired_state_ = MBusWiredUartState::MBUS_WIRED_UART_STATE_START;
}

void MBusComponent::loop_wired_uart_() {
  const uint32_t now = App.get_loop_component_start_time();

  switch (this->wired_state_) {
    case MBusWiredUartState::MBUS_WIRED_UART_STATE_IDLE:
      return;
    case MBusWiredUartState::MBUS_WIRED_UART_STATE_START:
      if (this->wired_uart_busy_()) {
        return;
      }
      this->wired_nke_count_ = 0;
      this->wired_datagram_count_ = 0;
      this->wired_state_ = MBusWiredUartState::MBUS_WIRED_UART_STATE_SEND_NKE;
      return;
    case MBusWiredUartState::MBUS_WIRED_UART_STATE_WAKEUP_SEND: {
      static const uint8_t PATTERN[32] = {
          0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
          0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
      };
      const uint32_t baud_rate = this->parent_->get_baud_rate();
      // End of what has been written so far, if the transmitter never ran dry
      uint32_t line_end =
          this->wired_wakeup_start_ +
          wired_frame_codec::bits_to_ms(this->wired_wakeup_sent_ * wired_frame_codec::WAKEUP_BITS_PER_BYTE, baud_rate);
      if (static_cast<int32_t>(now - line_end) > 0) {
        // It did run dry: the bytes written now start late
        this->wired_wakeup_start_ += now - line_end;
      }
      // Keep the transmitter fed without blocking: take the room there is, pass by pass
      size_t room = this->parent_->paced_write_room(this->wired_timer_);
      this->wired_timer_ = now;
      while (room > 0 && this->wired_wakeup_remaining_ > 0) {
        const size_t count = std::min({room, static_cast<size_t>(this->wired_wakeup_remaining_), sizeof(PATTERN)});
        this->write_array(PATTERN, count);
        room -= count;
        this->wired_wakeup_remaining_ -= count;
        this->wired_wakeup_sent_ += count;
      }
      if (this->wired_wakeup_remaining_ == 0) {
        this->wired_state_ = MBusWiredUartState::MBUS_WIRED_UART_STATE_WAKEUP_DRAIN;
      }
      return;
    }
    case MBusWiredUartState::MBUS_WIRED_UART_STATE_WAKEUP_DRAIN: {
      // The byte format may only change once the last byte has left the transmitter; that moment is taken from
      // the line time, because flush() would block
      const uint32_t line_end =
          this->wired_wakeup_start_ +
          wired_frame_codec::bits_to_ms(this->wired_wakeup_sent_ * wired_frame_codec::WAKEUP_BITS_PER_BYTE,
                                        this->parent_->get_baud_rate());
      if (static_cast<int32_t>(now - line_end) < 0) {
        return;
      }
      this->set_wired_parity_(uart::UART_CONFIG_PARITY_EVEN);
      this->drain_wired_rx_();
      this->wired_timer_ = line_end;
      this->wired_timeout_ = wired_frame_codec::WAKEUP_PAUSE_MS;
      this->wired_state_ = MBusWiredUartState::MBUS_WIRED_UART_STATE_WAKEUP_PAUSE;
      return;
    }
    case MBusWiredUartState::MBUS_WIRED_UART_STATE_WAKEUP_PAUSE:
      if (this->wired_timeout_elapsed_(now)) {
        this->wired_last_activity_ = now;
        this->wired_state_ = this->wired_wakeup_next_state_;
      }
      return;
    case MBusWiredUartState::MBUS_WIRED_UART_STATE_SEND_NKE:
      if (this->wired_wakeup_(MBusWiredUartState::MBUS_WIRED_UART_STATE_SEND_NKE, now)) {
        return;
      }
      this->send_wired_short_(wired_frame_codec::CONTROL_SND_NKE, 1, now);
      this->wired_nke_count_++;
      this->wired_state_ = MBusWiredUartState::MBUS_WIRED_UART_STATE_WAIT_NKE;
      return;
    case MBusWiredUartState::MBUS_WIRED_UART_STATE_WAIT_NKE: {
      const bool received = this->receive_wired_() == wired_frame_codec::ReceiveResult::RECEIVE_RESULT_FRAME;
      if (received) {
        this->report_wired_link_frame_();
      }
      const bool acknowledged =
          received && this->wired_receiver_.frame().type == wired_frame_codec::FrameType::FRAME_TYPE_ACK;
      if (!acknowledged && !this->wired_timeout_elapsed_(now)) {
        return;
      }
      MBusWiredUartState next_state;
      if (this->has_secondary_address_) {
        next_state = this->wired_nke_count_ < WIRED_NKE_COUNT_SELECTED
                         ? MBusWiredUartState::MBUS_WIRED_UART_STATE_SEND_NKE
                         : MBusWiredUartState::MBUS_WIRED_UART_STATE_SEND_SELECT_SECONDARY;
      } else if (acknowledged) {
        next_state = MBusWiredUartState::MBUS_WIRED_UART_STATE_SEND_DATAGRAM;
      } else if (this->wired_nke_count_ < WIRED_NKE_MAX_PRIMARY) {
        next_state = MBusWiredUartState::MBUS_WIRED_UART_STATE_SEND_NKE;
      } else {
        // Reading without acknowledge is what EN 1434-3:2008, Annex D.2.1 e) asks for
        ESP_LOGD(TAG, "Wired UART: no acknowledge of SND_NKE, reading anyway");
        next_state = MBusWiredUartState::MBUS_WIRED_UART_STATE_SEND_DATAGRAM;
      }
      if (next_state != MBusWiredUartState::MBUS_WIRED_UART_STATE_SEND_NKE) {
        this->wired_begin_exchange_(next_state, WIRED_MAX_ATTEMPTS);
      }
      if (acknowledged) {
        this->wired_gap_(next_state, now);
      } else {
        this->wired_state_ = next_state;
      }
      return;
    }
    case MBusWiredUartState::MBUS_WIRED_UART_STATE_WAIT_GAP:
      if (this->wired_timeout_elapsed_(now)) {
        this->wired_state_ = this->wired_gap_next_state_;
      }
      return;
    case MBusWiredUartState::MBUS_WIRED_UART_STATE_SEND_SELECT_SECONDARY: {
      if (this->wired_wakeup_(MBusWiredUartState::MBUS_WIRED_UART_STATE_SEND_SELECT_SECONDARY, now)) {
        return;
      }
      const auto frame = wired_frame_codec::build_select_frame(this->secondary_address_);
      this->send_wired_(frame.data(), frame.size(), 1, now);
      this->wired_state_ = MBusWiredUartState::MBUS_WIRED_UART_STATE_WAIT_SELECT_ACK;
      return;
    }
    case MBusWiredUartState::MBUS_WIRED_UART_STATE_WAIT_SELECT_ACK:
      switch (this->receive_wired_()) {
        case wired_frame_codec::ReceiveResult::RECEIVE_RESULT_WAITING:
          if (this->wired_timeout_elapsed_(now)) {
            this->retry_wired_uart_("no acknowledge of the selection", MBusResult::MBUS_RESULT_NO_RESPONSE, now);
          }
          return;
        case wired_frame_codec::ReceiveResult::RECEIVE_RESULT_INVALID:
          this->retry_wired_uart_("invalid answer to the selection", MBusResult::MBUS_RESULT_INVALID_RESPONSE, now);
          return;
        case wired_frame_codec::ReceiveResult::RECEIVE_RESULT_FRAME:
          break;
      }
      this->report_wired_link_frame_();
      if (this->wired_receiver_.frame().type != wired_frame_codec::FrameType::FRAME_TYPE_ACK) {
        this->retry_wired_uart_("unexpected answer to the selection", MBusResult::MBUS_RESULT_INVALID_RESPONSE, now);
        return;
      }
      this->wired_timer_ = now;
      this->wired_timeout_ =
          wired_frame_codec::response_timeout_ms(0, 0, this->parent_->get_baud_rate()) + App.get_loop_interval();
      this->wired_state_ = MBusWiredUartState::MBUS_WIRED_UART_STATE_WAIT_SELECT_SETTLE;
      return;
    case MBusWiredUartState::MBUS_WIRED_UART_STATE_WAIT_SELECT_SETTLE:
      // More than one slave matched the selection if anything follows the acknowledge
      if (this->available() > 0) {
        this->wired_receiver_.reset();
        this->retry_wired_uart_("select settle collision", MBusResult::MBUS_RESULT_INVALID_RESPONSE, now);
        return;
      }
      if (this->wired_timeout_elapsed_(now)) {
        this->wired_begin_exchange_(MBusWiredUartState::MBUS_WIRED_UART_STATE_SEND_DATAGRAM, this->wired_max_attempts_);
        this->wired_state_ = MBusWiredUartState::MBUS_WIRED_UART_STATE_SEND_DATAGRAM;
      }
      return;
    case MBusWiredUartState::MBUS_WIRED_UART_STATE_SEND_DATAGRAM:
      if (this->wired_wakeup_(MBusWiredUartState::MBUS_WIRED_UART_STATE_SEND_DATAGRAM, now)) {
        return;
      }
      switch (this->wired_operation_) {
        case MBusWiredOperation::MBUS_WIRED_OPERATION_SEND_NKE:
          this->send_wired_short_(wired_frame_codec::CONTROL_SND_NKE, 1, now);
          break;
        case MBusWiredOperation::MBUS_WIRED_OPERATION_SEND_UD:
          this->send_wired_(this->wired_tx_buffer_.data(), this->wired_tx_size_, wired_frame_codec::LONG_FRAME_MAX_SIZE,
                            now);
          break;
        case MBusWiredOperation::MBUS_WIRED_OPERATION_READOUT:
        case MBusWiredOperation::MBUS_WIRED_OPERATION_REQUEST_UD2:
          this->send_wired_short_(wired_frame_codec::CONTROL_REQ_UD2, wired_frame_codec::LONG_FRAME_MAX_SIZE, now);
          break;
      }
      this->wired_state_ = MBusWiredUartState::MBUS_WIRED_UART_STATE_WAIT_DATAGRAM;
      return;
    case MBusWiredUartState::MBUS_WIRED_UART_STATE_WAIT_DATAGRAM: {
      switch (this->receive_wired_()) {
        case wired_frame_codec::ReceiveResult::RECEIVE_RESULT_WAITING:
          if (!this->wired_timeout_elapsed_(now)) {
            return;
          }
          if (this->wired_receiver_.size() > 0) {
            this->retry_wired_uart_("incomplete response", MBusResult::MBUS_RESULT_INVALID_RESPONSE, now);
          } else {
            this->retry_wired_uart_("no response", MBusResult::MBUS_RESULT_NO_RESPONSE, now);
          }
          return;
        case wired_frame_codec::ReceiveResult::RECEIVE_RESULT_INVALID:
          this->retry_wired_uart_("invalid response", MBusResult::MBUS_RESULT_INVALID_RESPONSE, now);
          return;
        case wired_frame_codec::ReceiveResult::RECEIVE_RESULT_FRAME:
          break;
      }
      const wired_frame_codec::Frame &frame = this->wired_receiver_.frame();
      const bool ack = frame.type == wired_frame_codec::FrameType::FRAME_TYPE_ACK;
      // RSP_UD is 08h, 18h, 28h or 38h
      const bool rsp_ud = wired_frame_codec::is_rsp_ud(frame);
      bool valid = rsp_ud;
      if (this->wired_operation_ == MBusWiredOperation::MBUS_WIRED_OPERATION_SEND_NKE) {
        valid = ack;
      } else if (this->wired_operation_ == MBusWiredOperation::MBUS_WIRED_OPERATION_SEND_UD) {
        // A meter may answer a command with the data right away instead of an acknowledge
        valid = ack || rsp_ud;
      }
      if (!valid) {
        this->report_wired_link_frame_();
        this->retry_wired_uart_("unexpected response", MBusResult::MBUS_RESULT_INVALID_RESPONSE, now);
        return;
      }
      // The next datagram of this direction is a new one
      if (this->wired_operation_ == MBusWiredOperation::MBUS_WIRED_OPERATION_SEND_UD) {
        this->wired_fcb_snd_ = !this->wired_fcb_snd_;
      } else if (this->wired_operation_ != MBusWiredOperation::MBUS_WIRED_OPERATION_SEND_NKE) {
        this->wired_fcb_req_ = !this->wired_fcb_req_;
      }
      if (ack) {
        this->report_wired_link_frame_();
        this->finish_wired_transaction_(MBusResult::MBUS_RESULT_ACK);
        return;
      }
      MBusApplicationResult answer;
      this->handle_wired_link_frame_(frame, &answer);
      // Not an answer of the meter of this instance, so nothing it received
      if (answer.other_meter) {
        this->finish_wired_transaction_(MBusResult::MBUS_RESULT_INVALID_RESPONSE);
        return;
      }
      this->report_wired_frame_(answer.result, answer.ci, answer.data);
      // The meter answered, so a repetition would change nothing: it refused, or its data cannot be read
      if (answer.result != MBusResult::MBUS_RESULT_OK) {
        this->finish_wired_transaction_(answer.result, &answer);
        return;
      }
      if (this->wired_operation_ == MBusWiredOperation::MBUS_WIRED_OPERATION_READOUT) {
        this->wired_datagram_count_++;
        // The master reads on until an answer comes without DIF 1Fh (EN 13757-3:2018, 6.5)
        if (this->more_records_follow_) {
          if (this->wired_datagram_count_ < this->wired_max_datagrams_) {
            this->wired_begin_exchange_(MBusWiredUartState::MBUS_WIRED_UART_STATE_SEND_DATAGRAM, WIRED_MAX_ATTEMPTS);
            this->wired_gap_(MBusWiredUartState::MBUS_WIRED_UART_STATE_SEND_DATAGRAM, now);
            return;
          }
          if (this->wired_max_datagrams_ > 1) {
            ESP_LOGW(TAG, "Wired UART: the meter has more than max_datagrams=%u datagrams", this->wired_max_datagrams_);
          }
        }
      }
      this->finish_wired_transaction_(MBusResult::MBUS_RESULT_OK, &answer);
      return;
    }
    case MBusWiredUartState::MBUS_WIRED_UART_STATE_WAIT_QUIET:
      if (this->available() > 0) {
        this->drain_wired_rx_();
        this->wired_timer_ = now;
        return;
      }
      if (!this->wired_timeout_elapsed_(now)) {
        return;
      }
      this->wired_retry_count_--;
      if (this->wired_retry_count_ > 0) {
        ESP_LOGD(TAG, "Wired UART: retrying, attempts left=%u", this->wired_retry_count_);
        this->wired_state_ = this->wired_retry_state_;
        return;
      }
      ESP_LOGW(TAG, "Wired UART: failed, %s", this->wired_failure_reason_);
      this->wired_receiver_.reset();
      this->finish_wired_transaction_(this->wired_failure_result_);
      return;
  }
}

MBusResult MBusComponent::send_nke(uint8_t max_retries, mbus_response_callback_t callback, void *context) {
  return this->start_wired_operation_(MBusWiredOperation::MBUS_WIRED_OPERATION_SEND_NKE,
                                      static_cast<uint8_t>(max_retries + 1), callback, context);
}

MBusResult MBusComponent::request_ud2(uint8_t max_retries, mbus_response_callback_t callback, void *context,
                                      bool parse_records) {
  const MBusResult result = this->start_wired_operation_(MBusWiredOperation::MBUS_WIRED_OPERATION_REQUEST_UD2,
                                                         static_cast<uint8_t>(max_retries + 1), callback, context);
  if (result == MBusResult::MBUS_RESULT_OK) {
    this->wired_parse_records_ = parse_records;
  }
  return result;
}

MBusResult MBusComponent::send_ud(uint8_t ci, const uint8_t *data, size_t length, uint8_t max_retries,
                                  mbus_response_callback_t callback, void *context) {
  // The buffer of a running SND_UD must stay as it is
  const MBusResult available = this->wired_available_();
  if (available != MBusResult::MBUS_RESULT_OK) {
    return available;
  }
  // SND_UD is 53h or 73h (EN 13757-2:2018, 5.7.3.5)
  const uint8_t control = wired_frame_codec::control_with_fcb(wired_frame_codec::CONTROL_SND_UD, this->wired_fcb_snd_);
  this->wired_tx_size_ = wired_frame_codec::build_long_frame(
      control, this->wired_address_, ci, data, length, this->wired_tx_buffer_.data(), this->wired_tx_buffer_.size());
  if (this->wired_tx_size_ == 0) {
    ESP_LOGW(TAG, "SND_UD with %u data bytes does not fit a datagram", static_cast<unsigned>(length));
    return MBusResult::MBUS_RESULT_INVALID_REQUEST;
  }
  return this->start_wired_operation_(MBusWiredOperation::MBUS_WIRED_OPERATION_SEND_UD,
                                      static_cast<uint8_t>(max_retries + 1), callback, context);
}
#else
MBusResult MBusComponent::send_nke(uint8_t max_retries, mbus_response_callback_t callback, void *context) {
  return MBusResult::MBUS_RESULT_INVALID_REQUEST;
}

MBusResult MBusComponent::request_ud2(uint8_t max_retries, mbus_response_callback_t callback, void *context,
                                      bool parse_records) {
  return MBusResult::MBUS_RESULT_INVALID_REQUEST;
}

MBusResult MBusComponent::send_ud(uint8_t ci, const uint8_t *data, size_t length, uint8_t max_retries,
                                  mbus_response_callback_t callback, void *context) {
  return MBusResult::MBUS_RESULT_INVALID_REQUEST;
}
#endif

}  // namespace esphome::mbus
