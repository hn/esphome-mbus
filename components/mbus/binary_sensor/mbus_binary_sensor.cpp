#include "mbus_binary_sensor.h"

#include "esphome/core/log.h"

namespace esphome::mbus {

static const char *const TAG = "mbus.binary_sensor";

void MBusBinarySensor::dump_config() {
  LOG_BINARY_SENSOR("", "M-Bus Binary Sensor", this);
  this->dump_config_matcher(TAG);
  ESP_LOGCONFIG(TAG, "  Bit: %u", this->bit_);
}

bool MBusBinarySensor::matches(const MBusRecord &record) const {
  return record.has_raw_value && this->matches_record(record);
}

void MBusBinarySensor::publish_record(const MBusRecord &record) {
  this->publish_state((static_cast<uint64_t>(record.raw_value) & (uint64_t(1) << this->bit_)) != 0);
}

}  // namespace esphome::mbus
