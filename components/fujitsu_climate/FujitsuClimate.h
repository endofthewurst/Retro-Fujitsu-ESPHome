#pragma once

#include "esphome/core/component.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/uart/uart.h"
#include "FujiHeatPump.h"

namespace esphome {
namespace fujitsu_climate {

class FujitsuClimate : public climate::Climate, public PollingComponent, public uart::UARTDevice {
 public:
  // Explicit constructor: initialises PollingComponent with the publish interval.
  FujitsuClimate();

  void setup() override;
  void update() override;
  void dump_config() override;
  
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }
  
  // Climate traits
  climate::ClimateTraits traits() override;
  
  // Control the climate
  void control(const climate::ClimateCall &call) override;
  
 protected:
  FujiHeatPump hp_;
  bool hardware_present_{false};
  
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
