#include "FujitsuClimate.h"
#include "esphome/core/log.h"
#include <cmath>

// ---------------------------------------------------------------------------
// Phase 4 rebuild (19 Aug 2026) -- see FujitsuClimate.h and
// tx-architecture-review-and-adoption-plan.md for the full reasoning.
//
// Diagnostics retired in this rebuild (all were built to reverse-engineer/validate
// this project's OLD hand-rolled decode, byte by byte, before the field layout was
// known to already match upstream): corrected_mode/corrected_fan_raw/
// corrected_economy text sensors (redundant now -- the climate entity's own mode/
// fan_mode/preset are the vendored engine's decode directly, nothing left to
// cross-check against), message_dest/message_type (existed to confirm upstream's
// addressing/writeBit model live -- confirmed, 3B.29-3B.38, no longer needed as a
// running diagnostic), boot_probe/frame_timing (one-shot exploratory captures from
// before the addressing model was understood -- superseded). Kept: bus_alive/
// bus_status (still the project's real safety-monitoring signal), corrected_setpoint/
// corrected_room_temp (numeric, already used elsewhere in HA history/automations per
// state-of-play.md).
//
// 20 Aug 2026: re-added a diagnostic for the old "Mystery Bit" (frame[6] bit0 /
// controllerPresent), labelled "Thermo Sensor (unconfirmed)" rather than as a solved
// Local/Remote readout -- see FujiHeatPump.h's Deviation #3 and
// test-and-dev-workflow.md's "Thermo Sensor / Mystery Bit investigation" for why the
// mapping is still not settled. Also fixed a real bug in update_climate_state_()'s
// target-temperature handling: it was clearing target_temperature to NAN whenever the
// raw byte happened to read exactly 0 in ANY mode, when the intent (per its own
// comment) was only ever to suppress a setpoint in FAN_ONLY mode, which genuinely
// carries none. Scoped the clamp to FAN_ONLY specifically.
// ---------------------------------------------------------------------------

namespace esphome {
namespace fujitsu_climate {

static const char *const TAG = "fujitsu.climate";

#ifdef ESP32
// Dedicated bus-I/O task -- see FujitsuClimate.h's top-of-file comment. Modelled on
// the reference esphome-fujitsu wrapper's serialTask(), adapted for this project's
// own measured timing (no delay(60) -- FujiHeatPump::sendPendingFrame()'s reply gate
// is already 0 here, see FujiHeatPump.h's Deviation #1) and to yield explicitly when
// idle rather than busy-spinning, since this bus does have real idle stretches
// between UNIT+CTRL cycles (this project's own 3B.23 measurement).
static void fujitsu_bus_task(void *pv) {
  auto *climate = static_cast<FujitsuClimate *>(pv);
  ESP_LOGD(TAG, "fujitsu_bus_task started on core %d", xPortGetCoreID());

  for (;;) {
    if (climate->heat_pump.waitForFrame()) {
      // Reply immediately -- see FujiHeatPump.h's Deviation #1 comment for why this
      // project doesn't reproduce upstream's delay(60) here.
      climate->heat_pump.sendPendingFrame();
      climate->pending_update = false;

      if (xSemaphoreTake(climate->lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        memcpy(&(climate->shared_state), climate->heat_pump.getCurrentState(), sizeof(FujiFrame));
        xSemaphoreGive(climate->lock);
      }
    } else {
      // Nothing was available to read this pass -- yield rather than busy-spinning.
      // This bus has real idle stretches between UNIT+CTRL cycles (3B.23 measured up
      // to ~11ms in Dual DIP mode, and there's always some gap between full 16-byte
      // cycles even in Normal mode), so a 1-tick yield here doesn't risk missing a
      // reply window the way it would if inserted after a real receive.
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
}
#endif

void FujitsuClimate::setup() {
  target_temperature = NAN;
  current_temperature = NAN;
  mode = climate::CLIMATE_MODE_OFF;
  fan_mode = climate::CLIMATE_FAN_AUTO;
  action = climate::CLIMATE_ACTION_IDLE;

#ifdef ESP32
  this->lock = xSemaphoreCreateBinary();
  xSemaphoreGive(this->lock);
  memcpy(&this->shared_state, this->heat_pump.getCurrentState(), sizeof(FujiFrame));

  // Pull-up on the RX pin -- carried forward unchanged from the previous version's
  // uart: block (hardware-and-protocol.md: "Without it the floating pin crashes the
  // ESP32 when powered but not wired to the bus"). Set explicitly here since the
  // vendored engine owns the pin directly now rather than through ESPHome's uart:
  // component, which used to apply this via its own `mode: {pullup: true}` option.
  pinMode(this->rx_pin_, INPUT_PULLUP);

  this->heat_pump.connect(&Serial2, /*secondary=*/true, this->rx_pin_, this->tx_pin_);
  // Upstream's own debugPrint field (not a setDebug() method -- vendored verbatim,
  // see FujiHeatPump.h) defaults to false already; set explicitly here for clarity.
  this->heat_pump.debugPrint = false;

  ESP_LOGI(TAG, "Fujitsu Climate (Phase 4): connecting as SECONDARY on Serial2, rx=%d tx=%d",
           this->rx_pin_, this->tx_pin_);

  xTaskCreatePinnedToCore(fujitsu_bus_task, "FujiTask", 10000, (void *) this, configMAX_PRIORITIES - 1,
                          &(this->task_handle), 1);
#else
  ESP_LOGE(TAG, "fujitsu_climate requires ESP32 (FreeRTOS task + Serial2)");
#endif
}

void FujitsuClimate::loop() {
  this->update_climate_state_();
  this->update_bus_status_();
}

void FujitsuClimate::update_bus_status_() {
  // Deliberately reading heat_pump's raw timestamp directly rather than through the
  // semaphore-guarded shared_state -- same simplification the pre-rewrite code made
  // for this exact value (getLastFrameTime()), across many live sessions with no
  // observed ill effect. It's a single machine word; worst case a status display is
  // one tick stale, which doesn't matter for a health indicator on a ~1s cadence.
  unsigned long last_frame = this->heat_pump.getLastFrameReceived();
  uint32_t now = millis();
  bool alive = last_frame != 0 && (now - static_cast<uint32_t>(last_frame)) < BUS_TIMEOUT_MS;

  if (!this->hardware_present_ && last_frame != 0) {
    this->hardware_present_ = true;
    ESP_LOGI(TAG, "First frame received -- hardware confirmed present");
  }

  uint8_t state = alive ? 1 : (last_frame != 0 ? 2 : 3);

  // loop() runs far faster than this actually changes -- only publish_state() on a
  // real transition, not on every tick (see the header comment next to
  // bus_status_initialized_ for why this gate exists).
  if (!this->bus_status_initialized_ || alive != this->last_bus_alive_) {
    this->bus_status_initialized_ = true;
    this->last_bus_alive_ = alive;
    if (this->bus_alive_binary_sensor_ != nullptr) {
      this->bus_alive_binary_sensor_->publish_state(alive);
    }
  }

  if (state != this->last_bus_state_) {
    this->last_bus_state_ = state;
    if (this->bus_status_text_sensor_ != nullptr) {
      this->bus_status_text_sensor_->publish_state(alive ? "Bus OK" : (last_frame != 0 ? "No Signal" : "Never Seen"));
    }
  }

  // 20 Aug 2026 -- passive, unconfirmed diagnostic. See FujiHeatPump.h's Deviation #3
  // and this file's top-of-file comment: this is the old "Mystery Bit"
  // (frame[6] bit0 / controllerPresent), read directly off the incoming frame before
  // our own reply logic reuses that field, but its mapping to the UTY-RNNUM's Thermo
  // Sensor Local/Remote setting was never confirmed across four rounds of live
  // testing (test-and-dev-workflow.md) -- reported here as a raw bit, not a label.
  if (this->hardware_present_ && this->thermo_sensor_text_sensor_ != nullptr) {
    int bit = this->heat_pump.getLastRawControllerPresent();
    if (!this->thermo_sensor_initialized_ || bit != this->last_thermo_sensor_bit_) {
      this->thermo_sensor_initialized_ = true;
      this->last_thermo_sensor_bit_ = bit;
      this->thermo_sensor_text_sensor_->publish_state(bit ? "1 (unconfirmed)" : "0 (unconfirmed)");
    }
  }

  // 21 Aug 2026 -- second passive/unconfirmed diagnostic, for FujiFrame::unknownBit
  // (readBuf[1] bit 7). Never exposed before today -- added as a live-test candidate
  // for the Thermo Sensor setting after controllerPresent was ruled out (two
  // USB-tethered live tests, same day, zero correlation with the physical button).
  // Same passive capture point/reasoning, same change-gated publish pattern.
  if (this->hardware_present_ && this->unknown_bit_text_sensor_ != nullptr) {
    int bit = this->heat_pump.getLastRawUnknownBit();
    if (!this->unknown_bit_initialized_ || bit != this->last_unknown_bit_) {
      this->unknown_bit_initialized_ = true;
      this->last_unknown_bit_ = bit;
      this->unknown_bit_text_sensor_->publish_state(bit ? "1 (unconfirmed)" : "0 (unconfirmed)");
    }
  }

  // 21 Aug 2026 -- full raw frame diagnostic (see FujiHeatPump.h's lastRawFrame).
  // Throttled to at most one publish every 250ms on top of the change-gate below --
  // unlike the single-bit diagnostics above, this changes on almost every valid
  // frame during normal operation (temperature/updateMagic churn alone would do
  // it), so an un-throttled change-gate here would reproduce exactly the
  // unthrottled hot-path publish pattern this project's prior crashes trace back
  // to. 250ms bounds it to ~4/s even during a Thermo Sensor-style oscillation burst.
  if (this->hardware_present_ && this->raw_frame_text_sensor_ != nullptr) {
    byte *frame = this->heat_pump.getLastRawFrame();
    uint32_t now = millis();
    bool changed = !this->raw_frame_initialized_ || memcmp(frame, this->last_raw_frame_, 8) != 0;
    bool due = (now - this->last_raw_frame_publish_ms_) > 250;
    if (changed && (due || !this->raw_frame_initialized_)) {
      this->raw_frame_initialized_ = true;
      memcpy(this->last_raw_frame_, frame, 8);
      this->last_raw_frame_publish_ms_ = now;
      char buf[24];
      snprintf(buf, sizeof(buf), "%02X %02X %02X %02X %02X %02X %02X %02X",
               frame[0], frame[1], frame[2], frame[3], frame[4], frame[5], frame[6], frame[7]);
      this->raw_frame_text_sensor_->publish_state(buf);
    }
  }
}

void FujitsuClimate::update_climate_state_() {
  if (!this->hardware_present_) {
    return;  // keep the NAN/OFF defaults until the first real frame arrives
  }

  FujiFrame local{};
#ifdef ESP32
  if (xSemaphoreTake(this->lock, pdMS_TO_TICKS(50)) != pdTRUE) {
    return;  // didn't get the lock this tick -- try again next loop() call, cheap either way
  }
  local = this->shared_state;
  xSemaphoreGive(this->lock);
#else
  return;
#endif

  bool updated = false;

  bool on = local.onOff == 1;
  climate::ClimateMode new_mode = on ? this->fuji_mode_to_climate_mode_(static_cast<FujiMode>(local.acMode))
                                      : climate::CLIMATE_MODE_OFF;
  if (new_mode != this->mode) {
    this->mode = new_mode;
    updated = true;
  }

  // Fixed 20 Aug 2026: this used to clear the setpoint to NAN whenever the raw byte
  // read exactly 0, in ANY mode -- but per hardware-and-protocol.md/state-of-play.md,
  // only FAN_ONLY genuinely carries no setpoint; every other mode (including AUTO)
  // has been live-confirmed to report a real setpoint. Scoping this to FAN_ONLY
  // specifically stops a real setpoint from being hidden in other modes.
  float new_target = static_cast<float>(local.temperature);
  if (new_mode == climate::CLIMATE_MODE_FAN_ONLY) {
    new_target = NAN;  // FAN mode carries no setpoint -- see hardware-and-protocol.md
  }
  if (!(new_target == this->target_temperature) && !(std::isnan(new_target) && std::isnan(this->target_temperature))) {
    this->target_temperature = new_target;
    updated = true;
  }

  float new_current = static_cast<float>(local.controllerTemp);
  if (new_current != this->current_temperature) {
    this->current_temperature = new_current;
    updated = true;
  }

  climate::ClimateFanMode new_fan = this->fuji_fan_to_climate_fan_(static_cast<FujiFanMode>(local.fanMode));
  if (new_fan != this->fan_mode) {
    this->fan_mode = new_fan;
    updated = true;
  }

  climate::ClimatePreset new_preset =
      local.economyMode ? climate::CLIMATE_PRESET_ECO : climate::CLIMATE_PRESET_NONE;
  if (new_preset != this->preset) {
    this->preset = new_preset;
    updated = true;
  }

  climate::ClimateAction new_action = climate::CLIMATE_ACTION_IDLE;
  if (!on) {
    new_action = climate::CLIMATE_ACTION_OFF;
  } else {
    switch (static_cast<FujiMode>(local.acMode)) {
      case FujiMode::HEAT: new_action = climate::CLIMATE_ACTION_HEATING; break;
      case FujiMode::COOL: new_action = climate::CLIMATE_ACTION_COOLING; break;
      case FujiMode::DRY: new_action = climate::CLIMATE_ACTION_DRYING; break;
      case FujiMode::FAN: new_action = climate::CLIMATE_ACTION_FAN; break;
      default: new_action = climate::CLIMATE_ACTION_IDLE; break;
    }
  }
  if (new_action != this->action) {
    this->action = new_action;
    updated = true;
  }

  if (updated) {
    uint32_t now_ms = millis();
    if (now_ms - this->state_log_last_ms_ >= 1000) {
      this->state_log_last_ms_ = now_ms;
      ESP_LOGD(TAG, "State updated - mode=%d target=%.0f current=%.0f fan=%d preset=%d", this->mode,
               this->target_temperature, this->current_temperature, this->fan_mode.value_or(-1), this->preset.value_or(-1));
    }
    this->publish_state();
  }

  // Same publish-flood problem as update_bus_status_() above: these two run every
  // loop() tick, so they need their own change-detection rather than publishing
  // unconditionally every time (NaN-safe -- local.temperature == 0 in FAN mode is a
  // real, recurring value, not a one-off).
  float new_setpoint = new_mode == climate::CLIMATE_MODE_FAN_ONLY ? NAN : static_cast<float>(local.temperature);
  bool setpoint_changed = !this->corrected_sensors_initialized_ ||
                           !(new_setpoint == this->last_corrected_setpoint_ ||
                             (std::isnan(new_setpoint) && std::isnan(this->last_corrected_setpoint_)));
  if (setpoint_changed) {
    this->last_corrected_setpoint_ = new_setpoint;
    if (this->corrected_setpoint_sensor_ != nullptr) {
      this->corrected_setpoint_sensor_->publish_state(new_setpoint);
    }
  }

  float new_room_temp = static_cast<float>(local.controllerTemp);
  if (!this->corrected_sensors_initialized_ || new_room_temp != this->last_corrected_room_temp_) {
    this->last_corrected_room_temp_ = new_room_temp;
    if (this->corrected_room_temp_sensor_ != nullptr) {
      this->corrected_room_temp_sensor_->publish_state(new_room_temp);
    }
  }

  this->corrected_sensors_initialized_ = true;
}

void FujitsuClimate::dump_config() {
  ESP_LOGCONFIG(TAG, "Fujitsu Heat Pump Climate (Phase 4 -- vendored unreality/FujiHeatPump engine):");
  ESP_LOGCONFIG(TAG, "  RX pin: GPIO%d  TX pin: GPIO%d", this->rx_pin_, this->tx_pin_);
  ESP_LOGCONFIG(TAG, "  Role: SECONDARY controller (continuous reply loop, dedicated FreeRTOS task)");
  ESP_LOGCONFIG(TAG, "  Control enabled: %s (see the \"Aircon Control Enabled\" switch)",
                this->control_enabled_ ? "YES -- LIVE" : "no -- read-only");
}

void FujitsuClimate::set_control_enabled(bool enabled) {
  this->control_enabled_ = enabled;
  ESP_LOGW(TAG, "Aircon control %s", enabled ? "ENABLED -- HA commands will now be sent to the real unit"
                                              : "disabled -- back to read-only monitoring");
}

climate::ClimateTraits FujitsuClimate::traits() {
  auto traits = climate::ClimateTraits();

  traits.set_supported_modes({
      climate::CLIMATE_MODE_OFF,
      climate::CLIMATE_MODE_AUTO,
      climate::CLIMATE_MODE_COOL,
      climate::CLIMATE_MODE_HEAT,
      climate::CLIMATE_MODE_DRY,
      climate::CLIMATE_MODE_FAN_ONLY,
  });

  traits.set_supported_fan_modes({
      climate::CLIMATE_FAN_AUTO,
      climate::CLIMATE_FAN_QUIET,
      climate::CLIMATE_FAN_LOW,
      climate::CLIMATE_FAN_MEDIUM,
      climate::CLIMATE_FAN_HIGH,
  });

  traits.set_supported_presets({
      climate::CLIMATE_PRESET_NONE,
      climate::CLIMATE_PRESET_ECO,
  });

  traits.set_supports_current_temperature(true);
  traits.set_visual_min_temperature(16.0f);
  traits.set_visual_max_temperature(30.0f);
  traits.set_visual_temperature_step(1.0f);
  traits.set_supports_two_point_target_temperature(false);
  traits.set_supports_action(true);

  return traits;
}

void FujitsuClimate::control(const climate::ClimateCall &call) {
  // Kill switch, per hardware-and-protocol.md's "ESP32 is never the boss" design
  // principle. This is the first time this project's control() path has ever been
  // wired to real setters rather than logging and ignoring every command; command
  // adoption under the new continuous-reply architecture was confirmed live on 20 Aug
  // 2026 (state-of-play.md's "4A.1" section) after a locking hardening pass following
  // a crash. Per James's request, "Aircon Control Enabled" now defaults to on (see
  // retrofujitsu.yaml) rather than requiring it to be flipped on each session --
  // this switch still exists and still gates every command, so control can always be
  // switched back to read-only from HA or the web UI without touching hardware.
  if (!this->control_enabled_) {
    ESP_LOGW(TAG, "Climate control request ignored -- flip \"Aircon Control Enabled\" to arm live control");
    return;
  }

  // NOTE (updated after the first live control test, 20 Aug 2026): these setters
  // mutate heat_pump's internal updateFields/updateState directly from this (the
  // ESPHome main/API) thread, while the dedicated bus task concurrently reads/writes
  // the same FujiHeatPump instance on the other core. This originally matched the
  // reference esphome-fujitsu wrapper's own control()/setState() pattern exactly (it
  // has the same gap, and is reportedly used this way without issue elsewhere) --
  // but this project's first-ever live control test ended in the ESP32 crashing a few
  // minutes after control was enabled, and this unguarded cross-core access is a real,
  // pre-existing candidate cause (not confirmed -- no serial/crash backtrace was
  // captured; a brownout from the compressor's relay switching on the shared 12V rail
  // is an equally plausible, unrelated explanation). Since it costs nothing to close a
  // known race regardless of whether it's the actual cause, control()'s writes are now
  // taken under the same lock the bus task already uses for shared_state, so at least
  // this specific access can no longer race the bus task's own use of heat_pump. This
  // does NOT fully eliminate contention inside waitForFrame() itself (that's vendored,
  // unmodified upstream code, and holding this lock across its full blocking call would
  // starve update_climate_state_()'s own reads) -- so treat this as real hardening, not
  // a confirmed fix. Getting an actual USB-serial capture on the next crash is the only
  // way to know the true cause for certain.
  bool updated = false;

#ifdef ESP32
  if (xSemaphoreTake(this->lock, pdMS_TO_TICKS(100)) != pdTRUE) {
    ESP_LOGW(TAG, "Climate control request dropped -- could not get the bus lock in time, try again");
    return;
  }
#endif

  if (call.get_mode().has_value()) {
    auto climate_mode = call.get_mode().value();
    if (climate_mode == climate::CLIMATE_MODE_OFF) {
      this->heat_pump.setOnOff(false);
    } else {
      this->heat_pump.setMode(static_cast<byte>(this->climate_mode_to_fuji_mode_(climate_mode)));
      this->heat_pump.setOnOff(true);
    }
    updated = true;
  }

  if (call.get_target_temperature().has_value()) {
    this->heat_pump.setTemp(static_cast<byte>(call.get_target_temperature().value()));
    updated = true;
  }

  if (call.get_fan_mode().has_value()) {
    this->heat_pump.setFanMode(static_cast<byte>(this->climate_fan_to_fuji_fan_(call.get_fan_mode().value())));
    updated = true;
  }

  if (call.get_preset().has_value()) {
    this->heat_pump.setEconomyMode(call.get_preset().value() == climate::CLIMATE_PRESET_ECO ? 1 : 0);
    updated = true;
  }

  if (updated) {
    this->pending_update = true;
  }

#ifdef ESP32
  xSemaphoreGive(this->lock);
#endif

  if (updated) {
    ESP_LOGW(TAG, "Climate control request armed (control ENABLED) -- will be sent as the reply to the next "
                  "addressed frame, not instantly");
  }
}

climate::ClimateMode FujitsuClimate::fuji_mode_to_climate_mode_(FujiMode mode) {
  switch (mode) {
    case FujiMode::HEAT: return climate::CLIMATE_MODE_HEAT;
    case FujiMode::COOL: return climate::CLIMATE_MODE_COOL;
    case FujiMode::DRY: return climate::CLIMATE_MODE_DRY;
    case FujiMode::FAN: return climate::CLIMATE_MODE_FAN_ONLY;
    case FujiMode::AUTO: return climate::CLIMATE_MODE_AUTO;
    default: return climate::CLIMATE_MODE_AUTO;
  }
}

FujiMode FujitsuClimate::climate_mode_to_fuji_mode_(climate::ClimateMode mode) {
  switch (mode) {
    case climate::CLIMATE_MODE_HEAT: return FujiMode::HEAT;
    case climate::CLIMATE_MODE_COOL: return FujiMode::COOL;
    case climate::CLIMATE_MODE_DRY: return FujiMode::DRY;
    case climate::CLIMATE_MODE_FAN_ONLY: return FujiMode::FAN;
    case climate::CLIMATE_MODE_AUTO: return FujiMode::AUTO;
    default: return FujiMode::AUTO;
  }
}

climate::ClimateFanMode FujitsuClimate::fuji_fan_to_climate_fan_(FujiFanMode fan) {
  switch (fan) {
    case FujiFanMode::FAN_AUTO: return climate::CLIMATE_FAN_AUTO;
    case FujiFanMode::FAN_QUIET: return climate::CLIMATE_FAN_QUIET;
    case FujiFanMode::FAN_LOW: return climate::CLIMATE_FAN_LOW;
    case FujiFanMode::FAN_MEDIUM: return climate::CLIMATE_FAN_MEDIUM;
    case FujiFanMode::FAN_HIGH: return climate::CLIMATE_FAN_HIGH;
    default: return climate::CLIMATE_FAN_AUTO;
  }
}

FujiFanMode FujitsuClimate::climate_fan_to_fuji_fan_(climate::ClimateFanMode fan) {
  switch (fan) {
    case climate::CLIMATE_FAN_AUTO: return FujiFanMode::FAN_AUTO;
    case climate::CLIMATE_FAN_QUIET: return FujiFanMode::FAN_QUIET;
    case climate::CLIMATE_FAN_LOW: return FujiFanMode::FAN_LOW;
    case climate::CLIMATE_FAN_MEDIUM: return FujiFanMode::FAN_MEDIUM;
    case climate::CLIMATE_FAN_HIGH: return FujiFanMode::FAN_HIGH;
    default: return FujiFanMode::FAN_AUTO;
  }
}

}  // namespace fujitsu_climate
}  // namespace esphome
