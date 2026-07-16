import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from .. import MBusComponent, mbus_ns

DEPENDENCIES = ["mbus"]

CONF_DIF = "dif"
CONF_FUNCTION = "function"
CONF_MBUS_ID = "mbus_id"
CONF_STORAGE = "storage"
CONF_SUBUNIT = "subunit"
CONF_TARIFF = "tariff"
CONF_VIF = "vif"
CONF_VIF_EXT = "vif_ext"
CONF_VIFE = "vife"

MBusTextSensor = mbus_ns.class_("MBusTextSensor", text_sensor.TextSensor, cg.Component)

FUNCTIONS = {
    "instantaneous": 0,
    "maximum": 1,
    "minimum": 2,
    "value_during_error": 3,
}

CONFIG_SCHEMA = (
    text_sensor.text_sensor_schema(MBusTextSensor)
    .extend(
        {
            cv.GenerateID(CONF_MBUS_ID): cv.use_id(MBusComponent),
            cv.Required(CONF_DIF): cv.hex_uint8_t,
            cv.Required(CONF_VIF): cv.hex_uint8_t,
            cv.Optional(CONF_VIF_EXT): cv.hex_uint16_t,
            cv.Optional(CONF_VIFE): cv.ensure_list(cv.hex_uint8_t),
            cv.Optional(CONF_STORAGE): cv.uint32_t,
            cv.Optional(CONF_TARIFF): cv.uint32_t,
            cv.Optional(CONF_SUBUNIT): cv.uint32_t,
            cv.Optional(CONF_FUNCTION, default="instantaneous"): cv.enum(FUNCTIONS, lower=True),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await text_sensor.new_text_sensor(config)
    await cg.register_component(var, config)

    cg.add(var.set_dif(config[CONF_DIF]))
    cg.add(var.set_vif(config[CONF_VIF]))
    cg.add(var.set_function(config[CONF_FUNCTION]))
    if CONF_VIF_EXT in config:
        cg.add(var.set_vif_ext(config[CONF_VIF_EXT]))
    if CONF_VIFE in config:
        cg.add(var.set_vife(config[CONF_VIFE]))
    if CONF_STORAGE in config:
        cg.add(var.set_storage(config[CONF_STORAGE]))
    if CONF_TARIFF in config:
        cg.add(var.set_tariff(config[CONF_TARIFF]))
    if CONF_SUBUNIT in config:
        cg.add(var.set_subunit(config[CONF_SUBUNIT]))

    parent = await cg.get_variable(config[CONF_MBUS_ID])
    cg.add(parent.register_record_listener(var))
