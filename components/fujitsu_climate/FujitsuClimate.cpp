#include "FujitsuClimate.h"
#include "esphome/core/log.h"

namespace esphome {
namespace fujitsu_climate {

static const char *const TAG = "fujitsu.climate";

void FujitsuClimate::setup() {
  // Connect to heat pump via UART
  hp_.connect(this->parent_, true);  // true = secondary controller
  hp_.setDebug(true);  // Enable debug logging
  
  ESP_LOGI(TAG, "Fujitsu Climate component initialized");
  ESP_LOGI(TAG, "WARNING: No hardware connected yet - this is Phase 3A (software only)");
  
  // Don't try to read initial state in Phase 3A (no hardware)
  // We'll just start with default values
  ESP_LOGI(TAG, "Skipping initial state read - will listen in loop()");
}

void FujitsuClimate::loop() {
  // Try to read frames from the bus
  if (hp_.readFrame()) {
    update_climate_state();
  }
  
  // Send any pending commands
  if (hp_.hasPendingFrame()) {
    hp_.sendPendingFrame();
  }
  
  // Periodically publish state to Home Assistant
  uint32_t now = millis();
  if (now - last_publish_ > PUBLISH_INTERVAL_MS) {
    this->publish_state();
    last_publish_ = now;
  }
}

void FujitsuClimate::dump_config() {
  ESP_LOGCONFIG(TAG, "Fujitsu Heat Pump Climate:");
  ESP_LOGCONFIG(TAG, "  Connected: %s", hp_.isConnected() ? "YES" : "NO");
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
  
  // Handle mode changes
  if (call.get_mode().has_value()) {
    climate::ClimateMode mode = *call.get_mode();
    ESP_LOGD(TAG, "Mode change requested: %d", mode);
    
    if (mode == climate::CLIMATE_MODE_OFF) {
      hp_.setOnOff(false);
    } else {
      hp_.setOnOff(true);
      hp_.setMode(climate_mode_to_fuji_mode(mode));
    }
  }
  
  // Handle target temperature
  if (call.get_target_temperature().has_value()) {
    float temp = *call.get_target_temperature();
    ESP_LOGD(TAG, "Temperature change requested: %.1f", temp);
    hp_.setTemperature(temp);
  }
  
  // Handle fan mode
  if (call.get_fan_mode().has_value()) {
    climate::ClimateFanMode fan = *call.get_fan_mode();
    ESP_LOGD(TAG, "Fan mode change requested: %d", fan);
    hp_.setFanMode(climate_fan_to_fuji_fan(fan));
  }
  
  // Update state immediately
  update_climate_state();
}

void FujitsuClimate::update_climate_state() {
  // Update mode
  if (hp_.getOnOff()) {
    this->mode = fuji_mode_to_climate_mode(hp_.getMode());
  } else {
    this->mode = climate::CLIMATE_MODE_OFF;
  }
  
  // Update target temperature
  this->target_temperature = hp_.getTemperature();
  
  // Update current temperature
  this->current_temperature = hp_.getCurrentTemperature();
  
  // Update fan mode
  this->fan_mode = fuji_fan_to_climate_fan(hp_.getFanMode());
  
  // Update action (what the heat pump is currently doing)
  if (!hp_.getOnOff()) {
    this->action = climate::CLIMATE_ACTION_OFF;
  } else {
    switch (hp_.getMode()) {
      case FujiMode::HEAT:
        this->action = climate::CLIMATE_ACTION_HEATING;
        break;
      case FujiMode::COOL:
        this->action = climate::CLIMATE_ACTION_COOLING;
        break;
      case FujiMode::DRY:
        this->action = climate::CLIMATE_ACTION_DRYING;
        break;
      case FujiMode::FAN:
        this->action = climate::CLIMATE_ACTION_FAN;
        break;
      default:
        this->action = climate::CLIMATE_ACTION_IDLE;
        break;
    }
  }
  
  // Publish state to Home Assistant
  this->publish_state();
  
  ESP_LOGD(TAG, "State updated - Mode: %d, Target: %.1f°C, Current: %.1f°C",
           this->mode, this->target_temperature, this->current_temperature);
}

climate::ClimateMode FujitsuClimate::fuji_mode_to_climate_mode(FujiMode mode) {
  switch (mode) {
    case FujiMode::MODE_AUTO:
      return climate::CLIMATE_MODE_AUTO;
    case FujiMode::COOL:
      return climate::CLIMATE_MODE_COOL;
    case FujiMode::DRY:
      return climate::CLIMATE_MODE_DRY;
    case FujiMode::FAN:
      return climate::CLIMATE_MODE_FAN_ONLY;
    case FujiMode::HEAT:
      return climate::CLIMATE_MODE_HEAT;
    default:
      return climate::CLIMATE_MODE_AUTO;
  }
}

FujiMode FujitsuClimate::climate_mode_to_fuji_mode(climate::ClimateMode mode) {
  switch (mode) {
    case climate::CLIMATE_MODE_AUTO:
      return FujiMode::MODE_AUTO;
    case climate::CLIMATE_MODE_COOL:
      return FujiMode::COOL;
    case climate::CLIMATE_MODE_DRY:
      return FujiMode::DRY;
    case climate::CLIMATE_MODE_FAN_ONLY:
      return FujiMode::FAN;
    case climate::CLIMATE_MODE_HEAT:
      return FujiMode::HEAT;
    default:
      return FujiMode::MODE_AUTO;
  }
}

climate::ClimateFanMode FujitsuClimate::fuji_fan_to_climate_fan(FujiFanMode fan) {
  switch (fan) {
    case FujiFanMode::FAN_AUTO:
      return climate::CLIMATE_FAN_AUTO;
    case FujiFanMode::QUIET:
      return climate::CLIMATE_FAN_QUIET;
    case FujiFanMode::FAN_LOW:
      return climate::CLIMATE_FAN_LOW;
    case FujiFanMode::MEDIUM:
      return climate::CLIMATE_FAN_MEDIUM;
    case FujiFanMode::FAN_HIGH:
      return climate::CLIMATE_FAN_HIGH;
    default:
      return climate::CLIMATE_FAN_AUTO;
  }
}

FujiFanMode FujitsuClimate::climate_fan_to_fuji_fan(climate::ClimateFanMode fan) {
  switch (fan) {
    case climate::CLIMATE_FAN_AUTO:
      return FujiFanMode::FAN_AUTO;
    case climate::CLIMATE_FAN_QUIET:
      return FujiFanMode::QUIET;
    case climate::CLIMATE_FAN_LOW:
      return FujiFanMode::FAN_LOW;
    case climate::CLIMATE_FAN_MEDIUM:
      return FujiFanMode::MEDIUM;
    case climate::CLIMATE_FAN_HIGH:
      return FujiFanMode::FAN_HIGH;
    default:
      return FujiFanMode::FAN_AUTO;
  }
}

}  // namespace fujitsu_climate
}  // namespace esphome
