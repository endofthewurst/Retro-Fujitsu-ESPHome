import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor

from . import FujitsuClimate

CONF_FUJITSU_CLIMATE_ID = "fujitsu_climate_id"

CONFIG_SCHEMA = text_sensor.text_sensor_schema(
    entity_category="diagnostic",
    icon="mdi:pulse",
).extend(
    {
        cv.Required(CONF_FUJITSU_CLIMATE_ID): cv.use_id(FujitsuClimate),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_FUJITSU_CLIMATE_ID])
    sens = await text_sensor.new_text_sensor(config)
    cg.add(parent.set_bus_status_text_sensor(sens))