import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor

from . import FujitsuClimate

CONF_FUJITSU_CLIMATE_ID = "fujitsu_climate_id"

CONFIG_SCHEMA = binary_sensor.binary_sensor_schema(
    device_class="connectivity",
    entity_category="diagnostic",
).extend(
    {
        cv.Required(CONF_FUJITSU_CLIMATE_ID): cv.use_id(FujitsuClimate),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_FUJITSU_CLIMATE_ID])
    sens = await binary_sensor.new_binary_sensor(config)
    cg.add(parent.set_bus_alive_binary_sensor(sens))