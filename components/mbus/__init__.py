from esphome import automation
import esphome.codegen as cg
from esphome.components import cc1101, sx126x, sx127x, uart
from esphome.components.const import CONF_ON_RECEIVE
import esphome.config_validation as cv
from esphome.const import (
    CONF_DATA,
    CONF_ID,
    CONF_ON_ERROR,
    CONF_ON_SUCCESS,
    CONF_STORAGE,
    CONF_UPDATE_INTERVAL,
    PLATFORM_ESP32,
    PLATFORM_ESP8266,
    PLATFORM_HOST,
    PLATFORM_RP2040,
)
from esphome.core import CORE, ID
from esphome.cpp_generator import MockObj
from esphome.types import ConfigType

CODEOWNERS = ["@hn"]
DOMAIN = "mbus"
MULTI_CONF = True

mbus_ns = cg.esphome_ns.namespace("mbus")
MBusComponent = mbus_ns.class_("MBusComponent", cg.PollingComponent)

CONF_CI = "ci"
CONF_CONTINUE_ON_ERROR = "continue_on_error"
CONF_DIF = "dif"
CONF_DUMP_RECORDS = "dump_records"
CONF_ENCRYPTION_KEY = "encryption_key"
CONF_PARSE_RECORDS = "parse_records"
CONF_COMPACT_FRAMES = "compact_frames"
CONF_FUNCTION = "function"
CONF_MBUS_ID = "mbus_id"
CONF_MAX_RETRIES = "max_retries"
CONF_MAX_DATAGRAMS = "max_datagrams"
CONF_METER_ID = "meter_id"
CONF_PRIMARY_ADDRESS = "primary_address"
CONF_RADIO_CC1101_ID = "radio_cc1101_id"
CONF_RADIO_SX126X_ID = "radio_sx126x_id"
CONF_RADIO_SX127X_ID = "radio_sx127x_id"
CONF_SECONDARY_ADDRESS = "secondary_address"
CONF_SUBUNIT = "subunit"
CONF_TARIFF = "tariff"
CONF_VIF = "vif"
CONF_VIF_EXT = "vif_ext"
CONF_VIFE = "vife"
CONF_WAIT_FOR_RESPONSE = "wait_for_response"
CONF_WAKEUP_AFTER_IDLE = "wakeup_after_idle"
CONF_WIRED_UART_ID = "wired_uart_id"

SOURCE_KEYS = [
    CONF_RADIO_CC1101_ID,
    CONF_RADIO_SX126X_ID,
    CONF_RADIO_SX127X_ID,
    CONF_WIRED_UART_ID,
]


def validate_primary_address(value: object) -> int:
    # 0 is the address of unconfigured slaves, 1 to 250 are assignable, 254 is answered by every slave
    # (EN 13757-2:2018, 5.7.5)
    value = cv.int_range(min=0, max=254)(value)
    if 251 <= value <= 253:
        raise cv.Invalid(
            "primary_address must be 0 to 250 or 254; use secondary_address to select a meter"
        )
    return value


def validate_source_options(config: ConfigType) -> ConfigType:
    wired = CONF_WIRED_UART_ID in config
    # A wired instance has its meter by its address; the filter is for what a radio hears
    if wired and CONF_METER_ID in config:
        raise cv.Invalid(
            "meter_id is only valid with a radio; a wired meter is selected by its address"
        )
    # The formats of a compact frame are kept per instance, so the instance has to belong to one meter
    if config[CONF_COMPACT_FRAMES] and not wired and CONF_METER_ID not in config:
        raise cv.Invalid("compact_frames requires meter_id")
    addresses = [
        key for key in (CONF_PRIMARY_ADDRESS, CONF_SECONDARY_ADDRESS) if key in config
    ]
    if wired and len(addresses) != 1:
        raise cv.Invalid(
            "wired_uart_id requires exactly one of primary_address and secondary_address"
        )
    if not wired and addresses:
        raise cv.Invalid(f"{addresses[0]} is only valid with wired_uart_id")
    if CONF_WIRED_UART_ID not in config and CONF_UPDATE_INTERVAL in config:
        raise cv.Invalid("update_interval is only valid with wired_uart_id")
    if not wired and CONF_MAX_DATAGRAMS in config:
        raise cv.Invalid("max_datagrams is only valid with wired_uart_id")
    if CONF_WAKEUP_AFTER_IDLE in config:
        if not wired:
            raise cv.Invalid("wakeup_after_idle is only valid with wired_uart_id")
        # Every wake-up costs the meter battery, so the interval has to be a decision
        if CONF_UPDATE_INTERVAL not in config:
            raise cv.Invalid(
                "wakeup_after_idle requires update_interval to be set (never turns "
                "the cyclic readout off)"
            )
        # The wake-up changes the parity at runtime, which the other UARTs cannot do
        if not (CORE.is_esp32 or CORE.is_esp8266 or CORE.is_host):
            raise cv.Invalid("wakeup_after_idle is only supported on ESP32 and ESP8266")
    if CONF_WIRED_UART_ID in config and CONF_UPDATE_INTERVAL not in config:
        config = {**config, CONF_UPDATE_INTERVAL: cv.update_interval("60s")}
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(MBusComponent),
            cv.Optional(CONF_RADIO_CC1101_ID): cv.use_id(cc1101.CC1101Component),
            cv.Optional(CONF_RADIO_SX126X_ID): cv.use_id(sx126x.SX126x),
            cv.Optional(CONF_RADIO_SX127X_ID): cv.use_id(sx127x.SX127x),
            cv.Optional(CONF_WIRED_UART_ID): cv.use_id(uart.UARTComponent),
            cv.Optional(CONF_MAX_DATAGRAMS): cv.int_range(min=1, max=255),
            cv.Optional(CONF_WAKEUP_AFTER_IDLE): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_PRIMARY_ADDRESS): validate_primary_address,
            cv.Optional(CONF_SECONDARY_ADDRESS): cv.hex_uint64_t,
            cv.Optional(CONF_UPDATE_INTERVAL): cv.update_interval,
            cv.Optional(CONF_ENCRYPTION_KEY): cv.bind_key(name="encryption_key"),
            cv.Optional(CONF_METER_ID): cv.hex_uint32_t,
            cv.Optional(CONF_DUMP_RECORDS, default=False): cv.boolean,
            cv.Optional(CONF_COMPACT_FRAMES, default=False): cv.boolean,
            cv.Optional(CONF_ON_RECEIVE): automation.validate_automation({}),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    # Decryption needs PSA (ESP32) or BearSSL (ESP8266, RP2040). The host platform is there for
    # tests and has neither: a key there needs BearSSL added to the build, see crypto.h.
    cv.only_on([PLATFORM_ESP32, PLATFORM_ESP8266, PLATFORM_HOST, PLATFORM_RP2040]),
    cv.has_exactly_one_key(*SOURCE_KEYS),
    validate_source_options,
)

# Records are matched by these fields; dif and vif are required, the others only match when set. function and
# the lowest bit of storage repeat what the DIF says, so they are never needed, only allowed.
RECORD_FUNCTIONS = {
    "instantaneous": 0,
    "maximum": 1,
    "minimum": 2,
    "value_during_error": 3,
}

RECORD_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_MBUS_ID): cv.use_id(MBusComponent),
        cv.Required(CONF_DIF): cv.hex_uint8_t,
        cv.Required(CONF_VIF): cv.hex_uint8_t,
        cv.Optional(CONF_VIF_EXT): cv.hex_uint16_t,
        cv.Optional(CONF_VIFE): cv.ensure_list(cv.hex_uint8_t),
        cv.Optional(CONF_STORAGE): cv.uint32_t,
        cv.Optional(CONF_TARIFF): cv.uint32_t,
        cv.Optional(CONF_SUBUNIT): cv.uint32_t,
        cv.Optional(CONF_FUNCTION): cv.enum(RECORD_FUNCTIONS, lower=True),
    }
)


def validate_record_matcher(config: ConfigType) -> ConfigType:
    """Reject a matcher that contradicts its own DIF and so could never match."""
    # The DIF carries the function field in bits 4 and 5 and the lowest bit of the storage number in bit 6
    # (EN 13757-3:2018, 6.3)
    dif = config[CONF_DIF]
    function = (dif >> 4) & 0x03
    if CONF_FUNCTION in config and RECORD_FUNCTIONS[config[CONF_FUNCTION]] != function:
        name = next(key for key, value in RECORD_FUNCTIONS.items() if value == function)
        raise cv.Invalid(
            f"dif 0x{dif:02X} says function {name}, which contradicts function "
            f"{config[CONF_FUNCTION]}",
            path=[CONF_FUNCTION],
        )
    if (storage := config.get(CONF_STORAGE)) is not None and storage & 1 != (
        dif >> 6
    ) & 1:
        raise cv.Invalid(
            f"dif 0x{dif:02X} says the storage number is {'odd' if dif & 0x40 else 'even'}, "
            f"which contradicts storage {storage}",
            path=[CONF_STORAGE],
        )
    return config


async def register_record_matcher(var: MockObj, config: ConfigType) -> None:
    """Apply the RECORD_SCHEMA fields to a matcher entity and register it with its hub."""
    cg.add(var.set_dif(config[CONF_DIF]))
    cg.add(var.set_vif(config[CONF_VIF]))
    cg.add_optional(var.set_function, config.get(CONF_FUNCTION))
    cg.add_optional(var.set_vif_ext, config.get(CONF_VIF_EXT))
    if (vife := config.get(CONF_VIFE)) is not None:
        cg.add(var.set_vife(vife))
    cg.add_optional(var.set_storage, config.get(CONF_STORAGE))
    cg.add_optional(var.set_tariff, config.get(CONF_TARIFF))
    cg.add_optional(var.set_subunit, config.get(CONF_SUBUNIT))
    parent = await cg.get_variable(config[CONF_MBUS_ID])
    cg.add(parent.register_record_listener(var))


# 8E1 is fixed (EN 13757-2:2018, 5.4), the baud rate is not (5.2). The pins are not claimed: several
# instances may share one bus.
_WIRED_UART_SCHEMA = uart.final_validate_device_schema(
    "mbus", uart_bus=CONF_WIRED_UART_ID, data_bits=8, parity="EVEN", stop_bits=1
)


def _final_validate(config: ConfigType) -> ConfigType:
    if CONF_WIRED_UART_ID in config:
        _WIRED_UART_SCHEMA(config)
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_dump_records(config[CONF_DUMP_RECORDS]))
    if (key := config.get(CONF_ENCRYPTION_KEY)) is not None:
        cg.add_define("USE_MBUS_ENCRYPTION_KEY")
        cg.add(var.set_encryption_key(*bytes.fromhex(key)))
    cg.add_optional(var.set_meter_id, config.get(CONF_METER_ID))
    if config[CONF_COMPACT_FRAMES]:
        cg.add_define("USE_MBUS_COMPACT_FRAMES")
        cg.add(var.set_compact_frames())

    for conf in config.get(CONF_ON_RECEIVE, []):
        await automation.build_callback_automation(
            var,
            "add_on_receive_callback",
            [
                *_HANDLER_ARGS,
                (cg.float_, "rssi"),
                (cg.float_, "freq_offset"),
                (cg.float_, "lqi"),
            ],
            conf,
        )

    # The radio framing is only built for a configuration that has a radio
    if CONF_WIRED_UART_ID not in config:
        cg.add_define("USE_MBUS_RADIO")
    if CONF_RADIO_CC1101_ID in config:
        cg.add_define("USE_MBUS_CC1101")
        radio = await cg.get_variable(config[CONF_RADIO_CC1101_ID])
        cg.add(var.set_radio_cc1101(radio))
    if CONF_RADIO_SX126X_ID in config:
        cg.add_define("USE_MBUS_SX126X")
        radio = await cg.get_variable(config[CONF_RADIO_SX126X_ID])
        cg.add(var.set_radio_sx126x(radio))
    if CONF_RADIO_SX127X_ID in config:
        cg.add_define("USE_MBUS_SX127X")
        radio = await cg.get_variable(config[CONF_RADIO_SX127X_ID])
        cg.add(var.set_radio_sx127x(radio))
    if CONF_WIRED_UART_ID in config:
        cg.add_define("USE_MBUS_WIRED_UART")
        wired_uart = await cg.get_variable(config[CONF_WIRED_UART_ID])
        cg.add(var.set_wired_uart(wired_uart))
        cg.add_optional(var.set_max_datagrams, config.get(CONF_MAX_DATAGRAMS))
        cg.add_optional(var.set_wakeup_after_idle, config.get(CONF_WAKEUP_AFTER_IDLE))
        cg.add_optional(var.set_primary_address, config.get(CONF_PRIMARY_ADDRESS))
        cg.add_optional(var.set_secondary_address, config.get(CONF_SECONDARY_ADDRESS))


MBusResult = mbus_ns.enum("MBusResult", is_class=True)
SendNkeAction = mbus_ns.class_("SendNkeAction", automation.Action)
RequestUd2Action = mbus_ns.class_("RequestUd2Action", automation.Action)
SendUdAction = mbus_ns.class_("SendUdAction", automation.Action)

_BYTE_SPAN = cg.std_span.template(cg.uint8.operator("const"))
# Variables of on_receive, on_success and on_error
_HANDLER_ARGS = [
    (MBusResult, "result"),
    (cg.uint8, "ci"),
    (_BYTE_SPAN, "x"),
    (_BYTE_SPAN, "data"),
]
# A long frame carries up to 252 bytes behind the CI field (EN 13757-2:2018, 5.7)
MAX_DATA_LENGTH = 252


def _validate_datagram_action(config: ConfigType) -> ConfigType:
    if not config[CONF_WAIT_FOR_RESPONSE] and not config[CONF_CONTINUE_ON_ERROR]:
        raise cv.Invalid(
            f"'{CONF_CONTINUE_ON_ERROR}' cannot be false if '{CONF_WAIT_FOR_RESPONSE}' "
            "is false, because the automation does not wait for the result",
            path=[CONF_CONTINUE_ON_ERROR],
        )
    return config


_DATAGRAM_ACTION_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(MBusComponent),
        cv.Optional(CONF_WAIT_FOR_RESPONSE, default=True): cv.boolean,
        cv.Optional(CONF_CONTINUE_ON_ERROR, default=True): cv.boolean,
        cv.Optional(CONF_ON_SUCCESS): automation.validate_action_list,
        cv.Optional(CONF_ON_ERROR): automation.validate_action_list,
    }
)

# Repeating SND_NKE or REQ_UD2 does no harm, so they get the two repetitions of EN 1434-3:2008, Annex D.2.1
_SEND_NKE_SCHEMA = _DATAGRAM_ACTION_SCHEMA.extend(
    {cv.Optional(CONF_MAX_RETRIES, default=2): cv.int_range(min=0, max=10)}
)
_SIMPLE_DATAGRAM_ACTION_SCHEMA = automation.maybe_simple_id(
    cv.All(_SEND_NKE_SCHEMA, _validate_datagram_action)
)

# parse_records: false hands the answer to on_success without reading it as records, for data a meter sends
# without marking it as manufacturer specific
_REQUEST_UD2_ACTION_SCHEMA = automation.maybe_simple_id(
    cv.All(
        _SEND_NKE_SCHEMA.extend(
            {cv.Optional(CONF_PARSE_RECORDS, default=True): cv.boolean}
        ),
        _validate_datagram_action,
    )
)

_SEND_UD_ACTION_SCHEMA = cv.All(
    _DATAGRAM_ACTION_SCHEMA.extend(
        {
            cv.Required(CONF_CI): cv.templatable(cv.hex_uint8_t),
            cv.Optional(CONF_DATA, default=[]): cv.templatable(
                cv.All(cv.ensure_list(cv.hex_uint8_t), cv.Length(max=MAX_DATA_LENGTH))
            ),
            cv.Optional(CONF_MAX_RETRIES, default=0): cv.int_range(min=0, max=10),
        }
    ),
    _validate_datagram_action,
)


async def _datagram_action_to_code(
    config: ConfigType,
    action_id: ID,
    template_arg: cg.TemplateArguments,
    args: list[tuple],
) -> MockObj:
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    # Both default to true in C++ as well
    if not config[CONF_WAIT_FOR_RESPONSE]:
        cg.add(var.set_wait_for_response(False))
    if not config[CONF_CONTINUE_ON_ERROR]:
        cg.add(var.set_continue_on_error(False))
    if config[CONF_MAX_RETRIES]:
        cg.add(var.set_max_retries(config[CONF_MAX_RETRIES]))
    # Only mbus.request_ud2 has it, with true as the default in C++ as well
    if not config.get(CONF_PARSE_RECORDS, True):
        cg.add(var.set_parse_records(False))
    # A handler inside another handler sees its own variables; those of the outer one keep their place in the
    # argument list under another name
    names = {name for _, name in _HANDLER_ARGS}
    handler_template_arg = cg.TemplateArguments(
        *template_arg, *(type_ for type_, _ in _HANDLER_ARGS)
    )
    handler_args = [
        *((type_, f"outer_{name}" if name in names else name) for type_, name in args),
        *_HANDLER_ARGS,
    ]
    for key, method in (
        (CONF_ON_SUCCESS, var.add_on_success),
        (CONF_ON_ERROR, var.add_on_error),
    ):
        if handlers := config.get(key):
            actions = await automation.build_action_list(
                handlers, handler_template_arg, handler_args
            )
            cg.add(method(actions))
    return var


@automation.register_action(
    "mbus.send_nke", SendNkeAction, _SIMPLE_DATAGRAM_ACTION_SCHEMA, synchronous=False
)
@automation.register_action(
    "mbus.request_ud2",
    RequestUd2Action,
    _REQUEST_UD2_ACTION_SCHEMA,
    synchronous=False,
)
async def datagram_action_to_code(
    config: ConfigType,
    action_id: ID,
    template_arg: cg.TemplateArguments,
    args: list[tuple],
) -> MockObj:
    return await _datagram_action_to_code(config, action_id, template_arg, args)


@automation.register_action(
    "mbus.send_ud", SendUdAction, _SEND_UD_ACTION_SCHEMA, synchronous=False
)
async def send_ud_action_to_code(
    config: ConfigType,
    action_id: ID,
    template_arg: cg.TemplateArguments,
    args: list[tuple],
) -> MockObj:
    var = await _datagram_action_to_code(config, action_id, template_arg, args)
    ci = await cg.templatable(config[CONF_CI], args, cg.uint8)
    cg.add(var.set_ci(ci))
    await automation.templatable_bytes(
        config[CONF_DATA],
        args,
        var.set_data_template,
        var.set_data_static,
        "mbus_send_ud_data",
    )
    return var
