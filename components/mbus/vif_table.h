#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace esphome {
struct LogString;
}  // namespace esphome

namespace esphome::mbus {

// Date and time types of EN 13757-3:2018, Annex A
enum class MBusDateTimeType : uint8_t {
  MBUS_DATE_TIME_TYPE_NONE,
  MBUS_DATE_TIME_TYPE_G,  // date, 2 bytes
  MBUS_DATE_TIME_TYPE_F,  // date and time, 4 bytes
  MBUS_DATE_TIME_TYPE_J,  // time, 3 bytes
  MBUS_DATE_TIME_TYPE_I,  // date and time with seconds, 6 bytes
};

// Date or time type of a record. It needs a date/time VIF or a "date(/time) of" VIFE, and the data field then
// selects the type (EN 13757-3:2018, Table 10 and Table 15, footnote e). Any other data field leaves the record
// a plain number.
MBusDateTimeType record_date_time_type(uint8_t dif, uint16_t vif_key, bool vife_timestamp);

// Returns a human-readable data type name, taking date/time VIFs into account. Like the other names of this file
// it is a flash string on ESP8266: log it with LOG_STR_ARG.
const LogString *record_data_type_to_str(uint8_t dif, uint16_t vif_key, bool vife_timestamp = false);

// True for the standard date (0x6C) and date/time (0x6D) VIFs.
bool vif_is_datetime(uint16_t vif_key);

// Fills scale and unit (a flash string on ESP8266, log it with LOG_STR_ARG) for the given VIF. Returns true when a
// scaled numeric value makes sense (e.g. volume). The VIF key is either the base VIF or the extension key produced from
// 0xFB/0xFD and the first VIFE.
bool vif_scale_unit(uint16_t vif_key, double *scale, const LogString **unit);

// True if the raw VIF byte is a VIF extension marker (0xFB/0xFD/0xEF/0xFF).
// For these the first following VIFE belongs to the VIF code itself.
bool vif_is_extension_marker(uint8_t vif_raw);

// What the combinable VIFEs of a record say about its value (EN 13757-3:2018, Tables 15 and 16)
struct VifeEffects {
  double factor{1.0};          // product of the multiplicative corrections
  bool timestamp{false};       // "date(/time) of": the data is a date or a date and time
  bool unsigned_value{false};  // the data is an unsigned integer (data type C)
};

// Human-readable name for a combinable VIFE of Table 15 (e.g. 0x3C -> "backward_flow").
// Returns nullptr if unknown.
const LogString *vife_combinable_to_str(uint8_t vife);

// The same for a VIFE of Table 16, which follows the combinable VIFE FCh
const LogString *vife_extension_to_str(uint8_t vife);

// Walks the combinable VIFEs of a record. A VIFE behind FCh is read from Table 16, the ones behind FFh are
// manufacturer specific and left alone. Appends the known names to flags, separated by commas.
VifeEffects vife_interpret(const uint8_t *vife, size_t count, std::string *flags);

// Decodes an M-Bus type G date (2 bytes) into "YYYY-MM-DD".
// Returns false and sets out to "invalid" when fields are out of range.
bool decode_date_type_g(const uint8_t *data, std::string *out);

// Decodes an M-Bus type F date and time (4 bytes) into "YYYY-MM-DDThh:mm" or a type I one (6 bytes) into
// "YYYY-MM-DDThh:mm:ss". Returns false and sets out to "invalid" when the datagram marks the time as invalid or
// fields are out of range.
bool decode_datetime_type_f(const uint8_t *data, size_t length, std::string *out);

// Decodes an M-Bus type J time (3 bytes) into "hh:mm:ss".
// Returns false and sets out to "invalid" when fields are out of range.
bool decode_time_type_j(const uint8_t *data, std::string *out);

}  // namespace esphome::mbus
