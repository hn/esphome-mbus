#include "mbus.h"
#include "security_codec.h"
#include "wired_frame_codec.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <algorithm>

namespace esphome::mbus {

ESPHOME_LOG_TAG(TAG, "mbus");

// CI fields of answers (EN 13757-7:2018, Table 2): 7Ah with the short header, 72h with the long header, 70h
// application error, 71h alarm. 73h and 77h are the fixed data structure of older editions of the standard, 76h
// and 77h their variants with the most significant byte first; today 73h is the compact frame and 76h and 77h
// are reserved, so 76h is not read.
static constexpr uint8_t MBUS_CI_APP_SHORT = 0x7A;
static constexpr uint8_t MBUS_CI_APP_NO_HEADER = 0x78;
static constexpr uint8_t MBUS_CI_VARIABLE_RESPONSE = 0x72;
static constexpr uint8_t MBUS_CI_COMPACT_LONG = 0x73;
static constexpr uint8_t MBUS_CI_COMPACT_NO_HEADER = 0x79;
static constexpr uint8_t MBUS_CI_COMPACT_SHORT = 0x7B;
static constexpr uint8_t MBUS_CI_ALARM = 0x71;
// Short header: access number, status, configuration field (EN 13757-7:2018, 7.3). Long header: identification
// number, manufacturer, version and device type in front of that (7.4).
static constexpr size_t MBUS_APP_SHORT_HEADER_LENGTH = 4;
static constexpr size_t MBUS_VARIABLE_DATA_HEADER_LENGTH = 12;
// Extended Link Layer of radio datagrams (EN 13757-4:2019, 13.2.2 to 13.2.5): 8Ch with CC and ACC, 8Dh with a
// session number and a payload CRC behind them, 8Eh with a destination address, 8Fh with all of these. The top
// three bits of the session number tell whether the rest of the datagram is encrypted at this layer (Table 53).
static constexpr uint8_t MBUS_CI_ELL_FIRST = 0x8C;
static constexpr uint8_t MBUS_CI_ELL_LAST = 0x8F;
static constexpr uint8_t MBUS_ELL_LENGTH[] = {2, 8, 10, 16};
// Application protocols of the manufacturer (EN 13757-3:2018, 13)
static constexpr uint8_t MBUS_CI_MANUFACTURER_FIRST = 0xA0;
static constexpr uint8_t MBUS_CI_MANUFACTURER_LAST = 0xB7;
// Security modes of the configuration field (EN 13757-7:2018, Table 19)
static constexpr uint8_t MBUS_SECURITY_MODE_5 = 5;
static constexpr uint8_t MBUS_SECURITY_MODE_7 = 7;
// Configuration field of mode 7 (Table 33): TPL padding and a message counter in the header. Its extension
// (Table 34): key derivation function in bits 4 and 5 (Table 25), bit 6 announces a key version byte.
static constexpr uint16_t MBUS_MODE_7_PADDING = 0x0008;
static constexpr uint16_t MBUS_MODE_7_COUNTER = 0x2000;
static constexpr uint8_t MBUS_MODE_7_KEY_VERSION = 0x40;

// Identification number, most significant digit first. The digits are BCD; a wildcard or a faulty digit shows
// as its hex value.
static constexpr size_t BCD_ID_SIZE = 9;
static void bcd_id_to_str(const uint8_t *data, char *out) {
  for (size_t i = 0; i < 4; i++) {
    const uint8_t byte = data[3 - i];
    out[i * 2] = format_hex_pretty_char(byte >> 4);
    out[i * 2 + 1] = format_hex_pretty_char(byte & 0x0F);
  }
  out[BCD_ID_SIZE - 1] = '\0';
}

// First byte of a general application error (EN 13757-3:2018, Table 21)
static const LogString *application_error_to_str(uint8_t code) {
  switch (code) {
    case 0x00:
      return LOG_STR("unspecified error");
    case 0x01:
      return LOG_STR("unimplemented CI field");
    case 0x02:
      return LOG_STR("buffer too long");
    case 0x03:
      return LOG_STR("too many records");
    case 0x04:
      return LOG_STR("premature end of record");
    case 0x05:
      return LOG_STR("more than 10 DIFEs");
    case 0x06:
      return LOG_STR("more than 10 VIFEs");
    case 0x08:
      return LOG_STR("application busy");
    case 0x09:
      return LOG_STR("too many readouts");
    case 0x11:
      return LOG_STR("function not implemented");
    case 0x12:
      return LOG_STR("data not available");
    case 0x13:
      return LOG_STR("routing error");
    case 0x14:
      return LOG_STR("access violation");
    case 0x15:
      return LOG_STR("parameter missing or wrong");
    case 0x16:
      return LOG_STR("size error");
    case 0x20:
      return LOG_STR("security error");
    case 0x21:
      return LOG_STR("security mechanism not supported");
    case 0x22:
      return LOG_STR("inadequate security method");
    default:
      return code >= 0xF0 ? LOG_STR("manufacturer specific") : LOG_STR("reserved");
  }
}

static constexpr uint8_t MBUS_KEY_PROBLEM_NO_KEY = 0x01;
static constexpr uint8_t MBUS_KEY_PROBLEM_VERIFICATION = 0x02;
static constexpr uint8_t MBUS_KEY_PROBLEM_MAC = 0x04;
static constexpr uint8_t MBUS_KEY_PROBLEM_LINK_LAYER = 0x08;
static constexpr uint8_t MBUS_KEY_PROBLEM_UNPROTECTED = 0x10;

// Status byte of the long header (EN 13757-7:2018, 7.5.6, Tables 14 and 15)
static constexpr uint8_t MBUS_STATUS_APPLICATION_MASK = 0x03;
static constexpr uint8_t MBUS_STATUS_POWER_LOW = 0x04;
static constexpr uint8_t MBUS_STATUS_PERMANENT_ERROR = 0x08;
static constexpr uint8_t MBUS_STATUS_TEMPORARY_ERROR = 0x10;
static constexpr uint8_t MBUS_STATUS_MANUFACTURER_MASK = 0xE0;

static const LogString *application_status_to_str(uint8_t status) {
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
  ESP_LOGW(TAG, "Meter status 0x%02X: %s%s%s%s%s", status, LOG_STR_ARG(application_status_to_str(status)),
           (status & MBUS_STATUS_POWER_LOW) != 0 ? LOG_STR_LITERAL(", power low") : LOG_STR_LITERAL(""),
           (status & MBUS_STATUS_PERMANENT_ERROR) != 0 ? LOG_STR_LITERAL(", permanent error") : LOG_STR_LITERAL(""),
           (status & MBUS_STATUS_TEMPORARY_ERROR) != 0 ? LOG_STR_LITERAL(", temporary error") : LOG_STR_LITERAL(""),
           (status & MBUS_STATUS_MANUFACTURER_MASK) != 0 ? LOG_STR_LITERAL(", manufacturer specific bits")
                                                         : LOG_STR_LITERAL(""));
}

void MBusComponent::log_key_problem_(const MBusApplicationFrame &frame, uint8_t kind, const char *message) {
  // A wired datagram has no link layer address; its meter is the one of the instance
  const uint32_t meter = frame.has_meter_address ? encode_uint32(frame.meter_address[5], frame.meter_address[4],
                                                                 frame.meter_address[3], frame.meter_address[2])
                                                 : 0;
  if (meter != this->key_problem_meter_) {
    this->key_problem_meter_ = meter;
    this->key_problems_ = 0;
  }
  const bool first = (this->key_problems_ & kind) == 0;
  this->key_problems_ |= kind;
  if (!frame.has_meter_address) {
    if (first) {
      ESP_LOGW(TAG, "%s", message);
    } else {
      ESP_LOGD(TAG, "%s", message);
    }
  } else if (first) {
    ESP_LOGW(TAG, "%s (meter ID 0x%08X)", message, static_cast<unsigned>(meter));
  } else {
    ESP_LOGD(TAG, "%s (meter ID 0x%08X)", message, static_cast<unsigned>(meter));
  }
}

// Whoever configures a key expects the data of this meter to be protected. Anything else could be sent by
// anybody in range, so it is not read; an encrypted Extended Link Layer in front counts as protection.
bool MBusComponent::reject_unprotected_(const MBusApplicationFrame &frame) {
  if (!this->has_encryption_key_ || frame.link_layer_decrypted) {
    return false;
  }
  this->log_key_problem_(frame, MBUS_KEY_PROBLEM_UNPROTECTED,
                         "Unencrypted datagram ignored: an encryption_key is configured");
  return true;
}

// The part both headers share: access number, status and configuration field, then the data, which the
// configuration field may declare encrypted. header_offset is 0 for the short header and the length of the address
// for the long one; frame.meter_address is the address that goes into the IV and the key derivation.
void MBusComponent::handle_transport_header_(const MBusApplicationFrame &frame, size_t header_offset,
                                             MBusApplicationResult *out) {
  if (frame.data == nullptr || frame.length < header_offset + MBUS_APP_SHORT_HEADER_LENGTH) {
    ESP_LOGW(TAG, "Short application header truncated: bytes=%u", static_cast<unsigned>(frame.length));
    return;
  }

  const uint8_t *header = frame.data + header_offset;
  const uint8_t access_number = header[0];
  const uint8_t status = header[1];
  // Configuration field, sent LSB first: security mode in bits 8 to 12; for the modes 5 and 7 the number of
  // encrypted blocks in bits 4 to 7 (EN 13757-7:2018, Tables 19, 31 and 33). The other modes use these bits
  // differently, mode 0 for the content of the message (Table 27).
  const uint16_t config = encode_uint16(header[3], header[2]);
  const uint8_t encryption_mode = (config >> 8) & 0x1F;
  const bool block_mode = encryption_mode == MBUS_SECURITY_MODE_5 || encryption_mode == MBUS_SECURITY_MODE_7;
  const uint8_t encrypted_blocks = block_mode ? (config >> 4) & 0x0F : 0;
  size_t payload_offset = header_offset + MBUS_APP_SHORT_HEADER_LENGTH;
  uint8_t key_derivation = security_codec::KEY_DERIVATION_NONE;
  const uint8_t *counter = frame.has_afl && frame.afl.has_counter ? frame.afl.counter : nullptr;
  // With a security mode the data counts as not decrypted until it is decrypted and verified
  if (encryption_mode != 0) {
    out->result = MBusResult::MBUS_RESULT_SECURITY_ERROR;
  }
  if (encryption_mode == MBUS_SECURITY_MODE_7) {
    // Configuration field extension, then a key version and a message counter if announced (Table 32). A
    // counter in this header goes before the one of the AFL (9.4.5.1).
    size_t offset = payload_offset + 1;
    if (offset <= frame.length) {
      const uint8_t extension = frame.data[payload_offset];
      key_derivation = (extension >> 4) & 0x03;
      if ((extension & MBUS_MODE_7_KEY_VERSION) != 0) {
        offset++;
      }
      if ((config & MBUS_MODE_7_COUNTER) != 0) {
        counter = frame.data + offset;
        offset += afl_codec::COUNTER_SIZE;
      }
    }
    if (offset > frame.length) {
      ESP_LOGW(TAG, "Mode 7 header truncated: bytes=%u", static_cast<unsigned>(frame.length));
      return;
    }
    payload_offset = offset;
  }
  const size_t payload_length = frame.length - payload_offset;
  // All bits of the number set: no partial encryption, everything behind the header is encrypted (7.6.5)
  static constexpr uint8_t ALL_BLOCKS = 0x0F;
  const size_t encrypted_length = encrypted_blocks == ALL_BLOCKS ? payload_length : encrypted_blocks * 16;
  this->log_status_change_(status);
  out->data = std::span<const uint8_t>(frame.data + payload_offset, payload_length);

  // The long header has its own line with these fields; it gets this one only if a security mode is in use
  if (header_offset == 0 || encryption_mode != 0) {
    ESP_LOGD(TAG, "Header: CI=0x%02X AccessNr=%u Status=0x%02X Config=0x%04X Mode=%u EncryptedBytes=%u", frame.ci,
             access_number, status, config, encryption_mode, static_cast<unsigned>(encrypted_length));
  }

  if (encrypted_length > payload_length || encrypted_length % Aes128CbcDecryptor::BLOCK_SIZE != 0) {
    ESP_LOGW(TAG, "Encrypted payload is truncated: offset=%u encrypted=%u payload=%u",
             static_cast<unsigned>(payload_offset), static_cast<unsigned>(encrypted_length),
             static_cast<unsigned>(payload_length));
    return;
  }
  if (encrypted_length > 0) {
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE
    char hex[format_hex_pretty_size(MBUS_LOG_HEX_MAX_BYTES)];
    ESP_LOGV(
        TAG, "Encrypted payload data: %s",
        format_hex_pretty_to(hex, frame.data + payload_offset, std::min(encrypted_length, MBUS_LOG_HEX_MAX_BYTES)));
#endif
  }
  if (encryption_mode == 0) {
    if (this->reject_unprotected_(frame)) {
      out->result = MBusResult::MBUS_RESULT_SECURITY_ERROR;
      return;
    }
    if (this->process_application_data_(out->data, frame.ci, 0)) {
      out->result = MBusResult::MBUS_RESULT_OK;
    }
    return;
  }
  if (!block_mode) {
    ESP_LOGW(TAG, "Security mode %u not implemented yet", encryption_mode);
    return;
  }
  // Without encrypted blocks the data is sent in the clear and, in mode 7, still authenticated (7.6.5)
  const bool encrypted = encrypted_length > 0;
  if (encrypted && !this->has_encryption_key_) {
    this->log_key_problem_(frame, MBUS_KEY_PROBLEM_NO_KEY, "Encrypted payload present, no encryption_key configured");
    return;
  }

  std::vector<uint8_t> decrypted;
  // Mode 5 has no authentication; mode 7 has the MAC of the AFL, if the datagram carries one
  const char *authentication = "";
  if (encryption_mode == MBUS_SECURITY_MODE_5) {
    if (encrypted && !this->decrypt_mode_5_(frame, payload_offset, encrypted_length, access_number, &decrypted)) {
      ESP_LOGW(TAG, "Mode 5 decryption failed");
      return;
    }
  } else if (this->has_encryption_key_) {
    bool authenticated = false;
    if (!this->decrypt_mode_7_(frame, payload_offset, encrypted_length, key_derivation, counter, &decrypted,
                               &authenticated)) {
      return;
    }
    authentication = authenticated ? "MAC ok, " : "no MAC, ";
    // Not encrypted and not authenticated either
    if (!encrypted && !authenticated && this->reject_unprotected_(frame)) {
      return;
    }
  } else {
    authentication = "MAC not checked without encryption_key, ";
  }
  // Mode 5 has no authentication, so without an encrypted block nothing protects the data
  if (!encrypted && encryption_mode == MBUS_SECURITY_MODE_5 && this->reject_unprotected_(frame)) {
    return;
  }
  // The plaintext of the modes 5 and 7 starts with two bytes 2Fh (EN 13757-7:2018, 9.4.4.2 and 9.4.5.2);
  // anything else means a wrong key
  if (encrypted && (decrypted.size() < 2 || decrypted[0] != 0x2F || decrypted[1] != 0x2F)) {
    this->log_key_problem_(frame, MBUS_KEY_PROBLEM_VERIFICATION,
                           encryption_mode == MBUS_SECURITY_MODE_5
                               ? "Mode 5 decryption failed: no 0x2F2F verification bytes, wrong encryption_key?"
                               : "Mode 7 decryption failed: no 0x2F2F verification bytes, wrong encryption_key?");
    return;
  }
  if (encrypted && encryption_mode == MBUS_SECURITY_MODE_7 && (config & MBUS_MODE_7_PADDING) != 0) {
    // TPL padding: 1 to 16 bytes at the end, each holding their number (9.4.5.4)
    const uint8_t padding = decrypted.back();
    if (padding == 0 || padding > Aes128CbcDecryptor::BLOCK_SIZE || padding > decrypted.size() - 2) {
      ESP_LOGW(TAG, "Mode 7 decryption failed: TPL padding of %u bytes", padding);
      return;
    }
    decrypted.resize(decrypted.size() - padding);
  }
  // Partial encryption: the data behind the encrypted blocks is not encrypted (7.6.5). In mode 7 the MAC
  // covers it as well.
  const size_t plain_length = frame.length - payload_offset - encrypted_length;
  decrypted.insert(decrypted.end(), frame.data + payload_offset + encrypted_length, frame.data + frame.length);
  out->plain = std::move(decrypted);
  out->data = out->plain;
  out->result = MBusResult::MBUS_RESULT_NOT_DECODED;
  this->key_problems_ = 0;
  ESP_LOGD(TAG, "Security: mode %u, %s%s", encryption_mode, authentication,
           encrypted ? LOG_STR_LITERAL("decryption ok") : LOG_STR_LITERAL("not encrypted"));
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE
  char hex[format_hex_pretty_size(MBUS_LOG_HEX_MAX_BYTES)];
  ESP_LOGV(TAG, "Decrypted payload: %s",
           format_hex_pretty_to(hex, out->plain.data(), std::min(out->plain.size(), MBUS_LOG_HEX_MAX_BYTES)));
#endif
  if (encrypted && plain_length > 0 && wired_frame_codec::is_compact_frame(frame.ci)) {
    // The fillers of the encrypted part cannot be told from data once both parts are joined
    ESP_LOGW(TAG, "Partially encrypted compact frame (CI=0x%02X) not implemented yet", frame.ci);
    return;
  }
  if (this->process_application_data_(out->plain, frame.ci, encrypted ? 2 : 0)) {
    out->result = MBusResult::MBUS_RESULT_OK;
  }
}

// Compact frames are read by instances that keep the formats of their meter (EN 13757-3:2018, Annex G)
bool MBusComponent::reads_compact_frames_() const {
#ifdef USE_MBUS_COMPACT_FRAMES
  return this->compact_formats_ != nullptr;
#else
  return false;
#endif
}

bool MBusComponent::process_application_data_(std::span<const uint8_t> payload, uint8_t ci, size_t check_bytes) {
#ifdef USE_MBUS_COMPACT_FRAMES
  // A caller that takes the data as it is gets a compact frame as sent
  if (this->compact_formats_ != nullptr && this->records_wanted_()) {
    if (wired_frame_codec::is_compact_frame(ci)) {
      return this->process_compact_frame_(payload.data() + check_bytes, payload.size() - check_bytes);
    }
    this->learn_compact_format_(payload);
  }
#endif
  return this->process_records_(payload);
}

#ifdef USE_MBUS_COMPACT_FRAMES
// Format signature and full frame CRC, then the data of the records without their DIF/VIF bytes
bool MBusComponent::process_compact_frame_(const uint8_t *data, size_t length) {
  if (length < compact_frame_codec::COMPACT_HEADER_SIZE) {
    ESP_LOGW(TAG, "Compact frame truncated: bytes=%u", static_cast<unsigned>(length));
    return false;
  }
  const uint16_t signature = encode_uint16(data[1], data[0]);
  const uint16_t crc = encode_uint16(data[3], data[2]);
  const compact_frame_codec::FormatStore::Format *format = this->compact_formats_->find(signature);
  if (format == nullptr) {
    ESP_LOGD(TAG, "Compact frame: format 0x%04X not known yet, waiting for a full frame", signature);
    return false;
  }
  std::vector<uint8_t> records;
  const compact_frame_codec::ExpandResult result =
      compact_frame_codec::expand(format->bytes.data(), format->length, data + compact_frame_codec::COMPACT_HEADER_SIZE,
                                  length - compact_frame_codec::COMPACT_HEADER_SIZE, &records);
  if (result != compact_frame_codec::ExpandResult::EXPAND_OK || compact_frame_codec::full_frame_crc(records) != crc) {
    // The meter has changed its records since the full frame the format is from
    ESP_LOGW(TAG, "Compact frame: format 0x%04X does not fit its data any more, dropped", signature);
    this->compact_formats_->forget(signature);
    return false;
  }
  ESP_LOGD(TAG, "Compact frame: format 0x%04X, records of %u bytes", signature, static_cast<unsigned>(records.size()));
  return this->process_records_(records);
}

void MBusComponent::learn_compact_format_(std::span<const uint8_t> payload) {
  uint8_t format[compact_frame_codec::MAX_FORMAT_SIZE];
  size_t length = 0;
  switch (compact_frame_codec::extract_format(payload.data(), payload.size(), format, sizeof(format), &length)) {
    case compact_frame_codec::FormatResult::FORMAT_OK: {
      const uint16_t signature = compact_frame_codec::format_signature(format, length);
      if (this->compact_formats_->put(signature, format, length)) {
        ESP_LOGD(TAG, "Compact frame: format 0x%04X of %u bytes learned", signature, static_cast<unsigned>(length));
      }
      break;
    }
    case compact_frame_codec::FormatResult::FORMAT_TOO_LONG:
      ESP_LOGD(TAG,
               "Compact frame: format of %u bytes does not fit (%u at most), compact frames of this meter are not read",
               static_cast<unsigned>(length), static_cast<unsigned>(compact_frame_codec::MAX_FORMAT_SIZE));
      break;
    default:
      break;
  }
}
#endif

void MBusComponent::handle_application_frame_(const MBusApplicationFrame &received, MBusApplicationResult *out) {
  MBusApplicationFrame frame = received;
  // An Extended Link Layer and an AFL may stand in front of the transport layer
  uint8_t ell_ci = 0;
  // Holds what an encrypted Extended Link Layer decrypts to; frame.data points into it from then on
  std::vector<uint8_t> &ell_plain = out->link_plain;
  out->ci = frame.ci;
  out->data = std::span<const uint8_t>(frame.data, frame.data != nullptr ? frame.length : 0);
  while (frame.data != nullptr) {
    size_t skip;
    if (frame.ci == afl_codec::CI_AFL) {
      if (!afl_codec::parse(frame.data, frame.length, &frame.afl)) {
        ESP_LOGW(TAG, "AFL truncated or inconsistent: bytes=%u", static_cast<unsigned>(frame.length));
        return;
      }
      if (frame.afl.fragmented()) {
        ESP_LOGW(TAG, "Fragmented message (AFL FCL=0x%04X) not implemented yet", frame.afl.fcl);
        return;
      }
      frame.has_afl = true;
      skip = frame.afl.length;
    } else if (frame.ci >= MBUS_CI_ELL_FIRST && frame.ci <= MBUS_CI_ELL_LAST) {
      skip = MBUS_ELL_LENGTH[frame.ci - MBUS_CI_ELL_FIRST];
      ell_ci = frame.ci;
      // 8Dh and 8Fh end with the session number and two bytes of payload CRC; the session number says whether
      // everything from that CRC on is encrypted (EN 13757-4:2019, 13.2.11 and 13.2.12)
      const bool has_session = (frame.ci & 0x01) != 0;
      if (has_session && frame.length >= skip) {
        const size_t crc_offset = skip - security_codec::ELL_PAYLOAD_CRC_SIZE;
        const uint8_t *session_number = frame.data + crc_offset - security_codec::ELL_SESSION_NUMBER_SIZE;
        const uint8_t encryption = security_codec::ell_encryption(session_number);
        if (encryption != security_codec::ELL_ENCRYPTION_NONE) {
          // Until the rest of the datagram is decrypted
          out->result = MBusResult::MBUS_RESULT_SECURITY_ERROR;
          out->data = std::span<const uint8_t>(frame.data + crc_offset, frame.length - crc_offset);
          if (encryption != security_codec::ELL_ENCRYPTION_AES_CTR || !frame.has_meter_address) {
            ESP_LOGW(TAG, "Link layer encryption %u (ELL CI=0x%02X) not implemented yet", encryption, frame.ci);
            return;
          }
          if (!this->has_encryption_key_) {
            this->log_key_problem_(frame, MBUS_KEY_PROBLEM_NO_KEY,
                                   "Encrypted payload present, no encryption_key configured");
            return;
          }
          if (!security_codec::ell_decrypt(this->cmac_, frame.meter_address.data(), frame.data[0], session_number,
                                           frame.data + crc_offset, frame.length - crc_offset, &ell_plain) ||
              !security_codec::ell_payload_crc_ok(ell_plain)) {
            this->log_key_problem_(frame, MBUS_KEY_PROBLEM_LINK_LAYER,
                                   "Link layer decryption failed: payload CRC mismatch, wrong encryption_key?");
            return;
          }
          out->result = MBusResult::MBUS_RESULT_NOT_DECODED;
          this->key_problems_ = 0;
          ESP_LOGD(TAG, "Security: link layer (ELL CI=0x%02X), decryption ok", frame.ci);
          frame.link_layer_decrypted = true;
          // Go on behind the payload CRC of the decrypted data
          frame.data = ell_plain.data();
          frame.length = ell_plain.size();
          skip = security_codec::ELL_PAYLOAD_CRC_SIZE;
        }
      }
    } else {
      break;
    }
    if (frame.length < skip + 1) {
      ESP_LOGW(TAG, "Layer CI=0x%02X truncated: bytes=%u", frame.ci, static_cast<unsigned>(frame.length));
      return;
    }
    frame.ci = frame.data[skip];
    frame.data += skip + 1;
    frame.length -= skip + 1;
    out->ci = frame.ci;
    out->data = std::span<const uint8_t>(frame.data, frame.length);
  }
  if (frame.has_afl) {
    char ell[20]{};
    if (ell_ci != 0) {
      snprintf(ell, sizeof(ell), "ELL CI=0x%02X, ", ell_ci);
    }
    // The counter is 0 if the AFL has none
    ESP_LOGD(TAG, "Layers: %sAFL FCL=0x%04X AuthType=%u Counter=%u MacBytes=%u", ell, frame.afl.fcl,
             frame.afl.authentication_type,
             static_cast<unsigned>(
                 encode_uint32(frame.afl.counter[3], frame.afl.counter[2], frame.afl.counter[1], frame.afl.counter[0])),
             static_cast<unsigned>(frame.afl.mac_length));
  } else if (ell_ci != 0) {
    ESP_LOGD(TAG, "Layers: ELL CI=0x%02X", ell_ci);
  }

  // A compact frame has the header of the full frame it stands for (EN 13757-3:2018, Table G.2)
  const bool compact = this->reads_compact_frames_() && wired_frame_codec::is_compact_frame(frame.ci);
  if (frame.ci == MBUS_CI_APP_SHORT || (compact && frame.ci == MBUS_CI_COMPACT_SHORT)) {
    this->handle_transport_header_(frame, 0, out);
    return;
  }

  // General application error (EN 13757-3:2018, 10.3). It is reported even if an encryption_key is configured
  // and the datagram is not protected: a meter should send the error that follows a security failure unencrypted
  // (EN 13757-7:2018, 9.5). Such a datagram carries no values, so a forged one can only raise this result. The
  // error bytes are taken as sent; a header that declares them encrypted is not evaluated.
  if (wired_frame_codec::is_application_error(frame.ci)) {
    const uint8_t code = wired_frame_codec::first_error_byte(frame.ci, frame.data, frame.length);
    ESP_LOGW(TAG, "Application error of the meter: CI=0x%02X Code=0x%02X (%s)", frame.ci, code,
             LOG_STR_ARG(application_error_to_str(code)));
    out->result = MBusResult::MBUS_RESULT_APPLICATION_ERROR;
    out->data = out->data.subspan(std::min(wired_frame_codec::application_header_size(frame.ci), frame.length));
    return;
  }

  // Only the long header is needed even by a caller that takes the data as it is: it names the meter
  const bool long_header = frame.ci == MBUS_CI_VARIABLE_RESPONSE || (compact && frame.ci == MBUS_CI_COMPACT_LONG);
  if (frame.data == nullptr || frame.length == 0) {
    ESP_LOGD(TAG, "No application payload present for CI=0x%02X", frame.ci);
    if (!this->records_wanted_() && !long_header) {
      out->result = MBusResult::MBUS_RESULT_OK;
    }
    return;
  }

  // No transport layer header: the records follow the CI field (EN 13757-7:2018, Table 2)
  if (frame.ci == MBUS_CI_APP_NO_HEADER || (compact && frame.ci == MBUS_CI_COMPACT_NO_HEADER)) {
    ESP_LOGD(TAG, "Header: CI=0x%02X, none", frame.ci);
    if (this->reject_unprotected_(frame)) {
      out->result = MBusResult::MBUS_RESULT_SECURITY_ERROR;
      return;
    }
    if (this->process_application_data_(out->data, frame.ci, 0)) {
      out->result = MBusResult::MBUS_RESULT_OK;
    }
    return;
  }

  if (long_header) {
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
      out->result = MBusResult::MBUS_RESULT_INVALID_RESPONSE;
      out->other_meter = true;
      return;
    }

    const uint16_t manufacturer = encode_uint16(frame.data[5], frame.data[4]);
    char manufacturer_str[4]{};
    this->manufacturer_to_str_(manufacturer, manufacturer_str);
    char id_str[BCD_ID_SIZE];
    bcd_id_to_str(frame.data, id_str);
    const uint8_t access_number = frame.data[8];
    const uint8_t status = frame.data[9];
    const uint16_t config = encode_uint16(frame.data[11], frame.data[10]);
    ESP_LOGD(TAG,
             "Header: CI=0x%02X ID=0x%s M=%s Version=0x%02X DevType=0x%02X AccessNr=%u Status=0x%02X "
             "Config=0x%04X",
             frame.ci, id_str, manufacturer_str, frame.data[6], frame.data[7], access_number, status, config);
    // Behind the address the long header goes on like the short one. Where both the link layer and the header
    // have an address, the one of the header is the meter's (EN 13757-7:2018, 7.4).
    MBusApplicationFrame addressed = frame;
    addressed.has_meter_address = true;
    security_codec::address_from_long_header(frame.data, addressed.meter_address.data());
    this->handle_transport_header_(addressed, MBUS_VARIABLE_DATA_HEADER_LENGTH - MBUS_APP_SHORT_HEADER_LENGTH, out);
    return;
  }
  if (!this->records_wanted_()) {
    // The caller takes the data as it is, whatever the CI field says about it
    ESP_LOGD(TAG, "Response: CI=0x%02X, %u bytes, not parsed as records", frame.ci,
             static_cast<unsigned>(frame.length));
    out->result = MBusResult::MBUS_RESULT_OK;
    return;
  }
  if (frame.ci == MBUS_CI_COMPACT_LONG) {
    ESP_LOGW(TAG, "CI=0x%02X (compact frame, formerly fixed data structure) not implemented yet", frame.ci);
    return;
  }
  if (frame.ci == MBUS_CI_ALARM) {
    ESP_LOGW(TAG, "Alarm/status response CI=0x71 Code=%u not implemented yet", static_cast<unsigned>(frame.data[0]));
    return;
  }

  // Nothing the component could read; the handlers get it as not decoded
  if (frame.ci >= MBUS_CI_MANUFACTURER_FIRST && frame.ci <= MBUS_CI_MANUFACTURER_LAST) {
    ESP_LOGD(TAG, "Manufacturer specific CI=0x%02X, not read", frame.ci);
    return;
  }
  ESP_LOGW(TAG, "Application CI=0x%02X not implemented yet", frame.ci);
}

// Called once from setup(); the plain key is wiped afterwards because the decryptor keeps its own form of it.
void MBusComponent::init_decryptor_() {
  const bool ok =
      this->decryptor_.set_key(this->encryption_key_.data()) && this->cmac_.set_key(this->encryption_key_.data());
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

  // With the short header the address is the one of the link layer
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE
  uint8_t iv[Aes128CbcDecryptor::BLOCK_SIZE];
  security_codec::mode_5_iv(frame.meter_address.data(), access_number, iv);
  char hex[format_hex_pretty_size(sizeof(iv))];
  ESP_LOGV(TAG, "Mode 5 IV: %s", format_hex_pretty_to(hex, iv, sizeof(iv)));
#endif
  return security_codec::mode_5_decrypt(this->decryptor_, frame.meter_address.data(), access_number,
                                        frame.data + payload_offset, encrypted_length, out);
}

// Security mode 7 with the message as it sits in the frame: the TPDU starts at the CI field in front of the data
bool MBusComponent::decrypt_mode_7_(const MBusApplicationFrame &frame, size_t payload_offset, size_t encrypted_length,
                                    uint8_t key_derivation, const uint8_t *counter, std::vector<uint8_t> *out,
                                    bool *authenticated) {
  using security_codec::Mode7Result;
  if (!frame.has_meter_address || frame.data == nullptr || payload_offset + encrypted_length > frame.length) {
    ESP_LOGW(TAG, "Mode 7 decryption failed: no meter address or truncated payload");
    return false;
  }
  security_codec::Mode7Message message;
  message.identification = frame.meter_address.data() + 2;
  message.counter = counter;
  message.key_derivation = key_derivation;
  message.afl = frame.has_afl ? &frame.afl : nullptr;
  message.tpdu = frame.data - 1;
  message.tpdu_length = frame.length + 1;
  message.encrypted = frame.data + payload_offset;
  message.encrypted_length = encrypted_length;
  switch (security_codec::mode_7_decrypt(this->cmac_, this->decryptor_, message, out, authenticated)) {
    case Mode7Result::MODE_7_OK:
      return true;
    case Mode7Result::MODE_7_KEY_DERIVATION_UNKNOWN:
      ESP_LOGW(TAG, "Mode 7 key derivation function %u not implemented yet", key_derivation);
      return false;
    case Mode7Result::MODE_7_NO_COUNTER:
      ESP_LOGW(TAG, "Mode 7 decryption failed: no message counter for the key derivation");
      return false;
    case Mode7Result::MODE_7_AUTHENTICATION_UNKNOWN:
      ESP_LOGW(TAG, "Authentication with AES-GMAC not implemented yet");
      return false;
    case Mode7Result::MODE_7_KEY_DERIVATION_FAILED:
      ESP_LOGW(TAG, "Mode 7 key derivation failed");
      return false;
    case Mode7Result::MODE_7_MAC_NOT_COMPUTED:
      ESP_LOGW(TAG, "Mode 7 authentication failed: MAC not computed");
      return false;
    case Mode7Result::MODE_7_MAC_MISMATCH:
      this->log_key_problem_(frame, MBUS_KEY_PROBLEM_MAC,
                             "Mode 7 authentication failed: MAC mismatch, wrong encryption_key?");
      return false;
    default:
      ESP_LOGW(TAG, "Mode 7 decryption failed");
      return false;
  }
}

}  // namespace esphome::mbus
