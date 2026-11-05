#include "mbus.h"
#include "records_codec.h"
#include "vif_table.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace esphome::mbus {

ESPHOME_LOG_TAG(TAG, "mbus");

static MBusRecordFunction dif_function_from_dif(uint8_t dif) {
  switch ((dif >> 4) & 0x03) {
    case 0:
      return MBusRecordFunction::MBUS_RECORD_FUNCTION_INSTANTANEOUS;
    case 1:
      return MBusRecordFunction::MBUS_RECORD_FUNCTION_MAXIMUM;
    case 2:
      return MBusRecordFunction::MBUS_RECORD_FUNCTION_MINIMUM;
    case 3:
      return MBusRecordFunction::MBUS_RECORD_FUNCTION_VALUE_DURING_ERROR;
    default:
      return MBusRecordFunction::MBUS_RECORD_FUNCTION_INSTANTANEOUS;
  }
}

static const char *record_function_to_string(MBusRecordFunction function) {
  return mbus_record_function_to_string(static_cast<uint8_t>(function));
}

// Appends printf-style text to a fixed buffer; output beyond the buffer is cut off.
static void append_to(char *buffer, size_t size, size_t &length, const char *format, ...) {
  if (length + 1 >= size) {
    return;
  }
  va_list args;
  va_start(args, format);
  const int written = vsnprintf(buffer + length, size - length, format, args);
  va_end(args);
  if (written > 0) {
    length = std::min(size - 1, length + static_cast<size_t>(written));
  }
}

static void append_hex_list(char *buffer, size_t size, size_t &length, const char *name, const uint8_t *data,
                            size_t count) {
  append_to(buffer, size, length, " %s=[", name);
  for (size_t i = 0; i < count; i++) {
    append_to(buffer, size, length, i == 0 ? "0x%02X" : ",0x%02X", data[i]);
  }
  append_to(buffer, size, length, "]");
}

static void log_record(const MBusRecord &record) {
  char message[512];
  size_t length = 0;
  append_to(message, sizeof(message), length,
            "Record: dif=0x%02X vif=0x%02X storage=%u tariff=%u subunit=%u function=%s pos=%u vif_raw=0x%02X",
            record.dif, record.vif, static_cast<unsigned>(record.storage), static_cast<unsigned>(record.tariff),
            static_cast<unsigned>(record.subunit), record_function_to_string(record.function),
            static_cast<unsigned>(record.pos), record.vif_raw);
  if (record.has_vif_ext) {
    append_to(message, sizeof(message), length, " vif_ext=0x%04X", static_cast<unsigned>(record.vif_ext));
  }
  append_hex_list(message, sizeof(message), length, "dife", record.dife.data(), record.dife.size());
  append_hex_list(message, sizeof(message), length, "vife", record.vife.data(), record.vife.size());
  char raw[format_hex_pretty_size(MBUS_LOG_HEX_MAX_BYTES)];
  format_hex_pretty_to(raw, record.raw_data, std::min(record.raw_length, MBUS_LOG_HEX_MAX_BYTES), '.');
  append_to(message, sizeof(message), length, " data_type=%s raw=%s", record.data_type, raw);
  if (!record.custom_vif.empty()) {
    append_to(message, sizeof(message), length, " custom_vif=%s", record.custom_vif.c_str());
  }
  if (!record.flags.empty()) {
    append_to(message, sizeof(message), length, " flags=%s", record.flags.c_str());
  }
  if (!record.decoded.empty()) {
    append_to(message, sizeof(message), length, "; decoded: value=%s", record.decoded.c_str());
  } else {
    if (record.has_raw_value) {
      append_to(message, sizeof(message), length, " raw_value=%lld", static_cast<long long>(record.raw_value));
    }
    if (record.has_decoded_value) {
      append_to(message, sizeof(message), length, "; decoded: scale=%.12g unit=%s value=%.12g", record.scale,
                record.unit != nullptr ? LOG_STR_ARG(record.unit) : LOG_STR_LITERAL("unknown"), record.decoded_value);
    }
  }
  ESP_LOGD(TAG, "%s", message);
}

static void log_special_record(uint8_t dif, size_t pos, const char *data_type, const uint8_t *data, size_t length) {
  char raw[format_hex_pretty_size(MBUS_LOG_HEX_MAX_BYTES)];
  format_hex_pretty_to(raw, data, std::min(length, MBUS_LOG_HEX_MAX_BYTES), '.');
  ESP_LOGD(TAG, "Record: dif=0x%02X pos=%u data_type=%s raw=%s; decoded: type=%s", dif, static_cast<unsigned>(pos),
           data_type, raw, data_type);
}

template<typename F> void MBusComponent::parse_records_(const std::vector<uint8_t> &payload, F &&callback) const {
  size_t pos = 0;
  while (pos < payload.size()) {
    while (pos < payload.size() && payload[pos] == 0x2F) {
      pos++;
    }
    if (pos >= payload.size()) {
      return;
    }

    const size_t record_pos = pos;
    const uint8_t dif = payload[pos++];
    if (dif == 0x0F) {
      if (this->dump_records_enabled_) {
        log_special_record(dif, record_pos, "manufacturer_specific", payload.data() + pos, payload.size() - pos);
      }
      return;
    }
    if (dif == 0x1F) {
      if (this->dump_records_enabled_) {
        log_special_record(dif, record_pos, "more_records_follow", payload.data() + pos, payload.size() - pos);
      }
      return;
    }

    uint32_t storage = (dif & 0x40) != 0 ? 1 : 0;
    uint32_t tariff = 0;
    uint32_t subunit = 0;
    uint8_t storage_shift = 1;
    uint8_t tariff_shift = 0;
    uint8_t subunit_shift = 0;
    std::vector<uint8_t> dife;
    uint8_t extension = dif & 0x80;
    while (extension != 0) {
      if (pos >= payload.size()) {
        ESP_LOGW(TAG, "Record parse stopped: pos=%u reason=truncated DIFE dif=0x%02X",
                 static_cast<unsigned>(record_pos), dif);
        return;
      }
      if (dife.size() >= 8) {
        ESP_LOGW(TAG, "Record parse stopped: pos=%u reason=too many DIFE entries dif=0x%02X",
                 static_cast<unsigned>(record_pos), dif);
        return;
      }
      const uint8_t item = payload[pos++];
      dife.push_back(item);
      storage |= uint32_t(item & 0x0F) << storage_shift;
      tariff |= uint32_t((item >> 4) & 0x03) << tariff_shift;
      subunit |= uint32_t((item >> 6) & 0x01) << subunit_shift;
      storage_shift += 4;
      tariff_shift += 2;
      subunit_shift += 1;
      extension = item & 0x80;
    }

    if (pos >= payload.size()) {
      ESP_LOGW(TAG, "Record parse stopped: pos=%u reason=missing VIF dif=0x%02X", static_cast<unsigned>(record_pos),
               dif);
      return;
    }
    const uint8_t vif_raw = payload[pos++];
    const uint8_t vif_base = vif_raw & 0x7F;
    std::string custom_vif;
    if (vif_base == 0x7C) {
      if (pos >= payload.size()) {
        ESP_LOGW(TAG, "Record parse stopped: pos=%u reason=missing custom VIF length dif=0x%02X vif=0x%02X",
                 static_cast<unsigned>(record_pos), dif, vif_raw);
        return;
      }
      const uint8_t custom_vif_length = payload[pos++];
      if (custom_vif_length > 128) {
        ESP_LOGW(TAG, "Record parse stopped: pos=%u reason=custom VIF too long dif=0x%02X vif=0x%02X length=%u",
                 static_cast<unsigned>(record_pos), dif, vif_raw, static_cast<unsigned>(custom_vif_length));
        return;
      }
      if (pos + custom_vif_length > payload.size()) {
        ESP_LOGW(
            TAG,
            "Record parse stopped: pos=%u reason=truncated custom VIF dif=0x%02X vif=0x%02X needed=%u available=%u",
            static_cast<unsigned>(record_pos), dif, vif_raw, static_cast<unsigned>(custom_vif_length),
            static_cast<unsigned>(payload.size() - pos));
        return;
      }
      custom_vif = records_codec::decode_variable_ascii(payload.data() + pos, custom_vif_length);
      pos += custom_vif_length;
    }
    // Collect all VIFE bytes following the VIF.
    std::vector<uint8_t> all_vife;
    extension = vif_raw & 0x80;
    while (extension != 0) {
      if (pos >= payload.size()) {
        ESP_LOGW(TAG, "Record parse stopped: pos=%u reason=truncated VIFE dif=0x%02X vif=0x%02X",
                 static_cast<unsigned>(record_pos), dif, vif_raw);
        return;
      }
      const uint8_t item = payload[pos++];
      all_vife.push_back(item);
      extension = item & 0x80;
      if (all_vife.size() > 8) {
        ESP_LOGW(TAG, "Record parse stopped: pos=%u reason=too many VIFE entries dif=0x%02X vif=0x%02X",
                 static_cast<unsigned>(record_pos), dif, vif_raw);
        return;
      }
    }

    // For extension markers (0xFB/0xFC/0xFD/0xEF/0xFF) the first VIFE is part of the
    // VIF code itself. Everything else is a combinable VIFE.
    const bool vif_extension = vif_is_extension_marker(vif_raw);
    uint16_t vif_key = vif_base;
    std::vector<uint8_t> vife = all_vife;
    if (vif_extension && !vife.empty()) {
      vif_key = static_cast<uint16_t>((vif_base << 8) | (vife.front() & 0x7F));
      vife.erase(vife.begin());
    }

    MBusRecord record;
    record.pos = record_pos;
    record.dif = dif;
    record.vif_raw = vif_raw;
    record.vif = vif_base;
    record.has_vif_ext = vif_extension;
    record.vif_ext = vif_key;
    record.dife = dife;
    record.vife = vife;
    record.custom_vif = custom_vif;
    record.storage = storage;
    record.tariff = tariff;
    record.subunit = subunit;
    record.function = dif_function_from_dif(dif);
    record.data_type = record_data_type_to_string(dif, vif_key);

    // Decode combinable VIFE flags (only when this is not an extension marker VIF).
    for (const uint8_t item : vife) {
      const char *name = vife_combinable_to_string(item);
      if (name != nullptr) {
        if (!record.flags.empty()) {
          record.flags += ",";
        }
        record.flags += name;
      }
    }

    if ((dif & 0x0F) == 0x0D) {
      if (pos >= payload.size()) {
        ESP_LOGW(TAG, "Record parse stopped: pos=%u reason=missing LVAR dif=0x%02X vif=0x%02X",
                 static_cast<unsigned>(record_pos), dif, vif_raw);
        return;
      }
      const uint8_t lvar = payload[pos++];
      size_t data_length = 0;
      if (!records_codec::variable_data_length(lvar, &data_length)) {
        ESP_LOGW(TAG, "Record parse stopped: pos=%u reason=unsupported LVAR dif=0x%02X vif=0x%02X lvar=0x%02X",
                 static_cast<unsigned>(record_pos), dif, vif_raw, lvar);
        return;
      }
      if (pos + data_length > payload.size()) {
        ESP_LOGW(TAG,
                 "Record parse stopped: pos=%u reason=truncated variable data dif=0x%02X vif=0x%02X lvar=0x%02X "
                 "needed=%u available=%u",
                 static_cast<unsigned>(record_pos), dif, vif_raw, lvar, static_cast<unsigned>(data_length),
                 static_cast<unsigned>(payload.size() - pos));
        return;
      }
      const uint8_t *data = payload.data() + pos;
      record.raw_data = data;
      record.raw_length = data_length;
      if (lvar <= 0xBF) {
        record.data_type = "lvar_ascii";
        record.decoded = records_codec::decode_variable_ascii(data, data_length);
      } else if (lvar >= 0xC0 && lvar <= 0xCF) {
        record.data_type = "lvar_bcd";
        if (data_length <= 8) {
          record.raw_value = records_codec::decode_bcd_le(data, data_length);
          record.has_raw_value = true;
          record.value = static_cast<double>(record.raw_value);
          record.has_value = true;
        }
      } else if (lvar >= 0xD0 && lvar <= 0xDF) {
        record.data_type = "lvar_negative_bcd";
        if (data_length <= 8) {
          record.raw_value = records_codec::decode_lvar_negative_bcd(data, data_length);
          record.has_raw_value = true;
          record.value = static_cast<double>(record.raw_value);
          record.has_value = true;
        }
      } else if (lvar >= 0xE0 && lvar <= 0xEF) {
        record.data_type = "lvar_binary";
        if (data_length <= 7) {
          record.raw_value = static_cast<int64_t>(records_codec::decode_unsigned_le(data, data_length));
          record.has_raw_value = true;
          record.value = static_cast<double>(record.raw_value);
          record.has_value = true;
        }
      } else if (lvar == 0xF4) {
        record.data_type = "lvar_real32";
        record.value = records_codec::decode_real32_le(data);
        record.has_value = true;
      } else {
        record.data_type = "lvar_real";
      }
      if (record.has_value && vif_scale_unit(vif_key, &record.scale, &record.unit)) {
        record.decoded_value = record.value * record.scale;
        record.has_decoded_value = true;
      } else {
        vif_scale_unit(vif_key, &record.scale, &record.unit);
      }
      callback(record);
      pos += data_length;
      continue;
    }

    const size_t data_length = records_codec::dif_data_length(dif);
    if (pos + data_length > payload.size()) {
      ESP_LOGW(TAG, "Record parse stopped: pos=%u reason=truncated data dif=0x%02X vif=0x%02X needed=%u available=%u",
               static_cast<unsigned>(record_pos), dif, vif_raw, static_cast<unsigned>(data_length),
               static_cast<unsigned>(payload.size() - pos));
      return;
    }

    const uint8_t *data = payload.data() + pos;
    record.raw_data = data;
    record.raw_length = data_length;

    // Date/time records are decoded from their bit layout, not as scaled numbers.
    if (vif_is_datetime(vif_key)) {
      if (vif_key == 0x6C) {
        decode_date_type_g(data, &record.decoded);
      } else {
        decode_datetime_type_f(data, data_length, &record.decoded);
      }
      callback(record);
      pos += data_length;
      continue;
    }

    if (records_codec::dif_is_integer(dif)) {
      record.raw_value = records_codec::decode_signed_le(data, data_length);
      record.has_raw_value = true;
      record.value = static_cast<double>(record.raw_value);
      record.has_value = true;
    } else if (records_codec::dif_is_bcd(dif)) {
      record.raw_value = records_codec::decode_bcd_le(data, data_length);
      record.has_raw_value = true;
      record.value = static_cast<double>(record.raw_value);
      record.has_value = true;
    } else if ((dif & 0x0F) == 0x05) {
      record.value = records_codec::decode_real32_le(data);
      record.has_value = true;
    }

    // VIF/VIFE interpretation is only diagnostic; sensor publishing uses the
    // unscaled value so ESPHome filters remain in control.
    if (record.has_value && vif_scale_unit(vif_key, &record.scale, &record.unit)) {
      record.decoded_value = record.value * record.scale;
      for (const uint8_t item : vife) {
        vife_apply_correction(item, &record.decoded_value);
      }
      record.has_decoded_value = true;
    } else {
      vif_scale_unit(vif_key, &record.scale, &record.unit);
    }

    callback(record);
    pos += data_length;
  }
}

void MBusComponent::dump_records_(const std::vector<uint8_t> &payload) const {
  this->parse_records_(payload, [](const MBusRecord &record) { log_record(record); });
}

void MBusComponent::process_records_(const std::vector<uint8_t> &payload) const {
  if (!this->dump_records_enabled_ && this->record_listeners_.empty()) {
    return;
  }
  this->parse_records_(payload, [this](const MBusRecord &record) {
    if (this->dump_records_enabled_) {
      log_record(record);
    }
    this->publish_record_(record);
  });
}

void MBusComponent::publish_record_(const MBusRecord &record) const {
  size_t matches = 0;
  for (auto *listener : this->record_listeners_) {
    if (listener->matches(record)) {
      listener->publish_record(record);
      matches++;
    }
  }
  if (matches == 0) {
    ESP_LOGV(TAG, "No configured sensor matched record: dif=0x%02X vif=0x%02X storage=%u tariff=%u subunit=%u",
             record.dif, record.vif, static_cast<unsigned>(record.storage), static_cast<unsigned>(record.tariff),
             static_cast<unsigned>(record.subunit));
    return;
  }
}

}  // namespace esphome::mbus
