#pragma once

#include "afl_codec.h"
#include "crypto.h"
#include "wmbus_frame_codec.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

// Security modes 5 and 7 of the transport layer (EN 13757-7:2018, 9.4.4 and 9.4.5) and the encryption of the
// Extended Link Layer (EN 13757-4:2019, 13.2.12), without any logging so the host tests can run it
namespace esphome::mbus::security_codec {

static constexpr uint8_t KEY_DERIVATION_NONE = 0;
static constexpr uint8_t KEY_DERIVATION_A = 1;
static constexpr size_t ADDRESS_SIZE = 8;
static constexpr size_t IDENTIFICATION_SIZE = 4;

enum class Mode7Result : uint8_t {
  MODE_7_OK,
  MODE_7_KEY_DERIVATION_UNKNOWN,
  MODE_7_NO_COUNTER,
  MODE_7_AUTHENTICATION_UNKNOWN,
  MODE_7_KEY_DERIVATION_FAILED,
  MODE_7_MAC_NOT_COMPUTED,
  MODE_7_MAC_MISMATCH,
  MODE_7_DECRYPTION_FAILED,
};

// Address of the meter in the order of the link layer (manufacturer, identification number, version, device
// type) from a long header, which starts with the identification number (EN 13757-7:2018, 7.4)
inline void address_from_long_header(const uint8_t *header, uint8_t *address) {
  std::copy_n(header + IDENTIFICATION_SIZE, 2, address);
  std::copy_n(header, IDENTIFICATION_SIZE, address + 2);
  std::copy_n(header + IDENTIFICATION_SIZE + 2, 2, address + 2 + IDENTIFICATION_SIZE);
}

// Mode 5 IV: manufacturer, identification number, version and device type as transmitted, then the access number
// 8 times (Table 48)
inline void mode_5_iv(const uint8_t *address, uint8_t access_number, uint8_t *iv) {
  std::copy_n(address, ADDRESS_SIZE, iv);
  std::fill_n(iv + ADDRESS_SIZE, Aes128CbcDecryptor::BLOCK_SIZE - ADDRESS_SIZE, access_number);
}

// Mode 5: AES-128-CBC with the IV above; length is a multiple of the block size
inline bool mode_5_decrypt(Aes128CbcDecryptor &decryptor, const uint8_t *address, uint8_t access_number,
                           const uint8_t *encrypted, size_t length, std::vector<uint8_t> *out) {
  uint8_t iv[Aes128CbcDecryptor::BLOCK_SIZE];
  mode_5_iv(address, access_number, iv);
  out->assign(length, 0);
  return decryptor.decrypt(iv, encrypted, out->data(), length);
}

// Encryption subfield of the session number of the Extended Link Layer: its bits 31 to 29, in the last byte as
// transmitted (EN 13757-4:2019, Tables 52 and 53)
static constexpr uint8_t ELL_ENCRYPTION_NONE = 0;
static constexpr uint8_t ELL_ENCRYPTION_AES_CTR = 1;
static constexpr size_t ELL_SESSION_NUMBER_SIZE = 4;
static constexpr size_t ELL_PAYLOAD_CRC_SIZE = 2;
inline uint8_t ell_encryption(const uint8_t *session_number) { return session_number[3] >> 5; }

// Extended Link Layer in AES-128 counter mode: everything from the payload CRC to the end of the datagram is
// encrypted. The initial counter block is the address of the link layer, the communication control field
// without the bits a repeater changes (hop count and repeated access), the session number, a frame number of 0
// and the number of the block (Table 54). The key is the one in cipher.
inline bool ell_decrypt(Aes128Cmac &cipher, const uint8_t *address, uint8_t communication_control,
                        const uint8_t *session_number, const uint8_t *encrypted, size_t length,
                        std::vector<uint8_t> *out) {
  static constexpr uint8_t REPEATER_BITS = 0x12;
  uint8_t counter[Aes128Cmac::BLOCK_SIZE]{};
  std::copy_n(address, ADDRESS_SIZE, counter);
  counter[ADDRESS_SIZE] = communication_control & static_cast<uint8_t>(~REPEATER_BITS);
  std::copy_n(session_number, ELL_SESSION_NUMBER_SIZE, counter + ADDRESS_SIZE + 1);
  // One byte of block number is all there is
  if (length > 256 * Aes128Cmac::BLOCK_SIZE) {
    return false;
  }
  out->assign(encrypted, encrypted + length);
  for (size_t offset = 0; offset < length; offset += Aes128Cmac::BLOCK_SIZE) {
    uint8_t stream[Aes128Cmac::BLOCK_SIZE];
    std::copy_n(counter, sizeof(stream), stream);
    stream[sizeof(stream) - 1] = static_cast<uint8_t>(offset / Aes128Cmac::BLOCK_SIZE);
    if (!cipher.encrypt_block(stream)) {
      return false;
    }
    const size_t block = std::min(length - offset, sizeof(stream));
    for (size_t i = 0; i < block; i++) {
      (*out)[offset + i] ^= stream[i];
    }
  }
  return true;
}

// The decrypted data starts with the payload CRC over everything behind it, low byte first (13.2.15). It is
// the only sign of the right key.
inline bool ell_payload_crc_ok(const std::vector<uint8_t> &plain) {
  if (plain.size() < ELL_PAYLOAD_CRC_SIZE) {
    return false;
  }
  const uint16_t expected = static_cast<uint16_t>(plain[0] | plain[1] << 8);
  return wmbus_frame_codec::crc16_en13757(plain.data() + ELL_PAYLOAD_CRC_SIZE, plain.size() - ELL_PAYLOAD_CRC_SIZE) ==
         expected;
}

struct Mode7Message {
  const uint8_t *identification{nullptr};  // identification number of the meter, 4 bytes as transmitted
  const uint8_t *counter{nullptr};         // message counter, 4 bytes as transmitted; nullptr if there is none
  uint8_t key_derivation{KEY_DERIVATION_NONE};
  const afl_codec::Afl *afl{nullptr};  // nullptr without AFL
  const uint8_t *tpdu{nullptr};        // from the CI field to the last byte
  size_t tpdu_length{0};
  const uint8_t *encrypted{nullptr};  // within the TPDU
  size_t encrypted_length{0};         // multiple of the block size; 0 for a message that is only authenticated
};

// Mode 7: AES-128-CBC with an IV of zero. With key derivation function A the key is derived from the master key
// for every message, and so is the key of the MAC in the AFL. The MAC is checked before anything is decrypted;
// authenticated tells whether there was one.
inline Mode7Result mode_7_decrypt(Aes128Cmac &master_cmac, Aes128CbcDecryptor &master_decryptor,
                                  const Mode7Message &message, std::vector<uint8_t> *out, bool *authenticated) {
  static constexpr uint8_t ZERO_IV[Aes128CbcDecryptor::BLOCK_SIZE]{};
  // Derivation constants of the direction from the meter (Table 53)
  static constexpr uint8_t DERIVE_ENCRYPTION_KEY = 0x00;
  static constexpr uint8_t DERIVE_MAC_KEY = 0x01;

  *authenticated = false;
  if (message.key_derivation != KEY_DERIVATION_NONE && message.key_derivation != KEY_DERIVATION_A) {
    return Mode7Result::MODE_7_KEY_DERIVATION_UNKNOWN;
  }
  const bool derive = message.key_derivation == KEY_DERIVATION_A;
  if (derive && message.counter == nullptr) {
    return Mode7Result::MODE_7_NO_COUNTER;
  }
  const bool has_mac = message.afl != nullptr && message.afl->mac != nullptr;
  if (has_mac && message.afl->authentication_type == afl_codec::AUTHENTICATION_TYPE_GMAC) {
    return Mode7Result::MODE_7_AUTHENTICATION_UNKNOWN;
  }

  // K = CMAC(MK, DC || C || ID || 07h x 7), counter and identification number as transmitted (9.6.2.7). With
  // the short header the identification number is the one of the link layer (9.6.2.5).
  uint8_t input[Aes128Cmac::BLOCK_SIZE];
  uint8_t key[Aes128Cmac::KEY_SIZE];
  if (derive) {
    std::copy_n(message.counter, afl_codec::COUNTER_SIZE, input + 1);
    std::copy_n(message.identification, IDENTIFICATION_SIZE, input + 1 + afl_codec::COUNTER_SIZE);
    std::fill_n(input + 1 + afl_codec::COUNTER_SIZE + IDENTIFICATION_SIZE, 7, 0x07);
  }

  if (has_mac) {
    // The MAC covers MCL to ML of the AFL and the TPDU (9.3.2)
    Aes128Cmac derived;
    Aes128Cmac *cmac = &master_cmac;
    if (derive) {
      input[0] = DERIVE_MAC_KEY;
      if (!master_cmac.compute(input, sizeof(input), nullptr, 0, key) || !derived.set_key(key)) {
        std::fill_n(key, sizeof(key), 0);
        return Mode7Result::MODE_7_KEY_DERIVATION_FAILED;
      }
      cmac = &derived;
    }
    uint8_t mac[Aes128Cmac::BLOCK_SIZE];
    if (!cmac->compute(message.afl->authenticated, message.afl->authenticated_length, message.tpdu, message.tpdu_length,
                       mac)) {
      std::fill_n(key, sizeof(key), 0);
      return Mode7Result::MODE_7_MAC_NOT_COMPUTED;
    }
    if (!std::equal(mac, mac + message.afl->mac_length, message.afl->mac)) {
      std::fill_n(key, sizeof(key), 0);
      return Mode7Result::MODE_7_MAC_MISMATCH;
    }
    *authenticated = true;
  }

  out->assign(message.encrypted_length, 0);
  bool ok = true;
  if (message.encrypted_length > 0) {
    if (derive) {
      Aes128CbcDecryptor derived;
      input[0] = DERIVE_ENCRYPTION_KEY;
      ok = master_cmac.compute(input, sizeof(input), nullptr, 0, key) && derived.set_key(key) &&
           derived.decrypt(ZERO_IV, message.encrypted, out->data(), message.encrypted_length);
    } else {
      ok = master_decryptor.decrypt(ZERO_IV, message.encrypted, out->data(), message.encrypted_length);
    }
  }
  std::fill_n(key, sizeof(key), 0);
  return ok ? Mode7Result::MODE_7_OK : Mode7Result::MODE_7_DECRYPTION_FAILED;
}

}  // namespace esphome::mbus::security_codec
