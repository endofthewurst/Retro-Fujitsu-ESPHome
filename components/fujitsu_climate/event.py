import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import event

from . import FujitsuClimate

CONF_FUJITSU_CLIMATE_ID = "fujitsu_climate_id"
CONF_CONTROL_CHANGE = "control_change"

# 4A.12 -- control-authority attribution. One event per attributed state change, so a
# repeated human change (e.g. temp-up pressed twice) fires twice -- a text sensor that
# stays "Human" would not retrigger an HA automation. The matching last_change_source /
# last_change_detail text sensors are published just BEFORE each event fires.
CHANGE_EVENT_TYPES = ["human", "ha"]

CONFIG_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_FUJITSU_CLIMATE_ID): cv.use_id(FujitsuClimate),
        cv.Optional(CONF_CONTROL_CHANGE): event.event_schema(
            event.Event,
            icon="mdi:account-switch",
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_FUJITSU_CLIMATE_ID])
    if CONF_CONTROL_CHANGE in config:
        ev = await event.new_event(config[CONF_CONTROL_CHANGE], event_types=CHANGE_EVENT_TYPES)
        cg.add(parent.set_change_event(ev))
