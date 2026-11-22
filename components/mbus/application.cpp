#include "mbus.h"
#include "wired_frame_codec.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <algorithm>

namespace esphome::mbus {

ESPHOME_LOG_TAG(TAG, "mbus");

// CI fields of answers (EN 13757-7:2018, Table 2): 7Ah with the short header, 72h with the long header, 70h
// application error, 71h alarm. 73h, 76h and 77h are the fixed data structure and the MSB first variants of
// older editions of the standard; today 73h is the compact frame and 76h and 77h are reserved.
static constexpr uint8_t MBUS_CI_APP_SHORT = 0x7A;
static constexpr uint8_t MBUS_CI_VARIABLE_RESPONSE = 0x72;
static constexpr uint8_t MBUS_CI_VARIABLE_RESPONSE_2 = 0x76;
static constexpr uint8_t MBUS_CI_FIXED_RESPONSE = 0x73;
static constexpr uint8_t MBUS_CI_FIXED_RESPONSE_2 = 0x77;
static constexpr uint8_t MBUS_CI_ERROR = 0x70;
static constexpr uint8_t MBUS_CI_ALARM = 0x71;
// Short header: access number, status, configuration field (EN 13757-7:2018, 7.3). Long header: identification
// number, manufacturer, version and device type in front of that (7.4).
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

// Status byte of the long header (EN 13757-7:2018, 7.5.6, Tables 14 and 15)
static constexpr uint8_t MBUS_STATUS_APPLICATION_MASK = 0x03;
static constexpr uint8_t MBUS_STATUS_POWER_LOW = 0x04;
static constexpr uint8_t MBUS_STATUS_PERMANENT_ERROR = 0x08;
static constexpr uint8_t MBUS_STATUS_TEMPORARY_ERROR = 0x10;
static constexpr uint8_t MBUS_STATUS_MANUFACTURER_MASK = 0xE0;

static const LogString *application_status_to_string_(uint8_t status) {
  switch (status & MBUS_STATUS_APPLICATION_MASK) {
    case 0x01:
      return LOG_STR("application busy");
    case 0x02:
      return LOG_STR("application error");
    case 0x03:
      return LOG_STR("abnormal condition");
    default:
      return LOG_STR("no application error");
  }
}

void MBusComponent::log_status_change_(uint8_t status) {
  if (status == this->last_status_) {
    return;
  }
  this->last_status_ = status;
  if (status == 0) {
    ESP_LOGI(TAG, "Meter status back to 0x00");
    return;
  }
  ESP_LOGW(TAG, "Meter status 0x%02X: %s%s%s%s%s", status, LOG_STR_ARG(application_status_to_string_(status)),
           (status & MBUS_STATUS_POWER_LOW) != 0 ? ", power low" : "",
           (status & MBUS_STATUS_PERMANENT_ERROR) != 0 ? ", permanent error" : "",
           (status & MBUS_STATUS_TEMPORARY_ERROR) != 0 ? ", temporary error" : "",
           (status & MBUS_STATUS_MANUFACTURER_MASK) != 0 ? ", manufacturer specific bits" : "");
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
           frame.ci, access_number, status, config, encryption_mode, encrypted_blocks,
           encrypted ? LOG_STR_LITERAL("yes") : LOG_STR_LITERAL("no"));

  if (encrypted_length > payload_length) {
    ESP_LOGW(TAG, "Encrypted payload is truncated: offset=%u encrypted=%u payload=%u",
             static_cast<unsigned>(payload_offset), static_cast<unsigned>(encrypted_length),
             static_cast<unsigned>(payload_length));
    return;
  }
  if (encrypted_length > 0) {
    ESP_LOGD(TAG, "Encrypted payload: offset=%u bytes=%u", static_cast<unsigned>(payload_offset),
             static_cast<unsigned>(encrypted_length));
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE
    char hex[format_hex_pretty_size(MBUS_LOG_HEX_MAX_BYTES)];
    ESP_LOGV(
        TAG, "Encrypted payload data: %s",
        format_hex_pretty_to(hex, frame.data + payload_offset, std::min(encrypted_length, MBUS_LOG_HEX_MAX_BYTES)));
#endif
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
  // Mode 5 plaintext starts with two bytes 2Fh (EN 13757-7:2018, 9.4.4.2); anything else means a wrong key
  if (decrypted.size() < 2 || decrypted[0] != 0x2F || decrypted[1] != 0x2F) {
    ESP_LOGW(TAG, "Mode 5 decryption failed: no 0x2F2F verification bytes, wrong encryption_key?");
    return;
  }
  ESP_LOGD(TAG, "Decryption: OK, decrypted payload bytes=%u", static_cast<unsigned>(decrypted.size()));
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE
  char hex[format_hex_pretty_size(MBUS_LOG_HEX_MAX_BYTES)];
  ESP_LOGV(TAG, "Decrypted payload: %s",
           format_hex_pretty_to(hex, decrypted.data(), std::min(decrypted.size(), MBUS_LOG_HEX_MAX_BYTES)));
#endif
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

    if (this->has_secondary_address_ &&
        !wired_frame_codec::secondary_address_matches(this->secondary_address_, frame.data)) {
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE
      char hex[format_hex_pretty_size(8)];
      ESP_LOGV(TAG, "Ignoring variable data response for secondary address %s",
               format_hex_pretty_to(hex, frame.data, 8));
#endif
      return;
    }

    const uint16_t manufacturer = encode_uint16(frame.data[5], frame.data[4]);
    char manufacturer_str[4]{};
    this->manufacturer_to_string_(manufacturer, manufacturer_str);
    const uint8_t access_number = frame.data[8];
    const uint8_t status = frame.data[9];
    const uint16_t config = encode_uint16(frame.data[11], frame.data[10]);
    ESP_LOGD(TAG,
             "Variable data: CI=0x%02X ID=%s M=%s Version=0x%02X Medium=0x%02X AccessNr=%u Status=0x%02X "
             "Config=0x%04X",
             frame.ci, bcd_id_to_string_(frame.data).c_str(), manufacturer_str, frame.data[6], frame.data[7],
             access_number, status, config);
    this->log_status_change_(status);
    // A configuration field other than 0 announces encrypted data (EN 13757-7:2018, 7.5.8), which is not
    // decoded behind a long header yet
    if (config != 0x0000) {
      ESP_LOGW(TAG, "Variable data with configuration field 0x%04X (encryption mode %u) not implemented yet", config,
               (config >> 8) & 0x1F);
      return;
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

// Called once from setup(); the plain key is wiped afterwards because the decryptor keeps its own form of it.
void MBusComponent::init_decryptor_() {
  const bool ok = this->decryptor_.set_key(this->encryption_key_.data());
  this->encryption_key_.fill(0);
  if (!ok) {
    this->mark_failed(LOG_STR("Encryption key setup failed"));
  }
}

bool MBusComponent::decrypt_mode_5_(const MBusApplicationFrame &frame, size_t payload_offset, size_t encrypted_length,
                                    uint8_t access_number, std::vector<uint8_t> *out) {
  if (!frame.has_meter_address || frame.data == nullptr || payload_offset + encrypted_length > frame.length) {
    return false;
  }

  // Mode 5 IV: manufacturer, identification number, version and device type, then the access number 8 times
  // (EN 13757-7:2018, Table 48). With the short header these come from the link layer.
  uint8_t iv[Aes128CbcDecryptor::BLOCK_SIZE];
  std::copy_n(frame.meter_address.data(), frame.meter_address.size(), iv);
  std::fill_n(iv + 8, 8, access_number);
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE
  char hex[format_hex_pretty_size(sizeof(iv))];
  ESP_LOGV(TAG, "Mode 5 IV: %s", format_hex_pretty_to(hex, iv, sizeof(iv)));
#endif

  out->assign(encrypted_length, 0);
  return this->decryptor_.decrypt(iv, frame.data + payload_offset, out->data(), encrypted_length);
}

}  // namespace esphome::mbus
