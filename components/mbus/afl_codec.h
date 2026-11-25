#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome::mbus::afl_codec {

// Authentication and Fragmentation Sublayer (EN 13757-7:2018, clause 6): CI 90h, the length AFLL, the
// fragmentation control field FCL and the fields FCL announces, in the order MCL, KI, MCR, MAC, ML (Table 3).
// Multi byte fields are sent LSB first, except the MAC.
static constexpr uint8_t CI_AFL = 0x90;

// FCL (Table 4)
static constexpr uint16_t FCL_MORE_FRAGMENTS = 0x4000;
static constexpr uint16_t FCL_MCL_PRESENT = 0x2000;
static constexpr uint16_t FCL_ML_PRESENT = 0x1000;
static constexpr uint16_t FCL_MCR_PRESENT = 0x0800;
static constexpr uint16_t FCL_MAC_PRESENT = 0x0400;
static constexpr uint16_t FCL_KI_PRESENT = 0x0200;
static constexpr uint16_t FCL_FRAGMENT_ID_MASK = 0x00FF;

// Authentication type in the low nibble of MCL (Tables 5 and 6)
static constexpr uint8_t MCL_AUTHENTICATION_TYPE_MASK = 0x0F;
static constexpr uint8_t AUTHENTICATION_TYPE_NONE = 0;
static constexpr uint8_t AUTHENTICATION_TYPE_GMAC = 8;

static constexpr size_t COUNTER_SIZE = 4;

// Length of the MAC for an authentication type; 0 for none and for the reserved values
inline size_t mac_length(uint8_t authentication_type) {
  switch (authentication_type) {
    case 3:
      return 2;
    case 4:
      return 4;
    case 5:
      return 8;
    case 6:
    case AUTHENTICATION_TYPE_GMAC:
      return 12;
    case 7:
      return 16;
    default:
      return 0;
  }
}

struct Afl {
  // Bytes behind the CI field, AFLL included
  size_t length{0};
  uint16_t fcl{0};
  uint8_t authentication_type{AUTHENTICATION_TYPE_NONE};
  // Message counter as transmitted, which is how the key derivation takes it (9.6.2.7)
  bool has_counter{false};
  uint8_t counter[COUNTER_SIZE]{};
  const uint8_t *mac{nullptr};
  size_t mac_length{0};
  // MCL, KI, MCR and ML as transmitted: what the MAC covers in front of the TPDU (9.3.2)
  uint8_t authenticated[9]{};
  size_t authenticated_length{0};

  bool fragmented() const { return (this->fcl & (FCL_MORE_FRAGMENTS | FCL_FRAGMENT_ID_MASK)) != 0; }
};

// data points behind the CI field 90h. False if the layer is truncated or does not fit its own length.
inline bool parse(const uint8_t *data, size_t length, Afl *afl) {
  *afl = Afl{};
  if (length < 3 || data[0] < 2 || static_cast<size_t>(data[0]) + 1 > length) {
    return false;
  }
  const size_t end = static_cast<size_t>(data[0]) + 1;
  afl->length = end;
  afl->fcl = static_cast<uint16_t>(data[1] | data[2] << 8);
  size_t offset = 3;
  const auto authenticate = [&](size_t size) {
    for (size_t i = 0; i < size; i++) {
      afl->authenticated[afl->authenticated_length++] = data[offset + i];
    }
    offset += size;
  };
  if ((afl->fcl & FCL_MCL_PRESENT) != 0) {
    if (offset + 1 > end) {
      return false;
    }
    afl->authentication_type = data[offset] & MCL_AUTHENTICATION_TYPE_MASK;
    authenticate(1);
  }
  if ((afl->fcl & FCL_KI_PRESENT) != 0) {
    if (offset + 2 > end) {
      return false;
    }
    authenticate(2);
  }
  if ((afl->fcl & FCL_MCR_PRESENT) != 0) {
    if (offset + COUNTER_SIZE > end) {
      return false;
    }
    afl->has_counter = true;
    for (size_t i = 0; i < COUNTER_SIZE; i++) {
      afl->counter[i] = data[offset + i];
    }
    authenticate(COUNTER_SIZE);
  }
  if ((afl->fcl & FCL_MAC_PRESENT) != 0) {
    afl->mac_length = mac_length(afl->authentication_type);
    if (afl->mac_length == 0 || offset + afl->mac_length > end) {
      return false;
    }
    afl->mac = data + offset;
    offset += afl->mac_length;
  }
  if ((afl->fcl & FCL_ML_PRESENT) != 0) {
    if (offset + 2 > end) {
      return false;
    }
    authenticate(2);
  }
  return true;
}

}  // namespace esphome::mbus::afl_codec
