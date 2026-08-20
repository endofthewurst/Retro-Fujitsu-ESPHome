import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate
from esphome.const import CONF_ID

from . import fujitsu_climate_ns, FujitsuClimate

CONF_RX_PIN = "rx_pin"
CONF_TX_PIN = "tx_pin"

# Phase 4 rebuild: rx_pin/tx_pin replace the old uart_id reference (see __init__.py).
# Defaults match this project's known-good wiring (hardware-and-protocol.md): GPIO16
# from the LIN3TL transceiver's TX into the ESP32's RX, GPIO17 out to the
# transceiver's RX. Plain pin numbers rather than ESPHome's GPIO pin schema, since
# these are passed straight through to Arduino's HardwareSerial::begin(baud, config,
# rxPin, txPin) rather than used as an ESPHome-managed GPIOPin.
CONFIG_SCHEMA = (
    climate.CLIMATE_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(FujitsuClimate),
            cv.Optional(CONF_RX_PIN, default=16): cv.int_range(min=0, max=39),
            cv.Optional(CONF_TX_PIN, default=17): cv.int_range(min=0, max=39),
        }
    ).extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await climate.register_climate(var, config)
    cg.add(var.set_rx_pin(config[CONF_RX_PIN]))
    cg.add(var.set_tx_pin(config[CONF_TX_PIN]))
