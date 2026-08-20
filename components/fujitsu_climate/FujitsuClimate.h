#pragma once

// ---------------------------------------------------------------------------
// Phase 4 rebuild (19 Aug 2026) -- see tx-architecture-review-and-adoption-plan.md
// for the full review this rebuild responds to. The prior version of this file
// polled a hand-rolled byte state machine from inside ESPHome's shared, cooperative
// loop(), and only ever transmitted via a pile of one-shot, manually-armed test
// methods (armLoginAckTest(), armStatusCommandTest(), armModeCommandTest(), etc.).
// That review's central finding: this project never actually ran as a secondary
// controller in the sense upstream (unreality/FujiHeatPump) means it -- a device
// that automatically and continuously replies to every frame addressed to it,
// forever, from the moment it connects. This file rebuilds the wrapper on the same
// architecture the reference ESPHome integration
// (https://github.com/FujiHeatPump/esphome-fujitsu) actually uses: a dedicated
// FreeRTOS task, pinned to a core, doing blocking HardwareSerial I/O against the
// vendored FujiHeatPump engine (see FujiHeatPump.h) -- completely decoupled from
// ESPHome's own loop budget, WiFi, and the HA API connection. FujiHeatPump itself now
// owns the automatic reply loop (waitForFrame() replies to every addressed frame
// unconditionally), so all the one-shot arm/test machinery is retired along with it --
// see FujitsuClimate.cpp's control() for how real HA commands reach it now, gated
// behind a kill switch (control_enabled_) per hardware-and-protocol.md's "ESP32 is
// never the boss" design principle, since command adoption under continuous presence
// has not yet been tested live.
//
// 20 Aug 2026 update: added set_thermo_sensor_text_sensor()/update_thermo_sensor_() --
// a passive, clearly-labelled-unconfirmed diagnostic for the raw controllerPresent
// bit some of this project's history hypothesized might be the UTY-RNNUM's Thermo
// Sensor Local/Remote setting. See FujiHeatPump.h's Deviation #3 comment for why this
// needed a new getter, and test-and-dev-workflow.md's "Thermo Sensor / Mystery Bit
// investigation" for why this is exposed as "unconfirmed" rather than mapped to
// Local/Remote outright -- four rounds of live testing never settled the mapping.
// Also fixed update_climate_state_()'s FAN-only setpoint clamp, which had incorrectly
// been clearing target_temperature to NAN for ANY mode whenever the raw byte happened
// to read 0, not just FAN_ONLY.
// ---------------------------------------------------------------------------

#include "esphome/core/component.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/sensor/sensor.h"

#include "FujiHeatPump.h"

#ifdef ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#endif

namespace esphome {
namespace fujitsu_climate {

class FujitsuClimate : public climate::Climate, public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;

  // Unchanged from the 3B.22 fix -- this component's setup() (which starts the bus
  // task) must run before WiFi association, not after, on the shared 12V rail where
  // the ESP32 and the Fujitsu unit boot together. See the original 3B.22 writeup in
  // state-of-play.md for the full reasoning; still applies unchanged to this rebuild.
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  // Pins (added for this rebuild -- the previous version used ESPHome's `uart:`
  // component; the vendored upstream engine owns a raw HardwareSerial directly, so
  // these are now plain GPIO numbers passed straight to FujiHeatPump::connect()
  // rather than a uart_id reference). Defaults match this project's known-good wiring
  // (hardware-and-protocol.md): GPIO16 = RX (from the LIN3TL transceiver), GPIO17 = TX.
  void set_rx_pin(int pin) { rx_pin_ = pin; }
  void set_tx_pin(int pin) { tx_pin_ = pin; }

  // Kill switch (added for this rebuild) -- hardware-and-protocol.md's "ESP32 is
  // never the boss" design principle calls for exactly this: a way to put the system
  // into observe-only mode without touching hardware. control() below stays a no-op
  // until this is explicitly turned on via the "Aircon Control Enabled" switch (see
  // retrofujitsu.yaml).
  void set_control_enabled(bool enabled);
  bool get_control_enabled() const { return control_enabled_; }

  // Diagnostics (trimmed for this rebuild -- see the .cpp file's top-of-file comment
  // for which of the old exploratory diagnostics were retired and why).
  void set_bus_alive_binary_sensor(binary_sensor::BinarySensor *s) { bus_alive_binary_sensor_ = s; }
  void set_bus_status_text_sensor(text_sensor::TextSensor *s) { bus_status_text_sensor_ = s; }
  void set_corrected_setpoint_sensor(sensor::Sensor *s) { corrected_setpoint_sensor_ = s; }
  void set_corrected_room_temp_sensor(sensor::Sensor *s) { corrected_room_temp_sensor_ = s; }
  // 20 Aug 2026 -- see the file-header comment above and FujiHeatPump.h's Deviation #3.
  void set_thermo_sensor_text_sensor(text_sensor::TextSensor *s) { thermo_sensor_text_sensor_ = s; }

  // Public so the free-standing task function (fujitsu_bus_task, in the .cpp) can
  // reach it -- matches the reference esphome-fujitsu wrapper's own pattern
  // (FujitsuClimate::heatPump is public there too, for the same reason: a plain C
  // FreeRTOS task function isn't a member function and needs a pointer to work with).
  FujiHeatPump heat_pump;
  FujiFrame shared_state;
#ifdef ESP32
  SemaphoreHandle_t lock{nullptr};
  TaskHandle_t task_handle{nullptr};
#endif
  // Set by the bus task the moment it has anything to send (built from a control()
  // call); cleared once FujiHeatPump has actually sent it. Mirrors the reference
  // wrapper's pendingUpdate flag -- lets control() avoid piling up several changes
  // into updateFields before the previous send has gone out.
  volatile bool pending_update{false};

 protected:
  bool hardware_present_{false};
  bool control_enabled_{false};
  int rx_pin_{16};
  int tx_pin_{17};

  binary_sensor::BinarySensor *bus_alive_binary_sensor_{nullptr};
  text_sensor::TextSensor *bus_status_text_sensor_{nullptr};
  sensor::Sensor *corrected_setpoint_sensor_{nullptr};
  sensor::Sensor *corrected_room_temp_sensor_{nullptr};
  text_sensor::TextSensor *thermo_sensor_text_sensor_{nullptr};

  // Bus-alive/status threshold (ms) -- carried forward unchanged from 3B.19's fix.
  // This project's own history found 1-3s quiet gaps are normal on this bus; a
  // threshold at or below FujiHeatPump::isBound()'s own fixed 1000ms flickers on them.
  static constexpr uint32_t BUS_TIMEOUT_MS = 4000;

  void update_bus_status_();
  void update_climate_state_();

  uint32_t state_log_last_ms_{0};

  // Change-detection state for the diagnostic sensors. loop() (and therefore
  // update_bus_status_()/update_climate_state_()) runs on every ESPHome main-loop
  // tick -- hundreds of times a second, not once a second like the old
  // PollingComponent-based version -- so publish_state() must be gated on an actual
  // change here, or every tick re-sends every diagnostic entity's state over the API
  // connection. (Found live on first boot of the Phase 4 rebuild: this gating was
  // missing, and the resulting publish flood was almost certainly the cause of the
  // repeated "component took a long time" warnings during that first boot.)
  bool bus_status_initialized_{false};
  bool last_bus_alive_{false};
  uint8_t last_bus_state_{0};  // 0=uninitialized, 1=Bus OK, 2=No Signal, 3=Never Seen
  bool corrected_sensors_initialized_{false};
  float last_corrected_setpoint_{NAN};
  float last_corrected_room_temp_{NAN};
  bool thermo_sensor_initialized_{false};
  int last_thermo_sensor_bit_{-1};

  climate::ClimateMode fuji_mode_to_climate_mode_(FujiMode mode);
  FujiMode climate_mode_to_fuji_mode_(climate::ClimateMode mode);
  climate::ClimateFanMode fuji_fan_to_climate_fan_(FujiFanMode fan);
  FujiFanMode climate_fan_to_fuji_fan_(climate::ClimateFanMode fan);
};

}  // namespace fujitsu_climate
}  // namespace esphome
