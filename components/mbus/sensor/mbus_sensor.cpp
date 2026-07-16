#include "mbus_sensor.h"

#include "esphome/core/log.h"

namespace esphome::mbus {

static const char *const TAG = "mbus.sensor";

void MBusSensor::dump_config() {
  LOG_SENSOR("", "M-Bus Sensor", this);
  this->dump_config_matcher(TAG);
}

bool MBusSensor::matches(const MBusRecord &record) const {
  return this->matches_record(record) && (record.has_value || !record.decoded.empty());
}

static bool parse_2_digits(const char *str, int *out) {
  if (str[0] < '0' || str[0] > '9' || str[1] < '0' || str[1] > '9') {
    return false;
  }
  *out = (str[0] - '0') * 10 + (str[1] - '0');
  return true;
}

static bool parse_4_digits(const char *str, int *out) {
  if (str[0] < '0' || str[0] > '9' || str[1] < '0' || str[1] > '9' || str[2] < '0' || str[2] > '9' || str[3] < '0' ||
      str[3] > '9') {
    return false;
  }
  *out = (str[0] - '0') * 1000 + (str[1] - '0') * 100 + (str[2] - '0') * 10 + (str[3] - '0');
  return true;
}

static int64_t days_from_civil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int>(doe) - 719468;
}

static bool parse_timestamp(const std::string &decoded, double *timestamp) {
  if (decoded.size() != 10 && decoded.size() != 16 && decoded.size() != 19) {
    return false;
  }
  if (decoded[4] != '-' || decoded[7] != '-') {
    return false;
  }
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!parse_4_digits(decoded.c_str(), &year) || !parse_2_digits(decoded.c_str() + 5, &month) ||
      !parse_2_digits(decoded.c_str() + 8, &day)) {
    return false;
  }
  if (decoded.size() > 10) {
    if (decoded[10] != 'T' || decoded[13] != ':' || !parse_2_digits(decoded.c_str() + 11, &hour) ||
        !parse_2_digits(decoded.c_str() + 14, &minute)) {
      return false;
    }
    if (decoded.size() == 19 && (decoded[16] != ':' || !parse_2_digits(decoded.c_str() + 17, &second))) {
      return false;
    }
  }
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59) {
    return false;
  }
  *timestamp = static_cast<double>(days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second);
  return true;
}

void MBusSensor::publish_record(const MBusRecord &record) {
  if (record.has_value) {
    this->publish_state(record.value);
    return;
  }
  double timestamp = 0;
  if (!record.decoded.empty() && parse_timestamp(record.decoded, &timestamp)) {
    this->publish_state(timestamp);
    return;
  }
  ESP_LOGW(TAG, "Matched record is not numeric and not a timestamp: dif=0x%02X vif=0x%02X decoded=%s", record.dif,
           record.vif, record.decoded.c_str());
}

}  // namespace esphome::mbus
