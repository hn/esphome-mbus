#include "mbus.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <algorithm>

namespace esphome::mbus {

static const char *const TAG = "mbus";

// Imports encryption_key_ into PSA once, from setup(). Every decrypt_mode_5_() call afterwards reuses
// the cached key handle instead of importing/destroying a PSA key for every received telegram.
void MBusComponent::import_psa_key_() {
  psa_status_t status = psa_crypto_init();
  if (status != PSA_SUCCESS) {
    ESP_LOGW(TAG, "Mode 5 decrypt psa_crypto_init failed: status=%d", static_cast<int>(status));
    return;
  }

  psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
  psa_set_key_bits(&attributes, 128);
  psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DECRYPT);
  psa_set_key_algorithm(&attributes, PSA_ALG_CBC_NO_PADDING);

  status = psa_import_key(&attributes, this->encryption_key_.data(), this->encryption_key_.size(), &this->psa_key_id_);
  psa_reset_key_attributes(&attributes);
  if (status != PSA_SUCCESS) {
    ESP_LOGW(TAG, "Mode 5 decrypt psa_import_key failed: status=%d", static_cast<int>(status));
    return;
  }
  this->has_psa_key_ = true;
}

bool MBusComponent::decrypt_mode_5_(const MBusApplicationFrame &frame, size_t payload_offset, size_t encrypted_length,
                                    uint8_t access_number, std::vector<uint8_t> *out) const {
  if (!this->has_psa_key_ || !frame.has_meter_address || frame.data == nullptr ||
      payload_offset + encrypted_length > frame.length || encrypted_length % 16 != 0) {
    return false;
  }

  uint8_t iv[16];
  std::copy_n(frame.meter_address.data(), frame.meter_address.size(), iv);
  std::fill_n(iv + 8, 8, access_number);
  ESP_LOGV(TAG, "Mode 5 IV: %s", format_hex_pretty(iv, sizeof(iv)).c_str());

  std::vector<uint8_t> ciphertext(frame.data + payload_offset, frame.data + payload_offset + encrypted_length);
  out->assign(encrypted_length, 0);

  psa_cipher_operation_t operation = PSA_CIPHER_OPERATION_INIT;
  psa_status_t status = psa_cipher_decrypt_setup(&operation, this->psa_key_id_, PSA_ALG_CBC_NO_PADDING);
  if (status != PSA_SUCCESS) {
    ESP_LOGW(TAG, "Mode 5 decrypt psa_cipher_decrypt_setup failed: status=%d", static_cast<int>(status));
  } else {
    ESP_LOGVV(TAG, "Mode 5 decrypt psa_cipher_decrypt_setup status=%d", static_cast<int>(status));
  }
  if (status == PSA_SUCCESS) {
    status = psa_cipher_set_iv(&operation, iv, sizeof(iv));
    if (status != PSA_SUCCESS) {
      ESP_LOGW(TAG, "Mode 5 decrypt psa_cipher_set_iv failed: status=%d", static_cast<int>(status));
    } else {
      ESP_LOGVV(TAG, "Mode 5 decrypt psa_cipher_set_iv status=%d", static_cast<int>(status));
    }
  }

  size_t output_length = 0;
  size_t finish_length = 0;
  if (status == PSA_SUCCESS) {
    status =
        psa_cipher_update(&operation, ciphertext.data(), ciphertext.size(), out->data(), out->size(), &output_length);
    if (status != PSA_SUCCESS) {
      ESP_LOGW(TAG, "Mode 5 decrypt psa_cipher_update failed: status=%d output=%u", static_cast<int>(status),
               static_cast<unsigned>(output_length));
    } else {
      ESP_LOGVV(TAG, "Mode 5 decrypt psa_cipher_update status=%d output=%u", static_cast<int>(status),
                static_cast<unsigned>(output_length));
    }
  }
  if (status == PSA_SUCCESS) {
    status = psa_cipher_finish(&operation, out->data() + output_length, out->size() - output_length, &finish_length);
    if (status != PSA_SUCCESS) {
      ESP_LOGW(TAG, "Mode 5 decrypt psa_cipher_finish failed: status=%d output=%u", static_cast<int>(status),
               static_cast<unsigned>(finish_length));
    } else {
      ESP_LOGVV(TAG, "Mode 5 decrypt psa_cipher_finish status=%d output=%u", static_cast<int>(status),
                static_cast<unsigned>(finish_length));
    }
  }

  psa_cipher_abort(&operation);
  ESP_LOGVV(TAG, "Mode 5 decrypt final status=%d output=%u expected=%u", static_cast<int>(status),
            static_cast<unsigned>(output_length + finish_length), static_cast<unsigned>(out->size()));
  return status == PSA_SUCCESS && output_length + finish_length == out->size();
}

}  // namespace esphome::mbus
