import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.types import ConfigType

from .. import RECORD_SCHEMA, mbus_ns, register_record_matcher, validate_record_matcher

DEPENDENCIES = ["mbus"]

MBusSensor = mbus_ns.class_("MBusSensor", sensor.Sensor, cg.Component)

CONFIG_SCHEMA = cv.All(
    sensor.sensor_schema(MBusSensor).extend(RECORD_SCHEMA).extend(cv.COMPONENT_SCHEMA),
    validate_record_matcher,
)


async def to_code(config: ConfigType) -> None:
    var = await sensor.new_sensor(config)
    await cg.register_component(var, config)
    await register_record_matcher(var, config)
