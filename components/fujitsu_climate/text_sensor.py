import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.components import time as time_

from . import FujitsuClimate

CONF_FUJITSU_CLIMATE_ID = "fujitsu_climate_id"
CONF_BUS_STATUS = "bus_status"
CONF_THERMO_SENSOR = "thermo_sensor"
CONF_UNKNOWN_BIT = "unknown_bit"
CONF_RAW_FRAME = "raw_frame"
CONF_SYNC_MISMATCH = "sync_mismatch"
CONF_OUT_OF_SYNC_SINCE = "out_of_sync_since"
CONF_TIME_ID = "time_id"

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
#
# 2 Sep 2026: added sync_mismatch/out_of_sync_since -- the two text-sensor halves of
# the requested-vs-actual state sync feature (plan-to-completion.md's "Requested vs.
# actual state sync" design, dated 2 Sep 2026; the third half, a plain "in sync"
# binary_sensor, lives in binary_sensor.py). out_of_sync_since needs a real-time clock
# to timestamp the first divergence, hence the extra time_id option and the FujitsuClimate
# setter it wires up -- see FujitsuClimate.h/.cpp.
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
        cv.Optional(CONF_UNKNOWN_BIT): text_sensor.text_sensor_schema(
            entity_category="diagnostic",
            icon="mdi:help-circle-outline",
        ),
        cv.Optional(CONF_RAW_FRAME): text_sensor.text_sensor_schema(
            entity_category="diagnostic",
            icon="mdi:code-braces",
        ),
        cv.Optional(CONF_SYNC_MISMATCH): text_sensor.text_sensor_schema(
            icon="mdi:sync-alert",
        ),
        cv.Optional(CONF_OUT_OF_SYNC_SINCE): text_sensor.text_sensor_schema(
            icon="mdi:clock-alert-outline",
        ).extend(
            {
                cv.Required(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
            }
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
    if CONF_UNKNOWN_BIT in config:
        sens = await text_sensor.new_text_sensor(config[CONF_UNKNOWN_BIT])
        cg.add(parent.set_unknown_bit_text_sensor(sens))

    if CONF_RAW_FRAME in config:
        sens = await text_sensor.new_text_sensor(config[CONF_RAW_FRAME])
        cg.add(parent.set_raw_frame_text_sensor(sens))

    if CONF_SYNC_MISMATCH in config:
        sens = await text_sensor.new_text_sensor(config[CONF_SYNC_MISMATCH])
        cg.add(parent.set_sync_mismatch_text_sensor(sens))

    if CONF_OUT_OF_SYNC_SINCE in config:
        conf = config[CONF_OUT_OF_SYNC_SINCE]
        sens = await text_sensor.new_text_sensor(conf)
        cg.add(parent.set_out_of_sync_since_text_sensor(sens))
        time_var = await cg.get_variable(conf[CONF_TIME_ID])
        cg.add(parent.set_time_id(time_var))