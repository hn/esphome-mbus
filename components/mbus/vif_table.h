#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace esphome::mbus {

// Returns a human-readable data type name, taking date/time VIFs into account.
const char *record_data_type_to_string(uint8_t dif, uint16_t vif_key);

// True for the standard date (0x6C) and date/time (0x6D) VIFs.
bool vif_is_datetime(uint16_t vif_key);

// Fills scale and unit for the given VIF. Returns true when a scaled numeric
// value makes sense (e.g. volume). The VIF key is either the base VIF or the
// extension key produced from 0xFB/0xFD and the first VIFE.
bool vif_scale_unit(uint16_t vif_key, double *scale, const char **unit);

// True if the raw VIF byte is a VIF extension marker (0xFB/0xFD/0xEF/0xFF).
// For these the first following VIFE belongs to the VIF code itself.
bool vif_is_extension_marker(uint8_t vif_raw);

// Human-readable name for a combinable VIFE (e.g. 0x3C -> "backward_flow").
// Returns nullptr if unknown.
const char *vife_combinable_to_string(uint8_t vife);

// Applies EN 13757-3 combinable VIFE correction factors to an already scaled
// numeric value. Returns true when the value changed.
bool vife_apply_correction(uint8_t vife, double *value);

// Decodes an M-Bus type G date (2 bytes) into "YYYY-MM-DD".
// Returns false and sets out to "invalid" when fields are out of range.
bool decode_date_type_g(const uint8_t *data, std::string *out);

// Decodes an M-Bus type F/I date-time into "YYYY-MM-DDThh:mm" (4 bytes) or
// "YYYY-MM-DDThh:mm:ss" (6 bytes). Returns false and sets out to "invalid"
// when fields are out of range.
bool decode_datetime_type_f(const uint8_t *data, size_t length, std::string *out);

}  // namespace esphome::mbus
