#include "vif_table.h"

#include <cmath>
#include <cstdio>

namespace esphome::mbus {

static const char *dif_data_type_to_string(uint8_t dif) {
  switch (dif & 0x0F) {
    case 0x0:
      return "none";
    case 0x1:
      return "int8";
    case 0x2:
      return "int16";
    case 0x3:
      return "int24";
    case 0x4:
      return "int32";
    case 0x5:
      return "real32";
    case 0x6:
      return "int48";
    case 0x7:
      return "int64";
    case 0x8:
      return "selection";
    case 0x9:
      return "bcd2";
    case 0xA:
      return "bcd4";
    case 0xB:
      return "bcd6";
    case 0xC:
      return "bcd8";
    case 0xD:
      return "variable_length";
    case 0xE:
      return "bcd12";
    default:
      return "special";
  }
}

const char *record_data_type_to_string(uint8_t dif, uint16_t vif_key) {
  if (vif_key == 0x6C) {
    return "date_type_g";
  }
  if (vif_key == 0x6D || vif_key == 0x7D30 || vif_key == 0x7D70) {
    return "datetime_type_f";
  }
  return dif_data_type_to_string(dif);
}

bool vif_is_datetime(uint16_t vif_key) {
  return vif_key == 0x6C || vif_key == 0x6D || vif_key == 0x7D30 || vif_key == 0x7D70;
}

struct VifScaleUnit {
  uint16_t key;
  double scale;
  const char *unit;
};

static constexpr VifScaleUnit VIF_SCALE_UNITS[] = {
    {0x00, 1.0e-3, "Wh"},
    {0x01, 1.0e-2, "Wh"},
    {0x02, 1.0e-1, "Wh"},
    {0x03, 1.0e0, "Wh"},
    {0x04, 1.0e1, "Wh"},
    {0x05, 1.0e2, "Wh"},
    {0x06, 1.0e3, "Wh"},
    {0x07, 1.0e4, "Wh"},
    {0x08, 1.0e0, "J"},
    {0x09, 1.0e1, "J"},
    {0x0A, 1.0e2, "J"},
    {0x0B, 1.0e3, "J"},
    {0x0C, 1.0e4, "J"},
    {0x0D, 1.0e5, "J"},
    {0x0E, 1.0e6, "J"},
    {0x0F, 1.0e7, "J"},
    {0x10, 1.0e-6, "m3"},
    {0x11, 1.0e-5, "m3"},
    {0x12, 1.0e-4, "m3"},
    {0x13, 1.0e-3, "m3"},
    {0x14, 1.0e-2, "m3"},
    {0x15, 1.0e-1, "m3"},
    {0x16, 1.0e0, "m3"},
    {0x17, 1.0e1, "m3"},
    {0x18, 1.0e-3, "kg"},
    {0x19, 1.0e-2, "kg"},
    {0x1A, 1.0e-1, "kg"},
    {0x1B, 1.0e0, "kg"},
    {0x1C, 1.0e1, "kg"},
    {0x1D, 1.0e2, "kg"},
    {0x1E, 1.0e3, "kg"},
    {0x1F, 1.0e4, "kg"},
    {0x20, 1.0, "s"},
    {0x21, 60.0, "s"},
    {0x22, 3600.0, "s"},
    {0x23, 86400.0, "s"},
    {0x24, 1.0, "s"},
    {0x25, 60.0, "s"},
    {0x26, 3600.0, "s"},
    {0x27, 86400.0, "s"},
    {0x28, 1.0e-3, "W"},
    {0x29, 1.0e-2, "W"},
    {0x2A, 1.0e-1, "W"},
    {0x2B, 1.0e0, "W"},
    {0x2C, 1.0e1, "W"},
    {0x2D, 1.0e2, "W"},
    {0x2E, 1.0e3, "W"},
    {0x2F, 1.0e4, "W"},
    {0x30, 1.0e0, "J/h"},
    {0x31, 1.0e1, "J/h"},
    {0x32, 1.0e2, "J/h"},
    {0x33, 1.0e3, "J/h"},
    {0x34, 1.0e4, "J/h"},
    {0x35, 1.0e5, "J/h"},
    {0x36, 1.0e6, "J/h"},
    {0x37, 1.0e7, "J/h"},
    {0x38, 1.0e-6, "m3/h"},
    {0x39, 1.0e-5, "m3/h"},
    {0x3A, 1.0e-4, "m3/h"},
    {0x3B, 1.0e-3, "m3/h"},
    {0x3C, 1.0e-2, "m3/h"},
    {0x3D, 1.0e-1, "m3/h"},
    {0x3E, 1.0e0, "m3/h"},
    {0x3F, 1.0e1, "m3/h"},
    {0x40, 1.0e-7, "m3/min"},
    {0x41, 1.0e-6, "m3/min"},
    {0x42, 1.0e-5, "m3/min"},
    {0x43, 1.0e-4, "m3/min"},
    {0x44, 1.0e-3, "m3/min"},
    {0x45, 1.0e-2, "m3/min"},
    {0x46, 1.0e-1, "m3/min"},
    {0x47, 1.0e0, "m3/min"},
    {0x48, 1.0e-9, "m3/s"},
    {0x49, 1.0e-8, "m3/s"},
    {0x4A, 1.0e-7, "m3/s"},
    {0x4B, 1.0e-6, "m3/s"},
    {0x4C, 1.0e-5, "m3/s"},
    {0x4D, 1.0e-4, "m3/s"},
    {0x4E, 1.0e-3, "m3/s"},
    {0x4F, 1.0e-2, "m3/s"},
    {0x50, 1.0e-3, "kg/h"},
    {0x51, 1.0e-2, "kg/h"},
    {0x52, 1.0e-1, "kg/h"},
    {0x53, 1.0e0, "kg/h"},
    {0x54, 1.0e1, "kg/h"},
    {0x55, 1.0e2, "kg/h"},
    {0x56, 1.0e3, "kg/h"},
    {0x57, 1.0e4, "kg/h"},
    {0x58, 1.0e-3, "degC"},
    {0x59, 1.0e-2, "degC"},
    {0x5A, 1.0e-1, "degC"},
    {0x5B, 1.0e0, "degC"},
    {0x5C, 1.0e-3, "degC"},
    {0x5D, 1.0e-2, "degC"},
    {0x5E, 1.0e-1, "degC"},
    {0x5F, 1.0e0, "degC"},
    {0x60, 1.0e-3, "K"},
    {0x61, 1.0e-2, "K"},
    {0x62, 1.0e-1, "K"},
    {0x63, 1.0e0, "K"},
    {0x64, 1.0e-3, "degC"},
    {0x65, 1.0e-2, "degC"},
    {0x66, 1.0e-1, "degC"},
    {0x67, 1.0e0, "degC"},
    {0x68, 1.0e-3, "bar"},
    {0x69, 1.0e-2, "bar"},
    {0x6A, 1.0e-1, "bar"},
    {0x6B, 1.0e0, "bar"},
    {0x6C, 1.0, "datetime"},
    {0x6D, 1.0, "datetime"},
    {0x6E, 1.0, "hca"},
    {0x70, 1.0, "s"},
    {0x71, 60.0, "s"},
    {0x72, 3600.0, "s"},
    {0x73, 86400.0, "s"},
    {0x74, 1.0, "s"},
    {0x75, 60.0, "s"},
    {0x76, 3600.0, "s"},
    {0x77, 86400.0, "s"},
    {0x78, 1.0, ""},
    {0x79, 1.0, ""},
    {0x7A, 1.0, ""},
    // Standard VIF=0xFD extensions observed in stored wM-Bus frames.
    {0x7D08, 1.0, ""},
    {0x7D09, 1.0, ""},
    {0x7D0B, 1.0, ""},
    {0x7D0C, 1.0, ""},
    {0x7D0F, 1.0, ""},
    {0x7D10, 1.0, ""},
    {0x7D11, 1.0, ""},
    {0x7D17, 1.0, "flags"},
    {0x7D1B, 1.0, ""},
    {0x7D2C, 1.0, "s"},
    {0x7D31, 60.0, "s"},
    {0x7D47, 1.0e-2, "V"},
    {0x7D61, 1.0, ""},
    {0x7D66, 1.0, ""},
    {0x7D6C, 3600.0, "s"},
    {0x7D30, 1.0, "datetime"},
    {0x7D70, 1.0, "datetime"},
};

bool vif_scale_unit(uint16_t vif_key, double *scale, const char **unit) {
  for (const auto &item : VIF_SCALE_UNITS) {
    if (item.key == vif_key) {
      *scale = item.scale;
      *unit = item.unit;
      return !vif_is_datetime(vif_key);
    }
  }
  *scale = 1.0;
  *unit = "unknown";
  return false;
}

bool vif_is_extension_marker(uint8_t vif_raw) {
  const uint8_t code = vif_raw & 0x7F;
  return code == 0x7B || code == 0x7C || code == 0x7D || code == 0x6F || code == 0x7F;
}

const char *vife_combinable_to_string(uint8_t vife) {
  switch (vife & 0x7F) {
    case 0x70:
    case 0x71:
    case 0x72:
    case 0x73:
    case 0x74:
    case 0x75:
    case 0x76:
    case 0x77:
      return "multiply_correction";
    case 0x78:
    case 0x79:
    case 0x7A:
    case 0x7B:
      return "additive_correction";
    case 0x7D:
      return "multiply_1000";
    case 0x3C:
      return "backward_flow";
    case 0x20:
      return "per_second";
    case 0x21:
      return "per_minute";
    case 0x22:
      return "per_hour";
    case 0x23:
      return "per_day";
    case 0x24:
      return "per_week";
    case 0x25:
      return "per_month";
    case 0x26:
      return "per_year";
    default:
      return nullptr;
  }
}

bool vife_apply_correction(uint8_t vife, double *value) {
  switch (vife & 0x7F) {
    case 0x70:
    case 0x71:
    case 0x72:
    case 0x73:
    case 0x74:
    case 0x75:
    case 0x76:
    case 0x77:
      *value *= std::pow(10.0, static_cast<int>(vife & 0x07) - 6);
      return true;
    case 0x78:
    case 0x79:
    case 0x7A:
    case 0x7B:
      *value += std::pow(10.0, static_cast<int>(vife & 0x03) - 3);
      return true;
    case 0x7D:
      *value *= 1000.0;
      return true;
    default:
      return false;
  }
}

// EN 13757-3 type G date: 2 bytes.
//   lo: YYYd dddd  -> day (bits 0..4), year low 3 bits (bits 5..7)
//   hi: YYYY mmmm  -> month (bits 0..3), year high 4 bits (bits 4..7)
//   year = 2000 + year_low3 + year_high4
bool decode_date_type_g(const uint8_t *data, std::string *out) {
  const uint8_t lo = data[0];
  const uint8_t hi = data[1];
  const int day = lo & 0x1F;
  const int month = hi & 0x0F;
  const int year = 2000 + ((lo & 0xE0) >> 5) + ((hi & 0xF0) >> 1);
  if (day < 1 || day > 31 || month < 1 || month > 12) {
    *out = "invalid";
    return false;
  }
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", year, month, day);
  *out = buffer;
  return true;
}

// EN 13757-3 type F date-time: 4 bytes (date in high 2 bytes, time in low 2).
//   b0: ..mmmmmm -> minute (bits 0..5)
//   b1: ...hhhhh -> hour (bits 0..4)
//   b2/b3: same layout as type G date
// 6-byte variant additionally carries seconds in b0 (bits 0..5), with the
// date/time layout shifted up by one byte.
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
    if (sec > 59 || min > 59 || hour > 23 || day < 1 || day > 31 || month < 1 || month > 12) {
      *out = "invalid";
      return false;
    }
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d", year, month, day, hour, min, sec);
    *out = buffer;
    return true;
  }

  // Default: 4-byte type F.
  const int min = data[0] & 0x3F;
  const int hour = data[1] & 0x1F;
  const uint8_t date_lo = data[2];
  const uint8_t date_hi = data[3];
  const int day = date_lo & 0x1F;
  const int month = date_hi & 0x0F;
  const int year = 2000 + ((date_lo & 0xE0) >> 5) + ((date_hi & 0xF0) >> 1);
  if (min > 59 || hour > 23 || day < 1 || day > 31 || month < 1 || month > 12) {
    *out = "invalid";
    return false;
  }
  char buffer[20];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d", year, month, day, hour, min);
  *out = buffer;
  return true;
}

}  // namespace esphome::mbus
