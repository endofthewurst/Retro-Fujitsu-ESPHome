import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor

from . import FujitsuClimate

CONF_FUJITSU_CLIMATE_ID = "fujitsu_climate_id"
CONF_BUS_STATUS = "bus_status"
CONF_CORRECTED_MODE = "corrected_mode"
CONF_CORRECTED_FAN_RAW = "corrected_fan_raw"
CONF_CORRECTED_SETPOINT = "corrected_setpoint"
CONF_CORRECTED_ROOM_TEMP = "corrected_room_temp"
CONF_CORRECTED_ECONOMY = "corrected_economy"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_FUJITSU_CLIMATE_ID): cv.use_id(FujitsuClimate),
        cv.Optional(CONF_BUS_STATUS): text_sensor.text_sensor_schema(
            entity_category="diagnostic",
            icon="mdi:pulse",
        ),
        cv.Optional(CONF_CORRECTED_MODE): text_sensor.text_sensor_schema(
            entity_category="diagnostic",
            icon="mdi:flask-outline",
        ),
        cv.Optional(CONF_CORRECTED_FAN_RAW): text_sensor.text_sensor_schema(
            entity_category="diagnostic",
            icon="mdi:flask-outline",
        ),
        cv.Optional(CONF_CORRECTED_SETPOINT): text_sensor.text_sensor_schema(
            entity_category="diagnostic",
            icon="mdi:flask-outline",
        ),
        cv.Optional(CONF_CORRECTED_ROOM_TEMP): text_sensor.text_sensor_schema(
            entity_category="diagnostic",
            icon="mdi:flask-outline",
        ),
        cv.Optional(CONF_CORRECTED_ECONOMY): text_sensor.text_sensor_schema(
            entity_category="diagnostic",
            icon="mdi:flask-outline",
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_FUJITSU_CLIMATE_ID])
    if CONF_BUS_STATUS in config:
        sens = await text_sensor.new_text_sensor(config[CONF_BUS_STATUS])
        cg.add(parent.set_bus_status_text_sensor(sens))
    if CONF_CORRECTED_MODE in config:
        sens = await text_sensor.new_text_sensor(config[CONF_CORRECTED_MODE])
        cg.add(parent.set_corrected_mode_text_sensor(sens))
    if CONF_CORRECTED_FAN_RAW in config:
        sens = await text_sensor.new_text_sensor(config[CONF_CORRECTED_FAN_RAW])
        cg.add(parent.set_corrected_fan_raw_text_sensor(sens))
    if CONF_CORRECTED_SETPOINT in config:
        sens = await text_sensor.new_text_sensor(config[CONF_CORRECTED_SETPOINT])
        cg.add(parent.set_corrected_setpoint_text_sensor(sens))
    if CONF_CORRECTED_ROOM_TEMP in config:
        sens = await text_sensor.new_text_sensor(config[CONF_CORRECTED_ROOM_TEMP])
        cg.add(parent.set_corrected_room_temp_text_sensor(sens))
    if CONF_CORRECTED_ECONOMY in config:
        sens = await text_sensor.new_text_sensor(config[CONF_CORRECTED_ECONOMY])
        cg.add(parent.set_corrected_economy_text_sensor(sens))
