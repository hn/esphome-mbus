#pragma once

#include "mbus.h"

#include "esphome/core/automation.h"
#include "esphome/core/base_automation.h"

#include <optional>
#include <span>
#include <tuple>

namespace esphome::mbus {

// Base of the actions that send one datagram to a wired meter and report the outcome: on_success for the results
// ok and ack, on_error for every other one, both with the variables result, ci, x and data.
template<typename... Ts> class DatagramAction : public Action<Ts...>, public Parented<MBusComponent> {
 public:
  void set_wait_for_response(bool wait_for_response) { this->flags_.wait_for_response = wait_for_response; }
  void set_continue_on_error(bool continue_on_error) { this->flags_.continue_on_error = continue_on_error; }
  void set_max_retries(uint8_t max_retries) { this->max_retries_ = max_retries; }

  using handler_action_t = Action<Ts..., MBusResult, uint8_t, std::span<const uint8_t>, std::span<const uint8_t>>;

  // Call both after set_wait_for_response(): only a waiting run needs to continue after the handlers.
  void add_on_success(const std::initializer_list<handler_action_t *> &actions) {
    this->add_handlers_(this->success_, actions, true);
  }
  void add_on_error(const std::initializer_list<handler_action_t *> &actions) {
    this->add_handlers_(this->error_, actions, false);
  }

  void play_complex(const Ts &...x) override {
    this->num_running_++;
    // The component runs one exchange at a time, so one run of this action can wait for a result
    MBusResult result = MBusResult::MBUS_RESULT_BUSY;
    if (!this->pending_.has_value()) {
      this->pending_.emplace(x...);
      result = this->start_(x...);
      if (result != MBusResult::MBUS_RESULT_OK) {
        this->pending_.reset();
      }
    }
    // A datagram that could not be started is reported right away
    if (result != MBusResult::MBUS_RESULT_OK) {
      this->handle_result_(result, MBusResponse{}, x...);
    }
    if (!this->flags_.wait_for_response) {
      this->play_next_(x...);
    }
  }

 protected:
  using args_t = std::tuple<stored_trigger_arg_t<Ts>...>;
  using handler_list_t = ActionList<Ts..., MBusResult, uint8_t, std::span<const uint8_t>, std::span<const uint8_t>>;

  void add_handlers_(handler_list_t &list, const std::initializer_list<handler_action_t *> &actions, bool ok) {
    list.add_actions(actions);
    if (this->flags_.wait_for_response) {
      list.add_action(new LambdaAction<Ts..., MBusResult, uint8_t, std::span<const uint8_t>, std::span<const uint8_t>>(
          [this, ok](Ts... x, MBusResult /*result*/, uint8_t /*ci*/, std::span<const uint8_t> /*x*/,
                     std::span<const uint8_t> /*data*/) { this->after_handlers_(ok, x...); }));
    }
  }

  virtual MBusResult start_(const Ts &...x) = 0;

  void play(const Ts &...x) override { /* see play_complex */ }

  // The datagram of a waiting run is still sent, but its result is dropped
  void stop() override {
    this->pending_.reset();
    this->success_.stop();
    this->error_.stop();
  }

  static void notify(void *context, MBusResult result, const MBusResponse &response) {
    auto *self = static_cast<DatagramAction *>(context);
    if (!self->pending_.has_value()) {
      return;
    }
    // Free the slot first, so the handlers may run this action again
    args_t args = std::move(*self->pending_);
    self->pending_.reset();
    std::apply([self, result, &response](const auto &...x) { self->handle_result_(result, response, x...); }, args);
  }

  void *context_() { return static_cast<DatagramAction *>(this); }

  void handle_result_(MBusResult result, const MBusResponse &response, const Ts &...x) {
    const bool ok = result == MBusResult::MBUS_RESULT_OK || result == MBusResult::MBUS_RESULT_ACK;
    handler_list_t &handlers = ok ? this->success_ : this->error_;
    if (!handlers.empty()) {
      handlers.play(x..., result, response.ci, response.x, response.data);
      return;
    }
    this->after_handlers_(ok, x...);
  }

  // Continue or end the run once the handlers are done; a run that did not wait has already moved on.
  void after_handlers_(bool ok, const Ts &...x) {
    if (!this->flags_.wait_for_response) {
      return;
    }
    if (ok || this->flags_.continue_on_error) {
      this->play_next_(x...);
    } else if (this->num_running_ > 0) {
      this->num_running_--;
    }
  }

  std::optional<args_t> pending_{};
  handler_list_t success_;
  handler_list_t error_;
  uint8_t max_retries_{0};
  struct {
    uint8_t wait_for_response : 1;
    uint8_t continue_on_error : 1;
    uint8_t reserved : 6;
  } flags_{1, 1, 0};
};

template<typename... Ts> class SendNkeAction final : public DatagramAction<Ts...> {
 protected:
  MBusResult start_(const Ts &...x) override {
    return this->parent_->send_nke(this->max_retries_, &SendNkeAction::notify, this->context_());
  }
};

template<typename... Ts> class RequestUd2Action final : public DatagramAction<Ts...> {
 public:
  void set_parse_records(bool parse_records) { this->parse_records_ = parse_records; }

 protected:
  MBusResult start_(const Ts &...x) override {
    return this->parent_->request_ud2(this->max_retries_, &RequestUd2Action::notify, this->context_(),
                                      this->parse_records_);
  }

  bool parse_records_{true};
};

template<typename... Ts> class SendUdAction final : public DatagramAction<Ts...> {
  TEMPLATABLE_VALUE(uint8_t, ci)
  TEMPLATABLE_BYTES(data)

 protected:
  MBusResult start_(const Ts &...x) override {
    const uint8_t ci = this->ci_.value(x...);
    MBusResult result = MBusResult::MBUS_RESULT_INVALID_REQUEST;
    this->data_.visit(
        [&](const uint8_t *data, size_t length) {
          result =
              this->parent_->send_ud(ci, data, length, this->max_retries_, &SendUdAction::notify, this->context_());
        },
        x...);
    return result;
  }
};

}  // namespace esphome::mbus
