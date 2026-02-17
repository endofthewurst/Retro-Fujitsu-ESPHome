import esphome.codegen as cg
from esphome.components import climate, uart

CODEOWNERS = ["@unreality"]
DEPENDENCIES = ["uart", "climate"]

fujitsu_climate_ns = cg.esphome_ns.namespace("fujitsu_climate")
FujitsuClimate = fujitsu_climate_ns.class_(
    "FujitsuClimate", climate.Climate, cg.Component, uart.UARTDevice
)
