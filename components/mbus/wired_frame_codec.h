#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

// Link layer framing of wired M-Bus (EN 13757-2:2018, 5.7): format class FT1.2 of EN 60870-5-1 with the
// datagram structure of EN 60870-5-2. The optical interface of EN 1434-3:2008 uses the same link layer.
// Header-only and free of ESPHome dependencies, so the host tests can include it directly.
namespace esphome::mbus::wired_frame_codec {

// FT1.2 frame delimiters; examples in EN 13757-2:2018, Annex F
static constexpr uint8_t START_SHORT = 0x10;
static constexpr uint8_t START_LONG = 0x68;
static constexpr uint8_t STOP = 0x16;
// Single character acknowledge (EN 13757-2:2018, 5.7.3.1, NOTE)
static constexpr uint8_t ACK = 0xE5;

// Control field. Bit 6 is set in every master datagram and clear in every slave datagram.
static constexpr uint8_t CONTROL_DIRECTION_MASTER = 0x40;
// Frame count bit and frame count valid bit of master datagrams (EN 13757-2:2018, 5.7.7)
static constexpr uint8_t CONTROL_FCB = 0x20;
static constexpr uint8_t CONTROL_FCV = 0x10;
// Access demand and data flow control bits of slave datagrams (EN 13757-2:2018, 5.7.3.4)
static constexpr uint8_t CONTROL_ACD = 0x20;
static constexpr uint8_t CONTROL_DFC = 0x10;
// SND_NKE needs FCB and FCV cleared (EN 13757-2:2018, 5.7.3.1, 5.7.7)
static constexpr uint8_t CONTROL_SND_NKE = 0x40;
// REQ_UD2 needs FCV set; FCB is added per request (EN 13757-2:2018, 5.7.3.3, 5.7.7)
static constexpr uint8_t CONTROL_REQ_UD2 = 0x5B;
// SND_UD is 53h or 73h, i.e. with FCB (EN 13757-2:2018, 5.7.3.5)
static constexpr uint8_t CONTROL_SND_UD = 0x53;
// SND_UD2 = 0x43 (EN 13757-2:2018, 5.7.3.5): not implemented
static constexpr uint8_t CONTROL_RSP_UD = 0x08;

// Addresses (EN 13757-2:2018, 5.7.5)
static constexpr uint8_t ADDRESS_UNCONFIGURED = 0;
static constexpr uint8_t ADDRESS_PRIMARY_MAX = 250;
static constexpr uint8_t ADDRESS_SELECTED = 253;
static constexpr uint8_t ADDRESS_TEST = 254;
static constexpr uint8_t ADDRESS_BROADCAST = 255;

// Selection of a device by its secondary address (EN 13757-7:2018, Table 2 and 8.4)
static constexpr uint8_t CI_SELECT = 0x52;

// Application header behind the CI field of an answer (EN 13757-7:2018, Table 2): long (12 bytes) with 72h, the
// compact frame 73h, the application error 6Fh and the alarm 75h, short (4 bytes) with 7Ah, 7Bh, 6Eh and 74h,
// none else
inline size_t application_header_size(uint8_t ci) {
  switch (ci) {
    case 0x6F:
    case 0x72:
    case 0x73:
    case 0x75:
      return 12;
    case 0x6E:
    case 0x74:
    case 0x7A:
    case 0x7B:
      return 4;
    default:
      return 0;
  }
}

static constexpr size_t SHORT_FRAME_SIZE = 5;
// Start, L, L, start in front of the user data; checksum and stop behind it
static constexpr size_t LONG_FRAME_OVERHEAD = 6;
// The L field counts C, A and CI plus the data behind them (EN 13757-2:2018, Annex F.2)
static constexpr size_t LONG_FRAME_MIN_L = 3;
static constexpr size_t LONG_FRAME_MAX_DATA = 255 - LONG_FRAME_MIN_L;
static constexpr size_t LONG_FRAME_MAX_SIZE = 255 + LONG_FRAME_OVERHEAD;
static constexpr size_t SELECT_FRAME_SIZE = 17;

// Arithmetic sum over C, A, CI and data, modulo 256 (FT1.2)
inline uint8_t checksum(const uint8_t *data, size_t length) {
  uint8_t sum = 0;
  for (size_t i = 0; i < length; i++) {
    sum += data[i];
  }
  return sum;
}

inline uint8_t control_with_fcb(uint8_t control, bool fcb) { return fcb ? (control | CONTROL_FCB) : control; }

inline std::array<uint8_t, SHORT_FRAME_SIZE> build_short_frame(uint8_t control, uint8_t address) {
  return {START_SHORT, control, address, static_cast<uint8_t>(control + address), STOP};
}

// Writes a long frame to out and returns its size, or 0 if the data does not fit.
inline size_t build_long_frame(uint8_t control, uint8_t address, uint8_t ci, const uint8_t *data, size_t length,
                               uint8_t *out, size_t capacity) {
  const size_t size = length + LONG_FRAME_MIN_L + LONG_FRAME_OVERHEAD;
  if (length > LONG_FRAME_MAX_DATA || size > capacity) {
    return 0;
  }
  const uint8_t l_field = static_cast<uint8_t>(length + LONG_FRAME_MIN_L);
  out[0] = START_LONG;
  out[1] = l_field;
  out[2] = l_field;
  out[3] = START_LONG;
  out[4] = control;
  out[5] = address;
  out[6] = ci;
  for (size_t i = 0; i < length; i++) {
    out[7 + i] = data[i];
  }
  out[7 + length] = checksum(out + 4, l_field);
  out[8 + length] = STOP;
  return size;
}

// Secondary address as sent on the wire: identification number (BCD, LSB first), manufacturer, version, device
// type (EN 13757-7:2018, 7.5.1 to 7.5.4). The 64-bit value is written as 16 hex digits: the identification
// number as printed on the meter, then the remaining four bytes in wire order, e.g. 0x12345678FA120207 is
// ID 12345678, manufacturer bytes FA 12, version 02, type 07.
inline std::array<uint8_t, 8> secondary_address_bytes(uint64_t secondary_address) {
  return {static_cast<uint8_t>(secondary_address >> 32), static_cast<uint8_t>(secondary_address >> 40),
          static_cast<uint8_t>(secondary_address >> 48), static_cast<uint8_t>(secondary_address >> 56),
          static_cast<uint8_t>(secondary_address >> 24), static_cast<uint8_t>(secondary_address >> 16),
          static_cast<uint8_t>(secondary_address >> 8),  static_cast<uint8_t>(secondary_address)};
}

// Compares the address a slave answers with (first eight bytes of the long header) to the configured one, the
// way the slave does on selection (EN 13757-7:2018, 8.4): a nibble Fh in the identification number and a byte
// FFh in manufacturer, version or device type are wildcards (7.5.1 to 7.5.4).
inline bool secondary_address_matches(uint64_t secondary_address, const uint8_t *actual) {
  const std::array<uint8_t, 8> expected = secondary_address_bytes(secondary_address);
  for (size_t i = 0; i < 4; i++) {
    for (uint8_t mask : {uint8_t{0xF0}, uint8_t{0x0F}}) {
      if ((expected[i] & mask) != mask && (expected[i] & mask) != (actual[i] & mask)) {
        return false;
      }
    }
  }
  for (size_t i = 4; i < 8; i++) {
    if (expected[i] != 0xFF && expected[i] != actual[i]) {
      return false;
    }
  }
  return true;
}

// Selection datagram (EN 13757-7:2018, Table 44). The slave ignores the FCB of a selection (8.4), so it is not
// part of the FCB sequence and always goes out with the C field 73h.
inline std::array<uint8_t, SELECT_FRAME_SIZE> build_select_frame(uint64_t secondary_address) {
  const std::array<uint8_t, 8> address = secondary_address_bytes(secondary_address);
  std::array<uint8_t, SELECT_FRAME_SIZE> frame{};
  build_long_frame(control_with_fcb(CONTROL_SND_UD, true), ADDRESS_SELECTED, CI_SELECT, address.data(), address.size(),
                   frame.data(), frame.size());
  return frame;
}

enum class FrameType : uint8_t {
  FRAME_TYPE_ACK,
  FRAME_TYPE_SHORT,
  FRAME_TYPE_LONG,
};

enum class ParseResult : uint8_t {
  // The bytes so far are the beginning of a frame; size holds the total frame size once it is known, else 0
  PARSE_RESULT_INCOMPLETE,
  PARSE_RESULT_FRAME,
  // The bytes are not the beginning of a valid frame
  PARSE_RESULT_INVALID,
};

struct Frame {
  FrameType type{FrameType::FRAME_TYPE_ACK};
  // Bytes the frame occupies in the buffer
  size_t size{0};
  // Short and long frames
  uint8_t control{0};
  uint8_t address{0};
  // Long frames only; data points into the parsed buffer
  uint8_t ci{0};
  const uint8_t *data{nullptr};
  size_t data_length{0};
};

// Parses the frame that starts at data[0]. Bytes behind a complete frame are left alone.
inline ParseResult parse_frame(const uint8_t *data, size_t length, Frame *frame) {
  *frame = Frame{};
  if (length == 0) {
    return ParseResult::PARSE_RESULT_INCOMPLETE;
  }
  if (data[0] == ACK) {
    frame->type = FrameType::FRAME_TYPE_ACK;
    frame->size = 1;
    return ParseResult::PARSE_RESULT_FRAME;
  }
  if (data[0] == START_SHORT) {
    frame->type = FrameType::FRAME_TYPE_SHORT;
    frame->size = SHORT_FRAME_SIZE;
    if (length < SHORT_FRAME_SIZE) {
      return ParseResult::PARSE_RESULT_INCOMPLETE;
    }
    if (data[3] != static_cast<uint8_t>(data[1] + data[2]) || data[4] != STOP) {
      return ParseResult::PARSE_RESULT_INVALID;
    }
    frame->control = data[1];
    frame->address = data[2];
    return ParseResult::PARSE_RESULT_FRAME;
  }
  if (data[0] != START_LONG) {
    return ParseResult::PARSE_RESULT_INVALID;
  }
  frame->type = FrameType::FRAME_TYPE_LONG;
  if (length < 2) {
    return ParseResult::PARSE_RESULT_INCOMPLETE;
  }
  const size_t l_field = data[1];
  if (l_field < LONG_FRAME_MIN_L || (length >= 3 && data[2] != data[1]) || (length >= 4 && data[3] != START_LONG)) {
    return ParseResult::PARSE_RESULT_INVALID;
  }
  if (length < 3) {
    return ParseResult::PARSE_RESULT_INCOMPLETE;
  }
  frame->size = l_field + LONG_FRAME_OVERHEAD;
  if (length < frame->size) {
    return ParseResult::PARSE_RESULT_INCOMPLETE;
  }
  if (data[frame->size - 2] != checksum(data + 4, l_field) || data[frame->size - 1] != STOP) {
    return ParseResult::PARSE_RESULT_INVALID;
  }
  frame->control = data[4];
  frame->address = data[5];
  frame->ci = data[6];
  frame->data = data + 7;
  frame->data_length = l_field - LONG_FRAME_MIN_L;
  return ParseResult::PARSE_RESULT_FRAME;
}

// A datagram sent by a master. Received on the own line it is the echo of what was just sent (level converters
// and optical heads commonly loop the transmit signal back).
inline bool is_master_frame(const Frame &frame) {
  return frame.type != FrameType::FRAME_TYPE_ACK && (frame.control & CONTROL_DIRECTION_MASTER) != 0;
}

// RSP_UD, whatever its ACD and DFC bits say
inline bool is_rsp_ud(const Frame &frame) {
  return frame.type == FrameType::FRAME_TYPE_LONG &&
         (frame.control & static_cast<uint8_t>(~(CONTROL_ACD | CONTROL_DFC))) == CONTROL_RSP_UD;
}

// CI fields of a compact frame without, with the short and with the long header (EN 13757-3:2018, Table G.2)
inline bool is_compact_frame(uint8_t ci) { return ci == 0x79 || ci == 0x7B || ci == 0x73; }

// CI fields of a general application error (EN 13757-3:2018, 10.3)
inline bool is_application_error(uint8_t ci) { return ci == 0x6E || ci == 0x6F || ci == 0x70; }

// First error byte of an application error, which sits behind the header of its CI field; 0 (unspecified error)
// if there is none (EN 13757-3:2018, Table 21). data points behind the CI field.
inline uint8_t first_error_byte(uint8_t ci, const uint8_t *data, size_t length) {
  const size_t header = application_header_size(ci);
  return data != nullptr && length > header ? data[header] : 0x00;
}

// Bits on the line per byte: start bit, 8 data bits, even parity, stop bit (EN 13757-2:2018, 5.4)
static constexpr uint32_t BITS_PER_BYTE = 11;
// Longest answer time of a slave, counted from the end of the master datagram, in bit times plus a fixed part
// (EN 13757-2:2018, 5.7.6)
static constexpr uint32_t ANSWER_TIME_BITS = 330;
static constexpr uint32_t ANSWER_TIME_EXTRA_MS = 50;

// Time on the line in ms, rounded up
inline uint32_t bits_to_ms(uint32_t bits, uint32_t baud_rate) {
  return static_cast<uint32_t>((static_cast<uint64_t>(bits) * 1000 + baud_rate - 1) / baud_rate);
}

// Time from handing a datagram of tx_size bytes to the UART until an answer of up to rx_size bytes has arrived
inline uint32_t response_timeout_ms(size_t tx_size, size_t rx_size, uint32_t baud_rate) {
  const uint32_t bits = static_cast<uint32_t>(tx_size + rx_size) * BITS_PER_BYTE + ANSWER_TIME_BITS;
  return bits_to_ms(bits, baud_rate) + ANSWER_TIME_EXTRA_MS;
}

// Wake-up of a meter on the optical interface (EN 1434-3:2008, 5.1.1): alternating zeros and ones for
// (2.2 +- 0.1) s after an idle time of more than 330 bit times, then a pause of 33 to 330 bit times. 55h in 8N1
// gives the alternating pattern: start bit 0, data bits 1 0 1 0 1 0 1 0 (LSB first), stop bit 1. With a parity
// bit the pattern would break.
// The pause is deliberately longer than the 330 bit times of the standard (137 ms at 2400 Bd): meters may need
// more to get ready (the software of one manufacturer waits about 0.9 s), and none is expected to fall asleep
// again within a second.
static constexpr uint8_t WAKEUP_BYTE = 0x55;
static constexpr uint32_t WAKEUP_BITS_PER_BYTE = 10;
static constexpr uint32_t WAKEUP_DURATION_MS = 2200;
static constexpr uint32_t WAKEUP_PAUSE_MS = 400;

inline uint32_t wakeup_byte_count(uint32_t baud_rate) {
  return static_cast<uint32_t>(static_cast<uint64_t>(baud_rate) * WAKEUP_DURATION_MS / (WAKEUP_BITS_PER_BYTE * 1000));
}

enum class ReceiveResult : uint8_t {
  RECEIVE_RESULT_WAITING,
  RECEIVE_RESULT_FRAME,
  RECEIVE_RESULT_INVALID,
};

// Assembles the answer of a slave from the bytes of the line, however they are chunked. Own datagrams that are
// echoed back are skipped; anything that is not a valid frame ends the reception as invalid.
class FrameReceiver {
 public:
  void reset() {
    this->length_ = 0;
    this->echoes_ = 0;
    this->frame_ = Frame{};
    this->result_ = ReceiveResult::RECEIVE_RESULT_WAITING;
  }

  // Number of bytes to read next. Never reaches behind the frame in progress, and is 0 once there is a result.
  size_t wanted() const {
    if (this->result_ != ReceiveResult::RECEIVE_RESULT_WAITING) {
      return 0;
    }
    return this->frame_.size > this->length_ ? this->frame_.size - this->length_ : 1;
  }

  // Where the next wanted() bytes go
  uint8_t *tail() { return this->buffer_.data() + this->length_; }

  // Takes over count bytes written to tail(), at most wanted()
  ReceiveResult advance(size_t count) {
    if (count > this->wanted()) {
      this->result_ = ReceiveResult::RECEIVE_RESULT_INVALID;
      return this->result_;
    }
    this->length_ += count;
    switch (parse_frame(this->buffer_.data(), this->length_, &this->frame_)) {
      case ParseResult::PARSE_RESULT_INCOMPLETE:
        break;
      case ParseResult::PARSE_RESULT_FRAME:
        if (is_master_frame(this->frame_)) {
          this->echoes_++;
          this->length_ = 0;
          this->frame_ = Frame{};
        } else {
          this->result_ = ReceiveResult::RECEIVE_RESULT_FRAME;
        }
        break;
      case ParseResult::PARSE_RESULT_INVALID:
        this->result_ = ReceiveResult::RECEIVE_RESULT_INVALID;
        break;
    }
    return this->result_;
  }

  ReceiveResult result() const { return this->result_; }
  // Valid while result() is RECEIVE_RESULT_FRAME; its data points into the buffer of the receiver
  const Frame &frame() const { return this->frame_; }
  // Bytes of the frame in progress, or of what made the reception invalid
  const uint8_t *data() const { return this->buffer_.data(); }
  size_t size() const { return this->length_; }
  uint8_t echoes() const { return this->echoes_; }

 protected:
  std::array<uint8_t, LONG_FRAME_MAX_SIZE> buffer_{};
  size_t length_{0};
  Frame frame_{};
  ReceiveResult result_{ReceiveResult::RECEIVE_RESULT_WAITING};
  uint8_t echoes_{0};
};

}  // namespace esphome::mbus::wired_frame_codec
