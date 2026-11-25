import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.types import ConfigType

from .. import RECORD_SCHEMA, mbus_ns, register_record_matcher, validate_record_matcher

DEPENDENCIES = ["mbus"]

CONF_BIT = "bit"

MBusBinarySensor = mbus_ns.class_(
    "MBusBinarySensor", binary_sensor.BinarySensor, cg.Component
)

CONFIG_SCHEMA = cv.All(
    binary_sensor.binary_sensor_schema(MBusBinarySensor)
    .extend(RECORD_SCHEMA)
    .extend({cv.Required(CONF_BIT): cv.int_range(min=0, max=63)})
    .extend(cv.COMPONENT_SCHEMA),
    validate_record_matcher,
)


async def to_code(config: ConfigType) -> None:
    var = await binary_sensor.new_binary_sensor(config)
    await cg.register_component(var, config)
    cg.add(var.set_bit(config[CONF_BIT]))
    await register_record_matcher(var, config)
