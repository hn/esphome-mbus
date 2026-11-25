#include "mbus.h"
#include "wmbus_frame_codec.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#ifdef USE_MBUS_RADIO
namespace esphome::mbus {

ESPHOME_LOG_TAG(TAG, "mbus");

// First block of the link layer: L, C, M (2 bytes) and A (6 bytes) (EN 13757-4:2019, Tables 27 and 30)
static constexpr size_t MBUS_DLL_HEADER_LENGTH = 10;

static constexpr size_t RADIO_VALUES_SIZE = 48;

// The signal values for a log line, with a leading blank: only what the radio reports
static void format_radio_values(const MBusReceiveMeta &meta, char *out) {
  size_t length = 0;
  out[0] = '\0';
  if (meta.has_rssi) {
    length += snprintf(out, RADIO_VALUES_SIZE, " rssi=%.0f", meta.rssi);
  }
  if (meta.has_lqi || meta.has_freq_offset) {
    snprintf(out + length, RADIO_VALUES_SIZE - length, " lqi=%u freq_offset=%.0f", meta.lqi, meta.freq_offset);
  }
}

// One line per received frame; result is appended once the frame has passed its checks.
static void log_received(const char *frame_type, size_t received, size_t expected, uint8_t l_field,
                         const MBusReceiveMeta &meta, const char *result) {
  char radio[RADIO_VALUES_SIZE];
  format_radio_values(meta, radio);
  ESP_LOGD(TAG, "Received %s: bytes=%u expected=%u L=%u%s%s", frame_type, static_cast<unsigned>(received),
           static_cast<unsigned>(expected), l_field, radio, result);
}

void MBusComponent::parse_c_mode_(const std::vector<uint8_t> &packet, const MBusReceiveMeta &meta) {
  if (packet.size() <= 12) {
    char radio[RADIO_VALUES_SIZE];
    format_radio_values(meta, radio);
    ESP_LOGD(TAG, "Ignoring short mode C packet: bytes=%u%s", static_cast<unsigned>(packet.size()), radio);
    return;
  }

  WMBusFrameFormat format = WMBusFrameFormat::WMBUS_FRAME_FORMAT_UNKNOWN;
  if (packet[1] == wmbus_frame_codec::WMBUS_FRAME_A_PREFIX) {
    format = WMBusFrameFormat::WMBUS_FRAME_FORMAT_A;
  } else if (packet[1] == wmbus_frame_codec::WMBUS_FRAME_B_PREFIX) {
    format = WMBusFrameFormat::WMBUS_FRAME_FORMAT_B;
  } else {
    ESP_LOGD(TAG, "Ignoring mode C packet with unknown frame marker: marker=0x%02X bytes=%u", packet[1],
             static_cast<unsigned>(packet.size()));
    return;
  }

  const uint8_t *frame = packet.data() + 2;
  const uint8_t l_field = frame[0];
  const size_t expected = format == WMBusFrameFormat::WMBUS_FRAME_FORMAT_A
                              ? wmbus_frame_codec::packet_len_frame_a(l_field) + 2
                              : wmbus_frame_codec::packet_len_frame_b(l_field) + 2;
  const char *format_name = format == WMBusFrameFormat::WMBUS_FRAME_FORMAT_A ? "A" : "B";
  const char *frame_type =
      format == WMBusFrameFormat::WMBUS_FRAME_FORMAT_A ? "mode C format A frame" : "mode C format B frame";

  if (expected == 0 || packet.size() < expected) {
    log_received(frame_type, packet.size(), expected, l_field, meta, "");
    ESP_LOGD(TAG, "Length mismatch for mode C format %s frame: received=%u expected=%u L=%u", format_name,
             static_cast<unsigned>(packet.size()), static_cast<unsigned>(expected), l_field);
    return;
  }

  // Some transports (e.g. fixed-length SX126x reception) always deliver a fixed-size buffer padded
  // with trailing noise after the real frame; only the part accounted for by the L field is real.
  const size_t frame_length = expected - 2;

  uint8_t failed_block = 0;
  std::vector<uint8_t> frame_without_crcs;
  const bool crc_ok =
      format == WMBusFrameFormat::WMBUS_FRAME_FORMAT_A
          ? wmbus_frame_codec::strip_frame_a_crcs(frame, frame_length, &frame_without_crcs, &failed_block)
          : wmbus_frame_codec::strip_frame_b_crcs(frame, frame_length, &frame_without_crcs, &failed_block);
  if (!crc_ok) {
    log_received(frame_type, packet.size(), expected, l_field, meta, "");
    ESP_LOGD(TAG, "CRC failed: mode=C/%s block=%u", format_name, failed_block);
    return;
  }

  log_received(frame_type, packet.size(), expected, l_field, meta, ", CRC ok");
  this->handle_wmbus_link_frame_(frame_without_crcs, meta);
}

void MBusComponent::parse_t_mode_(const std::vector<uint8_t> &packet, const MBusReceiveMeta &meta) {
  uint8_t l_field = 0;
  if (!wmbus_frame_codec::decode_3of6_first_byte(packet.data(), packet.size(), &l_field)) {
    ESP_LOGD(TAG, "Ignoring non-mode-C packet with invalid mode T 3-of-6 prefix: bytes=%u first=0x%02X",
             static_cast<unsigned>(packet.size()), packet[0]);
    return;
  }

  const size_t expected_decoded = wmbus_frame_codec::packet_len_frame_a(l_field);
  const size_t expected_encoded = wmbus_frame_codec::encoded_size_3of6(expected_decoded);
  static constexpr char FRAME_TYPE[] = "mode T frame";
  if (expected_decoded == 0 || packet.size() < expected_encoded) {
    log_received(FRAME_TYPE, packet.size(), expected_encoded, l_field, meta, "");
    ESP_LOGD(TAG, "Length mismatch for mode T frame: received=%u expected=%u L=%u",
             static_cast<unsigned>(packet.size()), static_cast<unsigned>(expected_encoded), l_field);
    return;
  }

  // As in parse_c_mode_(): decode only the L-field-accounted-for portion, ignoring any trailing
  // padding a fixed-length transport may have delivered.
  std::vector<uint8_t> decoded_frame;
  if (!wmbus_frame_codec::decode_3of6(packet.data(), expected_encoded, &decoded_frame) ||
      decoded_frame.size() != expected_decoded) {
    log_received(FRAME_TYPE, packet.size(), expected_encoded, l_field, meta, "");
    ESP_LOGD(TAG, "3-of-6 decoding failed: mode=T");
    return;
  }

  uint8_t failed_block = 0;
  std::vector<uint8_t> frame_without_crcs;
  if (!wmbus_frame_codec::strip_frame_a_crcs(decoded_frame.data(), decoded_frame.size(), &frame_without_crcs,
                                             &failed_block)) {
    log_received(FRAME_TYPE, packet.size(), expected_encoded, l_field, meta, "");
    ESP_LOGD(TAG, "CRC failed: mode=T block=%u", failed_block);
    return;
  }

  log_received(FRAME_TYPE, packet.size(), expected_encoded, l_field, meta, ", CRC ok");
  this->handle_wmbus_link_frame_(frame_without_crcs, meta);
}

// C fields a meter sends by itself (EN 13757-4:2019, Table 34), with the leading blank of the log line
static const char *control_name(uint8_t control) {
  switch (control) {
    case 0x44:
      return " (SND-NR)";
    case 0x46:
      return " (SND-IR)";
    case 0x47:
      return " (ACC-NR)";
    case 0x48:
      return " (ACC-DMD)";
    default:
      return "";
  }
}

void MBusComponent::handle_wmbus_link_frame_(const std::vector<uint8_t> &frame, const MBusReceiveMeta &meta) {
  // The frame has a CI field behind the first block: the length calculation accepts no L field without one
  const uint16_t manufacturer = encode_uint16(frame[3], frame[2]);
  char manufacturer_str[4]{};
  this->manufacturer_to_str_(manufacturer, manufacturer_str);
  const uint32_t meter_id =
      (uint32_t(frame[7]) << 24) | (uint32_t(frame[6]) << 16) | (uint32_t(frame[5]) << 8) | uint32_t(frame[4]);
  // The configured meter may also be named by the second address of the link layer or by a long header
  if (this->has_meter_id_ && !wmbus_frame_codec::has_meter_id(frame.data(), frame.size(), this->meter_id_)) {
    ESP_LOGV(TAG, "Ignoring frame for meter ID 0x%08X, configured meter ID is 0x%08X", static_cast<unsigned>(meter_id),
             static_cast<unsigned>(this->meter_id_));
    return;
  }

  ESP_LOGD(TAG, "Link: C=0x%02X%s M=%s ID=0x%08X Version=0x%02X DevType=0x%02X", frame[1], control_name(frame[1]),
           manufacturer_str, static_cast<unsigned>(meter_id), frame[8], frame[9]);

  const uint8_t *app = frame.data() + MBUS_DLL_HEADER_LENGTH;
  MBusApplicationFrame app_frame;
  app_frame.ci = app[0];
  app_frame.data = app + 1;
  app_frame.length = frame.size() - MBUS_DLL_HEADER_LENGTH - 1;
  app_frame.has_control = true;
  app_frame.control = frame[1];
  app_frame.has_meter_address = true;
  for (size_t i = 0; i < app_frame.meter_address.size(); i++) {
    app_frame.meter_address[i] = frame[2 + i];
  }
  app_frame.meta = meta;
  MBusApplicationResult result;
  this->handle_application_frame_(app_frame, &result);
  this->report_received_(result.result, result.ci, frame, result.data, meta);
}

}  // namespace esphome::mbus
#endif  // USE_MBUS_RADIO
