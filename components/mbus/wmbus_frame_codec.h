#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace esphome::mbus::wmbus_frame_codec {

// CRC of the link layer blocks: x^16 + x^13 + x^12 + x^11 + x^10 + x^8 + x^6 + x^5 + x^2 + 1, initial value 0,
// final CRC complemented (EN 13757-4:2019, 12.5.7)
static constexpr uint16_t WMBUS_CRC_POLY = 0x3D65;

// Last 16 chips of the mode C synchronization pattern, which a receiver synchronizing on the part in front of
// them delivers as the first two bytes: 54h, then CDh for frame format A or 3Dh for frame format B
// (EN 13757-4:2019, 9.4.2). Anything else is treated as mode T, which has no such prefix and is "3 out of 6"
// encoded from its first byte (7.4.2).
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

// Frame format B: the L field counts all bytes behind it including the CRCs. Frames of up to 128 bytes have
// one CRC, frames of 131 to 256 bytes a second CRC over the optional block; 129 and 130 bytes do not exist
// (EN 13757-4:2019, 12.4, Tables 30 to 32, 12.5.3). The shortest frame holds the first block, the CI and a CRC.
inline size_t packet_len_frame_b(uint8_t l_field) {
  if (l_field < 12 || l_field == 128 || l_field == 129) {
    return 0;
  }
  return static_cast<size_t>(l_field) + 1;
}

// Frame format A: the L field does not count the CRCs. The first block has 10 bytes, the others 16, the last
// one what is left; each is followed by a CRC (EN 13757-4:2019, 12.3, Tables 27 to 29, 12.5.3)
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

// Format B has one CRC for frames up to 128 bytes and two CRCs for longer frames. The L field of the result
// counts the bytes behind it without the CRCs, as in format A.
inline bool strip_frame_b_crcs(const uint8_t *data, size_t length, std::vector<uint8_t> *out,
                               uint8_t *failed_block = nullptr) {
  if (length < 13 || length == 129 || length == 130) {
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

// "3 out of 6" code of mode T: 6 chips per nibble (EN 13757-4:2019, 7.4.2, Table 10)
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

// True if one of the addresses of a datagram without CRCs carries this identification number: the one of the
// link layer, the second address of an Extended Link Layer (CI 8Eh, 8Fh; EN 13757-4:2019, 13.2) or the one of
// a long transport header (EN 13757-7:2018, 7.4), which is the meter behind a radio adapter. Layers behind an
// encrypted Extended Link Layer cannot be looked at.
inline bool has_meter_id(const uint8_t *frame, size_t length, uint32_t meter_id) {
  auto id_at = [frame, length, meter_id](size_t pos) {
    return pos + 4 <= length &&
           (static_cast<uint32_t>(frame[pos]) | static_cast<uint32_t>(frame[pos + 1]) << 8 |
            static_cast<uint32_t>(frame[pos + 2]) << 16 | static_cast<uint32_t>(frame[pos + 3]) << 24) == meter_id;
  };
  // L, C, manufacturer, then the identification number
  if (id_at(4)) {
    return true;
  }
  size_t pos = 10;
  while (pos < length) {
    const uint8_t ci = frame[pos];
    if (ci >= 0x8C && ci <= 0x8F) {
      static constexpr uint8_t ELL_LENGTH[] = {2, 8, 10, 16};
      const size_t layer = ELL_LENGTH[ci - 0x8C];
      // 8Eh and 8Fh: CC, ACC, manufacturer, then the second address
      if (ci >= 0x8E && id_at(pos + 5)) {
        return true;
      }
      // 8Dh and 8Fh end with the session number, whose top bits announce encryption, and the payload CRC
      if ((ci & 0x01) != 0 && (pos + layer >= length || (frame[pos + layer - 2] >> 5) != 0)) {
        return false;
      }
      pos += 1 + layer;
    } else if (ci == 0x90) {
      if (pos + 1 >= length) {
        return false;
      }
      pos += 2 + frame[pos + 1];
    } else {
      // CI fields with a long header: it starts with the identification number
      return (ci == 0x72 || ci == 0x73 || ci == 0x6F || ci == 0x75) && id_at(pos + 1);
    }
  }
  return false;
}

}  // namespace esphome::mbus::wmbus_frame_codec
