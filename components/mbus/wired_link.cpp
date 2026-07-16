#include "mbus.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::mbus {

static const char *const TAG = "mbus";

static constexpr uint8_t MBUS_WIRED_LONG_START = 0x68;
static constexpr uint8_t MBUS_WIRED_STOP = 0x16;

static uint8_t calculate_wired_long_checksum_(const uint8_t *data, size_t length) {
  uint8_t checksum = 0;
  for (size_t i = 0; i < length; i++) {
    checksum += data[i];
  }
  return checksum;
}

bool MBusComponent::handle_wired_link_frame_(const std::vector<uint8_t> &frame, const MBusReceiveMeta &meta) {
  if (frame.size() < 9) {
    ESP_LOGW(TAG, "Wired long frame too short: bytes=%u", static_cast<unsigned>(frame.size()));
    return false;
  }

  if (frame[0] != MBUS_WIRED_LONG_START || frame[1] != frame[2] || frame[3] != MBUS_WIRED_LONG_START ||
      frame.back() != MBUS_WIRED_STOP) {
    ESP_LOGW(TAG, "Invalid wired long frame header: %s", format_hex_pretty(frame).c_str());
    return false;
  }

  const size_t expected_size = static_cast<size_t>(frame[1]) + 6;
  if (frame.size() != expected_size) {
    ESP_LOGW(TAG, "Invalid wired long frame length: got=%u expected=%u", static_cast<unsigned>(frame.size()),
             static_cast<unsigned>(expected_size));
    return false;
  }

  if (frame[1] < 3) {
    ESP_LOGW(TAG, "Invalid wired long frame L-field: %u", static_cast<unsigned>(frame[1]));
    return false;
  }

  const uint8_t checksum = calculate_wired_long_checksum_(frame.data() + 4, frame[1]);
  if (frame[frame.size() - 2] != checksum) {
    ESP_LOGW(TAG, "Invalid wired long frame checksum: got=0x%02X expected=0x%02X", frame[frame.size() - 2], checksum);
    return false;
  }

  MBusApplicationFrame app_frame;
  app_frame.ci = frame[6];
  app_frame.data = frame.data() + 7;
  app_frame.length = frame[1] - 3;
  app_frame.has_control = true;
  app_frame.control = frame[4];
  app_frame.has_address = true;
  app_frame.address = frame[5];
  app_frame.meta = meta;
  this->handle_application_frame_(app_frame);
  return true;
}

}  // namespace esphome::mbus
