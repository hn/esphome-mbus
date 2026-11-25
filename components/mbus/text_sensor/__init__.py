import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.types import ConfigType

from .. import RECORD_SCHEMA, mbus_ns, register_record_matcher, validate_record_matcher

DEPENDENCIES = ["mbus"]

MBusTextSensor = mbus_ns.class_("MBusTextSensor", text_sensor.TextSensor, cg.Component)

CONFIG_SCHEMA = cv.All(
    text_sensor.text_sensor_schema(MBusTextSensor)
    .extend(RECORD_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA),
    validate_record_matcher,
)


async def to_code(config: ConfigType) -> None:
    var = await text_sensor.new_text_sensor(config)
    await cg.register_component(var, config)
    await register_record_matcher(var, config)
