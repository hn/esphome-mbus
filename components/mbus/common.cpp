#include "mbus.h"
#include "records_codec.h"

#include "esphome/core/log.h"

#include <cstdio>

namespace esphome::mbus {

void MBusComponent::manufacturer_to_string_(uint16_t manufacturer, char *out) const {
  out[0] = static_cast<char>(((manufacturer >> 10) & 0x1F) + 0x40);
  out[1] = static_cast<char>(((manufacturer >> 5) & 0x1F) + 0x40);
  out[2] = static_cast<char>((manufacturer & 0x1F) + 0x40);
  out[3] = '\0';
}

const LogString *mbus_result_to_str(MBusResult result) {
  switch (result) {
    case MBusResult::MBUS_RESULT_OK:
      return LOG_STR("ok");
    case MBusResult::MBUS_RESULT_BUSY:
      return LOG_STR("busy");
    case MBusResult::MBUS_RESULT_NO_RESPONSE:
      return LOG_STR("no response");
    case MBusResult::MBUS_RESULT_INVALID_RESPONSE:
      return LOG_STR("invalid response");
    case MBusResult::MBUS_RESULT_INVALID_REQUEST:
      return LOG_STR("invalid request");
  }
  return LOG_STR("unknown");
}

std::span<const uint8_t> manufacturer_data(std::span<const uint8_t> data) {
  return data.subspan(records_codec::manufacturer_data_offset(data.data(), data.size()));
}

const char *mbus_record_function_to_string(uint8_t function) {
  switch (function) {
    case 0:
      return "instantaneous";
    case 1:
      return "maximum";
    case 2:
      return "minimum";
    case 3:
      return "value_during_error";
    default:
      return "unknown";
  }
}

void MBusRecordMatcher::dump_config_matcher(const char *tag) const {
  ESP_LOGCONFIG(tag, "  DIF: 0x%02X", this->dif_);
  ESP_LOGCONFIG(tag, "  VIF: 0x%02X", this->vif_);
  if (this->has_vif_ext_) {
    ESP_LOGCONFIG(tag, "  VIF extension: 0x%04X", this->vif_ext_);
  }
  ESP_LOGCONFIG(tag, "  Function: %s", mbus_record_function_to_string(this->function_));
  if (this->has_storage_) {
    ESP_LOGCONFIG(tag, "  Storage: %u", static_cast<unsigned>(this->storage_));
  }
  if (this->has_tariff_) {
    ESP_LOGCONFIG(tag, "  Tariff: %u", static_cast<unsigned>(this->tariff_));
  }
  if (this->has_subunit_) {
    ESP_LOGCONFIG(tag, "  Subunit: %u", static_cast<unsigned>(this->subunit_));
  }
  if (this->has_vife_) {
    std::string vife;
    for (size_t i = 0; i < this->vife_.size(); i++) {
      char item[8];
      std::snprintf(item, sizeof(item), "0x%02X", this->vife_[i]);
      if (i > 0) {
        vife += ",";
      }
      vife += item;
    }
    ESP_LOGCONFIG(tag, "  VIFE: [%s]", vife.c_str());
  }
}

}  // namespace esphome::mbus
