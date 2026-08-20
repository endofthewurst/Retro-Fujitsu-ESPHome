import esphome.codegen as cg
from esphome.components import climate

# Phase 4 rebuild (19 Aug 2026): dropped the "uart" dependency/base class -- the
# vendored unreality/FujiHeatPump engine owns a raw HardwareSerial (Serial2) directly
# via FujiHeatPump::connect(), matching how the reference esphome-fujitsu integration
# does it, rather than going through ESPHome's uart: component. See climate.py for the
# rx_pin/tx_pin config that replaces the old uart_id reference.
CODEOWNERS = ["@unreality", "@jamesadapt"]
DEPENDENCIES = ["climate"]

fujitsu_climate_ns = cg.esphome_ns.namespace("fujitsu_climate")
FujitsuClimate = fujitsu_climate_ns.class_("FujitsuClimate", climate.Climate, cg.Component)
