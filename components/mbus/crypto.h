#pragma once

#include "esphome/core/defines.h"

#include <cstddef>
#include <cstdint>

// Without an encryption_key nothing is decrypted, so no AES library gets into the firmware. With one: PSA where
// the framework has it (ESP32), BearSSL on ESP8266 and RP2040, whose Arduino cores ship it. The host platform has
// neither; a test that sets a key there has to add the BearSSL headers and its AES sources to the build.
#ifndef USE_MBUS_ENCRYPTION_KEY
#define MBUS_AES_NONE
#elif __has_include(<psa/crypto.h>)
#include <psa/crypto.h>
#define MBUS_AES_PSA
#elif __has_include(<bearssl/bearssl.h>)
#include <bearssl/bearssl.h>
#define MBUS_AES_BEARSSL
#elif defined(USE_HOST)
#error "encryption_key on the host platform needs BearSSL in the build (bearssl/bearssl.h and its AES sources)"
#else
#error "The platform doesn't provide a compatible AES library for mbus"
#endif

namespace esphome::mbus {

// AES-128-CBC decryption without padding, as used by the security modes 5 and 7.
class Aes128CbcDecryptor {
 public:
  static constexpr size_t KEY_SIZE = 16;
  static constexpr size_t BLOCK_SIZE = 16;

  Aes128CbcDecryptor() = default;
  Aes128CbcDecryptor(const Aes128CbcDecryptor &) = delete;
  Aes128CbcDecryptor &operator=(const Aes128CbcDecryptor &) = delete;
#if defined(MBUS_AES_PSA)
  // Destroys the imported key
  ~Aes128CbcDecryptor();
#else
  ~Aes128CbcDecryptor() = default;
#endif

  // Expands or imports the key once. The caller may wipe its own copy afterwards.
  bool set_key(const uint8_t *key);
  bool has_key() const { return this->has_key_; }
  // length must be a multiple of BLOCK_SIZE; in and out must not overlap.
  bool decrypt(const uint8_t *iv, const uint8_t *in, uint8_t *out, size_t length);

 protected:
#if defined(MBUS_AES_PSA)
  mbedtls_svc_key_id_t key_id_{};
#elif defined(MBUS_AES_BEARSSL)
  br_aes_ct_cbcdec_keys context_{};
#endif
  bool has_key_{false};
};

// AES-CMAC (NIST SP 800-38B) with a 128 bit key: the key derivation and the authentication of security mode 7
// (EN 13757-7:2018, 9.6.2 and 9.3.2).
class Aes128Cmac {
 public:
  static constexpr size_t KEY_SIZE = 16;
  static constexpr size_t BLOCK_SIZE = 16;

  Aes128Cmac() = default;
  Aes128Cmac(const Aes128Cmac &) = delete;
  Aes128Cmac &operator=(const Aes128Cmac &) = delete;
#if defined(MBUS_AES_PSA)
  // Destroys the imported key
  ~Aes128Cmac();
#else
  ~Aes128Cmac() = default;
#endif

  // Expands or imports the key once. The caller may wipe its own copy afterwards.
  bool set_key(const uint8_t *key);
  bool has_key() const { return this->has_key_; }
  // MAC over head followed by data, either of which may be empty; mac takes BLOCK_SIZE bytes.
  bool compute(const uint8_t *head, size_t head_length, const uint8_t *data, size_t data_length, uint8_t *mac);
  // The block cipher itself: encrypts BLOCK_SIZE bytes in place. The counter mode of the Extended Link Layer
  // is built on it.
  bool encrypt_block(uint8_t *block) { return this->encrypt_block_(block); }

 protected:
  bool encrypt_block_(uint8_t *block);

#if defined(MBUS_AES_PSA)
  mbedtls_svc_key_id_t key_id_{};
#elif defined(MBUS_AES_BEARSSL)
  br_aes_ct_cbcenc_keys context_{};
#endif
  bool has_key_{false};
};

}  // namespace esphome::mbus
