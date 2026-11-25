# esphome-mbus

A generic, transport-independent M-Bus / wM-Bus [external component](https://esphome.io/components/external_components/)
for [ESPHome](https://esphome.io/): wireless (CC1101, SX126x, SX127x) and wired (UART) M-Bus in one
component core, with a full DIF/VIF/VIFE record dump instead of built-in per-meter drivers.

> [!NOTE]
> **Project Status:** Early stage. Wireless C1 Format A with security modes 5 and 7 is hardware-validated
> against a real WaterStarM water meter, over CC1101, SX126x, and SX127x; wired M-Bus is validated over the
> same meter's optical (infrared) interface. Everything else (C1 Format B, compact frames, the two-wire
> bus) is implemented but not yet validated against real hardware — see
> [Supported Transports](#supported-transports--validation-status) below.

> [!TIP]
> **Help wanted:** I only have a few meters to test with. If you try this with your M-Bus hardware, please
> open an issue and tell me how it went — whether it worked or not.

Please do me a favor: :thumbsup: If you use any information or code you find here, please link back to this page.
:star: Also, please consider to star this project. I really like to keep track of who is using this to do creative things, especially if you are from other parts of the world.
:smiley: You are welcome to open an issue to report on your personal success project and share it with others.

## Design Philosophy

Other (w)M-Bus options exist for ESPHome; this one is built around a few specific choices:

- **Reuses existing ESPHome hardware components.** Wireless (CC1101, SX126x, SX127x) and wired (UART, via
  an external transceiver) all go through ESPHome's own radio/UART components — no custom hardware
  drivers to maintain.
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
wired UART (two-wire / infrared)      >--  normalized frame  -->  application layer  -->  records  -->  entities
(TCP, conceptual only)               /
```

The `mbus` component picks exactly one source per instance (`radio_cc1101_id`, `radio_sx126x_id`,
`radio_sx127x_id`, or `wired_uart_id`); multiple `mbus:` instances are supported (e.g. several meters on
one link). See [`components/mbus`](components/mbus) for the implementation.

## Supported Transports & Validation Status

### Wireless (CC1101, SX126x, SX127x)

All three radios go through the same transport-independent wM-Bus framing/decoding, so what's
implemented doesn't vary by chip — only what's actually been validated against real hardware does:

| wM-Bus mode/format | Validated against real hardware |
|---|---|
| C1 Format A | Yes — WaterStarM water meter, on all three radios |
| C1 Format B | No — host-side test vectors only |
| T1 | Yes — WaterStarM water meter, on SX127x |

SX126x is additionally hardware-capped at 255 bytes on the air interface — fine for most C1 telegrams,
but likely not enough for T1 (3-of-6 encoding inflates the on-air size). CC1101 and SX127x have no such
cap.

### Security and Application Layer

Decryption and the application layer are the same for every transport:

| Feature | Validated against real hardware |
|---|---|
| Security mode 5 (AES-128 CBC), short header | Yes — WaterStarM water meter, on all three radios |
| Security mode 7 (AES-128 CBC with key derivation, authenticated by the AFL) | Yes — WaterStarM water meter (OMS 4), on all three radios |
| Security modes 5 and 7 behind a long header, partial encryption | No — host-side test vectors only |
| Encrypted Extended Link Layer (AES-128 counter mode) | No — host-side test vectors only |
| Compact frames (`compact_frames: true`) | No — host-side test vectors only |

With an `encryption_key`, a datagram is only read if it is encrypted, or in mode 7 at least authenticated,
with that key; unprotected data from the same meter is ignored. The one exception is an application error
(CI `6Eh`, `6Fh`, `70h`): it is accepted unencrypted and reported as `application error`. This follows the
M-Bus standard (EN 13757-7), which has the meter answer in plain text after a security error. Such a
datagram carries no meter values. Without an `encryption_key`, no AES code is built into the firmware.

### Wired / Other

| Transport | Implemented | Validated against real hardware |
|---|---|---|
| Wired M-Bus over UART, optical (infrared) interface | Yes | Yes — WaterStarM water meter, primary address |
| Wired M-Bus over UART, two-wire bus with a transceiver | Yes | No — simulated meter only |
| TCP (M-Bus/wM-Bus gateway) | No, conceptual only, not committed to | — |

## Bundled radio component patches

This repository temporarily vendors patched copies of two ESPHome core components — `cc1101` under
[`components/cc1101`](components/cc1101) and `sx127x` under [`components/sx127x`](components/sx127x) —
because `mbus` needs long-packet RX support (`packet_length`/`payload_length` above 64 bytes, or given
as a lambda for protocols whose length isn't known upfront) that no ESPHome release has yet for either chip.
The copies are based on ESPHome 2026.11 and need that version or newer:

- `cc1101` — long-packet RX — [esphome/esphome#17578](https://github.com/esphome/esphome/pull/17578),
  merged, expected in ESPHome 2026.12.
- `sx127x` — long-packet RX plus real RSSI for FSK/OOK —
  [esphome/esphome#18547](https://github.com/esphome/esphome/pull/18547), pending review; plus the
  `afc_bandwidth` option — [esphome/esphome#22300](https://github.com/esphome/esphome/pull/22300), merged,
  expected in ESPHome 2026.12.

Once a release contains the change for your chip, `mbus` works against core directly, and you can drop
that component from `components:` in your config. The `sx127x` long-packet patch is still pending review
and may be rejected, changed significantly, or take a while — there's no guarantee it'll be merged as-is,
or at all. Until then, the vendored copy remains the way to get long-packet RX on the SX127x.

They also take the simplest approach available for now, following ESPHome's existing conventions rather
than introducing a new pattern; a preferred approach for latency-sensitive radio RX, if the core
maintainers recommend one, may be adopted later. The RX FIFO is drained from ESPHome's cooperative
`loop()`: a GPIO interrupt on the FIFO-threshold pin wakes the main loop immediately via
`enable_loop_soon_any_context()`, rather than waiting for the regular loop interval. That's the best
available within ESPHome's scheduling model, but it's not a hard real-time guarantee — another component
blocking its own `loop()` for long enough could still cause a FIFO overrun (detected and recovered from,
not silently corrupted). Hardware-validated reliably at up to 100 kbit/s (wM-Bus mode C1); see
[this discussion on esphome/esphome#18547](https://github.com/esphome/esphome/pull/18547#issuecomment-5366822112)
for more background.

## Configuration Example

`mbus` builds for ESP32, ESP8266 and RP2040; so far it has only been tested on real ESP32 hardware.
`encryption_key` is the meter's AES key as 32 hex characters.

```yaml
external_components:
  - source: github://hn/esphome-mbus@main
    components: [mbus, cc1101]  # drop cc1101 once it's no longer needed, see above

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
    # encryption_key: !secret meter_aes_key  # for encrypted (mode 5 or mode 7) meters
    # compact_frames: true  # optional: read compact frames, needs meter_id on a radio

sensor:
  - platform: mbus
    mbus_id: water_meter
    name: "Total water consumption"
    dif: 0x04
    vif: 0x13
    unit_of_measurement: "m³"
    device_class: water
    state_class: total_increasing
    accuracy_decimals: 3
    filters:
      - multiply: 0.001
```

Instead of `radio_cc1101_id`, you can use `radio_sx126x_id` or `radio_sx127x_id` with a configured
`sx126x`/`sx127x` radio component.

Some meters (Kamstrup and others) send most of their data as compact frames: the record headers come once
in a full frame, and the following datagrams carry the values only. `compact_frames: true` reads them
once a full frame has been seen. On a radio this needs `meter_id`, because the learned format belongs to
one meter.

### Wired M-Bus Example

Wired M-Bus over UART works the same way, just with a different source and no radio-specific settings.
The meter is addressed either by `secondary_address` or by `primary_address` (`0`-`250`, or `254` if it
is the only meter on the bus); `update_interval` controls how often it's polled (defaults to `60s`). If
the meter announces more datagrams, up to `max_datagrams` of them are read per readout (defaults to `8`).
The UART has to be set to 8 data bits, even parity and 1 stop bit:

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

Wired M-Bus needs an external M-Bus master transceiver between the UART pins and the physical
two-wire bus — it supplies the bus's 24-36V power and converts between the bus's current-loop modulation
and standard UART logic levels. `esphome-mbus` only speaks the resulting byte stream on top of that; it
doesn't provide or replace the transceiver itself.

### Infrared M-Bus Example

Many meters also speak M-Bus over their optical interface. That is the wired protocol over an infrared
LED and a photodiode on the UART pins (again 8 data bits, even parity, 1 stop bit), plus a wake-up before
the first datagram:

```yaml
mbus:
  - id: water_meter
    wired_uart_id: ir_uart
    primary_address: 254
    # Optical readouts cost the meter battery, and some meters limit them per day
    update_interval: 24h
    # The optical interface sleeps to save battery: after this long without traffic, the next request
    # starts with a wake-up sequence (2.2 s of 0x55). Needs ESP32 or ESP8266.
    wakeup_after_idle: 10s
    dump_records: true
```

`sensor:`/`binary_sensor:`/`text_sensor:` entries work as with any other source. To read the meter on
demand instead, set `update_interval: never` and use the `mbus.request_ud2` action:

```yaml
button:
  - platform: template
    name: "Read meter"
    on_press:
      - mbus.request_ud2: water_meter
```

### Automations

`on_receive` on an `mbus:` instance fires for every valid datagram the instance accepts, after the
entities have been updated: with a `meter_id`, or on a wired link, only those of its meter, otherwise
those of every meter in range.

```yaml
mbus:
  - id: water_meter
    radio_cc1101_id: cc1101_radio
    on_receive:
      - lambda: |-
          ESP_LOGI("main", "%s, CI=0x%02X, %u bytes, rssi=%.0f", LOG_STR_ARG(mbus::mbus_result_to_str(result)),
                   ci, (unsigned) x.size(), rssi);
```

The wired actions `mbus.send_nke`, `mbus.request_ud2` and `mbus.send_ud` take `on_success` and `on_error`,
and `max_retries` (default `2`, for `mbus.send_ud` `0`). `mbus.request_ud2` with `parse_records: false`
decrypts the answer without reading it as records.

```yaml
button:
  - platform: template
    name: "Read meter"
    on_press:
      - mbus.request_ud2:
          id: water_meter
          on_success:
            - logger.log: "Meter read"
          on_error:
            - lambda: |-
                ESP_LOGW("main", "Readout failed: %s", LOG_STR_ARG(mbus::mbus_result_to_str(result)));
```

All three handlers have the same variables:

| Variable | Content |
|---|---|
| `result` | what became of the datagram or the exchange (`mbus::MBusResult`), see below |
| `ci` | the CI field that `data` belongs to; `0` without a datagram |
| `x` | the datagram as received: on a radio from the L field on without CRCs, on a wired link as on the wire |
| `data` | everything behind the last header that could be read |

`on_receive` also has `rssi`, `freq_offset` and `lqi` as `float`, `NAN` where the source has no such value.
`mbus::manufacturer_data(data)` and `mbus::application_error_code(data)` help with `data`.

| `result` | Meaning | `on_receive` | Action |
|---|---|---|---|
| `ok` | read as requested; `data` is plain application data | yes | `on_success` |
| `ack` | the meter acknowledged with a single character | yes | `on_success` |
| `security error` | not decrypted or not verified: no key, wrong key, wrong MAC, unprotected data although a key is set | yes | `on_error` |
| `not decoded` | plain data that was not read, or not to its end: CI field not implemented, compact frame with an unknown format | yes | `on_error` |
| `application error` | the meter reports an error; `data` holds its error bytes | yes | `on_error` |
| `invalid response` | a valid datagram that is not the expected answer, or an invalid one | first case only | `on_error` |
| `no response` | the meter did not answer | no | `on_error` |
| `busy` | the instance or its UART is in use | no | `on_error` |
| `invalid request` | the datagram cannot be sent | no | `on_error` |

### Complete Examples: WaterStarM Water Meter

[`watermeter-waterstarm-cc1101.yaml`](watermeter-waterstarm-cc1101.yaml),
[`watermeter-waterstarm-sx126x.yaml`](watermeter-waterstarm-sx126x.yaml) and
[`watermeter-waterstarm-sx127x.yaml`](watermeter-waterstarm-sx127x.yaml) are full, ready-to-flash
configurations for the same real meter (WaterStarM M-ETH Q3 2.5), one per radio chip — the meter this
component was originally hardware-validated against. Besides the radio and `mbus:` config, each
includes working `sensor:`/`binary_sensor:` entities, so any of them doubles as a complete reference for
wiring everything else in this README together into a real device.

- [`watermeter-waterstarm-cc1101.yaml`](watermeter-waterstarm-cc1101.yaml) — bare CC1101 breakout board.
- [`watermeter-waterstarm-sx126x.yaml`](watermeter-waterstarm-sx126x.yaml) — Heltec WiFi LoRa 32 board
  (not a bare breakout). Hardware-capped at 255 bytes on the air interface, see
  [Supported Transports](#supported-transports--validation-status) — may not be enough for longer/T1
  telegrams.
- [`watermeter-waterstarm-sx127x.yaml`](watermeter-waterstarm-sx127x.yaml) — bare SX1276 breakout board.

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
[D][mbus:034]: Received mode C format A frame: bytes=219 expected=219 L=190 rssi=-67 lqi=0 freq_offset=-3174, CRC ok
[D][mbus:164]: Link: C=0x44 (SND-NR) M=DWZ ID=0x12345678 Version=0x02 DevType=0x07
[D][mbus:497]: Layers: ELL CI=0x8C, AFL FCL=0x2C00 AuthType=5 Counter=543 MacBytes=8
[D][mbus:237]: Header: CI=0x7A AccessNr=43 Status=0x00 Config=0x0790 Mode=7 EncryptedBytes=144
[D][mbus:328]: Security: mode 7, MAC ok, decryption ok
[D][mbus:118]: Record: dif=0x04 vif=0x13 storage=0 tariff=0 subunit=0 function=instantaneous pos=8 vif_raw=0x13 dife=[] vife=[] data_type=int32 raw=A7.BF.01.00 raw_value=114599; decoded: scale=0.001 unit=m3 value=114.599
[D][mbus:118]: Record: dif=0x44 vif=0x13 storage=1 tariff=0 subunit=0 function=instantaneous pos=14 vif_raw=0x13 dife=[] vife=[0x3C] data_type=int32 raw=0C.00.00.00 flags=backward_flow raw_value=12; decoded: scale=0.001 unit=m3 value=0.012
[D][mbus:118]: Record: dif=0x04 vif=0x6D storage=0 tariff=0 subunit=0 function=instantaneous pos=20 vif_raw=0x6D dife=[] vife=[] data_type=datetime_type_f raw=1E.0E.0F.33; decoded: value=2024-03-15T14:30
[D][mbus:118]: Record: dif=0x02 vif=0x7D storage=0 tariff=0 subunit=0 function=instantaneous pos=26 vif_raw=0xFD vif_ext=0x7D17 dife=[] vife=[] data_type=int16 raw=50.00 raw_value=80; decoded: scale=1 unit=flags value=80
```

The first record line, for example, turns directly into:

```yaml
sensor:
  - platform: mbus
    mbus_id: water_meter
    name: "Total water consumption"
    dif: 0x04
    vif: 0x13
    unit_of_measurement: "m³"
    device_class: water
    state_class: total_increasing
    accuracy_decimals: 3
    filters:
      - multiply: 0.001  # matches "decoded: scale=0.001" from the dump
```

The fourth record line, with its `vif_ext=0x7D17` flags/status word, turns into a `binary_sensor:` picking out
one bit — `raw_value=80` is `0b01010000`, so bit 4 is set:

```yaml
binary_sensor:
  - platform: mbus
    mbus_id: water_meter
    name: "Meter battery"
    dif: 0x02
    vif: 0x7D
    vif_ext: 0x7D17
    bit: 4
    device_class: battery
```

### Matching rules

- `dif` and `vif` are always compared and must match exactly.
- `function` (`instantaneous`, `maximum`, `minimum`, `value_during_error`) and the lowest bit of `storage`
  are already part of the `dif`, so neither is ever needed to tell two records apart. You may still set
  them; a value that contradicts the `dif` is rejected when the configuration is validated.
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
- C1 Format B, compact frames, the encrypted Extended Link Layer and security modes 5 and 7 behind a long
  header have only run against test vectors, not against a real meter.
- Not implemented: a fragmented AFL, compact frames with partial encryption, format frames and alarm
  datagrams.
- SX126x is hardware-capped at 255 bytes on the air interface — enough for most C1 telegrams, but not
  necessarily for longer ones or T1 (3-of-6 encoding inflates the on-air size). This is a hardware limit,
  not a missing patch — there's no workaround.
- Wired M-Bus is only hardware-validated over the optical interface of one meter. Addressing by
  `secondary_address`, several meters on one bus and multi-telegram readouts have only run against a
  simulated meter so far.
- Wired M-Bus decodes the variable data structure only; the fixed data structure isn't supported, and
  there is no search for unknown meters on the bus.
- TCP transport is only a conceptual placeholder in the architecture; it's not committed to being
  implemented, by the maintainer or anyone else.

## License

Same as upstream ESPHome: the [ESPHome License](https://github.com/esphome/esphome/blob/dev/LICENSE)
(MIT and GPL, split by part of the codebase).
