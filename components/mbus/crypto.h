#pragma once

#include "esphome/core/defines.h"

#include <cstddef>
#include <cstdint>

// Same library choice as dlms_meter: PSA where the framework has it (ESP-IDF), plain mbedTLS elsewhere,
// BearSSL on ESP8266 and RP2040 (their Arduino cores ship BearSSL, not mbedTLS).
#if __has_include(<psa/crypto.h>)
#include <psa/crypto.h>
#define MBUS_AES_PSA
#elif !defined(USE_ESP8266) && __has_include(<mbedtls/aes.h>)
#if __has_include(<mbedtls/esp_config.h>)
#include <mbedtls/esp_config.h>
#endif
#include <mbedtls/aes.h>
#define MBUS_AES_MBEDTLS
#elif __has_include(<bearssl/bearssl.h>)
#include <bearssl/bearssl.h>
#define MBUS_AES_BEARSSL
#elif defined(USE_HOST)
// The host platform has no AES library; encryption_key is refused at config validation
#define MBUS_AES_NONE
#else
#error "The platform doesn't provide a compatible AES library for mbus"
#endif

namespace esphome::mbus {

// AES-128-CBC decryption without padding, as used by wM-Bus security mode 5.
class Aes128CbcDecryptor {
 public:
  static constexpr size_t KEY_SIZE = 16;
  static constexpr size_t BLOCK_SIZE = 16;

  // Expands or imports the key once. The caller may wipe its own copy afterwards.
  bool set_key(const uint8_t *key);
  bool has_key() const { return this->has_key_; }
  // length must be a multiple of BLOCK_SIZE; in and out must not overlap.
  bool decrypt(const uint8_t *iv, const uint8_t *in, uint8_t *out, size_t length);

 protected:
#if defined(MBUS_AES_PSA)
  mbedtls_svc_key_id_t key_id_{};
#elif defined(MBUS_AES_MBEDTLS)
  mbedtls_aes_context context_{};
#elif defined(MBUS_AES_BEARSSL)
  br_aes_ct_cbcdec_keys context_{};
#endif
  bool has_key_{false};
};

}  // namespace esphome::mbus
