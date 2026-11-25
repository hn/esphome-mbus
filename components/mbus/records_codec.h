#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>

namespace esphome::mbus::records_codec {

// A record has up to 10 DIFEs and up to 10 VIFEs (EN 13757-3:2018, 6.3.7 and 6.4.1)
inline constexpr size_t MAX_EXTENSIONS = 10;

// Length of the data from the data field of the DIF (EN 13757-3:2018, Table 4); 0 for the variable length
// code Dh, whose length is the LVAR byte
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

// True if the BCD digits form a number (EN 13757-3:2018, Table A.1): Ah to Eh in any digit position signal an
// invalid value, Fh is allowed in the most significant digit only, where it marks a negative number
inline bool bcd_is_valid(const uint8_t *data, size_t length) {
  for (size_t i = 0; i < length; i++) {
    const uint8_t high = data[i] >> 4;
    const uint8_t low = data[i] & 0x0F;
    if (low > 9 || (high > 9 && (high != 0x0F || i != length - 1))) {
      return false;
    }
  }
  return true;
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

// Data length behind an LVAR byte (EN 13757-3:2018, Table 5). The BCD ranges are taken up to CFh and DFh,
// although the table ends them at C9h and D9h.
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
  // Binary numbers of 16, 20, 24, 28 and 32 bytes, then 48 and 64 bytes; the codes behind are reserved
  if (lvar >= 0xF0 && lvar <= 0xF4) {
    *length = 4 * (lvar - 0xEC);
    return true;
  }
  if (lvar == 0xF5) {
    *length = 48;
    return true;
  }
  if (lvar == 0xF6) {
    *length = 64;
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

// Offset of the manufacturer specific data: the byte behind the MDH, which is the first DIF 0Fh or 1Fh
// (EN 13757-3:2018, 6.1). The records in front of it are walked, so a 0Fh inside a record is not taken for it.
// Returns length if there is no MDH or the records cannot be walked.
inline size_t manufacturer_data_offset(const uint8_t *data, size_t length) {
  size_t pos = 0;
  while (pos < length) {
    const uint8_t dif = data[pos++];
    if (dif == 0x2F) {
      continue;
    }
    if (dif == 0x0F || dif == 0x1F) {
      return pos;
    }
    // Another special function: the records cannot be walked any further
    if ((dif & 0x0F) == 0x0F) {
      return length;
    }
    uint8_t extension = dif & 0x80;
    while (extension != 0) {
      if (pos >= length) {
        return length;
      }
      extension = data[pos++] & 0x80;
    }
    if (pos >= length) {
      return length;
    }
    const uint8_t vif = data[pos++];
    // Plain text VIF: a length byte and that many characters
    if ((vif & 0x7F) == 0x7C) {
      if (pos >= length) {
        return length;
      }
      pos += static_cast<size_t>(data[pos]) + 1;
    }
    extension = vif & 0x80;
    while (extension != 0) {
      if (pos >= length) {
        return length;
      }
      extension = data[pos++] & 0x80;
    }
    size_t data_length = dif_data_length(dif);
    if ((dif & 0x0F) == 0x0D) {
      if (pos >= length || !variable_data_length(data[pos], &data_length)) {
        return length;
      }
      pos++;
    }
    pos += data_length;
  }
  return length;
}

}  // namespace esphome::mbus::records_codec
