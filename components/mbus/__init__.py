import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_UPDATE_INTERVAL
from esphome.components import cc1101, sx126x, sx127x, uart

CODEOWNERS = ["@hn"]

DEPENDENCIES = []
MULTI_CONF = True

mbus_ns = cg.esphome_ns.namespace("mbus")
MBusComponent = mbus_ns.class_("MBusComponent", cg.Component)

CONF_DUMP_RECORDS = "dump_records"
CONF_ENCRYPTION_KEY = "encryption_key"
CONF_METER_ID = "meter_id"
CONF_RADIO_CC1101_ID = "radio_cc1101_id"
CONF_RADIO_SX126X_ID = "radio_sx126x_id"
CONF_RADIO_SX127X_ID = "radio_sx127x_id"
CONF_SECONDARY_ADDRESS = "secondary_address"
CONF_WIRED_UART_ID = "wired_uart_id"

SOURCE_KEYS = [
    CONF_RADIO_CC1101_ID,
    CONF_RADIO_SX126X_ID,
    CONF_RADIO_SX127X_ID,
    CONF_WIRED_UART_ID,
]


def validate_encryption_key(value):
    value = cv.string(value).replace(" ", "").replace(":", "").replace("-", "")
    if len(value) != 32:
        raise cv.Invalid("encryption_key must be 16 bytes / 32 hex characters")
    try:
        return [int(value[i : i + 2], 16) for i in range(0, len(value), 2)]
    except ValueError as err:
        raise cv.Invalid("encryption_key must contain only hex characters") from err


def validate_single_source(config):
    present = [key for key in SOURCE_KEYS if key in config]
    if len(present) != 1:
        raise cv.Invalid(
            "Exactly one M-Bus source is required, one of: "
            + ", ".join(SOURCE_KEYS)
        )
    if CONF_WIRED_UART_ID in config and CONF_SECONDARY_ADDRESS not in config:
        raise cv.Invalid("secondary_address is required with wired_uart_id")
    if CONF_WIRED_UART_ID not in config and CONF_SECONDARY_ADDRESS in config:
        raise cv.Invalid("secondary_address is only valid with wired_uart_id")
    if CONF_WIRED_UART_ID not in config and CONF_UPDATE_INTERVAL in config:
        raise cv.Invalid("update_interval is only valid with wired_uart_id")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(MBusComponent),
            cv.Optional(CONF_RADIO_CC1101_ID): cv.use_id(cc1101.CC1101Component),
            cv.Optional(CONF_RADIO_SX126X_ID): cv.use_id(sx126x.SX126x),
            cv.Optional(CONF_RADIO_SX127X_ID): cv.use_id(sx127x.SX127x),
            cv.Optional(CONF_WIRED_UART_ID): cv.use_id(uart.UARTComponent),
            cv.Optional(CONF_SECONDARY_ADDRESS): cv.hex_uint64_t,
            cv.Optional(CONF_UPDATE_INTERVAL): cv.update_interval,
            cv.Optional(CONF_ENCRYPTION_KEY): validate_encryption_key,
            cv.Optional(CONF_METER_ID): cv.hex_uint32_t,
            cv.Optional(CONF_DUMP_RECORDS, default=False): cv.boolean,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    validate_single_source,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_config_id(str(config[CONF_ID])))
    cg.add(var.set_dump_records(config[CONF_DUMP_RECORDS]))
    if CONF_ENCRYPTION_KEY in config:
        cg.add(var.set_encryption_key(*config[CONF_ENCRYPTION_KEY]))
    if CONF_METER_ID in config:
        cg.add(var.set_meter_id(config[CONF_METER_ID]))

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
        cg.add(var.set_secondary_address(config[CONF_SECONDARY_ADDRESS]))
        cg.add(var.set_wired_update_interval(config.get(CONF_UPDATE_INTERVAL, 60000)))
