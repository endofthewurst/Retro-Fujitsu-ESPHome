import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor

from . import FujitsuClimate

CONF_FUJITSU_CLIMATE_ID = "fujitsu_climate_id"
CONF_BUS_STATUS = "bus_status"
CONF_CORRECTED_MODE = "corrected_mode"
CONF_CORRECTED_FAN_RAW = "corrected_fan_raw"
CONF_CORRECTED_ECONOMY = "corrected_economy"
# Renamed from "corrected_thermo_sensor" 11 Aug 2026 -- live testing didn't cleanly
# confirm this bit maps to the Thermo Sensor Local/Remote setting. Kept as a
# diagnostic until it's understood -- see FujiHeatPump.h's getCorrMysteryBit().
CONF_MYSTERY_BIT = "mystery_bit"
# Added 18 Aug 2026, continued -- see FujiHeatPump.h's getCorrMessageDest() comment
# and protocol-review-and-next-experiments.md's "real mechanism" addendum. Unlike
# boot_probe/frame_timing below, this updates continuously (not a one-shot summary),
# so it can be watched live across a DIP-mode change or a real power cycle.
CONF_MESSAGE_DEST = "message_dest"
# Added 19 Aug 2026 (3B.33) -- see FujiHeatPump.h's getCorrMessageType()/
# getCorrWriteBit() comment. Same "updates continuously" convention as
# message_dest above -- exists to catch what a real, indoor-unit-accepted write
# command from the WIRED remote actually looks like on the bus.
CONF_MESSAGE_TYPE = "message_type"
# Added 14 Aug 2026, Phase 2 item 2 -- one-shot summary of the boot/discovery-probe
# capture window, published once ~12s after boot. See FujiHeatPump.h/.cpp for the
# capture logic and protocol-review-and-next-experiments.md for the background.
CONF_BOOT_PROBE = "boot_probe"
# Added 14 Aug 2026, Phase 2 item 5 -- one-shot summary of the inter-frame timing
# capture window (UNIT->CTRL and CTRL->UNIT gap stats), published once ~15s after
# boot. See FujiHeatPump.h/.cpp for the capture logic and
# protocol-review-and-next-experiments.md for the background.
CONF_FRAME_TIMING = "frame_timing"

# NOTE: corrected_setpoint / corrected_room_temp moved to sensor.py (numeric) 11 Aug
# 2026, 3B.18 -- the underlying data is always whole-degree C, so a proper numeric
# sensor (with device_class/state_class) is the correct fit and lets HA graph it.

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
        cv.Optional(CONF_CORRECTED_ECONOMY): text_sensor.text_sensor_schema(
            entity_category="diagnostic",
            icon="mdi:flask-outline",
        ),
        cv.Optional(CONF_MYSTERY_BIT): text_sensor.text_sensor_schema(
            entity_category="diagnostic",
            icon="mdi:help-circle-outline",
        ),
        cv.Optional(CONF_MESSAGE_DEST): text_sensor.text_sensor_schema(
            entity_category="diagnostic",
            icon="mdi:account-arrow-right-outline",
        ),
        cv.Optional(CONF_MESSAGE_TYPE): text_sensor.text_sensor_schema(
            entity_category="diagnostic",
            icon="mdi:pencil-outline",
        ),
        cv.Optional(CONF_BOOT_PROBE): text_sensor.text_sensor_schema(
            entity_category="diagnostic",
            icon="mdi:timer-outline",
        ),
        cv.Optional(CONF_FRAME_TIMING): text_sensor.text_sensor_schema(
            entity_category="diagnostic",
            icon="mdi:clock-fast",
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
    if CONF_CORRECTED_ECONOMY in config:
        sens = await text_sensor.new_text_sensor(config[CONF_CORRECTED_ECONOMY])
        cg.add(parent.set_corrected_economy_text_sensor(sens))
    if CONF_MYSTERY_BIT in config:
        sens = await text_sensor.new_text_sensor(config[CONF_MYSTERY_BIT])
        cg.add(parent.set_mystery_bit_text_sensor(sens))
    if CONF_MESSAGE_DEST in config:
        sens = await text_sensor.new_text_sensor(config[CONF_MESSAGE_DEST])
        cg.add(parent.set_message_dest_text_sensor(sens))
    if CONF_MESSAGE_TYPE in config:
        sens = await text_sensor.new_text_sensor(config[CONF_MESSAGE_TYPE])
        cg.add(parent.set_message_type_text_sensor(sens))
    if CONF_BOOT_PROBE in config:
        sens = await text_sensor.new_text_sensor(config[CONF_BOOT_PROBE])
        cg.add(parent.set_boot_probe_text_sensor(sens))
    if CONF_FRAME_TIMING in config:
        sens = await text_sensor.new_text_sensor(config[CONF_FRAME_TIMING])
        cg.add(parent.set_frame_timing_text_sensor(sens))
