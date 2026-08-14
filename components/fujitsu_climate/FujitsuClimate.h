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

  // Changed 14 Aug 2026 from AFTER_WIFI to HARDWARE -- this was a directly-confirmed
  // finding from the Phase 2 item 4 investigation (protocol-review-and-
  // next-experiments.md / plan-to-completion.md's "confirm UART init happens in
  // setup() before WiFi/API connect" open item): this component's setup() -- which
  // calls hp_.connect() -- was explicitly scheduled to run AFTER WiFi finished
  // connecting, not before. On a shared power rail where the ESP32 and the Fujitsu
  // unit boot together, WiFi association can easily take longer than the ~4-second
  // discovery-probe window, meaning readFrame() (called from loop(), which ESPHome
  // only starts invoking after setup()) might not even be listening yet when the
  // probe arrives -- independent of and possibly compounding the parity/binding
  // questions elsewhere in Phase 2. HARDWARE runs well before WIFI's own priority, so
  // the UART hookup now happens as early as this component can make it, matching the
  // "the ESP32 must never be the reason a real-time bus event is missed" spirit of
  // hardware-and-protocol.md. Nothing in setup() below actually depends on WiFi/API
  // being up, so this should be safe.
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  // Climate traits
  climate::ClimateTraits traits() override;

  // Control the climate
  void control(const climate::ClimateCall &call) override;

  // Phase 2 TX test entry point (added 11 Aug 2026) -- see FujiHeatPump.cpp's
  // buildFrame() for the frame construction and .cpp below for why this exists as a
  // separate, deliberate, single-action method rather than being wired through
  // control() above. delta_c is added to the currently-decoded setpoint and sent as
  // a one-shot command frame; check the physical wall unit immediately after calling
  // this, per plan-to-completion.md Phase 2's test order (setpoint first).
  void test_setpoint_step(int delta_c);

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
  // Boot/discovery-probe diagnostic (added 14 Aug 2026) -- see update_boot_probe_()
  // and FujiHeatPump.h for the full explanation. A single compact text_sensor rather
  // than several numeric ones, to avoid adding five new entities for what's
  // fundamentally one exploratory diagnostic.
  void set_boot_probe_text_sensor(text_sensor::TextSensor *s) { boot_probe_text_sensor_ = s; }

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
  text_sensor::TextSensor *boot_probe_text_sensor_{nullptr};
  bool boot_probe_published_{false};  // publish the summary once it's ready, not every tick

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
  // diagnosed fix. A follow-up HA-history check (11 Aug 2026, later same day) found
  // zero flicker events across ~1.5h of runtime at this setting, vs. ~1/53s
  // immediately before the change -- looking like it's working in practice, though
  // still worth another check after a longer unattended run.
  static constexpr uint32_t BUS_FRAME_TIMEOUT_MS = 4000;
  static constexpr uint32_t BUS_BYTE_TIMEOUT_MS = 4000;

  void update_bus_status_();

  // Mirrors the corrected decode's raw fields to standalone diagnostic HA entities,
  // in parallel with the same decode now driving the main climate entity's state
  // (see update_climate_state()) -- lets the two be cross-checked against each other.
  void update_corrected_diagnostics_();

  // Publishes the boot/discovery-probe summary once the capture window has closed
  // (added 14 Aug 2026) -- see FujiHeatPump.h/.cpp for the capture logic itself.
  void update_boot_probe_();

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
