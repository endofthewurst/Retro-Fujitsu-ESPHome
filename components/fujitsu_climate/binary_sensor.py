import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor

from . import FujitsuClimate

CONF_FUJITSU_CLIMATE_ID = "fujitsu_climate_id"
CONF_BUS_ALIVE = "bus_alive"
CONF_IN_SYNC = "in_sync"

# Restructured 2 Sep 2026 (from a single flat schema) to a nested schema, matching
# text_sensor.py's existing pattern -- needed so a second binary_sensor, "In Sync"
# (requested-vs-actual state sync tracking; see plan-to-completion.md's "Requested
# vs. actual state sync" design, dated 2 Sep 2026), could be added alongside the
# existing Bus Alive connectivity sensor without a second, oddly-shaped
# `- platform: fujitsu_climate` entry. bus_alive keeps its previous
# device_class/entity_category defaults exactly; in_sync gets its own (no
# device_class -- "in sync" isn't a connectivity/problem/etc. concept HA has a
# built-in class for).
CONFIG_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_FUJITSU_CLIMATE_ID): cv.use_id(FujitsuClimate),
        cv.Optional(CONF_BUS_ALIVE): binary_sensor.binary_sensor_schema(
            device_class="connectivity",
            entity_category="diagnostic",
        ),
        cv.Optional(CONF_IN_SYNC): binary_sensor.binary_sensor_schema(
            icon="mdi:sync",
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_FUJITSU_CLIMATE_ID])
    if CONF_BUS_ALIVE in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_BUS_ALIVE])
        cg.add(parent.set_bus_alive_binary_sensor(sens))
    if CONF_IN_SYNC in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_IN_SYNC])
        cg.add(parent.set_in_sync_binary_sensor(sens))