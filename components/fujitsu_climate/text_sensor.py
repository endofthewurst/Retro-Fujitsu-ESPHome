import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor

from . import FujitsuClimate

CONF_FUJITSU_CLIMATE_ID = "fujitsu_climate_id"
CONF_BUS_STATUS = "bus_status"
CONF_THERMO_SENSOR = "thermo_sensor"

# Phase 4 rebuild: trimmed down to bus_status only -- corrected_mode/corrected_fan_raw/
# corrected_economy/mystery_bit/message_dest/message_type/boot_probe/frame_timing were
# all exploratory diagnostics for validating the OLD hand-rolled decode against
# upstream's field layout byte by byte. That validation is done (see
# tx-architecture-review-and-adoption-plan.md and upstream-comparison.md) and the
# vendored engine now drives the climate entity's mode/fan_mode/preset directly, so
# there's nothing left for those text sensors to cross-check against. bus_status
# remains because it's still the project's real safety-monitoring signal, independent
# of decode correctness.
#
# 20 Aug 2026: re-added thermo_sensor -- a passive, explicitly-labelled-unconfirmed
# readout of the old "Mystery Bit" (frame[6] bit0 / controllerPresent), captured raw
# off incoming frames (see FujiHeatPump.h's Deviation #3). This is NOT a confirmed
# Thermo Sensor Local/Remote decode -- see test-and-dev-workflow.md's "Thermo Sensor /
# Mystery Bit investigation" for the four rounds of live testing that failed to settle
# the mapping. Exposed so a future dedicated live test (single button press, precise
# timestamp) has somewhere to watch without needing new firmware first.
CONFIG_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_FUJITSU_CLIMATE_ID): cv.use_id(FujitsuClimate),
        cv.Optional(CONF_BUS_STATUS): text_sensor.text_sensor_schema(
            entity_category="diagnostic",
        ),
        cv.Optional(CONF_THERMO_SENSOR): text_sensor.text_sensor_schema(
            entity_category="diagnostic",
            icon="mdi:help-circle-outline",
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_FUJITSU_CLIMATE_ID])
    if CONF_BUS_STATUS in config:
        sens = await text_sensor.new_text_sensor(config[CONF_BUS_STATUS])
        cg.add(parent.set_bus_status_text_sensor(sens))
    if CONF_THERMO_SENSOR in config:
        sens = await text_sensor.new_text_sensor(config[CONF_THERMO_SENSOR])
        cg.add(parent.set_thermo_sensor_text_sensor(sens))
