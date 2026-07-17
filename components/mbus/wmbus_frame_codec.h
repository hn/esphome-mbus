#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace esphome::mbus::wmbus_frame_codec {

static constexpr uint16_t WMBUS_CRC_POLY = 0x3D65;

// First byte of a mode C over-the-air packet; anything else is treated as mode T (no such fixed prefix,
// already 3-of-6 encoded from the first byte).
static constexpr uint8_t WMBUS_MODE_C_PREFIX = 0x54;
static constexpr uint8_t WMBUS_FRAME_A_PREFIX = 0xCD;
static constexpr uint8_t WMBUS_FRAME_B_PREFIX = 0x3D;

inline uint16_t crc16_en13757(const uint8_t *data, size_t length) {
  uint16_t crc = 0;
  for (size_t i = 0; i < length; i++) {
    uint8_t value = data[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      if ((((crc & 0x8000) >> 8) ^ (value & 0x80)) != 0) {
        crc = (crc << 1) ^ WMBUS_CRC_POLY;
      } else {
        crc <<= 1;
      }
      value <<= 1;
    }
  }
  return ~crc;
}

inline size_t packet_len_frame_b(uint8_t l_field) {
  if (l_field < 12) {
    return 0;
  }
  if (l_field <= 127) {
    return 1 + l_field + 2;
  }
  if (l_field >= 129) {
    return 128 + (l_field - 129) + 2;
  }
  return 128;
}

inline size_t packet_len_frame_a(uint8_t l_field) {
  if (l_field <= 9) {
    return 0;
  }
  const size_t data_bytes = l_field - 9;
  const size_t num_blocks = data_bytes / 16;
  const size_t extra_bytes = data_bytes % 16 == 0 ? 0 : (data_bytes % 16) + 2;
  return 12 + num_blocks * 18 + extra_bytes;
}

inline bool strip_frame_a_crcs(const uint8_t *data, size_t length, std::vector<uint8_t> *out,
                               uint8_t *failed_block = nullptr) {
  if (length < 12) {
    if (failed_block != nullptr) {
      *failed_block = 1;
    }
    return false;
  }

  out->clear();
  out->reserve(length);
  size_t pos = 0;
  uint8_t block = 1;
  while (pos < length) {
    if (length - pos <= 2) {
      if (failed_block != nullptr) {
        *failed_block = block;
      }
      return false;
    }
    const size_t data_length = pos == 0 ? 10 : std::min<size_t>(16, length - pos - 2);
    if (pos + data_length + 2 > length) {
      if (failed_block != nullptr) {
        *failed_block = block;
      }
      return false;
    }
    const uint16_t expected = (uint16_t(data[pos + data_length]) << 8) | data[pos + data_length + 1];
    if (crc16_en13757(data + pos, data_length) != expected) {
      if (failed_block != nullptr) {
        *failed_block = block;
      }
      return false;
    }
    out->insert(out->end(), data + pos, data + pos + data_length);
    pos += data_length + 2;
    block++;
  }
  return true;
}

// Format B has one CRC for frames up to 128 bytes and two CRCs for longer
// frames. The L field is updated after CRC removal for long frames.
inline bool strip_frame_b_crcs(const uint8_t *data, size_t length, std::vector<uint8_t> *out,
                               uint8_t *failed_block = nullptr) {
  if (length < 12) {
    if (failed_block != nullptr) {
      *failed_block = 1;
    }
    return false;
  }

  const size_t first_crc_pos = length <= 128 ? length - 2 : 126;
  const size_t second_crc_pos = length <= 128 ? 0 : length - 2;
  const uint16_t first_expected = (uint16_t(data[first_crc_pos]) << 8) | data[first_crc_pos + 1];
  if (crc16_en13757(data, first_crc_pos) != first_expected) {
    if (failed_block != nullptr) {
      *failed_block = 1;
    }
    return false;
  }

  out->clear();
  out->reserve(length);
  out->insert(out->end(), data, data + first_crc_pos);

  if (second_crc_pos != 0) {
    const uint8_t *second_data = data + first_crc_pos + 2;
    const size_t second_length = second_crc_pos - first_crc_pos - 2;
    const uint16_t second_expected = (uint16_t(data[second_crc_pos]) << 8) | data[second_crc_pos + 1];
    if (crc16_en13757(second_data, second_length) != second_expected) {
      if (failed_block != nullptr) {
        *failed_block = 2;
      }
      return false;
    }
    out->insert(out->end(), second_data, second_data + second_length);
  }

  if (!out->empty()) {
    (*out)[0] = static_cast<uint8_t>(out->size() - 1);
  }
  return true;
}

inline int decode_3of6_nibble(uint8_t code) {
  switch (code) {
    case 0b010110:
      return 0x0;
    case 0b001101:
      return 0x1;
    case 0b001110:
      return 0x2;
    case 0b001011:
      return 0x3;
    case 0b011100:
      return 0x4;
    case 0b011001:
      return 0x5;
    case 0b011010:
      return 0x6;
    case 0b010011:
      return 0x7;
    case 0b101100:
      return 0x8;
    case 0b100101:
      return 0x9;
    case 0b100110:
      return 0xA;
    case 0b100011:
      return 0xB;
    case 0b110100:
      return 0xC;
    case 0b110001:
      return 0xD;
    case 0b110010:
      return 0xE;
    case 0b101001:
      return 0xF;
    default:
      return -1;
  }
}

inline size_t encoded_size_3of6(size_t decoded_size) { return (3 * decoded_size + 1) / 2; }

inline bool decode_3of6(const uint8_t *data, size_t length, std::vector<uint8_t> *out) {
  const size_t nibble_count = (length * 8) / 6;
  if ((nibble_count & 1) != 0) {
    return false;
  }

  out->clear();
  out->reserve(nibble_count / 2);
  for (size_t nibble_index = 0; nibble_index < nibble_count; nibble_index++) {
    uint8_t code = 0;
    for (size_t bit = 0; bit < 6; bit++) {
      const size_t bit_index = nibble_index * 6 + bit;
      code = static_cast<uint8_t>((code << 1) | ((data[bit_index / 8] >> (7 - (bit_index % 8))) & 0x01));
    }
    const int nibble = decode_3of6_nibble(code);
    if (nibble < 0) {
      return false;
    }
    if ((nibble_index & 1) == 0) {
      out->push_back(static_cast<uint8_t>(nibble << 4));
    } else {
      out->back() |= static_cast<uint8_t>(nibble);
    }
  }
  return true;
}

inline bool decode_3of6_first_byte(const uint8_t *data, size_t length, uint8_t *out) {
  if (length < 2) {
    return false;
  }
  std::vector<uint8_t> decoded;
  if (!decode_3of6(data, 2, &decoded) || decoded.empty()) {
    return false;
  }
  *out = decoded[0];
  return true;
}

// Computes the total over-the-air packet length (in raw radio bytes, as seen by a growing receive
// buffer) for a wM-Bus mode C (format A or B) or mode T packet, from the bytes received so far.
// Meant to be used directly as a cc1101/sx126x/sx127x `packet_length_lambda`: returns 0 while more
// bytes are needed to know the length, -1 if the bytes received so far can't be a valid wM-Bus packet,
// or the total expected packet length once it's known.
inline int32_t expected_packet_length(const std::vector<uint8_t> &x) {
  if (x.empty()) {
    return 0;
  }
  if (x[0] == WMBUS_MODE_C_PREFIX) {
    if (x.size() < 2) {
      return 0;
    }
    if (x[1] != WMBUS_FRAME_A_PREFIX && x[1] != WMBUS_FRAME_B_PREFIX) {
      return -1;
    }
    if (x.size() < 3) {
      return 0;
    }
    const uint8_t l_field = x[2];
    const size_t body_length = x[1] == WMBUS_FRAME_A_PREFIX ? packet_len_frame_a(l_field) : packet_len_frame_b(l_field);
    if (body_length == 0) {
      return -1;
    }
    return static_cast<int32_t>(body_length + 2);
  }

  // Mode T: no fixed prefix byte, the stream is 3-of-6 encoded from the start.
  if (x.size() < 2) {
    return 0;
  }
  uint8_t l_field = 0;
  if (!decode_3of6_first_byte(x.data(), x.size(), &l_field)) {
    return -1;
  }
  const size_t body_length = packet_len_frame_a(l_field);
  if (body_length == 0) {
    return -1;
  }
  return static_cast<int32_t>(encoded_size_3of6(body_length));
}

}  // namespace esphome::mbus::wmbus_frame_codec
