#include "mbus.h"
#include "wmbus_frame_codec.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::mbus {

static const char *const TAG = "mbus";

static constexpr size_t MBUS_DLL_HEADER_LENGTH = 10;

void MBusComponent::parse_c_mode_(const std::vector<uint8_t> &packet, const MBusReceiveMeta &meta) {
  if (packet.size() <= 12) {
    ESP_LOGD(TAG, "Ignoring short C-mode packet: bytes=%u rssi=%.0f lqi=%u", static_cast<unsigned>(packet.size()),
             meta.rssi, meta.lqi);
    return;
  }

  WMBusFrameFormat format = WMBusFrameFormat::UNKNOWN;
  if (packet[1] == wmbus_frame_codec::WMBUS_FRAME_A_PREFIX) {
    format = WMBusFrameFormat::A;
  } else if (packet[1] == wmbus_frame_codec::WMBUS_FRAME_B_PREFIX) {
    format = WMBusFrameFormat::B;
  } else {
    ESP_LOGD(TAG, "Ignoring C-mode packet with unknown frame marker: marker=0x%02X bytes=%u", packet[1],
             static_cast<unsigned>(packet.size()));
    return;
  }

  const uint8_t *frame = packet.data() + 2;
  const size_t frame_length = packet.size() - 2;
  const uint8_t l_field = frame[0];
  const size_t expected =
      format == WMBusFrameFormat::A ? this->packet_len_frame_a_(l_field) + 2 : this->packet_len_frame_b_(l_field) + 2;
  const char *format_name = format == WMBusFrameFormat::A ? "A" : "B";

  ESP_LOGD(TAG, "Received mode C frame with format %s: bytes=%u expected=%u l=%u rssi=%.0f lqi=%u dFreq=%.0f",
           format_name, static_cast<unsigned>(packet.size()), static_cast<unsigned>(expected), l_field, meta.rssi,
           meta.lqi, meta.freq_offset);

  if (expected == 0 || expected != packet.size()) {
    ESP_LOGW(TAG, "Length mismatch for mode C frame with format %s: received=%u expected=%u l=%u", format_name,
             static_cast<unsigned>(packet.size()), static_cast<unsigned>(expected), l_field);
    return;
  }

  uint8_t failed_block = 0;
  std::vector<uint8_t> frame_without_crcs;
  const bool crc_ok =
      format == WMBusFrameFormat::A
          ? wmbus_frame_codec::strip_frame_a_crcs(frame, frame_length, &frame_without_crcs, &failed_block)
          : wmbus_frame_codec::strip_frame_b_crcs(frame, frame_length, &frame_without_crcs, &failed_block);
  if (!crc_ok) {
    ESP_LOGW(TAG, "CRC failed: mode=C/%s block=%u", format_name, failed_block);
    return;
  }

  ESP_LOGD(TAG, "CRC: OK, stripped frame bytes=%u", static_cast<unsigned>(frame_without_crcs.size()));
  this->handle_wmbus_link_frame_(frame_without_crcs, meta);
}

void MBusComponent::parse_t_mode_(const std::vector<uint8_t> &packet, const MBusReceiveMeta &meta) {
  uint8_t l_field = 0;
  if (!wmbus_frame_codec::decode_3of6_first_byte(packet.data(), packet.size(), &l_field)) {
    ESP_LOGD(TAG, "Ignoring non-C-mode packet with invalid T-mode 3-of-6 prefix: bytes=%u first=0x%02X",
             static_cast<unsigned>(packet.size()), packet[0]);
    return;
  }

  const size_t expected_decoded = this->packet_len_frame_a_(l_field);
  const size_t expected_encoded = wmbus_frame_codec::encoded_size_3of6(expected_decoded);
  ESP_LOGD(TAG, "Received mode T frame: bytes=%u expected=%u l=%u rssi=%.0f lqi=%u dFreq=%.0f",
           static_cast<unsigned>(packet.size()), static_cast<unsigned>(expected_encoded), l_field, meta.rssi, meta.lqi,
           meta.freq_offset);
  if (expected_decoded == 0 || expected_encoded != packet.size()) {
    ESP_LOGW(TAG, "Length mismatch for mode T frame: received=%u expected=%u l=%u",
             static_cast<unsigned>(packet.size()), static_cast<unsigned>(expected_encoded), l_field);
    return;
  }

  std::vector<uint8_t> decoded_frame;
  if (!wmbus_frame_codec::decode_3of6(packet.data(), packet.size(), &decoded_frame) ||
      decoded_frame.size() != expected_decoded) {
    ESP_LOGW(TAG, "3-of-6 decoding failed: mode=T");
    return;
  }

  uint8_t failed_block = 0;
  std::vector<uint8_t> frame_without_crcs;
  if (!wmbus_frame_codec::strip_frame_a_crcs(decoded_frame.data(), decoded_frame.size(), &frame_without_crcs,
                                             &failed_block)) {
    ESP_LOGW(TAG, "CRC failed: mode=T/A block=%u", failed_block);
    return;
  }

  ESP_LOGD(TAG, "CRC: OK, stripped frame bytes=%u", static_cast<unsigned>(frame_without_crcs.size()));
  this->handle_wmbus_link_frame_(frame_without_crcs, meta);
}

void MBusComponent::handle_wmbus_link_frame_(const std::vector<uint8_t> &frame, const MBusReceiveMeta &meta) {
  if (frame.size() <= MBUS_DLL_HEADER_LENGTH) {
    ESP_LOGW(TAG, "Wireless frame too short: bytes=%u", static_cast<unsigned>(frame.size()));
    return;
  }

  const uint16_t manufacturer = encode_uint16(frame[3], frame[2]);
  char manufacturer_str[4]{};
  this->manufacturer_to_string_(manufacturer, manufacturer_str);
  const uint32_t meter_id =
      (uint32_t(frame[7]) << 24) | (uint32_t(frame[6]) << 16) | (uint32_t(frame[5]) << 8) | uint32_t(frame[4]);
  if (this->has_meter_id_ && meter_id != this->meter_id_) {
    ESP_LOGV(TAG, "Ignoring frame for meter ID 0x%08X, configured meter ID is 0x%08X", static_cast<unsigned>(meter_id),
             static_cast<unsigned>(this->meter_id_));
    return;
  }

  ESP_LOGD(TAG, "Block-1: L=%u C=0x%02X M=%s ID=0x%08X Version=0x%02X DevType=0x%02X", frame[0], frame[1],
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
  this->handle_application_frame_(app_frame);
}

size_t MBusComponent::packet_len_frame_a_(uint8_t l_field) const {
  return wmbus_frame_codec::packet_len_frame_a(l_field);
}

size_t MBusComponent::packet_len_frame_b_(uint8_t l_field) const {
  return wmbus_frame_codec::packet_len_frame_b(l_field);
}

}  // namespace esphome::mbus
