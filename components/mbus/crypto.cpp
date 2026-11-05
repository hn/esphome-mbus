#include "crypto.h"

#include "esphome/core/log.h"

#include <cstring>

namespace esphome::mbus {

ESPHOME_LOG_TAG(TAG, "mbus");

#if defined(MBUS_AES_PSA)

bool Aes128CbcDecryptor::set_key(const uint8_t *key) {
  psa_status_t status = psa_crypto_init();
  if (status != PSA_SUCCESS) {
    ESP_LOGE(TAG, "psa_crypto_init failed: status=%d", static_cast<int>(status));
    return false;
  }
  psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
  psa_set_key_bits(&attributes, KEY_SIZE * 8);
  psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DECRYPT);
  psa_set_key_algorithm(&attributes, PSA_ALG_CBC_NO_PADDING);
  status = psa_import_key(&attributes, key, KEY_SIZE, &this->key_id_);
  psa_reset_key_attributes(&attributes);
  if (status != PSA_SUCCESS) {
    ESP_LOGE(TAG, "psa_import_key failed: status=%d", static_cast<int>(status));
    return false;
  }
  this->has_key_ = true;
  return true;
}

bool Aes128CbcDecryptor::decrypt(const uint8_t *iv, const uint8_t *in, uint8_t *out, size_t length) {
  if (!this->has_key_ || length % BLOCK_SIZE != 0) {
    return false;
  }
  psa_cipher_operation_t operation = PSA_CIPHER_OPERATION_INIT;
  size_t output_length = 0;
  size_t finish_length = 0;
  psa_status_t status = psa_cipher_decrypt_setup(&operation, this->key_id_, PSA_ALG_CBC_NO_PADDING);
  if (status == PSA_SUCCESS) {
    status = psa_cipher_set_iv(&operation, iv, BLOCK_SIZE);
  }
  if (status == PSA_SUCCESS) {
    status = psa_cipher_update(&operation, in, length, out, length, &output_length);
  }
  if (status == PSA_SUCCESS) {
    status = psa_cipher_finish(&operation, out + output_length, length - output_length, &finish_length);
  }
  psa_cipher_abort(&operation);
  if (status != PSA_SUCCESS) {
    ESP_LOGW(TAG, "PSA decrypt failed: status=%d", static_cast<int>(status));
    return false;
  }
  return output_length + finish_length == length;
}

#elif defined(MBUS_AES_MBEDTLS)

bool Aes128CbcDecryptor::set_key(const uint8_t *key) {
  mbedtls_aes_init(&this->context_);
  const int result = mbedtls_aes_setkey_dec(&this->context_, key, KEY_SIZE * 8);
  if (result != 0) {
    ESP_LOGE(TAG, "mbedtls_aes_setkey_dec failed: %d", result);
    return false;
  }
  this->has_key_ = true;
  return true;
}

bool Aes128CbcDecryptor::decrypt(const uint8_t *iv, const uint8_t *in, uint8_t *out, size_t length) {
  if (!this->has_key_ || length % BLOCK_SIZE != 0) {
    return false;
  }
  // mbedTLS advances the IV in place
  uint8_t iv_copy[BLOCK_SIZE];
  std::memcpy(iv_copy, iv, BLOCK_SIZE);
  return mbedtls_aes_crypt_cbc(&this->context_, MBEDTLS_AES_DECRYPT, length, iv_copy, in, out) == 0;
}

#else  // MBUS_AES_BEARSSL

bool Aes128CbcDecryptor::set_key(const uint8_t *key) {
  br_aes_ct_cbcdec_init(&this->context_, key, KEY_SIZE);
  this->has_key_ = true;
  return true;
}

bool Aes128CbcDecryptor::decrypt(const uint8_t *iv, const uint8_t *in, uint8_t *out, size_t length) {
  if (!this->has_key_ || length % BLOCK_SIZE != 0) {
    return false;
  }
  // BearSSL decrypts in place and advances the IV
  uint8_t iv_copy[BLOCK_SIZE];
  std::memcpy(iv_copy, iv, BLOCK_SIZE);
  std::memcpy(out, in, length);
  br_aes_ct_cbcdec_run(&this->context_, iv_copy, out, length);
  return true;
}

#endif

}  // namespace esphome::mbus
