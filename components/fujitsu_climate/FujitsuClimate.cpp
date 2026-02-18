#include "FujitsuClimate.h"
#include "esphome/core/log.h"

namespace esphome {
namespace fujitsu_climate {

static const char *const TAG = "fujitsu.climate";

// Poll interval for updating Home Assistant (ms)
constexpr uint32_t PUBLISH_INTERVAL_MS = 5000;

FujitsuClimate::FujitsuClimate() : PollingComponent(PUBLISH_INTERVAL_MS) {
  // Initialize defaults for safe discovery
  target_temperature = 22.0f;
  current_temperature = 22.0f;
  mode = climate::CLIMATE_MODE_OFF;
  fan_mode = climate::CLIMATE_FAN_AUTO;
  action = climate::CLIMATE_ACTION_OFF;
  hardware_present_ = false;
}

void FujitsuClimate::setup() {
  // Try to connect to heat pump; returns true if successful
  hardware_present_ = hp_.connect(this->parent_, true);
  
  // Enable debug logging only if hardware is present
  hp_.setDebug(hardware_present_);

  ESP_LOGI(TAG, "Fujitsu Climate initialized. Hardware present: %s",
           hardware_present_ ? "YES" : "NO");

  // Skipping initial state read if no hardware connected
  if (!hardware_present_) {
    ESP_LOGW(TAG, "No hardware connected - starting with default values");
  }
}

void FujitsuClimate::update() {
  // Called periodically by PollingComponent (non-blocking)
  
  if (!hardware_present_) {
    // Optional: could attempt to reconnect every N cycles
    return;
  }

  // Read frame from bus if available
  if (hp_.readFrame()) {
    update_climate_state();
  }

  // Send any pending commands
  if (hp_.hasPendingFrame()) {
    hp_.sendPendingFrame();
  }

  // Publish current state to Home Assistant
  publish_state();
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

  ESP_LOGD(TAG, "State updated - Mode: %d, Target: %.1f°C, Current: %.1f°C",
           mode, target_temperature, current_temperature);
}

// Mode and fan mapping functions remain unchanged
climate::ClimateMode FujitsuClimate::fuji_mode_to_climate_mode(FujiMode mode) { /* ... */ }
FujiMode FujitsuClimate::climate_mode_to_fuji_mode(climate::ClimateMode mode) { /* ... */ }
climate::ClimateFanMode FujitsuClimate::fuji_fan_to_climate_fan(FujiFanMode fan) { /* ... */ }
FujiFanMode FujitsuClimate::climate_fan_to_fuji_fan(climate::ClimateFanMode fan) { /* ... */ }

}  // namespace fujitsu_climate
}  // namespace esphome