#include "mbus.h"

namespace esphome::mbus {

// Hands a valid RSP_UD to the application layer. The address field is not checked: a slave answers with its
// primary address or with 253 (EN 13757-7:2018, 8.4).
void MBusComponent::handle_wired_link_frame_(const wired_frame_codec::Frame &frame, MBusApplicationResult *out) {
  this->more_records_follow_ = false;
  MBusApplicationFrame app_frame;
  app_frame.ci = frame.ci;
  app_frame.data = frame.data;
  app_frame.length = frame.data_length;
  app_frame.has_control = true;
  app_frame.control = frame.control;
  app_frame.has_address = true;
  app_frame.address = frame.address;
  this->handle_application_frame_(app_frame, out);
}

}  // namespace esphome::mbus
