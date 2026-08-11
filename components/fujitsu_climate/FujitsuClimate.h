#pragma once

#include "esphome/core/component.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/sensor/sensor.h"
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
  // in YAML via the fujitsu_climate binary_sensor/text_sensor/sensor platforms.
  void set_bus_alive_binary_sensor(binary_sensor::BinarySensor *s) { bus_alive_binary_sensor_ = s; }
  void set_bus_status_text_sensor(text_sensor::TextSensor *s) { bus_status_text_sensor_ = s; }
  void set_corrected_mode_text_sensor(text_sensor::TextSensor *s) { corrected_mode_text_sensor_ = s; }
  void set_corrected_fan_raw_text_sensor(text_sensor::TextSensor *s) { corrected_fan_raw_text_sensor_ = s; }
  void set_corrected_setpoint_sensor(sensor::Sensor *s) { corrected_setpoint_sensor_ = s; }
  void set_corrected_room_temp_sensor(sensor::Sensor *s) { corrected_room_temp_sensor_ = s; }
  void set_corrected_economy_text_sensor(text_sensor::TextSensor *s) { corrected_economy_text_sensor_ = s; }
  void set_mystery_bit_text_sensor(text_sensor::TextSensor *s) { mystery_bit_text_sensor_ = s; }
  
 protected:
  FujiHeatPump hp_;
  bool hardware_present_{false};
  binary_sensor::BinarySensor *bus_alive_binary_sensor_{nullptr};
  text_sensor::TextSensor *bus_status_text_sensor_{nullptr};
  text_sensor::TextSensor *corrected_mode_text_sensor_{nullptr};
  text_sensor::TextSensor *corrected_fan_raw_text_sensor_{nullptr};
  sensor::Sensor *corrected_setpoint_sensor_{nullptr};
  sensor::Sensor *corrected_room_temp_sensor_{nullptr};
  text_sensor::TextSensor *corrected_economy_text_sensor_{nullptr};
  // Renamed from "Corrected Thermo Sensor" 11 Aug 2026 -- live testing didn't cleanly
  // confirm this bit maps to the Thermo Sensor Local/Remote setting (see
  // FujiHeatPump.h's getCorrMysteryBit() comment). Kept as a diagnostic until it's
  // understood.
  text_sensor::TextSensor *mystery_bit_text_sensor_{nullptr};

  // Bus-alive/status thresholds (ms) -- see update_bus_status_() for the logic.
  // Widened 2000 -> 4000ms on 11 Aug 2026 (3B.19): removing the unthrottled per-byte
  // ESP_LOGV (3B.18) did NOT eliminate the Bus Alive flicker -- HA history afterward
  // still showed recurring ~1s "off"/"No Signal" blips at irregular intervals (5s to
  // 144s apart), while `binary_sensor.aircon_status`/`sensor.aircon_uptime` stayed
  // perfectly solid the whole time (no reboot, no WiFi drop) -- so this is isolated to
  // the UART bus reception specifically, not a device-wide hiccup, and the 3B.18
  // logging theory was wrong or at best incomplete. "No Signal" (not "Noise") means
  // genuinely zero bytes arrived, not malformed ones. Root cause still unconfirmed --
  // could be inherent to the physical bus/protocol (a real, harmless quiet period) or
  // an as-yet-unidentified software stall; ruling it out further needs a live
  // oscilloscope/logic-analyzer capture on the LIN line during a gap, which isn't
  // possible remotely. Widening the timeout is a pragmatic mitigation (stop the
  // diagnostic from flapping on what may be normal short quiet spells), not a
  // diagnosed fix -- if flicker continues even at 4000ms, that's a stronger signal
  // the underlying gaps are longer than ~1s and worth investigating further.
  static constexpr uint32_t BUS_FRAME_TIMEOUT_MS = 4000;
  static constexpr uint32_t BUS_BYTE_TIMEOUT_MS = 4000;

  void update_bus_status_();

  // Mirrors the corrected decode's raw fields to standalone diagnostic HA entities,
  // in parallel with the same decode now driving the main climate entity's state
  // (see update_climate_state()) -- lets the two be cross-checked against each other.
  void update_corrected_diagnostics_();

  // Throttles the `State updated` log in update_climate_state() to 1/sec (added 10
  // Aug 2026) -- under real live-bus traffic this fired on every valid frame (several
  // times/sec), and its float formatting (%.1f x2) was a remaining contributor to
  // component-loop overruns after the FujiHeatPump-side dumps were throttled.
  uint32_t state_log_last_ms_{0};
  
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
