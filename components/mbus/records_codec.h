#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>

namespace esphome::mbus::records_codec {

inline size_t dif_data_length(uint8_t dif) {
  switch (dif & 0x0F) {
    case 0x0:
    case 0x8:
      return 0;
    case 0x1:
    case 0x9:
      return 1;
    case 0x2:
    case 0xA:
      return 2;
    case 0x3:
    case 0xB:
      return 3;
    case 0x4:
    case 0x5:
    case 0xC:
      return 4;
    case 0x6:
    case 0xE:
      return 6;
    case 0x7:
      return 8;
    default:
      return 0;
  }
}

inline bool dif_is_integer(uint8_t dif) {
  const uint8_t data_field = dif & 0x0F;
  return (data_field >= 0x1 && data_field <= 0x4) || data_field == 0x6 || data_field == 0x7;
}

inline bool dif_is_bcd(uint8_t dif) {
  const uint8_t data_field = dif & 0x0F;
  return (data_field >= 0x9 && data_field <= 0xC) || data_field == 0xE;
}

inline uint64_t decode_unsigned_le(const uint8_t *data, size_t length) {
  uint64_t value = 0;
  for (size_t i = 0; i < length; i++) {
    value |= uint64_t(data[i]) << (8 * i);
  }
  return value;
}

inline int64_t decode_signed_le(const uint8_t *data, size_t length) {
  const uint64_t value = decode_unsigned_le(data, length);
  if (length == 0 || (data[length - 1] & 0x80) == 0) {
    return static_cast<int64_t>(value);
  }
  const uint64_t mask = length >= 8 ? UINT64_MAX : ((uint64_t(1) << (length * 8)) - 1);
  const uint64_t magnitude = ((~value) & mask) + 1;
  if (magnitude == (uint64_t(1) << 63)) {
    return std::numeric_limits<int64_t>::min();
  }
  return -static_cast<int64_t>(magnitude);
}

// EN 13757-3 BCD: each byte holds two decimal digits (high nibble, low nibble). A high
// nibble of 0xF in the most significant byte marks the whole value as negative.
inline int64_t decode_bcd_le(const uint8_t *data, size_t length) {
  int64_t value = 0;
  for (size_t i = length; i > 0; i--) {
    value *= 10;
    const uint8_t high = data[i - 1] >> 4;
    if (high < 0x0A) {
      value += high;
    }
    value = value * 10 + (data[i - 1] & 0x0F);
  }
  if (length > 0 && (data[length - 1] >> 4) == 0x0F) {
    value *= -1;
  }
  return value;
}

// DIF=0x0D LVAR class 0xD0-0xDF ("negative BCD"): the LVAR length code itself already
// signals a negative value, so the BCD payload is expected to hold plain digits, without
// its own 0xF sign nibble. Only negate here if decode_bcd_le() did not already do so from
// such a sign nibble, to avoid cancelling out a genuine negative encoding.
inline int64_t decode_lvar_negative_bcd(const uint8_t *data, size_t length) {
  const int64_t decoded = decode_bcd_le(data, length);
  return decoded > 0 ? -decoded : decoded;
}

inline double decode_real32_le(const uint8_t *data) {
  uint32_t raw = decode_unsigned_le(data, 4);
  float value;
  std::memcpy(&value, &raw, sizeof(value));
  return value;
}

inline bool variable_data_length(uint8_t lvar, size_t *length) {
  if (lvar <= 0xBF) {
    *length = lvar;
    return true;
  }
  if (lvar >= 0xC0 && lvar <= 0xCF) {
    *length = (lvar - 0xC0) * 2;
    return true;
  }
  if (lvar >= 0xD0 && lvar <= 0xDF) {
    *length = (lvar - 0xD0) * 2;
    return true;
  }
  if (lvar >= 0xE0 && lvar <= 0xEF) {
    *length = lvar - 0xE0;
    return true;
  }
  if (lvar >= 0xF0 && lvar <= 0xFA) {
    *length = lvar - 0xF0;
    return true;
  }
  return false;
}

inline std::string decode_variable_ascii(const uint8_t *data, size_t length) {
  std::string value;
  value.reserve(length);
  for (size_t i = length; i > 0; i--) {
    value.push_back(static_cast<char>(data[i - 1]));
  }
  return value;
}

}  // namespace esphome::mbus::records_codec
