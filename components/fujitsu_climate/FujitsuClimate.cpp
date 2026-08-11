#include "FujitsuClimate.h"
#include "esphome/core/log.h"
#include <cstdio>
#include <cmath>

namespace esphome {
namespace fujitsu_climate {

static const char *const TAG = "fujitsu.climate";

// How often to publish state to Home Assistant (ms)
constexpr uint32_t PUBLISH_INTERVAL_MS = 1000;

FujitsuClimate::FujitsuClimate() : PollingComponent(PUBLISH_INTERVAL_MS) {
  // Use NAN for temperatures -- ESPHome/HA renders these as "unknown" until
  // real values arrive from the bus. Do NOT fabricate readings.
  target_temperature = NAN;
  current_temperature = NAN;
  mode = climate::CLIMATE_MODE_OFF;
  fan_mode = climate::CLIMATE_FAN_AUTO;
  action = climate::CLIMATE_ACTION_IDLE;
  hardware_present_ = false;
}

void FujitsuClimate::setup() {
  // Connect to heat pump driver; enable debug logging for Phase 3B capture
  hp_.connect(this->parent_, true);
  hp_.setDebug(true);

  ESP_LOGI(TAG, "Fujitsu Climate Phase 3B initialized -- hardware connected, listen mode");
  ESP_LOGI(TAG, "Waiting for LIN bus frames on UART (500 baud 8N1)...");
}

void FujitsuClimate::loop() {
  // Called every main-loop tick -- drain all available frames immediately.
  // This ensures no bus traffic is missed between HA publish intervals.
  while (hp_.readFrame()) {
    if (!hardware_present_) {
      hardware_present_ = true;
      ESP_LOGI(TAG, "First frame received -- hardware confirmed present");
    }
    update_climate_state();
  }

  // NOTE (11 Aug 2026, 3B.18): sendPendingFrame() is intentionally never called here
  // as part of the normal read loop. Phase 2 TX only ever happens synchronously
  // inside test_setpoint_step() below, one deliberate call at a time -- see
  // control() for why the general HA control path stays read-only.
}

void FujitsuClimate::update() {
  // Called on interval (1 s) -- just publish current state to Home Assistant
  if (hardware_present_) {
    publish_state();
  }

  // Bus health diagnostics run regardless of hardware_present_ -- that's the whole
  // point, they need to report "No Signal" even before the first valid frame ever
  // arrives, not just stay silent forever.
  update_bus_status_();
  update_corrected_diagnostics_();
}

void FujitsuClimate::update_bus_status_() {
  if (bus_alive_binary_sensor_ == nullptr && bus_status_text_sensor_ == nullptr) {
    return;  // Not configured in YAML -- nothing to do.
  }

  uint32_t now = millis();
  uint32_t last_frame = hp_.getLastFrameTime();
  uint32_t last_byte = hp_.getLastByteTime();

  bool frame_recent = (last_frame != 0) && (now - last_frame < BUS_FRAME_TIMEOUT_MS);
  bool byte_recent = (last_byte != 0) && (now - last_byte < BUS_BYTE_TIMEOUT_MS);

  if (bus_alive_binary_sensor_ != nullptr) {
    bus_alive_binary_sensor_->publish_state(frame_recent);
  }

  if (bus_status_text_sensor_ != nullptr) {
    const char *status;
    if (frame_recent) {
      status = "Bus OK";
    } else if (byte_recent) {
      status = "Noise";
    } else {
      status = "No Signal";
    }
    bus_status_text_sensor_->publish_state(status);
  }
}

void FujitsuClimate::update_corrected_diagnostics_() {
  if (corrected_mode_text_sensor_ == nullptr && corrected_fan_raw_text_sensor_ == nullptr &&
      corrected_setpoint_sensor_ == nullptr && corrected_room_temp_sensor_ == nullptr &&
      corrected_economy_text_sensor_ == nullptr && mystery_bit_text_sensor_ == nullptr) {
    return;  // Not configured in YAML -- nothing to do.
  }

  uint32_t last_corr = hp_.getCorrLastUpdateTime();
  bool have_corr = (last_corr != 0) && (millis() - last_corr < BUS_FRAME_TIMEOUT_MS);

  // Mode/Fan (renamed from "Corrected Mode"/"Corrected Fan Raw" 11 Aug 2026) now
  // publish just the friendly label -- no "(raw=N)" suffix, no bare numbers -- per
  // James's feedback that the diagnostic entities should show real values, not codes.
  if (corrected_mode_text_sensor_ != nullptr) {
    if (!have_corr) {
      corrected_mode_text_sensor_->publish_state("No Data");
    } else {
      uint8_t m = hp_.getCorrModeRaw();
      const char *label;
      switch (m) {
        case 1: label = "Fan"; break;
        case 2: label = "Dry"; break;
        case 3: label = "Cool"; break;
        case 4: label = "Heat"; break;
        case 5: label = "Auto"; break;
        default: label = "Unknown"; break;
      }
      corrected_mode_text_sensor_->publish_state(label);
    }
  }

  if (corrected_fan_raw_text_sensor_ != nullptr) {
    if (!have_corr) {
      corrected_fan_raw_text_sensor_->publish_state("No Data");
    } else {
      uint8_t f = hp_.getCorrFanRaw();
      const char *label;
      switch (f) {
        case 0: label = "Auto"; break;
        case 1: label = "Quiet"; break;
        case 2: label = "Low"; break;
        case 3: label = "Medium"; break;
        case 4: label = "High"; break;
        default: label = "Unknown"; break;
      }
      corrected_fan_raw_text_sensor_->publish_state(label);
    }
  }

  // Setpoint / room temp are numeric sensors (converted from text 11 Aug 2026,
  // 3B.18) -- the underlying data is always whole degrees C (see the frame layout
  // comment in FujiHeatPump.cpp's processCorrectedFrame()), so there's no fractional
  // precision being discarded here. NAN publishes as "unknown" in HA, same meaning as
  // the old "No Data"/"None" text values.
  if (corrected_setpoint_sensor_ != nullptr) {
    if (!have_corr) {
      corrected_setpoint_sensor_->publish_state(NAN);
    } else {
      uint8_t sp = hp_.getCorrSetpointRaw();
      corrected_setpoint_sensor_->publish_state(sp == 0 ? NAN : static_cast<float>(sp));
    }
  }

  if (corrected_room_temp_sensor_ != nullptr) {
    if (!have_corr) {
      corrected_room_temp_sensor_->publish_state(NAN);
    } else {
      corrected_room_temp_sensor_->publish_state(static_cast<float>(hp_.getCorrRoomTempRaw()));
    }
  }

  if (corrected_economy_text_sensor_ != nullptr) {
    if (!have_corr) {
      corrected_economy_text_sensor_->publish_state("No Data");
    } else {
      corrected_economy_text_sensor_->publish_state(hp_.getCorrEconomy() ? "ON" : "OFF");
    }
  }

  if (mystery_bit_text_sensor_ != nullptr) {
    if (!have_corr) {
      mystery_bit_text_sensor_->publish_state("No Data");
    } else {
      // Raw bit, not mapped to any label -- see FujiHeatPump.h's getCorrMysteryBit().
      mystery_bit_text_sensor_->publish_state(hp_.getCorrMysteryBit() ? "Bit=1" : "Bit=0");
    }
  }
}

void FujitsuClimate::dump_config() {
  ESP_LOGCONFIG(TAG, "Fujitsu Heat Pump Climate:");
  ESP_LOGCONFIG(TAG, "  Hardware present: %s", hardware_present_ ? "YES" : "NO");
  ESP_LOGCONFIG(TAG, "  Controller: Secondary");
  ESP_LOGCONFIG(TAG, "  LIN Interface: TJA1021");
  ESP_LOGCONFIG(TAG, "  TX: read-only via HA (control() ignores commands); Phase 2 TX test");
  ESP_LOGCONFIG(TAG, "      reachable only via test_setpoint_step() / the TX Test buttons");
}

climate::ClimateTraits FujitsuClimate::traits() {
  auto traits = climate::ClimateTraits();

  // Supported modes
  traits.set_supported_modes({
      climate::CLIMATE_MODE_OFF,
      climate::CLIMATE_MODE_AUTO,
      climate::CLIMATE_MODE_COOL,
      climate::CLIMATE_MODE_HEAT,
      climate::CLIMATE_MODE_DRY,
      climate::CLIMATE_MODE_FAN_ONLY,
  });

  // Supported fan modes
  traits.set_supported_fan_modes({
      climate::CLIMATE_FAN_AUTO,
      climate::CLIMATE_FAN_LOW,
      climate::CLIMATE_FAN_MEDIUM,
      climate::CLIMATE_FAN_HIGH,
      climate::CLIMATE_FAN_QUIET,
  });

  // Economy, wired as a preset (added 11 Aug 2026, 3B.18) -- reflects
  // corr_economy_/getCorrEconomy(), validated live in Session B via a clean 43s
  // OFF->ON->OFF toggle.
  traits.set_supported_presets({
      climate::CLIMATE_PRESET_NONE,
      climate::CLIMATE_PRESET_ECO,
  });

  // Temperature settings
  traits.set_supports_current_temperature(true);
  traits.set_visual_min_temperature(16.0f);
  traits.set_visual_max_temperature(30.0f);
  traits.set_visual_temperature_step(1.0f);
  traits.set_supports_two_point_target_temperature(false);

  // Action support
  traits.set_supports_action(true);

  return traits;
}

void FujitsuClimate::control(const climate::ClimateCall &call) {
  // TX (Phase 2) has not yet been validated against the real bus through this path --
  // buildFrame() in FujiHeatPump.cpp was rewritten 11 Aug 2026 against the validated
  // corrected-decode field layout, but has itself never been sent to the real bus
  // outside of the explicit, single-action test_setpoint_step() below.
  // hardware-and-protocol.md's "ESP32 is never the boss" design principle requires
  // the wired UTY-RNNUM controller to remain the one actually in control until Phase 2
  // transmit is validated per plan-to-completion.md's test plan (setpoint first, then
  // fan, mode, power, each checked against the physical display). Letting arbitrary,
  // still-unvalidated HA commands reach the live, working heat pump risks exactly the
  // "controllers disagree" failure mode Phase 2 exists to catch under controlled
  // conditions.
  //
  // As of 11 Aug 2026 (3B.18) this climate entity remains READ-ONLY for general HA
  // commands: any command from HA (mode, temperature, fan, preset) is logged and
  // ignored rather than sent to FujiHeatPump's setters. Re-enable by wiring these back
  // to hp_.setOnOff() / setMode() / setTemperature() / setFanMode() + sendPendingFrame()
  // once every step of the Phase 2 test plan has passed via the dedicated test path
  // (test_setpoint_step(), and its siblings once written for fan/mode/power).
  ESP_LOGW(TAG, "Climate control request ignored -- TX disabled pending Phase 2 validation (read-only monitoring only)");
}

void FujitsuClimate::test_setpoint_step(int delta_c) {
  // Phase 2 TX test entry point, added 11 Aug 2026 -- see plan-to-completion.md
  // Phase 2 step 4: test in strict order, setpoint first (smallest, most reversible
  // change). Deliberately NOT reachable through control() above -- this is a single,
  // explicit, manually-triggered action (wired to a dedicated HA button, see
  // retrofujitsu.yaml) so one specific test step can be tried and its effect on the
  // physical wall unit checked immediately after, one step at a time, rather than a
  // general-purpose control path a HA automation or a stray tap could trigger.
  if (!hardware_present_) {
    ESP_LOGW(TAG, "test_setpoint_step: no bus frames seen yet, refusing");
    return;
  }
  if (!hp_.hasLastCtrlRaw()) {
    ESP_LOGW(TAG, "test_setpoint_step: no real CTRL frame captured yet, refusing");
    return;
  }
  float current = hp_.getTemperature();
  if (std::isnan(current)) {
    ESP_LOGW(TAG, "test_setpoint_step: no current setpoint decoded (mode may not support one), refusing");
    return;
  }
  float target = current + static_cast<float>(delta_c);
  ESP_LOGW(TAG, "TX TEST: requesting setpoint %.0f -> %.0fdegC (Phase 2, unvalidated -- check the wall unit now)",
           current, target);
  hp_.setTemperature(target);
  if (hp_.hasPendingFrame()) {
    hp_.sendPendingFrame();
  } else {
    ESP_LOGW(TAG, "test_setpoint_step: buildFrame() did not produce a pending frame (see its own log for why)");
  }
}

void FujitsuClimate::update_climate_state() {
  if (!hardware_present_) {
    // Keep defaults
    return;
  }

  // Update mode
  if (hp_.getOnOff()) {
    mode = fuji_mode_to_climate_mode(hp_.getMode());
  } else {
    mode = climate::CLIMATE_MODE_OFF;
  }

  // Update temperatures
  target_temperature = hp_.getTemperature();
  current_temperature = hp_.getCurrentTemperature();

  // Update fan mode
  fan_mode = fuji_fan_to_climate_fan(hp_.getFanMode());

  // Update preset from economy (added 11 Aug 2026, 3B.18)
  preset = hp_.getCorrEconomy() ? climate::CLIMATE_PRESET_ECO : climate::CLIMATE_PRESET_NONE;

  // Update action
  if (!hp_.getOnOff()) {
    action = climate::CLIMATE_ACTION_OFF;
  } else {
    switch (hp_.getMode()) {
      case FujiMode::HEAT:
        action = climate::CLIMATE_ACTION_HEATING;
        break;
      case FujiMode::COOL:
        action = climate::CLIMATE_ACTION_COOLING;
        break;
      case FujiMode::DRY:
        action = climate::CLIMATE_ACTION_DRYING;
        break;
      case FujiMode::FAN:
        action = climate::CLIMATE_ACTION_FAN;
        break;
      default:
        action = climate::CLIMATE_ACTION_IDLE;
        break;
    }
  }

  uint32_t now_ms = millis();
  if (now_ms - state_log_last_ms_ >= 1000) {
    state_log_last_ms_ = now_ms;
    ESP_LOGD(TAG, "State updated - Mode: %d, Target: %.1fdegC, Current: %.1fdegC",
             mode, target_temperature, current_temperature);
  }
}

// Mode and fan mapping functions remain unchanged
climate::ClimateMode FujitsuClimate::fuji_mode_to_climate_mode(FujiMode mode) {
  switch (mode) {
    case FujiMode::HEAT:      return climate::CLIMATE_MODE_HEAT;
    case FujiMode::COOL:      return climate::CLIMATE_MODE_COOL;
    case FujiMode::DRY:       return climate::CLIMATE_MODE_DRY;
    case FujiMode::FAN:       return climate::CLIMATE_MODE_FAN_ONLY;
    case FujiMode::MODE_AUTO: return climate::CLIMATE_MODE_AUTO;
    default:                  return climate::CLIMATE_MODE_AUTO;
  }
}

FujiMode FujitsuClimate::climate_mode_to_fuji_mode(climate::ClimateMode mode) {
  switch (mode) {
    case climate::CLIMATE_MODE_HEAT:     return FujiMode::HEAT;
    case climate::CLIMATE_MODE_COOL:     return FujiMode::COOL;
    case climate::CLIMATE_MODE_DRY:      return FujiMode::DRY;
    case climate::CLIMATE_MODE_FAN_ONLY: return FujiMode::FAN;
    case climate::CLIMATE_MODE_AUTO:     return FujiMode::MODE_AUTO;
    default:                             return FujiMode::MODE_AUTO;
  }
}

climate::ClimateFanMode FujitsuClimate::fuji_fan_to_climate_fan(FujiFanMode fan) {
  switch (fan) {
    case FujiFanMode::FAN_AUTO: return climate::CLIMATE_FAN_AUTO;
    case FujiFanMode::QUIET:    return climate::CLIMATE_FAN_QUIET;
    case FujiFanMode::FAN_LOW:  return climate::CLIMATE_FAN_LOW;
    case FujiFanMode::MEDIUM:   return climate::CLIMATE_FAN_MEDIUM;
    case FujiFanMode::FAN_HIGH: return climate::CLIMATE_FAN_HIGH;
    default:                    return climate::CLIMATE_FAN_AUTO;
  }
}

FujiFanMode FujitsuClimate::climate_fan_to_fuji_fan(climate::ClimateFanMode fan) {
  switch (fan) {
    case climate::CLIMATE_FAN_AUTO:   return FujiFanMode::FAN_AUTO;
    case climate::CLIMATE_FAN_QUIET:  return FujiFanMode::QUIET;
    case climate::CLIMATE_FAN_LOW:    return FujiFanMode::FAN_LOW;
    case climate::CLIMATE_FAN_MEDIUM: return FujiFanMode::MEDIUM;
    case climate::CLIMATE_FAN_HIGH:   return FujiFanMode::FAN_HIGH;
    default:                          return FujiFanMode::FAN_AUTO;
  }
}

}  // namespace fujitsu_climate
}  // namespace esphome
