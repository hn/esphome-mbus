import esphome.codegen as cg
from esphome.components import cc1101, sx126x, sx127x, uart
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_STORAGE, CONF_UPDATE_INTERVAL
from esphome.cpp_generator import MockObj
from esphome.types import ConfigType

CODEOWNERS = ["@hn"]
DOMAIN = "mbus"
MULTI_CONF = True

mbus_ns = cg.esphome_ns.namespace("mbus")
MBusComponent = mbus_ns.class_("MBusComponent", cg.Component)

CONF_DIF = "dif"
CONF_DUMP_RECORDS = "dump_records"
CONF_ENCRYPTION_KEY = "encryption_key"
CONF_FUNCTION = "function"
CONF_MBUS_ID = "mbus_id"
CONF_METER_ID = "meter_id"
CONF_RADIO_CC1101_ID = "radio_cc1101_id"
CONF_RADIO_SX126X_ID = "radio_sx126x_id"
CONF_RADIO_SX127X_ID = "radio_sx127x_id"
CONF_SECONDARY_ADDRESS = "secondary_address"
CONF_SUBUNIT = "subunit"
CONF_TARIFF = "tariff"
CONF_VIF = "vif"
CONF_VIF_EXT = "vif_ext"
CONF_VIFE = "vife"
CONF_WIRED_UART_ID = "wired_uart_id"

SOURCE_KEYS = [
    CONF_RADIO_CC1101_ID,
    CONF_RADIO_SX126X_ID,
    CONF_RADIO_SX127X_ID,
    CONF_WIRED_UART_ID,
]


def validate_source_options(config: ConfigType) -> ConfigType:
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
            cv.Optional(CONF_ENCRYPTION_KEY): cv.bind_key(name="encryption_key"),
            cv.Optional(CONF_METER_ID): cv.hex_uint32_t,
            cv.Optional(CONF_DUMP_RECORDS, default=False): cv.boolean,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.has_exactly_one_key(*SOURCE_KEYS),
    validate_source_options,
)

# Records are matched by these fields; dif and vif are required, the others only match when set.
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
        cv.Optional(CONF_FUNCTION, default="instantaneous"): cv.enum(
            RECORD_FUNCTIONS, lower=True
        ),
    }
)


async def register_record_matcher(var: MockObj, config: ConfigType) -> None:
    """Apply the RECORD_SCHEMA fields to a matcher entity and register it with its hub."""
    cg.add(var.set_dif(config[CONF_DIF]))
    cg.add(var.set_vif(config[CONF_VIF]))
    cg.add(var.set_function(config[CONF_FUNCTION]))
    cg.add_optional(var.set_vif_ext, config.get(CONF_VIF_EXT))
    if (vife := config.get(CONF_VIFE)) is not None:
        cg.add(var.set_vife(vife))
    cg.add_optional(var.set_storage, config.get(CONF_STORAGE))
    cg.add_optional(var.set_tariff, config.get(CONF_TARIFF))
    cg.add_optional(var.set_subunit, config.get(CONF_SUBUNIT))
    parent = await cg.get_variable(config[CONF_MBUS_ID])
    cg.add(parent.register_record_listener(var))


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_config_id(str(config[CONF_ID])))
    cg.add(var.set_dump_records(config[CONF_DUMP_RECORDS]))
    if (key := config.get(CONF_ENCRYPTION_KEY)) is not None:
        cg.add(var.set_encryption_key(*bytes.fromhex(key)))
    cg.add_optional(var.set_meter_id, config.get(CONF_METER_ID))

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
