#include "FujitsuClimate.h"
#include "esphome/core/log.h"
#include <cstdio>

namespace esphome {
namespace fujitsu_climate {

static const char *const TAG = "fujitsu.climate";

// How often to publish state to Home Assistant (ms)
constexpr uint32_t PUBLISH_INTERVAL_MS = 1000;

FujitsuClimate::FujitsuClimate() : PollingComponent(PUBLISH_INTERVAL_MS) {
  // Use NAN for temperatures — ESPHome/HA renders these as "unknown" until
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

  ESP_LOGI(TAG, "Fujitsu Climate Phase 3B initialized — hardware connected, listen mode");
  ESP_LOGI(TAG, "Waiting for LIN bus frames on UART (500 baud 8N1)...");
}

void FujitsuClimate::loop() {
  // Called every main-loop tick — drain all available frames immediately.
  // This ensures no bus traffic is missed between HA publish intervals.
  while (hp_.readFrame()) {
    if (!hardware_present_) {
      hardware_present_ = true;
      ESP_LOGI(TAG, "First frame received — hardware confirmed present");
    }
    update_climate_state();
  }

  // Send any pending commands immediately after receiving
  if (hardware_present_ && hp_.hasPendingFrame()) {
    hp_.sendPendingFrame();
  }
}

void FujitsuClimate::update() {
  // Called on interval (1 s) — just publish current state to Home Assistant
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
      corrected_setpoint_text_sensor_ == nullptr && corrected_room_temp_text_sensor_ == nullptr &&
      corrected_economy_text_sensor_ == nullptr) {
    return;  // Not configured in YAML -- nothing to do.
  }

  uint32_t last_corr = hp_.getCorrLastUpdateTime();
  bool have_corr = (last_corr != 0) && (millis() - last_corr < BUS_FRAME_TIMEOUT_MS);

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
      char buf[24];
      snprintf(buf, sizeof(buf), "%s (raw=%d)", label, m);
      corrected_mode_text_sensor_->publish_state(buf);
    }
  }

  if (corrected_fan_raw_text_sensor_ != nullptr) {
    if (!have_corr) {
      corrected_fan_raw_text_sensor_->publish_state("No Data");
    } else {
      char buf[8];
      snprintf(buf, sizeof(buf), "%d", hp_.getCorrFanRaw());
      corrected_fan_raw_text_sensor_->publish_state(buf);
    }
  }

  if (corrected_setpoint_text_sensor_ != nullptr) {
    if (!have_corr) {
      corrected_setpoint_text_sensor_->publish_state("No Data");
    } else {
      uint8_t sp = hp_.getCorrSetpointRaw();
      char buf[16];
      if (sp == 0) {
        snprintf(buf, sizeof(buf), "None");
      } else {
        snprintf(buf, sizeof(buf), "%dC", sp);
      }
      corrected_setpoint_text_sensor_->publish_state(buf);
    }
  }

  if (corrected_room_temp_text_sensor_ != nullptr) {
    if (!have_corr) {
      corrected_room_temp_text_sensor_->publish_state("No Data");
    } else {
      char buf[16];
      snprintf(buf, sizeof(buf), "%dC", hp_.getCorrRoomTempRaw());
      corrected_room_temp_text_sensor_->publish_state(buf);
    }
  }

  if (corrected_economy_text_sensor_ != nullptr) {
    if (!have_corr) {
      corrected_economy_text_sensor_->publish_state("No Data");
    } else {
      corrected_economy_text_sensor_->publish_state(hp_.getCorrEconomy() ? "ON" : "OFF");
    }
  }
}

void FujitsuClimate::dump_config() {
  ESP_LOGCONFIG(TAG, "Fujitsu Heat Pump Climate:");
  ESP_LOGCONFIG(TAG, "  Hardware present: %s", hardware_present_ ? "YES" : "NO");
  ESP_LOGCONFIG(TAG, "  Controller: Secondary");
  ESP_LOGCONFIG(TAG, "  LIN Interface: TJA1021");
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
  ESP_LOGD(TAG, "Climate control called");

  if (!hardware_present_) {
    ESP_LOGW(TAG, "Hardware not present - ignoring control commands");
    return;
  }

  // Handle mode changes
  if (call.get_mode().has_value()) {
    climate::ClimateMode m = *call.get_mode();
    if (m == climate::CLIMATE_MODE_OFF) {
      hp_.setOnOff(false);
    } else {
      hp_.setOnOff(true);
      hp_.setMode(climate_mode_to_fuji_mode(m));
    }
  }

  // Handle target temperature
  if (call.get_target_temperature().has_value()) {
    hp_.setTemperature(*call.get_target_temperature());
  }

  // Handle fan mode
  if (call.get_fan_mode().has_value()) {
    hp_.setFanMode(climate_fan_to_fuji_fan(*call.get_fan_mode()));
  }

  // Update state immediately
  update_climate_state();
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
    ESP_LOGD(TAG, "State updated - Mode: %d, Target: %.1f°C, Current: %.1f°C",
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
