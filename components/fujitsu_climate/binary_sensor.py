import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor

from . import FujitsuClimate

CONF_FUJITSU_CLIMATE_ID = "fujitsu_climate_id"
CONF_BUS_ALIVE = "bus_alive"
CONF_IN_SYNC = "in_sync"
CONF_THERMO_ICON = "thermo_icon"
CONF_DEFROST_ACTIVE = "defrost_active"

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
        # 4 Oct 2026 (4A.9): byte 5 bit 7 of the frame addressed to us. On = the wired
        # controller's Thermo Sensor icon is shown (its own sensor in use).
        cv.Optional(CONF_THERMO_ICON): binary_sensor.binary_sensor_schema(
            entity_category="diagnostic",
            icon="mdi:thermometer-auto",
        ),
        # 17 Oct 2026 (4A.10): byte 2 bit 3 of the frame not addressed to us. On = the wired
        # controller's Defrost / Oil Recovery icon is shown. Debounced in FujitsuClimate.cpp.
        cv.Optional(CONF_DEFROST_ACTIVE): binary_sensor.binary_sensor_schema(
            icon="mdi:snowflake-melt",
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
    if CONF_THERMO_ICON in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_THERMO_ICON])
        cg.add(parent.set_thermo_icon_binary_sensor(sens))
    if CONF_DEFROST_ACTIVE in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_DEFROST_ACTIVE])
        cg.add(parent.set_defrost_binary_sensor(sens))
