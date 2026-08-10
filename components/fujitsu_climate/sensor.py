import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import DEVICE_CLASS_TEMPERATURE, STATE_CLASS_MEASUREMENT, UNIT_CELSIUS

from . import FujitsuClimate

CONF_FUJITSU_CLIMATE_ID = "fujitsu_climate_id"
CONF_CORRECTED_SETPOINT = "corrected_setpoint"
CONF_CORRECTED_ROOM_TEMP = "corrected_room_temp"

# Added 11 Aug 2026, 3B.18 -- these were text_sensors ("22C" / "None") until now; the
# underlying protocol field is a plain integer number of whole degrees C (see the
# frame layout comment in FujiHeatPump.cpp's processCorrectedFrame()), so a proper
# numeric sensor with accuracy_decimals=0 is the correct representation and lets HA
# graph/history these instead of just displaying a string.
_TEMP_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_CELSIUS,
    accuracy_decimals=0,
    device_class=DEVICE_CLASS_TEMPERATURE,
    state_class=STATE_CLASS_MEASUREMENT,
    entity_category="diagnostic",
    icon="mdi:thermometer",
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_FUJITSU_CLIMATE_ID): cv.use_id(FujitsuClimate),
        cv.Optional(CONF_CORRECTED_SETPOINT): _TEMP_SCHEMA,
        cv.Optional(CONF_CORRECTED_ROOM_TEMP): _TEMP_SCHEMA,
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_FUJITSU_CLIMATE_ID])
    if CONF_CORRECTED_SETPOINT in config:
        sens = await sensor.new_sensor(config[CONF_CORRECTED_SETPOINT])
        cg.add(parent.set_corrected_setpoint_sensor(sens))
    if CONF_CORRECTED_ROOM_TEMP in config:
        sens = await sensor.new_sensor(config[CONF_CORRECTED_ROOM_TEMP])
        cg.add(parent.set_corrected_room_temp_sensor(sens))
