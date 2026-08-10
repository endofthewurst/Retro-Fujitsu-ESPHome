#pragma once

#include "esphome/core/component.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "FujiHeatPump.h"

namespace esphome {
namespace fujitsu_climate {

class FujitsuClimate : public climate::Climate, public PollingComponent, public uart::UARTDevice {
 public:
  // Explicit constructor: initialises PollingComponent with the publish interval.
  FujitsuClimate();

  void setup() override;
  void loop() override;   // reads frames on every main-loop tick
  void update() override; // publishes state to HA on interval
  void dump_config() override;
  
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }
  
  // Climate traits
  climate::ClimateTraits traits() override;
  
  // Control the climate
  void control(const climate::ClimateCall &call) override;

  // Bus health diagnostics (added 10 Aug 2026) -- optional, set only if configured
  // in YAML via the fujitsu_climate binary_sensor/text_sensor platforms.
  void set_bus_alive_binary_sensor(binary_sensor::BinarySensor *s) { bus_alive_binary_sensor_ = s; }
  void set_bus_status_text_sensor(text_sensor::TextSensor *s) { bus_status_text_sensor_ = s; }
  
 protected:
  FujiHeatPump hp_;
  bool hardware_present_{false};
  binary_sensor::BinarySensor *bus_alive_binary_sensor_{nullptr};
  text_sensor::TextSensor *bus_status_text_sensor_{nullptr};

  // Bus-alive/status thresholds (ms) -- see update_bus_status_() for the logic.
  static constexpr uint32_t BUS_FRAME_TIMEOUT_MS = 2000;
  static constexpr uint32_t BUS_BYTE_TIMEOUT_MS = 2000;

  void update_bus_status_();
  
  // Update Home Assistant with current state
  void update_climate_state();
  
  // Conversion helpers
  climate::ClimateMode fuji_mode_to_climate_mode(FujiMode mode);
  FujiMode climate_mode_to_fuji_mode(climate::ClimateMode mode);
  climate::ClimateFanMode fuji_fan_to_climate_fan(FujiFanMode fan);
  FujiFanMode climate_fan_to_fuji_fan(climate::ClimateFanMode fan);
};

}  // namespace fujitsu_climate
}  // namespace esphome
