#include "mbus_text_sensor.h"

#include "esphome/core/log.h"

namespace esphome::mbus {

static const char *const TAG = "mbus.text_sensor";

void MBusTextSensor::dump_config() {
  LOG_TEXT_SENSOR("", "M-Bus Text Sensor", this);
  this->dump_config_matcher(TAG);
}

bool MBusTextSensor::matches(const MBusRecord &record) const {
  return this->matches_record(record) && !record.decoded.empty();
}

void MBusTextSensor::publish_record(const MBusRecord &record) { this->publish_state(record.decoded); }

}  // namespace esphome::mbus
