#pragma once

#include "../mbus.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/core/component.h"

namespace esphome::mbus {

class MBusSensor : public sensor::Sensor, public Component, public MBusRecordListener, public MBusRecordMatcher {
 public:
  void dump_config() override;
  bool matches(const MBusRecord &record) const override;
  std::string match_key() const override { return std::string("sensor:") + this->match_key_base(); }
  void publish_record(const MBusRecord &record) override;
};

}  // namespace esphome::mbus
