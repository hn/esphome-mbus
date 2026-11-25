#include "mbus.h"
#include "records_codec.h"

#include "esphome/core/log.h"
#include "esphome/core/progmem.h"

#include <cmath>
#include <cstdio>

namespace esphome::mbus {

void MBusComponent::manufacturer_to_str_(uint16_t manufacturer, char *out) const {
  out[0] = static_cast<char>(((manufacturer >> 10) & 0x1F) + 0x40);
  out[1] = static_cast<char>(((manufacturer >> 5) & 0x1F) + 0x40);
  out[2] = static_cast<char>((manufacturer & 0x1F) + 0x40);
  out[3] = '\0';
}

const LogString *mbus_result_to_str(MBusResult result) {
  switch (result) {
    case MBusResult::MBUS_RESULT_OK:
      return LOG_STR("ok");
    case MBusResult::MBUS_RESULT_ACK:
      return LOG_STR("ack");
    case MBusResult::MBUS_RESULT_BUSY:
      return LOG_STR("busy");
    case MBusResult::MBUS_RESULT_NO_RESPONSE:
      return LOG_STR("no response");
    case MBusResult::MBUS_RESULT_INVALID_RESPONSE:
      return LOG_STR("invalid response");
    case MBusResult::MBUS_RESULT_INVALID_REQUEST:
      return LOG_STR("invalid request");
    case MBusResult::MBUS_RESULT_APPLICATION_ERROR:
      return LOG_STR("application error");
    case MBusResult::MBUS_RESULT_NOT_DECODED:
      return LOG_STR("not decoded");
    case MBusResult::MBUS_RESULT_SECURITY_ERROR:
      return LOG_STR("security error");
  }
  return LOG_STR("unknown");
}

std::span<const uint8_t> manufacturer_data(std::span<const uint8_t> data) {
  return data.subspan(records_codec::manufacturer_data_offset(data.data(), data.size()));
}

uint8_t application_error_code(std::span<const uint8_t> data) { return data.empty() ? 0x00 : data[0]; }

void MBusComponent::report_received_(MBusResult result, uint8_t ci, std::span<const uint8_t> x,
                                     std::span<const uint8_t> data, const MBusReceiveMeta &meta) {
  this->receive_callbacks_.call(result, ci, x, data, meta.has_rssi ? meta.rssi : NAN,
                                meta.has_freq_offset ? meta.freq_offset : NAN,
                                meta.has_lqi ? static_cast<float>(meta.lqi) : NAN);
}

// Function field of the DIF (EN 13757-3:2018, Table 7)
PROGMEM_STRING_TABLE(RecordFunctions, "instantaneous", "maximum", "minimum", "value_during_error", "unknown");

const LogString *mbus_record_function_to_str(uint8_t function) {
  return RecordFunctions::get_log_str(function, RecordFunctions::LAST_INDEX);
}

void MBusRecordMatcher::dump_config_matcher(const char *tag) const {
  ESP_LOGCONFIG(tag, "  DIF: 0x%02X", this->dif_);
  ESP_LOGCONFIG(tag, "  VIF: 0x%02X", this->vif_);
  if (this->has_vif_ext_) {
    ESP_LOGCONFIG(tag, "  VIF extension: 0x%04X", this->vif_ext_);
  }
  if (this->has_function_) {
    ESP_LOGCONFIG(tag, "  Function: %s", LOG_STR_ARG(mbus_record_function_to_str(this->function_)));
  }
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
        vife += ',';
      }
      vife += item;
    }
    ESP_LOGCONFIG(tag, "  VIFE: [%s]", vife.c_str());
  }
}

}  // namespace esphome::mbus
