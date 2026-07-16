#include "mbus.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <algorithm>

namespace esphome::mbus {

static const char *const TAG = "mbus";

static constexpr uint8_t MBUS_CI_APP_SHORT = 0x7A;
static constexpr uint8_t MBUS_CI_VARIABLE_RESPONSE = 0x72;
static constexpr uint8_t MBUS_CI_VARIABLE_RESPONSE_2 = 0x76;
static constexpr uint8_t MBUS_CI_FIXED_RESPONSE = 0x73;
static constexpr uint8_t MBUS_CI_FIXED_RESPONSE_2 = 0x77;
static constexpr uint8_t MBUS_CI_ERROR = 0x70;
static constexpr uint8_t MBUS_CI_ALARM = 0x71;
static constexpr size_t MBUS_APP_SHORT_HEADER_LENGTH = 4;
static constexpr size_t MBUS_VARIABLE_DATA_HEADER_LENGTH = 12;

static std::string bcd_id_to_string_(const uint8_t *data) {
  char out[9]{};
  for (size_t i = 0; i < 4; i++) {
    const uint8_t byte = data[3 - i];
    out[i * 2] = static_cast<char>('0' + ((byte >> 4) & 0x0F));
    out[i * 2 + 1] = static_cast<char>('0' + (byte & 0x0F));
  }
  return out;
}

static std::array<uint8_t, 8> secondary_address_bytes_(uint64_t secondary_address) {
  return {static_cast<uint8_t>(secondary_address >> 32), static_cast<uint8_t>(secondary_address >> 40),
          static_cast<uint8_t>(secondary_address >> 48), static_cast<uint8_t>(secondary_address >> 56),
          static_cast<uint8_t>(secondary_address >> 24), static_cast<uint8_t>(secondary_address >> 16),
          static_cast<uint8_t>(secondary_address >> 8),  static_cast<uint8_t>(secondary_address)};
}

void MBusComponent::handle_short_application_frame_(const MBusApplicationFrame &frame) {
  if (frame.data == nullptr || frame.length < MBUS_APP_SHORT_HEADER_LENGTH) {
    ESP_LOGW(TAG, "Short application header truncated: bytes=%u", static_cast<unsigned>(frame.length));
    return;
  }

  const uint8_t access_number = frame.data[0];
  const uint8_t status = frame.data[1];
  const uint16_t config = encode_uint16(frame.data[2], frame.data[3]);
  const uint8_t encryption_mode = config & 0x1F;
  const uint8_t encrypted_blocks = (frame.data[2] >> 4) & 0x0F;
  const size_t payload_offset = MBUS_APP_SHORT_HEADER_LENGTH;
  const size_t payload_length = frame.length - payload_offset;
  const size_t encrypted_length = encrypted_blocks * 16;
  const bool encrypted = encryption_mode != 0;

  ESP_LOGD(TAG, "AppHeader: CI=0x%02X AccessNr=%u Status=0x%02X Config=0x%04X EncMode=%u EncBlocks=%u Encrypted=%s",
           frame.ci, access_number, status, config, encryption_mode, encrypted_blocks, encrypted ? "yes" : "no");

  if (encrypted_length > payload_length) {
    ESP_LOGW(TAG, "Encrypted payload is truncated: offset=%u encrypted=%u payload=%u",
             static_cast<unsigned>(payload_offset), static_cast<unsigned>(encrypted_length),
             static_cast<unsigned>(payload_length));
    return;
  }
  if (encrypted_length > 0) {
    ESP_LOGD(TAG, "Encrypted payload: offset=%u bytes=%u", static_cast<unsigned>(payload_offset),
             static_cast<unsigned>(encrypted_length));
    ESP_LOGV(TAG, "Encrypted payload data: %s",
             format_hex_pretty(frame.data + payload_offset, encrypted_length).c_str());
  }
  if (encryption_mode == 0) {
    std::vector<uint8_t> payload(frame.data + payload_offset, frame.data + frame.length);
    this->process_records_(payload);
    return;
  }
  if (encryption_mode != 5) {
    ESP_LOGW(TAG, "Application encryption mode %u not implemented yet", encryption_mode);
    return;
  }
  if (encrypted_length == 0) {
    ESP_LOGW(TAG, "Application encryption mode 5 without encrypted blocks not implemented yet");
    return;
  }
  if (!this->has_encryption_key_) {
    ESP_LOGD(TAG, "Encrypted payload present, no encryption_key configured");
    return;
  }

  std::vector<uint8_t> decrypted;
  if (!this->decrypt_mode_5_(frame, payload_offset, encrypted_length, access_number, &decrypted)) {
    ESP_LOGW(TAG, "Mode 5 decryption failed");
    return;
  }
  ESP_LOGD(TAG, "Decryption: OK, decrypted payload bytes=%u", static_cast<unsigned>(decrypted.size()));
  ESP_LOGV(TAG, "Decrypted payload: %s", format_hex_pretty(decrypted).c_str());
  this->process_records_(decrypted);
}

void MBusComponent::handle_application_frame_(const MBusApplicationFrame &frame) {
  if (frame.ci == MBUS_CI_APP_SHORT) {
    this->handle_short_application_frame_(frame);
    return;
  }

  if (frame.data == nullptr || frame.length == 0) {
    ESP_LOGD(TAG, "No application payload present for CI=0x%02X", frame.ci);
    return;
  }

  if (frame.ci == MBUS_CI_VARIABLE_RESPONSE || frame.ci == MBUS_CI_VARIABLE_RESPONSE_2) {
    if (frame.length < MBUS_VARIABLE_DATA_HEADER_LENGTH) {
      ESP_LOGW(TAG, "Variable data response header truncated: bytes=%u", static_cast<unsigned>(frame.length));
      return;
    }

    if (this->has_secondary_address_) {
      const std::array<uint8_t, 8> expected = secondary_address_bytes_(this->secondary_address_);
      if (!std::equal(expected.begin(), expected.end(), frame.data)) {
        ESP_LOGV(TAG, "Ignoring variable data response for secondary address %s",
                 format_hex_pretty(frame.data, 8).c_str());
        return;
      }
    }

    const uint16_t manufacturer = encode_uint16(frame.data[5], frame.data[4]);
    char manufacturer_str[4]{};
    this->manufacturer_to_string_(manufacturer, manufacturer_str);
    const uint8_t access_number = frame.data[8];
    const uint8_t status = frame.data[9];
    const uint16_t signature = encode_uint16(frame.data[11], frame.data[10]);
    ESP_LOGD(TAG,
             "Variable data: CI=0x%02X ID=%s M=%s Version=0x%02X Medium=0x%02X AccessNr=%u Status=0x%02X "
             "Signature=0x%04X",
             frame.ci, bcd_id_to_string_(frame.data).c_str(), manufacturer_str, frame.data[6], frame.data[7],
             access_number, status, signature);
    if (signature != 0x0000) {
      ESP_LOGW(TAG, "Variable data signature/encryption field 0x%04X not implemented yet", signature);
    }

    std::vector<uint8_t> payload(frame.data + MBUS_VARIABLE_DATA_HEADER_LENGTH, frame.data + frame.length);
    this->process_records_(payload);
    return;
  }
  if (frame.ci == MBUS_CI_FIXED_RESPONSE || frame.ci == MBUS_CI_FIXED_RESPONSE_2) {
    ESP_LOGW(TAG, "Fixed data response CI=0x%02X not implemented yet", frame.ci);
    return;
  }
  if (frame.ci == MBUS_CI_ERROR) {
    ESP_LOGW(TAG, "Application error response CI=0x70 code=%u not implemented yet",
             static_cast<unsigned>(frame.data[0]));
    return;
  }
  if (frame.ci == MBUS_CI_ALARM) {
    ESP_LOGW(TAG, "Alarm/status response CI=0x71 code=%u not implemented yet", static_cast<unsigned>(frame.data[0]));
    return;
  }

  ESP_LOGW(TAG, "Application CI=0x%02X not implemented yet", frame.ci);
}

}  // namespace esphome::mbus
