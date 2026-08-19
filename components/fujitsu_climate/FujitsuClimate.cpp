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
  update_boot_probe_();
  update_frame_timing_();
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
      corrected_economy_text_sensor_ == nullptr && mystery_bit_text_sensor_ == nullptr &&
      message_dest_text_sensor_ == nullptr && message_type_text_sensor_ == nullptr) {
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

  // messageDest diagnostic (added 18 Aug 2026, continued) -- see FujiHeatPump.h's
  // getCorrMessageDest() comment. Unlike boot_probe/frame_timing this updates every
  // tick (not a one-shot), so the running SECONDARY-sighting count and first-seen
  // timestamp can be watched live -- e.g. across a DIP-mode flip or a real power
  // cycle -- without needing a fresh capture window or a reflash.
  if (message_dest_text_sensor_ != nullptr) {
    if (!have_corr) {
      message_dest_text_sensor_->publish_state("No Data");
    } else {
      uint8_t d = hp_.getCorrMessageDest();
      const char *label;
      switch (d) {
        case 0: label = "START"; break;
        case 1: label = "UNIT"; break;
        case 32: label = "PRIMARY"; break;
        case 33: label = "SECONDARY"; break;
        default: label = "?"; break;
      }
      char buf[100];
      snprintf(buf, sizeof(buf), "cur=%d(%s) sec=%u/%u first_sec@%dms", d, label,
               hp_.getMessageDestSecondaryCount(), hp_.getMessageDestTotalCount(),
               hp_.getMessageDestFirstSecondaryMs());
      message_dest_text_sensor_->publish_state(buf);
    }
  }

  // messageType/writeBit diagnostic (added 19 Aug 2026, 3B.33) -- see FujiHeatPump.h's
  // getCorrMessageType()/getCorrWriteBit() comment. Persistent HA entity (not a
  // one-shot capture) so a real command sent from the WIRED remote can be caught in
  // HA history even if no live log capture happens to be running at that moment.
  if (message_type_text_sensor_ != nullptr) {
    if (!have_corr) {
      message_type_text_sensor_->publish_state("No Data");
    } else {
      uint8_t t = hp_.getCorrMessageType();
      const char *label;
      switch (t) {
        case 0: label = "STATUS"; break;
        case 1: label = "ERROR"; break;
        case 2: label = "LOGIN"; break;
        case 3: label = "?"; break;
        default: label = "?"; break;
      }
      char buf[64];
      snprintf(buf, sizeof(buf), "type=%d(%s) write=%d", t, label, hp_.getCorrWriteBit() ? 1 : 0);
      message_type_text_sensor_->publish_state(buf);
    }
  }
}

void FujitsuClimate::update_boot_probe_() {
  // Always let the (cheap, self-limiting, one-time) log dump happen regardless of
  // whether the text_sensor is configured in YAML -- it's useful via live log
  // capture even without the HA entity.
  hp_.maybeDumpBootCapture();

  if (boot_probe_text_sensor_ == nullptr || boot_probe_published_) {
    return;  // Not configured, or already published once -- this is a one-shot summary.
  }
  if (!hp_.isBootCaptureDumped()) {
    return;  // Capture window hasn't closed yet.
  }
  boot_probe_published_ = true;

  char buf[200];
  snprintf(buf, sizeof(buf),
           "first=%dms ctrl3_alt=%u@%dms unit_addr_alt=%u@%dms captured=%d",
           hp_.getBootFirstFrameMs(), hp_.getBootCtrl3AltCount(), hp_.getBootCtrl3AltMs(),
           hp_.getBootUnitAddrAltCount(), hp_.getBootUnitAddrAltMs(), (int) hp_.getBootCaptureCount());
  boot_probe_text_sensor_->publish_state(buf);
}

void FujitsuClimate::update_frame_timing_() {
  // Always let the (cheap, self-limiting, one-time) log dump happen regardless of
  // whether the text_sensor is configured in YAML -- same rationale as
  // update_boot_probe_() above, and it's the only place the individual CTRL->UNIT
  // gap samples are visible (the HA summary below only carries the aggregate stats).
  hp_.maybeDumpTimingCapture();

  if (frame_timing_text_sensor_ == nullptr || frame_timing_published_) {
    return;  // Not configured, or already published once -- this is a one-shot summary.
  }
  if (!hp_.isTimingCaptureDumped()) {
    return;  // Capture window hasn't closed yet.
  }
  frame_timing_published_ = true;

  char buf[220];
  snprintf(buf, sizeof(buf),
           "u2c: n=%u min=%.1fms max=%.1fms avg=%.1fms | c2u: n=%u min=%.1fms max=%.1fms avg=%.1fms",
           hp_.getTimingUnitToCtrlCount(), hp_.getTimingUnitToCtrlMinUs() / 1000.0f,
           hp_.getTimingUnitToCtrlMaxUs() / 1000.0f, hp_.getTimingUnitToCtrlAvgUs() / 1000.0f,
           hp_.getTimingCtrlToUnitCount(), hp_.getTimingCtrlToUnitMinUs() / 1000.0f,
           hp_.getTimingCtrlToUnitMaxUs() / 1000.0f, hp_.getTimingCtrlToUnitAvgUs() / 1000.0f);
  frame_timing_text_sensor_->publish_state(buf);
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
  // CHANGED 18 Aug 2026 (3B.25): no longer sends here directly. The frame is now armed
  // (has_pending_frame_ set by buildFrame(), called inside setTemperature()) and
  // FujiHeatPump::readFrame() sends it itself, immediately after the next real CTRL
  // frame -- timed to the actual measured CTRL->UNIT gap instead of an unsynchronized
  // fixed delay from this button-press moment. See sendPendingFrame()'s comment for
  // the full reasoning. Expect the TX log lines to appear up to one bus cycle
  // (sub-second) after this log line, not instantly.
  if (hp_.hasPendingFrame()) {
    ESP_LOGW(TAG, "test_setpoint_step: frame armed, will send at the start of the next CTRL->UNIT gap");
  } else {
    ESP_LOGW(TAG, "test_setpoint_step: buildFrame() did not produce a pending frame (see its own log for why)");
  }
}

void FujitsuClimate::test_login_handshake() {
  // Added 18 Aug 2026 (3B.27) -- see FujiHeatPump.h's armLoginHandshake() comment.
  if (!hardware_present_) {
    ESP_LOGW(TAG, "test_login_handshake: no bus frames seen yet, refusing");
    return;
  }
  if (!hp_.hasLastCtrlRaw()) {
    ESP_LOGW(TAG, "test_login_handshake: no real CTRL frame captured yet, refusing");
    return;
  }
  ESP_LOGW(TAG, "TX TEST: login handshake burst (3 LOGIN frames, one per bus cycle) -- "
                "check the wall unit now and watch for any change in the CORR debug log's raw[2] byte");
  hp_.armLoginHandshake(3);
}

void FujitsuClimate::test_login_ack() {
  // Added 18 Aug 2026, continued (3B.29) -- see FujiHeatPump.h's armLoginAckTest()
  // for the full reasoning. First TX test in this project gated on actually being
  // addressed rather than firing on this button press directly.
  if (!hardware_present_) {
    ESP_LOGW(TAG, "test_login_ack: no bus frames seen yet, refusing");
    return;
  }
  if (!hp_.hasLastCtrlRaw()) {
    ESP_LOGW(TAG, "test_login_ack: no real CTRL frame captured yet, refusing");
    return;
  }
  ESP_LOGW(TAG, "TX TEST: arming address-gated LOGIN-ack -- watch sensor.house_aircon_mystery_bit "
                "and the wall unit; will fire on the next messageDest==SECONDARY frame observed");
  hp_.armLoginAckTest();
}

void FujitsuClimate::test_status_command(int delta_c) {
  // Added 18 Aug 2026, continued (3B.31) -- see FujiHeatPump.h's
  // armStatusCommandTest() for the full reasoning. The actual command test, gated on
  // being addressed, using the corrected dest=SECONDARY addressing.
  if (!hardware_present_) {
    ESP_LOGW(TAG, "test_status_command: no bus frames seen yet, refusing");
    return;
  }
  if (!hp_.hasLastCtrlRaw()) {
    ESP_LOGW(TAG, "test_status_command: no real CTRL frame captured yet, refusing");
    return;
  }
  ESP_LOGW(TAG, "TX TEST: arming address-gated STATUS command (delta=%d) -- check the wall unit now, "
                "will fire on the next messageDest==SECONDARY frame observed", delta_c);
  hp_.armStatusCommandTest(delta_c);
}

void FujitsuClimate::test_minimal_setpoint(int delta_c) {
  // Added 19 Aug 2026 (3B.36) -- see FujiHeatPump.h's armMinimalSetpointTest() for the
  // full reasoning. The minimal-clone command test.
  if (!hardware_present_) {
    ESP_LOGW(TAG, "test_minimal_setpoint: no bus frames seen yet, refusing");
    return;
  }
  if (!hp_.hasLastCtrlRaw()) {
    ESP_LOGW(TAG, "test_minimal_setpoint: no real CTRL frame captured yet, refusing");
    return;
  }
  ESP_LOGW(TAG, "TX TEST: arming minimal-clone command (delta=%d) -- check the wall unit now, "
                "will fire on the next messageDest==SECONDARY frame observed", delta_c);
  hp_.armMinimalSetpointTest(delta_c);
}

void FujitsuClimate::test_minimal_setpoint_own_source(int delta_c) {
  // Added 19 Aug 2026 (3B.37) -- see FujiHeatPump.h's armMinimalSetpointOwnSourceTest()
  // for the full reasoning.
  if (!hardware_present_) {
    ESP_LOGW(TAG, "test_minimal_setpoint_own_source: no bus frames seen yet, refusing");
    return;
  }
  if (!hp_.hasLastCtrlRaw()) {
    ESP_LOGW(TAG, "test_minimal_setpoint_own_source: no real CTRL frame captured yet, refusing");
    return;
  }
  ESP_LOGW(TAG, "TX TEST: arming minimal-clone own-source command (delta=%d) -- check the wall unit now, "
                "will fire on the next messageDest==SECONDARY frame observed", delta_c);
  hp_.armMinimalSetpointOwnSourceTest(delta_c);
}

void FujitsuClimate::test_mode_command(FujiMode mode) {
  // Added 19 Aug 2026 (3B.38) -- see FujiHeatPump.h's armModeCommandTest() for the
  // full reasoning. TX test target pivoted to mode/power per James's direction --
  // verify via sensor.house_aircon_mode / climate.aircon_fujitsu_heat_pump AND
  // switch.ac (independent oracle), not the physical display.
  if (!hardware_present_) {
    ESP_LOGW(TAG, "test_mode_command: no bus frames seen yet, refusing");
    return;
  }
  if (!hp_.hasLastCtrlRaw()) {
    ESP_LOGW(TAG, "test_mode_command: no real CTRL frame captured yet, refusing");
    return;
  }
  ESP_LOGW(TAG, "TX TEST: arming address-gated mode command (mode=%d) -- check switch.ac / corrected_mode, "
                "will fire on the next messageDest==SECONDARY frame observed", static_cast<int>(mode));
  hp_.armModeCommandTest(mode);
}

void FujitsuClimate::test_power_command(bool on) {
  // Added 19 Aug 2026 (3B.38) -- see FujiHeatPump.h's armPowerCommandTest() for the
  // full reasoning.
  if (!hardware_present_) {
    ESP_LOGW(TAG, "test_power_command: no bus frames seen yet, refusing");
    return;
  }
  if (!hp_.hasLastCtrlRaw()) {
    ESP_LOGW(TAG, "test_power_command: no real CTRL frame captured yet, refusing");
    return;
  }
  ESP_LOGW(TAG, "TX TEST: arming address-gated power command (on=%s) -- check switch.ac / corrected_mode, "
                "will fire on the next messageDest==SECONDARY frame observed", on ? "true" : "false");
  hp_.armPowerCommandTest(on);
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
