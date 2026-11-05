#pragma once

#include "../mbus.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/core/component.h"

namespace esphome::mbus {

class MBusBinarySensor : public binary_sensor::BinarySensor,
                         public Component,
                         public MBusRecordListener,
                         public MBusRecordMatcher {
 public:
  void set_bit(uint8_t bit) { this->bit_ = bit; }

  void dump_config() override;
  bool matches(const MBusRecord &record) const override;
  void publish_record(const MBusRecord &record) override;

 protected:
  uint8_t bit_{0};
};

}  // namespace esphome::mbus
