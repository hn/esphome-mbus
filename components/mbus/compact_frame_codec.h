#pragma once

#include "records_codec.h"
#include "wmbus_frame_codec.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// M-Bus compact frames (EN 13757-3:2018, Annex G): a compact frame carries the data of the records without
// their DIF/VIF bytes. Those are learned from a full frame of the same meter and found again by their CRC, the
// format signature. Format frames are not read. Nothing here logs, so the host tests can run it.
namespace esphome::mbus::compact_frame_codec {

// CI fields without, with the short and with the long header (Table G.2)
static constexpr uint8_t CI_FULL_NO_HEADER = 0x78;
static constexpr uint8_t CI_FULL_SHORT = 0x7A;
static constexpr uint8_t CI_FULL_LONG = 0x72;
static constexpr uint8_t CI_COMPACT_NO_HEADER = 0x79;
static constexpr uint8_t CI_COMPACT_SHORT = 0x7B;
static constexpr uint8_t CI_COMPACT_LONG = 0x73;

// Longest DIF/VIF sequence that is kept, and how many of them
static constexpr size_t MAX_FORMAT_SIZE = 64;
static constexpr size_t FORMAT_SLOTS = 2;
// Format signature and full frame CRC in front of the data of a compact frame, low byte first (Table G.4)
static constexpr size_t COMPACT_HEADER_SIZE = 4;
// The application data of a datagram never gets longer than this
static constexpr size_t MAX_RECORDS_SIZE = 255;
static constexpr uint8_t DIF_IDLE_FILLER = 0x2F;
static constexpr uint8_t DIF_MANUFACTURER_DATA = 0x0F;
static constexpr uint8_t DIF_MORE_RECORDS = 0x1F;

enum class FormatResult : uint8_t {
  FORMAT_OK,
  // More than the capacity; the length is still reported
  FORMAT_TOO_LONG,
  // The records cannot be walked to their end
  FORMAT_UNREADABLE,
};

// Length of the header of the record at data: DIF, DIFEs, VIF, the text of a plain text VIF and VIFEs. 0 if
// it does not fit into length.
inline size_t record_header_length(const uint8_t *data, size_t length) {
  size_t pos = 0;
  if (pos >= length) {
    return 0;
  }
  uint8_t extension = data[pos++] & 0x80;
  while (extension != 0) {
    if (pos >= length) {
      return 0;
    }
    extension = data[pos++] & 0x80;
  }
  if (pos >= length) {
    return 0;
  }
  const uint8_t vif = data[pos++];
  if ((vif & 0x7F) == 0x7C) {
    if (pos >= length) {
      return 0;
    }
    pos += static_cast<size_t>(data[pos]) + 1;
    if (pos > length) {
      return 0;
    }
  }
  extension = vif & 0x80;
  while (extension != 0) {
    if (pos >= length) {
      return 0;
    }
    extension = data[pos++] & 0x80;
  }
  return pos;
}

// The format of the records of a full frame: the headers of all records, a manufacturer data header as it is
// (the data behind it is data), and the idle fillers that stand between records. Fillers in front of the first
// record are the check bytes of the encryption, fillers at the end its padding; neither belongs to the format
// (G.4). format_length is set for FORMAT_OK and FORMAT_TOO_LONG.
inline FormatResult extract_format(const uint8_t *records, size_t length, uint8_t *format, size_t capacity,
                                   size_t *format_length) {
  size_t pos = 0;
  size_t out = 0;
  size_t fillers = 0;
  auto put = [&](uint8_t byte) {
    if (out < capacity) {
      format[out] = byte;
    }
    out++;
  };
  while (pos < length) {
    const uint8_t dif = records[pos];
    if (dif == DIF_IDLE_FILLER) {
      // Kept only if a record follows
      if (out > 0) {
        fillers++;
      }
      pos++;
      continue;
    }
    for (; fillers > 0; fillers--) {
      put(DIF_IDLE_FILLER);
    }
    if (dif == DIF_MANUFACTURER_DATA || dif == DIF_MORE_RECORDS) {
      put(dif);
      break;
    }
    if ((dif & 0x0F) == 0x0F) {
      return FormatResult::FORMAT_UNREADABLE;
    }
    const size_t header = record_header_length(records + pos, length - pos);
    if (header == 0) {
      return FormatResult::FORMAT_UNREADABLE;
    }
    for (size_t i = 0; i < header; i++) {
      put(records[pos + i]);
    }
    pos += header;
    size_t data_length = records_codec::dif_data_length(dif);
    if ((dif & 0x0F) == 0x0D) {
      if (pos >= length || !records_codec::variable_data_length(records[pos], &data_length)) {
        return FormatResult::FORMAT_UNREADABLE;
      }
      pos++;
    }
    if (pos + data_length > length) {
      return FormatResult::FORMAT_UNREADABLE;
    }
    pos += data_length;
  }
  if (out == 0) {
    return FormatResult::FORMAT_UNREADABLE;
  }
  *format_length = out;
  return out > capacity ? FormatResult::FORMAT_TOO_LONG : FormatResult::FORMAT_OK;
}

// Format signature: the CRC of the format (G.4)
inline uint16_t format_signature(const uint8_t *format, size_t length) {
  return wmbus_frame_codec::crc16_en13757(format, length);
}

enum class ExpandResult : uint8_t {
  EXPAND_OK,
  // The format does not end with a complete record header
  EXPAND_FORMAT,
  // The compact frame has less data than the format asks for
  EXPAND_TRUNCATED,
  // Data other than idle fillers is left behind the last record
  EXPAND_DATA_LEFT,
  EXPAND_TOO_LONG,
};

// Puts format and data together to the records of the full frame, without the fillers at the end of the data
inline ExpandResult expand(const uint8_t *format, size_t format_length, const uint8_t *data, size_t data_length,
                           std::vector<uint8_t> *out) {
  out->clear();
  out->reserve(MAX_RECORDS_SIZE);
  size_t f = 0;
  size_t d = 0;
  auto copy = [out](const uint8_t *from, size_t count) {
    if (out->size() + count > MAX_RECORDS_SIZE) {
      return false;
    }
    out->insert(out->end(), from, from + count);
    return true;
  };
  while (f < format_length) {
    const uint8_t dif = format[f];
    if (dif == DIF_IDLE_FILLER) {
      if (!copy(format + f, 1)) {
        return ExpandResult::EXPAND_TOO_LONG;
      }
      f++;
      continue;
    }
    if (dif == DIF_MANUFACTURER_DATA || dif == DIF_MORE_RECORDS) {
      // Everything left is the data behind it
      if (f + 1 != format_length) {
        return ExpandResult::EXPAND_FORMAT;
      }
      if (!copy(format + f, 1) || !copy(data + d, data_length - d)) {
        return ExpandResult::EXPAND_TOO_LONG;
      }
      return ExpandResult::EXPAND_OK;
    }
    const size_t header = record_header_length(format + f, format_length - f);
    if (header == 0 || (dif & 0x0F) == 0x0F) {
      return ExpandResult::EXPAND_FORMAT;
    }
    if (!copy(format + f, header)) {
      return ExpandResult::EXPAND_TOO_LONG;
    }
    f += header;
    size_t length = records_codec::dif_data_length(dif);
    if ((dif & 0x0F) == 0x0D) {
      // The length byte of a variable length record is data
      if (d >= data_length || !records_codec::variable_data_length(data[d], &length)) {
        return ExpandResult::EXPAND_TRUNCATED;
      }
      length++;
    }
    if (d + length > data_length) {
      return ExpandResult::EXPAND_TRUNCATED;
    }
    if (!copy(data + d, length)) {
      return ExpandResult::EXPAND_TOO_LONG;
    }
    d += length;
  }
  if (!std::all_of(data + d, data + data_length, [](uint8_t byte) { return byte == DIF_IDLE_FILLER; })) {
    return ExpandResult::EXPAND_DATA_LEFT;
  }
  return ExpandResult::EXPAND_OK;
}

// Full frame CRC: over the records of the full frame from the first to the end of the last one (G.3)
inline uint16_t full_frame_crc(const std::vector<uint8_t> &records) {
  return wmbus_frame_codec::crc16_en13757(records.data(), records.size());
}

// The formats of one meter. The slot used longest ago is overwritten.
class FormatStore {
 public:
  struct Format {
    uint16_t signature{0};
    uint8_t length{0};
    uint8_t age{0};
    std::array<uint8_t, MAX_FORMAT_SIZE> bytes{};
  };

  const Format *find(uint16_t signature) {
    for (Format &format : this->formats_) {
      if (format.length != 0 && format.signature == signature) {
        this->touch_(&format);
        return &format;
      }
    }
    return nullptr;
  }

  // length is at most MAX_FORMAT_SIZE; true if the format was not there before
  bool put(uint16_t signature, const uint8_t *bytes, size_t length) {
    if (length == 0 || length > MAX_FORMAT_SIZE) {
      return false;
    }
    if (this->find(signature) != nullptr) {
      return false;
    }
    Format *oldest = &this->formats_[0];
    for (Format &format : this->formats_) {
      if (format.length == 0) {
        oldest = &format;
        break;
      }
      if (format.age > oldest->age) {
        oldest = &format;
      }
    }
    oldest->signature = signature;
    oldest->length = static_cast<uint8_t>(length);
    std::copy_n(bytes, length, oldest->bytes.begin());
    this->touch_(oldest);
    return true;
  }

  // A format whose frames no longer match their full frame CRC is outdated
  void forget(uint16_t signature) {
    for (Format &format : this->formats_) {
      if (format.signature == signature) {
        format.length = 0;
      }
    }
  }

 protected:
  void touch_(Format *used) {
    for (Format &format : this->formats_) {
      if (format.age < UINT8_MAX) {
        format.age++;
      }
    }
    used->age = 0;
  }

  std::array<Format, FORMAT_SLOTS> formats_{};
};

}  // namespace esphome::mbus::compact_frame_codec
