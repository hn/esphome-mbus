# esphome-mbus

A generic, transport-independent M-Bus / wM-Bus [external component](https://esphome.io/components/external_components/)
for [ESPHome](https://esphome.io/): wireless (CC1101, SX126x, SX127x) and wired (UART) M-Bus in one
component core, with a full DIF/VIF/VIFE record dump instead of built-in per-meter drivers.

> [!NOTE]
> **Project Status:** Early stage. Wireless C1 Format A is hardware-validated against a real WaterStarM water
> meter. Everything else (C1 Format B, T1, SX126x/SX127x, wired UART) is implemented but not yet validated
> against real hardware — see [Supported Transports](#supported-transports--validation-status) below.

## Design Philosophy

Other (w)M-Bus options exist for ESPHome; this one is built around a few specific choices:

- **Reuses existing ESPHome hardware components.** Wireless (CC1101, SX126x, SX127x) and wired (UART) all
  go through ESPHome's own radio/UART components — no custom hardware drivers to maintain.
- **One component, every transport.** Wireless and wired share the same core: link framing → application
  layer → DIF/VIF/VIFE records → ESPHome entities. A TCP transport (M-Bus/wM-Bus gateways) is kept in mind
  conceptually so the architecture wouldn't have to change to add it, but it is not committed to being
  built.
- **No built-in meter drivers.** Instead of a maintained per-model driver database, `esphome-mbus` decodes
  and dumps the full DIF/VIF/VIFE record structure — raw value, decoded value, unit, storage/tariff/
  subunit, function — so you can see exactly what your meter sends and build `sensor:`/`binary_sensor:`/
  `text_sensor:` entities for it directly in YAML, without waiting for someone to add your specific meter
  to a driver list.
- **No external libraries.** Framing, decryption and record decoding are pure ESPHome/ESP-IDF C++ —
  nothing extra to vendor or version-pin beyond ESPHome itself.
- **Transparent scaling.** Published sensor values are unscaled; you apply scaling via ESPHome's own
  `filters:`, so nothing is hidden inside a driver.

## Architecture

```
wireless (CC1101 / SX126x / SX127x)  \
wired UART                            >--  normalized frame  -->  application layer  -->  records  -->  entities
(TCP, conceptual only)               /
```

The `mbus` component picks exactly one source per instance (`radio_cc1101_id`, `radio_sx126x_id`,
`radio_sx127x_id`, or `wired_uart_id`); multiple `mbus:` instances are supported (e.g. several meters on
one link). See [`components/mbus`](components/mbus) for the implementation.

## Installation

```yaml
external_components:
  - source: github://hn/esphome-mbus@main
    components: [mbus, cc1101]  # drop cc1101 once it's no longer needed, see below
```

## The bundled `cc1101` copy

This repository temporarily vendors a patched copy of ESPHome core's `cc1101` component, under
[`components/cc1101`](components/cc1101). It exists only because `mbus` needs long-packet RX support
(`packet_length` above 64 bytes, and `packet_length_lambda` for protocols whose length isn't known
upfront) that core `cc1101` doesn't have yet. The patches are split into three upstream pull requests:

- [esphome/esphome#17576](https://github.com/esphome/esphome/pull/17576) — export `CC1101Listener` to
  Python (not required by `mbus` itself, but a prerequisite for other external components to use
  `cc1101.CC1101Listener`).
- [esphome/esphome#17577](https://github.com/esphome/esphome/pull/17577) — FOCCFG/BSCFG frequency offset
  and bit sync tuning options (not required, improves RX quality for narrowband protocols like wM-Bus).
- [esphome/esphome#17578](https://github.com/esphome/esphome/pull/17578) — long packet RX support
  (**required** by `mbus`; this is the actual blocker for dropping the vendored copy).

These pull requests are pending review and may be rejected, changed significantly, or take a while either
way — there's no guarantee they'll be merged as-is, or at all. If and when #17578 lands in an ESPHome
release, `mbus` can work against core `cc1101` directly, the local copy can be removed from this
repository, and `cc1101` would no longer need to be listed under `components:` above. Until then, this
vendored copy remains the way to get long-packet RX support.

## Supported Transports & Validation Status

| Transport | Implemented | Validated against real hardware |
|---|---|---|
| CC1101, wM-Bus C1 Format A | Yes | Yes (WaterStarM water meter) |
| CC1101, wM-Bus C1 Format B | Yes | No (host-side/synthetic test vectors only) |
| CC1101, wM-Bus T1 | Yes | No (host-side/synthetic test vectors only) |
| SX126x / SX127x, wM-Bus C1/T1 | Yes (shares the CC1101 code path via ESPHome core radio listeners) | No |
| Wired M-Bus over UART | Yes | No |
| TCP (M-Bus/wM-Bus gateway) | No, conceptual only, not committed to | — |

## Configuration Example

```yaml
external_components:
  - source:
      type: local
      path: components
    components: [cc1101, mbus]

spi:
  clk_pin: GPIOXX
  mosi_pin: GPIOXX
  miso_pin: GPIOXX

cc1101:
  id: cc1101_radio
  cs_pin: GPIOXX
  gdo0_pin: GPIOXX
  frequency: 868.95MHz
  # ... see the cc1101 documentation for the remaining radio tuning options

mbus:
  - id: water_meter
    radio_cc1101_id: cc1101_radio
    dump_records: true
    # meter_id: 0x12345678  # optional: only process frames from this meter's serial number

sensor:
  - platform: mbus
    mbus_id: water_meter
    name: "Total water consumption"
    dif: 0x04
    vif: 0x13
    function: instantaneous
    unit_of_measurement: "m³"
    device_class: water
    state_class: total_increasing
    accuracy_decimals: 3
    filters:
      - multiply: 0.001
```

Instead of `radio_cc1101_id`, you can use `radio_sx126x_id` or `radio_sx127x_id` with a configured
`sx126x`/`sx127x` radio component.

### Wired M-Bus Example

Wired M-Bus over UART works the same way, just with a different source and no radio-specific settings.
`secondary_address` is the meter's M-Bus secondary address; `update_interval` controls how often it's
polled (defaults to `60s`):

```yaml
uart:
  - id: mbus_uart
    tx_pin: GPIOXX
    rx_pin: GPIOXX
    baud_rate: 2400
    parity: EVEN

mbus:
  - id: heat_meter
    wired_uart_id: mbus_uart
    secondary_address: 0x1234567812345678
    update_interval: 60s
    dump_records: true
```

### Complete Example: WaterStarM Water Meter

[`watermeter-waterstarm-cc1101.yaml`](watermeter-waterstarm-cc1101.yaml) is a full, ready-to-flash
configuration for a real meter (WaterStarM M-ETH Q3 2.5, wireless via CC1101) — the one this component
was originally hardware-validated against. Besides `cc1101:`/`mbus:`, it includes WiFi, the native API,
OTA, a captive-portal fallback, and working `sensor:`/`binary_sensor:` entities, so it doubles as a
complete reference for wiring everything else in this README together into a real device.

## Building Your YAML From a Record Dump

There's no driver database to look your meter up in, so the workflow is to look at what your meter
actually sends and match it directly:

1. **Set `dump_records: true`** and flash. Every record your meter sends gets logged as one `Record:`
   line at `DEBUG` level, showing everything needed to match it: `dif`, `vif` (and `vif_ext` if the VIF
   uses an extension), `storage`/`tariff`/`subunit`, `function`, `vife`, plus the raw bytes and the
   decoded value/unit for context.
2. **Pick the fields for the value you want** and copy them into a `sensor:`/`binary_sensor:`/
   `text_sensor:` entry (see [matching rules](#matching-rules) below).
3. **Set `dump_records: false`** again once your entities are in place. Formatting and logging every
   record on every received telegram costs CPU on top of the actual decoding; only turn it back on
   temporarily when you're adding a new entity or debugging.

### Example dump output

Illustrative example, in the shape the real log lines take, for the WaterStarM meter (manufacturer
`DWZ`) this component was first validated against:

```text
[D][mbus:118]: Record: dif=0x04 vif=0x13 storage=0 tariff=0 subunit=0 function=instantaneous pos=8 vif_raw=0x13 dife=[] vife=[] data_type=int32 raw=A7.BF.01.00 raw_value=114599; decoded: scale=0.001 unit=m3 value=114.599
[D][mbus:118]: Record: dif=0x44 vif=0x13 storage=1 tariff=0 subunit=0 function=instantaneous pos=14 vif_raw=0x13 dife=[] vife=[0x3C] data_type=int32 raw=0C.00.00.00 flags=backward_flow raw_value=12; decoded: scale=0.001 unit=m3 value=0.012
[D][mbus:118]: Record: dif=0x04 vif=0x6D storage=0 tariff=0 subunit=0 function=instantaneous pos=20 vif_raw=0x6D dife=[] vife=[] data_type=datetime_type_f raw=1E.0E.0F.33; decoded: value=2024-03-15T14:30
[D][mbus:118]: Record: dif=0x02 vif=0x7D storage=0 tariff=0 subunit=0 function=instantaneous pos=26 vif_raw=0xFD vif_ext=0x7D17 dife=[] vife=[] data_type=int16 raw=50.00 raw_value=80; decoded: scale=1 unit=flags value=80
```

The first line, for example, turns directly into:

```yaml
sensor:
  - platform: mbus
    mbus_id: water_meter
    name: "Total water consumption"
    dif: 0x04
    vif: 0x13
    function: instantaneous
    unit_of_measurement: "m³"
    device_class: water
    state_class: total_increasing
    accuracy_decimals: 3
    filters:
      - multiply: 0.001  # matches "decoded: scale=0.001" from the dump
```

The fourth line, with its `vif_ext=0x7D17` flags/status word, turns into a `binary_sensor:` picking out
one bit — `raw_value=80` is `0b01010000`, so bit 4 is set:

```yaml
binary_sensor:
  - platform: mbus
    mbus_id: water_meter
    name: "Meter battery"
    dif: 0x02
    vif: 0x7D
    vif_ext: 0x7D17
    function: instantaneous
    bit: 4
    device_class: battery
```

### Matching rules

- `dif`, `vif` and `function` are always compared and must match exactly. `function` defaults to
  `instantaneous` if you don't set it, so there's no wildcard for it — set it explicitly if a record uses
  `maximum`, `minimum`, or `value_during_error`.
- `vif_ext`, `storage`, `tariff`, `subunit` and `vife` are optional. If you **set** one in YAML, the
  record's value for it must match exactly. If you **leave it unset**, it's ignored — the entity matches
  regardless of that field's value in the record.
- Every configured `sensor:`/`binary_sensor:`/`text_sensor:` entry that matches a record gets it; you can
  have more than one entity match the same record if that's useful.
- For `binary_sensor:`, `bit` selects which bit of the record's raw integer value to expose; it isn't part
  of the record matching itself, just which bit of an already-matched record to publish.

## Known Limitations / Roadmap

- No meter-, manufacturer-, or model-specific drivers, by design — see [Design
  Philosophy](#design-philosophy) above.
- C1 Format B and T1 need real captured regression fixtures; currently only validated with synthetic/
  standards-derived test vectors.
- Wired UART is implemented but frozen pending real hardware logs; response control field, status bits
  and non-zero signature handling still need hardening before first hardware use.
- TCP transport is only a conceptual placeholder in the architecture; it's not committed to being
  implemented, by the maintainer or anyone else.

## License

Same as upstream ESPHome: the [ESPHome License](https://github.com/esphome/esphome/blob/dev/LICENSE)
(MIT and GPL, split by part of the codebase).
