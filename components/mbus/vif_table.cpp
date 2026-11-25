#include "vif_table.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/core/progmem.h"

#include <cmath>
#include <cstdio>

namespace esphome::mbus {

// Names of the data field, the low nibble of the DIF (EN 13757-3:2018, Table 4)
PROGMEM_STRING_TABLE(DifDataTypes, "none", "int8", "int16", "int24", "int32", "real32", "int48", "int64", "selection",
                     "bcd2", "bcd4", "bcd6", "bcd8", "variable_length", "bcd12", "special");

MBusDateTimeType record_date_time_type(uint8_t dif, uint16_t vif_key, bool vife_timestamp) {
  const uint8_t data_field = dif & 0x0F;
  const bool date_vif = vif_key == 0x6C;
  const bool date_time_vif = vif_key == 0x6D || vif_key == 0x7D30 || vif_key == 0x7D70;
  if (data_field == 0x2 && (date_vif || vife_timestamp)) {
    return MBusDateTimeType::MBUS_DATE_TIME_TYPE_G;
  }
  if (!date_time_vif && !vife_timestamp) {
    return MBusDateTimeType::MBUS_DATE_TIME_TYPE_NONE;
  }
  switch (data_field) {
    case 0x3:
      return MBusDateTimeType::MBUS_DATE_TIME_TYPE_J;
    case 0x4:
      return MBusDateTimeType::MBUS_DATE_TIME_TYPE_F;
    case 0x6:
      return MBusDateTimeType::MBUS_DATE_TIME_TYPE_I;
    default:
      return MBusDateTimeType::MBUS_DATE_TIME_TYPE_NONE;
  }
}

const LogString *record_data_type_to_str(uint8_t dif, uint16_t vif_key, bool vife_timestamp) {
  switch (record_date_time_type(dif, vif_key, vife_timestamp)) {
    case MBusDateTimeType::MBUS_DATE_TIME_TYPE_G:
      return LOG_STR("date_type_g");
    case MBusDateTimeType::MBUS_DATE_TIME_TYPE_F:
      return LOG_STR("datetime_type_f");
    case MBusDateTimeType::MBUS_DATE_TIME_TYPE_J:
      return LOG_STR("time_type_j");
    case MBusDateTimeType::MBUS_DATE_TIME_TYPE_I:
      return LOG_STR("datetime_type_i");
    default:
      return DifDataTypes::get_log_str(dif & 0x0F, DifDataTypes::LAST_INDEX);
  }
}

bool vif_is_datetime(uint16_t vif_key) {
  return vif_key == 0x6C || vif_key == 0x6D || vif_key == 0x7D30 || vif_key == 0x7D70;
}

PROGMEM_STRING_TABLE(VifUnits, "Wh", "J", "m3", "kg", "s", "W", "J/h", "m3/h", "m3/min", "m3/s", "kg/h", "degC", "K",
                     "bar", "datetime", "hca", "", "flags", "V", "unknown");
static constexpr uint8_t VIF_UNIT_WH = 0;
static constexpr uint8_t VIF_UNIT_J = 1;
static constexpr uint8_t VIF_UNIT_M3 = 2;
static constexpr uint8_t VIF_UNIT_KG = 3;
static constexpr uint8_t VIF_UNIT_S = 4;
static constexpr uint8_t VIF_UNIT_W = 5;
static constexpr uint8_t VIF_UNIT_J_H = 6;
static constexpr uint8_t VIF_UNIT_M3_H = 7;
static constexpr uint8_t VIF_UNIT_M3_MIN = 8;
static constexpr uint8_t VIF_UNIT_M3_S = 9;
static constexpr uint8_t VIF_UNIT_KG_H = 10;
static constexpr uint8_t VIF_UNIT_DEGC = 11;
static constexpr uint8_t VIF_UNIT_K = 12;
static constexpr uint8_t VIF_UNIT_BAR = 13;
static constexpr uint8_t VIF_UNIT_DATETIME = 14;
static constexpr uint8_t VIF_UNIT_HCA = 15;
static constexpr uint8_t VIF_UNIT_NONE = 16;
static constexpr uint8_t VIF_UNIT_FLAGS = 17;
static constexpr uint8_t VIF_UNIT_V = 18;
static constexpr uint8_t VIF_UNIT_UNKNOWN = 19;

struct VifScaleUnit {
  double scale;
  uint16_t key;
  uint8_t unit;
};

static constexpr VifScaleUnit VIF_SCALE_UNITS[] PROGMEM = {
    {1.0e-3, 0x00, VIF_UNIT_WH},
    {1.0e-2, 0x01, VIF_UNIT_WH},
    {1.0e-1, 0x02, VIF_UNIT_WH},
    {1.0e0, 0x03, VIF_UNIT_WH},
    {1.0e1, 0x04, VIF_UNIT_WH},
    {1.0e2, 0x05, VIF_UNIT_WH},
    {1.0e3, 0x06, VIF_UNIT_WH},
    {1.0e4, 0x07, VIF_UNIT_WH},
    {1.0e0, 0x08, VIF_UNIT_J},
    {1.0e1, 0x09, VIF_UNIT_J},
    {1.0e2, 0x0A, VIF_UNIT_J},
    {1.0e3, 0x0B, VIF_UNIT_J},
    {1.0e4, 0x0C, VIF_UNIT_J},
    {1.0e5, 0x0D, VIF_UNIT_J},
    {1.0e6, 0x0E, VIF_UNIT_J},
    {1.0e7, 0x0F, VIF_UNIT_J},
    {1.0e-6, 0x10, VIF_UNIT_M3},
    {1.0e-5, 0x11, VIF_UNIT_M3},
    {1.0e-4, 0x12, VIF_UNIT_M3},
    {1.0e-3, 0x13, VIF_UNIT_M3},
    {1.0e-2, 0x14, VIF_UNIT_M3},
    {1.0e-1, 0x15, VIF_UNIT_M3},
    {1.0e0, 0x16, VIF_UNIT_M3},
    {1.0e1, 0x17, VIF_UNIT_M3},
    {1.0e-3, 0x18, VIF_UNIT_KG},
    {1.0e-2, 0x19, VIF_UNIT_KG},
    {1.0e-1, 0x1A, VIF_UNIT_KG},
    {1.0e0, 0x1B, VIF_UNIT_KG},
    {1.0e1, 0x1C, VIF_UNIT_KG},
    {1.0e2, 0x1D, VIF_UNIT_KG},
    {1.0e3, 0x1E, VIF_UNIT_KG},
    {1.0e4, 0x1F, VIF_UNIT_KG},
    {1.0, 0x20, VIF_UNIT_S},
    {60.0, 0x21, VIF_UNIT_S},
    {3600.0, 0x22, VIF_UNIT_S},
    {86400.0, 0x23, VIF_UNIT_S},
    {1.0, 0x24, VIF_UNIT_S},
    {60.0, 0x25, VIF_UNIT_S},
    {3600.0, 0x26, VIF_UNIT_S},
    {86400.0, 0x27, VIF_UNIT_S},
    {1.0e-3, 0x28, VIF_UNIT_W},
    {1.0e-2, 0x29, VIF_UNIT_W},
    {1.0e-1, 0x2A, VIF_UNIT_W},
    {1.0e0, 0x2B, VIF_UNIT_W},
    {1.0e1, 0x2C, VIF_UNIT_W},
    {1.0e2, 0x2D, VIF_UNIT_W},
    {1.0e3, 0x2E, VIF_UNIT_W},
    {1.0e4, 0x2F, VIF_UNIT_W},
    {1.0e0, 0x30, VIF_UNIT_J_H},
    {1.0e1, 0x31, VIF_UNIT_J_H},
    {1.0e2, 0x32, VIF_UNIT_J_H},
    {1.0e3, 0x33, VIF_UNIT_J_H},
    {1.0e4, 0x34, VIF_UNIT_J_H},
    {1.0e5, 0x35, VIF_UNIT_J_H},
    {1.0e6, 0x36, VIF_UNIT_J_H},
    {1.0e7, 0x37, VIF_UNIT_J_H},
    {1.0e-6, 0x38, VIF_UNIT_M3_H},
    {1.0e-5, 0x39, VIF_UNIT_M3_H},
    {1.0e-4, 0x3A, VIF_UNIT_M3_H},
    {1.0e-3, 0x3B, VIF_UNIT_M3_H},
    {1.0e-2, 0x3C, VIF_UNIT_M3_H},
    {1.0e-1, 0x3D, VIF_UNIT_M3_H},
    {1.0e0, 0x3E, VIF_UNIT_M3_H},
    {1.0e1, 0x3F, VIF_UNIT_M3_H},
    {1.0e-7, 0x40, VIF_UNIT_M3_MIN},
    {1.0e-6, 0x41, VIF_UNIT_M3_MIN},
    {1.0e-5, 0x42, VIF_UNIT_M3_MIN},
    {1.0e-4, 0x43, VIF_UNIT_M3_MIN},
    {1.0e-3, 0x44, VIF_UNIT_M3_MIN},
    {1.0e-2, 0x45, VIF_UNIT_M3_MIN},
    {1.0e-1, 0x46, VIF_UNIT_M3_MIN},
    {1.0e0, 0x47, VIF_UNIT_M3_MIN},
    {1.0e-9, 0x48, VIF_UNIT_M3_S},
    {1.0e-8, 0x49, VIF_UNIT_M3_S},
    {1.0e-7, 0x4A, VIF_UNIT_M3_S},
    {1.0e-6, 0x4B, VIF_UNIT_M3_S},
    {1.0e-5, 0x4C, VIF_UNIT_M3_S},
    {1.0e-4, 0x4D, VIF_UNIT_M3_S},
    {1.0e-3, 0x4E, VIF_UNIT_M3_S},
    {1.0e-2, 0x4F, VIF_UNIT_M3_S},
    {1.0e-3, 0x50, VIF_UNIT_KG_H},
    {1.0e-2, 0x51, VIF_UNIT_KG_H},
    {1.0e-1, 0x52, VIF_UNIT_KG_H},
    {1.0e0, 0x53, VIF_UNIT_KG_H},
    {1.0e1, 0x54, VIF_UNIT_KG_H},
    {1.0e2, 0x55, VIF_UNIT_KG_H},
    {1.0e3, 0x56, VIF_UNIT_KG_H},
    {1.0e4, 0x57, VIF_UNIT_KG_H},
    {1.0e-3, 0x58, VIF_UNIT_DEGC},
    {1.0e-2, 0x59, VIF_UNIT_DEGC},
    {1.0e-1, 0x5A, VIF_UNIT_DEGC},
    {1.0e0, 0x5B, VIF_UNIT_DEGC},
    {1.0e-3, 0x5C, VIF_UNIT_DEGC},
    {1.0e-2, 0x5D, VIF_UNIT_DEGC},
    {1.0e-1, 0x5E, VIF_UNIT_DEGC},
    {1.0e0, 0x5F, VIF_UNIT_DEGC},
    {1.0e-3, 0x60, VIF_UNIT_K},
    {1.0e-2, 0x61, VIF_UNIT_K},
    {1.0e-1, 0x62, VIF_UNIT_K},
    {1.0e0, 0x63, VIF_UNIT_K},
    {1.0e-3, 0x64, VIF_UNIT_DEGC},
    {1.0e-2, 0x65, VIF_UNIT_DEGC},
    {1.0e-1, 0x66, VIF_UNIT_DEGC},
    {1.0e0, 0x67, VIF_UNIT_DEGC},
    {1.0e-3, 0x68, VIF_UNIT_BAR},
    {1.0e-2, 0x69, VIF_UNIT_BAR},
    {1.0e-1, 0x6A, VIF_UNIT_BAR},
    {1.0e0, 0x6B, VIF_UNIT_BAR},
    {1.0, 0x6C, VIF_UNIT_DATETIME},
    {1.0, 0x6D, VIF_UNIT_DATETIME},
    {1.0, 0x6E, VIF_UNIT_HCA},
    {1.0, 0x70, VIF_UNIT_S},
    {60.0, 0x71, VIF_UNIT_S},
    {3600.0, 0x72, VIF_UNIT_S},
    {86400.0, 0x73, VIF_UNIT_S},
    {1.0, 0x74, VIF_UNIT_S},
    {60.0, 0x75, VIF_UNIT_S},
    {3600.0, 0x76, VIF_UNIT_S},
    {86400.0, 0x77, VIF_UNIT_S},
    {1.0, 0x78, VIF_UNIT_NONE},
    {1.0, 0x79, VIF_UNIT_NONE},
    {1.0, 0x7A, VIF_UNIT_NONE},
    // Standard VIF=0xFD extensions observed in stored wM-Bus frames.
    {1.0, 0x7D08, VIF_UNIT_NONE},
    {1.0, 0x7D09, VIF_UNIT_NONE},
    {1.0, 0x7D0B, VIF_UNIT_NONE},
    {1.0, 0x7D0C, VIF_UNIT_NONE},
    {1.0, 0x7D0F, VIF_UNIT_NONE},
    {1.0, 0x7D10, VIF_UNIT_NONE},
    {1.0, 0x7D11, VIF_UNIT_NONE},
    {1.0, 0x7D17, VIF_UNIT_FLAGS},
    {1.0, 0x7D1B, VIF_UNIT_NONE},
    {1.0, 0x7D2C, VIF_UNIT_S},
    {60.0, 0x7D31, VIF_UNIT_S},
    {1.0e-2, 0x7D47, VIF_UNIT_V},
    {1.0, 0x7D61, VIF_UNIT_NONE},
    {1.0, 0x7D66, VIF_UNIT_NONE},
    {3600.0, 0x7D6C, VIF_UNIT_S},
    {1.0, 0x7D30, VIF_UNIT_DATETIME},
    {1.0, 0x7D70, VIF_UNIT_DATETIME},
};

bool vif_scale_unit(uint16_t vif_key, double *scale, const LogString **unit) {
  for (const auto &entry : VIF_SCALE_UNITS) {
    if (progmem_read_uint16(&entry.key) != vif_key) {
      continue;
    }
    VifScaleUnit item;
    progmem_memcpy(&item, &entry, sizeof(item));
    *scale = item.scale;
    *unit = VifUnits::get_log_str(item.unit, VIF_UNIT_UNKNOWN);
    return !vif_is_datetime(vif_key);
  }
  *scale = 1.0;
  *unit = VifUnits::get_log_str(VIF_UNIT_UNKNOWN, VIF_UNIT_UNKNOWN);
  return false;
}

bool vif_is_extension_marker(uint8_t vif_raw) {
  const uint8_t code = vif_raw & 0x7F;
  return code == 0x7B || code == 0x7D || code == 0x6F || code == 0x7F;
}

const LogString *vife_combinable_to_str(uint8_t vife) {
  const uint8_t code = vife & 0x7F;
  if (code >= 0x70 && code <= 0x77) {
    return LOG_STR("multiply_correction");
  }
  if (code >= 0x78 && code <= 0x7B) {
    return LOG_STR("additive_correction");
  }
  // E100 uf1b and E110 1f1b
  if ((code & 0x72) == 0x42 || (code & 0x7A) == 0x6A) {
    return LOG_STR("datetime_of");
  }
  switch (code) {
    case 0x7D:
      return LOG_STR("multiply_1000");
    case 0x39:
      return LOG_STR("start_datetime_of");
    case 0x3B:
      return LOG_STR("forward_flow");
    case 0x3C:
      return LOG_STR("backward_flow");
    case 0x20:
      return LOG_STR("per_second");
    case 0x21:
      return LOG_STR("per_minute");
    case 0x22:
      return LOG_STR("per_hour");
    case 0x23:
      return LOG_STR("per_day");
    case 0x24:
      return LOG_STR("per_week");
    case 0x25:
      return LOG_STR("per_month");
    case 0x26:
      return LOG_STR("per_year");
    default:
      return nullptr;
  }
}

const LogString *vife_extension_to_str(uint8_t vife) {
  switch (vife & 0x7F) {
    case 0x01:
      return LOG_STR("phase_l1");
    case 0x02:
      return LOG_STR("phase_l2");
    case 0x03:
      return LOG_STR("phase_l3");
    case 0x04:
      return LOG_STR("neutral");
    case 0x05:
      return LOG_STR("phase_l1_l2");
    case 0x06:
      return LOG_STR("phase_l2_l3");
    case 0x07:
      return LOG_STR("phase_l3_l1");
    case 0x08:
      return LOG_STR("quadrant_q1");
    case 0x09:
      return LOG_STR("quadrant_q2");
    case 0x0A:
      return LOG_STR("quadrant_q3");
    case 0x0B:
      return LOG_STR("quadrant_q4");
    case 0x0C:
      return LOG_STR("delta_import_export");
    case 0x10:
      return LOG_STR("absolute_count");
    case 0x11:
      return LOG_STR("data_type_c");
    case 0x12:
      return LOG_STR("data_type_d");
    default:
      return nullptr;
  }
}

VifeEffects vife_interpret(const uint8_t *vife, size_t count, std::string *flags) {
  VifeEffects effects;
  auto add_flag = [flags](const LogString *name) {
    if (name == nullptr) {
      return;
    }
    if (!flags->empty()) {
      *flags += ',';
    }
    // The name is a flash string on ESP8266
    char buffer[32];
    ESPHOME_strncpy_P(buffer, LOG_STR_ARG(name), sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    *flags += buffer;
  };
  for (size_t i = 0; i < count; i++) {
    const uint8_t code = vife[i] & 0x7F;
    if (code == 0x7F) {
      add_flag(LOG_STR("manufacturer_specific"));
      break;
    }
    if (code == 0x7C) {
      if (++i >= count) {
        break;
      }
      add_flag(vife_extension_to_str(vife[i]));
      if ((vife[i] & 0x7F) == 0x11) {
        effects.unsigned_value = true;
      }
      continue;
    }
    add_flag(vife_combinable_to_str(code));
    if (code >= 0x70 && code <= 0x77) {
      effects.factor *= std::pow(10.0, static_cast<int>(code & 0x07) - 6);
    } else if (code >= 0x78 && code <= 0x7B) {
      // The record holds the additive constant itself, in 10^(nn-3) units of the VIF (Table 15, footnote g)
      effects.factor *= std::pow(10.0, static_cast<int>(code & 0x03) - 3);
    } else if (code == 0x7D) {
      effects.factor *= 1000.0;
    } else if (code == 0x39 || (code & 0x72) == 0x42 || (code & 0x7A) == 0x6A) {
      effects.timestamp = true;
    }
  }
  return effects;
}

// Two-digit years 81 to 99 belong to the 20th century (EN 13757-3:2018, Table A.5, footnote a)
static int year_from_two_digits(int year) { return year <= 80 ? 2000 + year : 1900 + year; }

// EN 13757-3:2018, Table A.6, type G date: 2 bytes.
//   lo: YYYd dddd  -> day (bits 0..4), year low 3 bits (bits 5..7)
//   hi: YYYY mmmm  -> month (bits 0..3), year high 4 bits (bits 4..7)
bool decode_date_type_g(const uint8_t *data, std::string *out) {
  const uint8_t lo = data[0];
  const uint8_t hi = data[1];
  const int day = lo & 0x1F;
  const int month = hi & 0x0F;
  const int year = ((lo & 0xE0) >> 5) + ((hi & 0xF0) >> 1);
  if (day < 1 || day > 31 || month < 1 || month > 12 || year > 99) {
    *out = "invalid";
    return false;
  }
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", year_from_two_digits(year), month, day);
  *out = buffer;
  return true;
}

// EN 13757-3:2018, Table A.5, type F date and time: 4 bytes.
//   b0: I.mmmmmm -> minute (bits 0..5), time invalid (bit 7)
//   b1: SHHhhhhh -> hour (bits 0..4), hundred year (bits 5..6), summer time (bit 7)
//   b2/b3: same layout as the type G date
// Table A.8, type I: 6 bytes with the second in b0, the time invalid bit in b1 (bit 7) and the date and time
// moved up by one byte; it has no hundred year.
bool decode_datetime_type_f(const uint8_t *data, size_t length, std::string *out) {
  if (length == 6) {
    const int sec = data[0] & 0x3F;
    const int min = data[1] & 0x3F;
    const int hour = data[2] & 0x1F;
    const uint8_t date_lo = data[3];
    const uint8_t date_hi = data[4];
    const int day = date_lo & 0x1F;
    const int month = date_hi & 0x0F;
    const int year = 2000 + ((date_lo & 0xE0) >> 5) + ((date_hi & 0xF0) >> 1);
    if ((data[1] & 0x80) != 0 || sec > 59 || min > 59 || hour > 23 || day < 1 || day > 31 || month < 1 || month > 12) {
      *out = "invalid";
      return false;
    }
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d", year, month, day, hour, min, sec);
    *out = buffer;
    return true;
  }

  const int min = data[0] & 0x3F;
  const int hour = data[1] & 0x1F;
  const int hundred_year = (data[1] >> 5) & 0x03;
  const uint8_t date_lo = data[2];
  const uint8_t date_hi = data[3];
  const int day = date_lo & 0x1F;
  const int month = date_hi & 0x0F;
  const int two_digit_year = ((date_lo & 0xE0) >> 5) + ((date_hi & 0xF0) >> 1);
  if ((data[0] & 0x80) != 0 || min > 59 || hour > 23 || day < 1 || day > 31 || month < 1 || month > 12 ||
      two_digit_year > 99) {
    *out = "invalid";
    return false;
  }
  // Meters with a two-digit year leave the hundred year at 0
  const int year =
      hundred_year == 0 ? year_from_two_digits(two_digit_year) : 1900 + 100 * hundred_year + two_digit_year;
  char buffer[20];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d", year, month, day, hour, min);
  *out = buffer;
  return true;
}

// EN 13757-3:2018, Table A.9, type J time: second, minute and hour in one byte each
bool decode_time_type_j(const uint8_t *data, std::string *out) {
  const int sec = data[0] & 0x3F;
  const int min = data[1] & 0x3F;
  const int hour = data[2] & 0x1F;
  if (sec > 59 || min > 59 || hour > 23) {
    *out = "invalid";
    return false;
  }
  char buffer[12];
  std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", hour, min, sec);
  *out = buffer;
  return true;
}

}  // namespace esphome::mbus
