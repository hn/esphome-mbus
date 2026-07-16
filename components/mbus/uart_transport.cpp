#include "mbus.h"

#ifdef USE_MBUS_WIRED_UART
#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <utility>
#endif

namespace esphome::mbus {

#ifdef USE_MBUS_WIRED_UART
static const char *const TAG = "mbus";

static constexpr uint8_t MBUS_SHORT_START = 0x10;
static constexpr uint8_t MBUS_LONG_START = 0x68;
static constexpr uint8_t MBUS_STOP = 0x16;
static constexpr uint8_t MBUS_ADDRESS_NETWORK_LAYER = 0xFD;
static constexpr uint8_t MBUS_CONTROL_SND_NKE = 0x40;
static constexpr uint8_t MBUS_CONTROL_REQ_UD2 = 0x5B;
static constexpr uint8_t MBUS_CONTROL_SELECT_SECONDARY = 0x73;
static constexpr uint8_t MBUS_CI_SELECT_SLAVE = 0x52;
static constexpr uint8_t MBUS_ACK = 0xE5;
static_assert(static_cast<uint8_t>(MBUS_CONTROL_SND_NKE + MBUS_ADDRESS_NETWORK_LAYER) == 0x3D,
              "Unexpected SND_NKE checksum");
static_assert(static_cast<uint8_t>(MBUS_CONTROL_REQ_UD2 + MBUS_ADDRESS_NETWORK_LAYER) == 0x58,
              "Unexpected REQ_UD2 checksum");

// One shared UART bus (uart::UARTComponent) may carry multiple mbus: instances (e.g. several
// meters on the same bus). The lock is keyed by the bus itself, not global across all instances,
// so two mbus: instances on two independent UART buses never block each other.
static std::vector<std::pair<uart::UARTComponent *, MBusComponent *>> wired_uart_owners;

static MBusComponent *get_wired_uart_owner_(uart::UARTComponent *bus) {
  for (auto &entry : wired_uart_owners) {
    if (entry.first == bus) {
      return entry.second;
    }
  }
  return nullptr;
}

static void set_wired_uart_owner_(uart::UARTComponent *bus, MBusComponent *owner) {
  for (auto &entry : wired_uart_owners) {
    if (entry.first == bus) {
      entry.second = owner;
      return;
    }
  }
  wired_uart_owners.push_back({bus, owner});
}

static uint8_t wired_sum_(const uint8_t *data, size_t length) {
  uint8_t checksum = 0;
  for (size_t i = 0; i < length; i++) {
    checksum += data[i];
  }
  return checksum;
}
#endif

// Wired M-Bus UART transport: bus reset, secondary selection, request/response
// timing and RX assembly. wired_link.cpp validates the received wired frame;
// application.cpp handles CI dispatch, application headers, decryption and records.

#ifdef USE_MBUS_WIRED_UART
void MBusComponent::setup_wired_uart_() {
  this->check_uart_settings(2400, 1, uart::UART_CONFIG_PARITY_EVEN, 8);
  this->wired_state_ = MBusWiredUartState::IDLE;
  this->wired_retry_count_ = 0;
  this->wired_timer_ = 0;
  const uint32_t baud_rate = this->parent_->get_baud_rate();
  const uint32_t bit_time_us = baud_rate == 0 ? 1000 : 1000000UL / baud_rate;
  this->wired_timeout_short_ = ((330 + 11) * bit_time_us) / 1000 + 150;
  this->wired_timeout_long_ = ((330 + 11 + 11 * 512) * bit_time_us) / 1000 + 150;
  this->wired_next_update_ = 0;
  this->wired_rx_buffer_.clear();
  this->wired_expected_length_ = 0;
  this->wired_update_due_ = false;
  ESP_LOGD(TAG, "Wired UART: state machine initialized");
}

bool MBusComponent::wired_timeout_elapsed_(uint32_t now, uint32_t timeout) const {
  return now - this->wired_timer_ >= timeout;
}

void MBusComponent::reset_wired_transaction_() {
  this->wired_rx_buffer_.clear();
  this->wired_expected_length_ = 0;
}

void MBusComponent::retry_wired_uart_(const char *reason) {
  this->reset_wired_transaction_();
  if (this->wired_retry_count_ > 0) {
    this->wired_retry_count_--;
  }
  if (this->wired_retry_count_ > 0) {
    ESP_LOGD(TAG, "Wired UART: retrying after %s, retries left=%u", reason, this->wired_retry_count_);
    this->wired_state_ = MBusWiredUartState::SEND_RESET_1;
    return;
  }

  ESP_LOGW(TAG, "Wired UART: failed after %s", reason);
  this->wired_state_ = MBusWiredUartState::FAILED;
}

std::array<uint8_t, 5> MBusComponent::build_wired_snd_nke_frame_() const {
  return {MBUS_SHORT_START, MBUS_CONTROL_SND_NKE, MBUS_ADDRESS_NETWORK_LAYER,
          static_cast<uint8_t>(MBUS_CONTROL_SND_NKE + MBUS_ADDRESS_NETWORK_LAYER), MBUS_STOP};
}

std::array<uint8_t, 5> MBusComponent::build_wired_req_ud2_frame_() const {
  return {MBUS_SHORT_START, MBUS_CONTROL_REQ_UD2, MBUS_ADDRESS_NETWORK_LAYER,
          static_cast<uint8_t>(MBUS_CONTROL_REQ_UD2 + MBUS_ADDRESS_NETWORK_LAYER), MBUS_STOP};
}

std::array<uint8_t, 17> MBusComponent::build_wired_select_secondary_frame_() const {
  std::array<uint8_t, 17> frame{
      MBUS_LONG_START,     0x0B, 0x0B, MBUS_LONG_START, MBUS_CONTROL_SELECT_SECONDARY, MBUS_ADDRESS_NETWORK_LAYER,
      MBUS_CI_SELECT_SLAVE};

  // EN13757 secondary address payload: ID as little-endian BCD, then manufacturer/version/medium.
  frame[7] = static_cast<uint8_t>(this->secondary_address_ >> 32);
  frame[8] = static_cast<uint8_t>(this->secondary_address_ >> 40);
  frame[9] = static_cast<uint8_t>(this->secondary_address_ >> 48);
  frame[10] = static_cast<uint8_t>(this->secondary_address_ >> 56);
  frame[11] = static_cast<uint8_t>(this->secondary_address_ >> 24);
  frame[12] = static_cast<uint8_t>(this->secondary_address_ >> 16);
  frame[13] = static_cast<uint8_t>(this->secondary_address_ >> 8);
  frame[14] = static_cast<uint8_t>(this->secondary_address_);
  frame[15] = wired_sum_(frame.data() + 4, 11);
  frame[16] = MBUS_STOP;
  return frame;
}

void MBusComponent::loop_wired_uart_() {
  const uint32_t now = App.get_loop_component_start_time();

  switch (this->wired_state_) {
    case MBusWiredUartState::IDLE:
      if (this->wired_update_due_ || now >= this->wired_next_update_) {
        this->wired_update_due_ = false;
        this->wired_state_ = MBusWiredUartState::START;
      }
      return;
    case MBusWiredUartState::START: {
      MBusComponent *owner = get_wired_uart_owner_(this->parent_);
      if (owner != nullptr && owner != this) {
        return;
      }
      set_wired_uart_owner_(this->parent_, this);
      this->reset_wired_transaction_();
      this->wired_retry_count_ = WIRED_MAX_RETRIES;
      this->wired_state_ = MBusWiredUartState::SEND_RESET_1;
      return;
    }
    case MBusWiredUartState::SEND_RESET_1:
      this->write_array(this->build_wired_snd_nke_frame_().data(), 5);
      this->wired_timer_ = now;
      this->wired_state_ = MBusWiredUartState::WAIT_RESET_1;
      return;
    case MBusWiredUartState::WAIT_RESET_1:
      if (this->wired_timeout_elapsed_(now, this->wired_timeout_short_)) {
        this->wired_state_ = MBusWiredUartState::SEND_RESET_2;
      }
      return;
    case MBusWiredUartState::SEND_RESET_2:
      this->write_array(this->build_wired_snd_nke_frame_().data(), 5);
      this->wired_timer_ = now;
      this->wired_state_ = MBusWiredUartState::WAIT_RESET_2;
      return;
    case MBusWiredUartState::WAIT_RESET_2:
      if (this->wired_timeout_elapsed_(now, this->wired_timeout_short_)) {
        this->wired_state_ = MBusWiredUartState::PURGE_RX;
      }
      return;
    case MBusWiredUartState::PURGE_RX:
      while (this->available() > 0) {
        uint8_t byte;
        this->read_byte(&byte);
      }
      this->wired_state_ = MBusWiredUartState::SEND_SELECT_SECONDARY;
      return;
    case MBusWiredUartState::SEND_SELECT_SECONDARY:
      this->write_array(this->build_wired_select_secondary_frame_().data(), 17);
      this->wired_timer_ = now;
      this->wired_state_ = MBusWiredUartState::WAIT_SELECT_ACK;
      return;
    case MBusWiredUartState::WAIT_SELECT_ACK:
      if (this->available() == 0) {
        if (this->wired_timeout_elapsed_(now, this->wired_timeout_short_)) {
          this->retry_wired_uart_("select ACK timeout");
        }
        return;
      }
      {
        uint8_t byte;
        if (!this->read_byte(&byte)) {
          this->retry_wired_uart_("select ACK read error");
          return;
        }
        if (byte != MBUS_ACK) {
          this->retry_wired_uart_("select ACK collision");
          return;
        }
      }
      this->wired_timer_ = now;
      this->wired_state_ = MBusWiredUartState::WAIT_SELECT_SETTLE;
      return;
    case MBusWiredUartState::WAIT_SELECT_SETTLE:
      if (this->available() > 0) {
        this->retry_wired_uart_("select settle collision");
        return;
      }
      if (this->wired_timeout_elapsed_(now, this->wired_timeout_short_)) {
        this->wired_state_ = MBusWiredUartState::SEND_REQ_UD2;
      }
      return;
    case MBusWiredUartState::SEND_REQ_UD2:
      this->write_array(this->build_wired_req_ud2_frame_().data(), 5);
      this->wired_timer_ = now;
      this->wired_state_ = MBusWiredUartState::WAIT_RESPONSE_HEADER;
      return;
    case MBusWiredUartState::WAIT_RESPONSE_HEADER:
      if (this->wired_timeout_elapsed_(now, this->wired_timeout_long_)) {
        this->retry_wired_uart_("response header timeout");
        return;
      }
      if (this->available() < 3) {
        return;
      }
      this->wired_rx_buffer_.assign(3, 0);
      if (!this->read_array(this->wired_rx_buffer_.data(), 3)) {
        this->retry_wired_uart_("response header read error");
        return;
      }
      if (this->wired_rx_buffer_[0] != MBUS_LONG_START || this->wired_rx_buffer_[1] != this->wired_rx_buffer_[2] ||
          this->wired_rx_buffer_[1] < 3) {
        this->retry_wired_uart_("invalid response header");
        return;
      }
      this->wired_expected_length_ = static_cast<size_t>(this->wired_rx_buffer_[1]) + 6;
      this->wired_state_ = MBusWiredUartState::WAIT_RESPONSE_BODY;
      return;
    case MBusWiredUartState::WAIT_RESPONSE_BODY:
      if (this->wired_timeout_elapsed_(now, this->wired_timeout_long_)) {
        this->retry_wired_uart_("response body timeout");
        return;
      }
      if (this->wired_expected_length_ <= this->wired_rx_buffer_.size()) {
        this->retry_wired_uart_("invalid response length");
        return;
      }
      if (this->available() < this->wired_expected_length_ - this->wired_rx_buffer_.size()) {
        return;
      }
      {
        const size_t offset = this->wired_rx_buffer_.size();
        this->wired_rx_buffer_.resize(this->wired_expected_length_);
        if (!this->read_array(this->wired_rx_buffer_.data() + offset, this->wired_expected_length_ - offset)) {
          this->retry_wired_uart_("response body read error");
          return;
        }
      }
      if (!this->handle_wired_link_frame_(this->wired_rx_buffer_)) {
        this->retry_wired_uart_("invalid response frame");
        return;
      }
      this->wired_state_ = MBusWiredUartState::DONE;
      return;
    case MBusWiredUartState::DONE:
      this->reset_wired_transaction_();
      this->wired_next_update_ = now + this->wired_update_interval_;
      if (get_wired_uart_owner_(this->parent_) == this) {
        set_wired_uart_owner_(this->parent_, nullptr);
      }
      this->wired_state_ = MBusWiredUartState::IDLE;
      return;
    case MBusWiredUartState::RETRY:
      this->retry_wired_uart_("retry state");
      return;
    case MBusWiredUartState::FAILED:
      this->reset_wired_transaction_();
      this->wired_next_update_ = now + this->wired_update_interval_;
      if (get_wired_uart_owner_(this->parent_) == this) {
        set_wired_uart_owner_(this->parent_, nullptr);
      }
      this->wired_state_ = MBusWiredUartState::IDLE;
      return;
  }

  ESP_LOGW(TAG, "Wired UART: unknown state, resetting to IDLE");
  if (get_wired_uart_owner_(this->parent_) == this) {
    set_wired_uart_owner_(this->parent_, nullptr);
  }
  this->wired_state_ = MBusWiredUartState::IDLE;
}
#endif

}  // namespace esphome::mbus
