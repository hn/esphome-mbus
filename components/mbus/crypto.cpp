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

Aes128CbcDecryptor::~Aes128CbcDecryptor() {
  if (this->has_key_) {
    psa_destroy_key(this->key_id_);
  }
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

bool Aes128Cmac::set_key(const uint8_t *key) {
  psa_status_t status = psa_crypto_init();
  if (status != PSA_SUCCESS) {
    ESP_LOGE(TAG, "psa_crypto_init failed: status=%d", static_cast<int>(status));
    return false;
  }
  psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
  psa_set_key_bits(&attributes, KEY_SIZE * 8);
  psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT);
  psa_set_key_algorithm(&attributes, PSA_ALG_ECB_NO_PADDING);
  status = psa_import_key(&attributes, key, KEY_SIZE, &this->key_id_);
  psa_reset_key_attributes(&attributes);
  if (status != PSA_SUCCESS) {
    ESP_LOGE(TAG, "psa_import_key failed: status=%d", static_cast<int>(status));
    return false;
  }
  this->has_key_ = true;
  return true;
}

Aes128Cmac::~Aes128Cmac() {
  if (this->has_key_) {
    psa_destroy_key(this->key_id_);
  }
}

bool Aes128Cmac::encrypt_block_(uint8_t *block) {
  uint8_t out[BLOCK_SIZE];
  size_t output_length = 0;
  const psa_status_t status =
      psa_cipher_encrypt(this->key_id_, PSA_ALG_ECB_NO_PADDING, block, BLOCK_SIZE, out, sizeof(out), &output_length);
  if (status != PSA_SUCCESS || output_length != BLOCK_SIZE) {
    ESP_LOGW(TAG, "PSA encrypt failed: status=%d", static_cast<int>(status));
    return false;
  }
  std::memcpy(block, out, BLOCK_SIZE);
  return true;
}

#elif defined(MBUS_AES_BEARSSL)

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

bool Aes128Cmac::set_key(const uint8_t *key) {
  br_aes_ct_cbcenc_init(&this->context_, key, KEY_SIZE);
  this->has_key_ = true;
  return true;
}

bool Aes128Cmac::encrypt_block_(uint8_t *block) {
  // CBC over one block with an IV of zero is the plain block encryption
  uint8_t iv[BLOCK_SIZE]{};
  br_aes_ct_cbcenc_run(&this->context_, iv, block, BLOCK_SIZE);
  return true;
}

#else  // MBUS_AES_NONE

bool Aes128CbcDecryptor::set_key(const uint8_t *key) { return false; }

bool Aes128CbcDecryptor::decrypt(const uint8_t *iv, const uint8_t *in, uint8_t *out, size_t length) { return false; }

bool Aes128Cmac::set_key(const uint8_t *key) { return false; }

bool Aes128Cmac::encrypt_block_(uint8_t *block) { return false; }

#endif

// Doubling in GF(2^128) for the subkeys K1 and K2 (NIST SP 800-38B, 6.1)
static void cmac_double(uint8_t *block) {
  const uint8_t carry = block[0] >> 7;
  for (size_t i = 0; i + 1 < Aes128Cmac::BLOCK_SIZE; i++) {
    block[i] = static_cast<uint8_t>(block[i] << 1 | block[i + 1] >> 7);
  }
  block[Aes128Cmac::BLOCK_SIZE - 1] = static_cast<uint8_t>(block[Aes128Cmac::BLOCK_SIZE - 1] << 1 ^ (carry * 0x87));
}

bool Aes128Cmac::compute(const uint8_t *head, size_t head_length, const uint8_t *data, size_t data_length,
                         uint8_t *mac) {
  if (!this->has_key_) {
    return false;
  }
  const size_t length = head_length + data_length;
  // A complete last block takes K1, an incomplete one is padded with 80h 00h ... and takes K2
  const bool complete = length != 0 && length % BLOCK_SIZE == 0;
  const size_t last = complete ? length - BLOCK_SIZE : length - length % BLOCK_SIZE;
  uint8_t subkey[BLOCK_SIZE]{};
  if (!this->encrypt_block_(subkey)) {
    return false;
  }
  cmac_double(subkey);
  if (!complete) {
    cmac_double(subkey);
  }
  uint8_t state[BLOCK_SIZE]{};
  for (size_t offset = 0; offset <= last; offset += BLOCK_SIZE) {
    for (size_t i = 0; i < BLOCK_SIZE; i++) {
      const size_t position = offset + i;
      if (position < length) {
        state[i] ^= position < head_length ? head[position] : data[position - head_length];
      } else if (position == length) {
        state[i] ^= 0x80;
      }
      if (offset == last) {
        state[i] ^= subkey[i];
      }
    }
    if (!this->encrypt_block_(state)) {
      return false;
    }
  }
  std::memcpy(mac, state, BLOCK_SIZE);
  return true;
}

}  // namespace esphome::mbus
