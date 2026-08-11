#include "FujiHeatPump.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace fujitsu_climate {

static const char *const TAG = "fujitsu.heatpump";

void FujiHeatPump::connect(uart::UARTComponent *uart, bool secondary) {
  uart_ = uart;
  secondary_ = secondary;
  connected_ = true;

  ESP_LOGI(TAG, "Fujitsu Heat Pump initialized");
  ESP_LOGI(TAG, "Controller type: %s", secondary ? "Secondary" : "Primary");
  ESP_LOGI(TAG, "LIN interface: TJA1021 compatible");
}

bool FujiHeatPump::readFrame() {
  // Non-blocking: consume only bytes currently available and assemble 8-byte frames
  while (uart_->available()) {
    uint8_t byte;
    if (!uart_->read_byte(&byte)) break;

    // Bus-activity timestamp for the alive/dead diagnostic -- cheap integer write,
    // deliberately placed before any logging (added 10 Aug 2026).
    last_any_byte_time_ = millis();

    // NOTE (11 Aug 2026, 3B.18): the previous unconditional per-byte ESP_LOGV here
    // ("RX byte: 0x%02X") was removed. With `logger: level: VERBOSE` in the YAML it
    // compiled in and fired on every single byte of continuous bus traffic (~50
    // bytes/sec) -- string formatting plus UART-console and API log forwarding on
    // every byte. This is the same class of hot-path-logging problem that caused the
    // Session A crash-loop (see the 10 Aug fix for ESP_LOGD in the per-frame dumps
    // below), just one level more verbose and left in place at the time. It's the
    // leading suspect for the `binary_sensor.house_aircon_bus_alive` flicker observed
    // increasingly often in Session B -- an occasional multi-second loop() stall here
    // (long enough to overflow the UART's small RX buffer, or just push the last-valid
    // -frame timestamp past the 2s Bus Alive timeout) is consistent with intermittent
    // sync loss with no other obvious cause. Paired with dropping `logger: level:` from
    // VERBOSE to DEBUG in retrofujitsu.yaml, which strips ESP_LOGV calls at compile
    // time entirely. 3B.19 update: this did NOT fully resolve the flicker on its own;
    // BUS_FRAME_TIMEOUT_MS was widened to 4000ms as a mitigation -- see
    // FujitsuClimate.h. An 11 Aug HA-history check found the widened timeout produced
    // zero flicker events over the following ~1.5h (vs. ~1/53s before), so this looks
    // like it's working in practice even though the underlying root cause of the
    // short quiet gaps themselves is still unconfirmed.

    // Corrected decode (now primary -- see FujiHeatPump.h) runs on every raw byte
    // independently of the sync/parse logic below.
    feedCorrectedSync(byte);

    if (rx_index_ == 0) {
      // Sync strategy: only 0xFE locks us onto a unit frame. The ctrl frame
      // immediately follows a valid unit frame, so we accept any start byte
      // only while expecting_ctrl_ is set. Everything else is discarded until
      // we see 0xFE again -- this prevents the infinite offset-drift loop.
      if (byte == FRAME_START || expecting_ctrl_) {
        rx_buffer_[rx_index_++] = byte;
        // expecting_ctrl_ stays set until the full 8-byte ctrl frame is done
      }
      continue;
    }

    // Collecting frame bytes; rx_index_ is always < FRAME_LENGTH here
    if (rx_index_ >= FRAME_LENGTH) {
      rx_index_ = 0;  // Safety reset; should not happen
    }
    rx_buffer_[rx_index_++] = byte;

    if (rx_index_ >= FRAME_LENGTH) {
      // Complete frame assembled; reset index for next frame
      rx_index_ = 0;

      uint8_t end_byte = rx_buffer_[FRAME_LENGTH - 1];
      uint8_t start_byte = rx_buffer_[0];
      bool valid_unit = (start_byte == FRAME_START && (end_byte == FRAME_END || end_byte == FRAME_END_ALT));
      bool valid_ctrl = (start_byte != FRAME_START) && (end_byte == FRAME_END_CTRL);

      if (valid_unit || valid_ctrl) {
        // Throttle the expensive per-frame dumps to 1/sec (added 10 Aug 2026): under
        // real, continuous, valid bus traffic these fired on every single frame --
        // several times a second -- and the string formatting cost was directly
        // observed causing 65-85ms component-loop overruns (once 527ms), well past the
        // 30ms budget. Frame-structure sync/decoding below is NOT gated -- only the
        // logging is.
        uint32_t now_ms = millis();
        bool log_details = (now_ms - debug_log_last_ms_ >= 1000);
        if (log_details) {
          debug_log_last_ms_ = now_ms;
        }

        if (log_details) {
          // Log frame type clearly for protocol capture
          char hex_buf[3 * FRAME_LENGTH + 1];
          for (size_t i = 0; i < FRAME_LENGTH; i++) {
            snprintf(hex_buf + i * 3, 4, "%02X ", rx_buffer_[i]);
          }
          hex_buf[3 * FRAME_LENGTH - 1] = '\0';
          ESP_LOGD(TAG, "%s  frame: %s", valid_unit ? "UNIT" : "CTRL", hex_buf);
        }

        if (valid_unit) {
          parseFrame(rx_buffer_, FRAME_LENGTH, log_details);
          expecting_ctrl_ = true;  // Next 8 bytes are the ctrl frame
        } else if (valid_ctrl) {
          parseCTRLFrame(rx_buffer_, FRAME_LENGTH, log_details);
          expecting_ctrl_ = false;  // Ctrl frame consumed

          // Snapshot the raw, unmodified CTRL frame as a template for buildFrame()
          // (added 11 Aug 2026 for Phase 2 TX work). This is deliberately captured
          // unconditionally -- not gated on hardware_present_ or any decode state --
          // so it's available as soon as any real CTRL frame has been seen, which is
          // the earliest point it's safe to build an outgoing frame at all.
          memcpy(last_ctrl_raw_, rx_buffer_, FRAME_LENGTH);
          have_last_ctrl_raw_ = true;
        }
        last_frame_time_ = millis();
        return true;
      } else {
        // Invalid frame -- if we were expecting a ctrl frame, drop it and
        // re-sync to the next 0xFE unit frame start
        expecting_ctrl_ = false;
        if (debug_) {
          ESP_LOGD(TAG, "Invalid frame: start=0x%02X end=0x%02X", start_byte, end_byte);
        }
      }
    }
  }

  return false;
}

void FujiHeatPump::parseFrame(const uint8_t *frame, size_t len, bool log_details) {
  if (len < FRAME_LENGTH) {
    ESP_LOGW(TAG, "Frame too short: %d bytes", len);
    return;
  }

  // --- Dump the raw frame at DEBUG level for capture/analysis, throttled to 1/sec by
  // the caller (see readFrame()) -- was unconditional, which under real live-bus
  // traffic caused sustained component-loop overruns (added 10 Aug 2026). ---
  // Format: RAW [src->dst]: FE 21 10 09 06 00 33 EB
  if (log_details) {
    char hex_buf[3 * FRAME_LENGTH + 1];
    for (size_t i = 0; i < FRAME_LENGTH; i++) {
      snprintf(hex_buf + i * 3, 4, "%02X ", frame[i]);
    }
    hex_buf[3 * FRAME_LENGTH - 1] = '\0';
    ESP_LOGD(TAG, "RAW [0x%02X->0x%02X]: %s", frame[1], frame[2], hex_buf);
  }

  // Frame structure -- ART30LUAK / UTY-RNNUM (RSG series ~2010).
  // [0] 0xFE  start marker
  // [1] 0xDF  fixed (unit address / frame type identifier)
  // [2] 0xDF  fixed
  // [3] 0x7F  fixed
  // [4] 0xFF  fixed
  // [5] state byte A, [6] state byte B, [7] 0x6B end marker (0xEB on some alternate frames)
  //
  // NOTE (11 Aug 2026, 3B.18): this decode's target-temperature and mode readings
  // (below) were confirmed WRONG/stuck against live button presses in Session B and
  // are kept here ONLY for frame-structure sync (feeding Bus Alive/Bus Status) and for
  // legacy debug logging -- they no longer write temperature_/mode_. The corrected
  // decode (see processCorrectedFrame()) is what now drives the live climate entity.

  if (log_details) {
    uint8_t raw_temp_dbg = (frame[5] >> 1) & 0x0F;
    uint8_t mode_nibble_dbg = (~(frame[5] >> 4)) & 0x0F;
    ESP_LOGD(TAG, "  B3=0x%02X B4=0x%02X (fixed overhead)", frame[3], frame[4]);
    ESP_LOGD(TAG, "  [legacy decode, not trusted] B5=0x%02X  temp_raw=%d (->%.0fdegC) mode_nibble=%d",
             frame[5], raw_temp_dbg, (float)raw_temp_dbg + TEMP_OFFSET, mode_nibble_dbg);
    ESP_LOGD(TAG, "  B6=0x%02X  (legacy decode, not trusted)", frame[6]);
  }
}

void FujiHeatPump::parseCTRLFrame(const uint8_t *frame, size_t len, bool log_details) {
  if (len < FRAME_LENGTH) return;

  // CTRL frame structure -- ART30LUAK.
  // [0] ctrl_start, [1]-[2] 0xFF fixed, [3] 0x5F/0x7E change-in-progress flag,
  // [4] 0xFF fixed, [5]-[6] mirror UNIT B5/B6, [7] 0x4B end marker.
  //
  // NOTE (11 Aug 2026, 3B.18): this decode's power and fan readings (below) were
  // confirmed WRONG/stuck against live button presses in Session B (fan bits never
  // moved across a full cycle test) and are kept here ONLY for frame-structure sync
  // and legacy debug logging -- they no longer write on_off_/fan_mode_. The corrected
  // decode (see processCorrectedFrame()) is what now drives the live climate entity.

  if (log_details) {
    uint8_t ctrl0 = frame[0];
    bool pwr_dbg = ((ctrl0 >> 1) & 0x01) != 0;
    uint8_t fan_dbg = (ctrl0 >> 2) & 0x07;
    ESP_LOGI(TAG, "[legacy decode, not trusted] CTRL: pwr=%s fan=%d (CTRL0=0x%02X)",
             pwr_dbg ? "ON" : "OFF", fan_dbg, ctrl0);
  }
}

// --- Corrected decode (added 10 Aug 2026, Session A; promoted to primary 11 Aug
// 2026, 3B.18) ---
// See FujiHeatPump.h for the full explanation. Summary: invert every raw byte, and read
// the meaningful 8-byte window starting 2 bytes after the raw (uninverted) 0xFE sync byte
// rather than at it. Session B (10-11 Aug 2026) validated this live against real button
// presses for power, mode (all 5), fan speed (4 of 5), setpoint, and economy -- all
// correct -- so it now feeds the live climate entity's state directly.

void FujiHeatPump::feedCorrectedSync(uint8_t raw_byte) {
  switch (corr_state_) {
    case CorrSyncState::SEEK_FE:
      if (raw_byte == FRAME_START) {  // raw (uninverted) 0xFE
        corr_state_ = CorrSyncState::SKIP_ONE;
      }
      break;
    case CorrSyncState::SKIP_ONE:
      // Discard the byte immediately after the raw 0xFE -- the corrected window starts
      // 2 bytes after the sync byte, not right at it.
      corr_state_ = CorrSyncState::CAPTURE;
      corr_index_ = 0;
      break;
    case CorrSyncState::CAPTURE:
      corr_buf_[corr_index_++] = raw_byte ^ 0xFF;  // invert every byte on the way in
      if (corr_index_ >= 8) {
        processCorrectedFrame(corr_buf_);
        corr_state_ = CorrSyncState::SEEK_FE;
      }
      break;
  }
}

void FujiHeatPump::processCorrectedFrame(const uint8_t *frame) {
  // Field layout -- validated live against real button presses in Session B
  // (10-11 Aug 2026) for every field except the mystery bit noted below:
  //   frame[3]  bit0=power  bits[3:1]=mode(1=Fan,2=Dry,3=Cool,4=Heat,5=Auto)
  //             bits[6:4]=fan (0=Auto,1=Quiet[unconfirmed],2=Low,3=Medium,4=High)
  //             bit7=error flag (untested)
  //   frame[4]  bits[6:0]=setpoint in degC directly, no offset (0 = no setpoint, e.g. FAN
  //             mode)  bit7=economy mode
  //   frame[6]  bit0=mystery bit, candidate for the Fujitsu "Thermo Sensor" Local/
  //             Remote setting per the manual, but NOT cleanly confirmed live (11 Aug
  //             2026) -- see getCorrMysteryBit() in the header for the full story.
  //             (frame[6] >> 1)=room/controller temperature in degC.
  // frame[0], frame[1], frame[2], frame[5], frame[7] were constant in every example seen
  // so far (0x20, 0x80, 0x00, 0x94, 0x00) -- logged raw below in case that changes.
  //
  // This 8-byte corrected frame straddles the raw 16-byte UNIT+CTRL wire cycle:
  // frame[0..5] = inverted(UNIT[2..7]), frame[6..7] = inverted(CTRL[0..1]). In other
  // words frame[3]/frame[4] here ARE raw UNIT bytes 5/6 ("B5"/"B6"), just read
  // inverted -- worked out 11 Aug 2026 while building buildFrame() for Phase 2 TX, see
  // that function for how this maps back onto outgoing raw CTRL bytes.
  bool corr_power = frame[3] & 0x01;
  uint8_t corr_mode = (frame[3] >> 1) & 0x07;
  uint8_t corr_fan = (frame[3] >> 4) & 0x07;
  bool corr_error = (frame[3] >> 7) & 0x01;
  bool corr_economy = (frame[4] >> 7) & 0x01;
  uint8_t corr_setpoint = frame[4] & 0x7F;
  bool corr_ctrl_present = frame[6] & 0x01;
  uint8_t corr_room_temp = frame[6] >> 1;

  // Mirror into the diagnostic raw fields (unchanged since 10-11 Aug 2026).
  corr_mode_raw_ = corr_mode;
  corr_fan_raw_ = corr_fan;
  corr_setpoint_raw_ = corr_setpoint;
  corr_room_temp_raw_ = corr_room_temp;
  corr_economy_ = corr_economy;
  corr_mystery_bit_ = corr_ctrl_present;
  corr_last_update_ms_ = millis();

  // --- Promoted to primary, 11 Aug 2026 (3B.18) ---
  // Drive the live climate entity's canonical state from the corrected decode instead
  // of parseFrame()/parseCTRLFrame() above -- see FujiHeatPump.h getters. Mode/fan enum
  // values are defined to line up 1:1 with these raw fields (see the enum comments in
  // the header), so no translation table is needed.
  on_off_ = corr_power;
  if (corr_mode >= static_cast<uint8_t>(FujiMode::FAN) && corr_mode <= static_cast<uint8_t>(FujiMode::MODE_AUTO)) {
    mode_ = static_cast<FujiMode>(corr_mode);
  }
  if (corr_fan <= static_cast<uint8_t>(FujiFanMode::FAN_HIGH)) {
    fan_mode_ = static_cast<FujiFanMode>(corr_fan);
  }
  // Setpoint raw 0 means "no setpoint" (e.g. Fan mode) -- represent as NAN/unknown
  // rather than a bogus 0degC, consistent with how this field starts out before any
  // frame has been seen.
  temperature_ = (corr_setpoint == 0) ? NAN : static_cast<float>(corr_setpoint);
  current_temperature_ = static_cast<float>(corr_room_temp);

  // Rate-limited to 1/sec (added 10 Aug 2026, post-crash-loop fix): with the RX pin
  // unwired/floating, noise can trigger this capture path far more often than the
  // real bus ever would, and unthrottled ESP_LOGD (plus API log forwarding) at that
  // rate was the leading suspect for the watchdog-reset crash loop seen this session.
  uint32_t now_ms = millis();
  if (now_ms - corr_last_log_ms_ >= 1000) {
    corr_last_log_ms_ = now_ms;
    ESP_LOGD(TAG,
             "CORR: pwr=%s mode=%d fan=%d err=%d econ=%d setpoint_raw=%d(0=none) "
             "room=%dC mystery_bit=%d  raw=[%02X %02X %02X %02X %02X %02X %02X %02X]",
             corr_power ? "ON" : "OFF", corr_mode, corr_fan, corr_error, corr_economy,
             corr_setpoint, corr_room_temp, corr_ctrl_present,
             frame[0], frame[1], frame[2], frame[3], frame[4], frame[5], frame[6], frame[7]);
  }
}

void FujiHeatPump::buildFrame() {
  // Build a CTRL-shaped 8-byte command frame -- rewritten 11 Aug 2026 for Phase 2.
  //
  // Approach, per plan-to-completion.md Phase 2: the CTRL frame is the wired
  // controller's own frame shape, so as a secondary controller we emit that shape
  // rather than invent a new one. We start from the last real CTRL frame actually
  // captured off the bus (raw, unmodified bytes -- see last_ctrl_raw_ in readFrame())
  // and only overwrite the two raw bytes now known, via the validated corrected
  // decode, to carry controller state:
  //
  //   raw CTRL byte[5] ("B5")  <- power (bit0) / mode (bits[3:1]) / fan (bits[6:4])
  //   raw CTRL byte[6] ("B6")  <- setpoint degC (bits[6:0], 0=none) / economy (bit7)
  //
  // These are the SAME bytes the corrected decode reads back out of the UNIT frame
  // (UNIT[5]/UNIT[6] mirror CTRL[5]/CTRL[6] byte-for-byte -- confirmed across all 1112
  // frames in the 29 Apr captures) -- just inverted, per processCorrectedFrame()'s
  // frame[3]/frame[4]. So: raw_byte = logical_byte ^ 0xFF.
  //
  // raw CTRL byte[3] (0x5F at rest, 0x7E in the 4 instances observed during real
  // button presses in the 29 Apr captures) is set to 0x7E to mark this as an active
  // command frame. [hypothesis] -- upstream-comparison.md's addendum offers a
  // competing "destination/flags byte" reading of this same byte under the
  // corrected/shifted alignment, which this project has NOT reconciled with the
  // original change-in-progress-flag reading yet. Using the literal value observed
  // during real captured button presses is the most evidence-based choice available,
  // but this whole byte is exactly the kind of thing Phase 2's bench test is meant to
  // either confirm or falsify -- watch closely for any sign the unit reacts
  // differently to this than expected.
  //
  // Byte[0] (room/controller temperature + mystery bit) is left untouched -- copied
  // straight from the last real CTRL frame -- since the ESP32 has no temperature
  // sensor of its own and there's no validated reason yet to touch it.
  //
  // NEVER SENT TO THE REAL BUS BEFORE 11 AUG 2026. Per hardware-and-protocol.md's
  // "ESP32 is never the boss" design principle, this is NOT wired into
  // FujitsuClimate::control() -- see that method's comment. It is reachable only via
  // FujitsuClimate::test_setpoint_step(), a single deliberate, manually-triggered
  // Phase 2 test entry point, so each test step in plan-to-completion.md's strict
  // order can be tried and checked against the physical wall unit one at a time.

  if (!have_last_ctrl_raw_) {
    // Never seen a real CTRL frame yet -- nothing safe to copy from. Refuse to
    // fabricate a frame out of thin air.
    has_pending_frame_ = false;
    ESP_LOGW(TAG, "buildFrame(): no CTRL frame captured from the bus yet, refusing to build a command frame");
    return;
  }

  memcpy(tx_buffer_, last_ctrl_raw_, FRAME_LENGTH);

  // raw byte[5]: power / mode / fan, encoded logically then inverted for the wire.
  uint8_t logical_b5 = 0;
  logical_b5 |= on_off_ ? 0x01 : 0x00;
  logical_b5 |= (static_cast<uint8_t>(mode_) & 0x07) << 1;
  logical_b5 |= (static_cast<uint8_t>(fan_mode_) & 0x07) << 4;
  tx_buffer_[5] = static_cast<uint8_t>(logical_b5 ^ 0xFF);

  // raw byte[6]: setpoint (0 = none) + economy. Economy is preserved from the last
  // real frame seen rather than invented here -- setEconomy() doesn't exist yet.
  uint8_t setpoint_raw = std::isnan(temperature_) ? 0 : static_cast<uint8_t>(temperature_);
  if (setpoint_raw > 30) setpoint_raw = 30;
  uint8_t last_logical_b6 = static_cast<uint8_t>(last_ctrl_raw_[6] ^ 0xFF);
  bool economy_bit = (last_logical_b6 & 0x80) != 0;
  uint8_t logical_b6 = (setpoint_raw & 0x7F) | (economy_bit ? 0x80 : 0x00);
  tx_buffer_[6] = static_cast<uint8_t>(logical_b6 ^ 0xFF);

  // raw byte[3]: 0x7E marks an active command frame. [hypothesis] -- see the comment
  // above buildFrame().
  tx_buffer_[3] = 0x7E;

  has_pending_frame_ = true;

  ESP_LOGW(TAG, "buildFrame(): built CTRL command frame (Phase 2, still unvalidated against real hardware):");
  for (int i = 0; i < FRAME_LENGTH; i++) {
    ESP_LOGW(TAG, "  [%d] = 0x%02X (was 0x%02X)", i, tx_buffer_[i], last_ctrl_raw_[i]);
  }
}

bool FujiHeatPump::sendPendingFrame() {
  if (!has_pending_frame_ || !connected_) {
    return false;
  }

  // Wait appropriate delay after last received frame
  uint32_t elapsed = millis() - last_frame_time_;
  if (elapsed < FRAME_REPLY_DELAY_MS) {
    delay(FRAME_REPLY_DELAY_MS - elapsed);
  }

  // Send the frame
  uart_->write_array(tx_buffer_, FRAME_LENGTH);
  uart_->flush();

  // Echo suppression (added 11 Aug 2026 for Phase 2 TX): on this single-wire
  // (half-duplex) LIN bus our own transmitted bytes come back on RX. Swallow them
  // here so the next readFrame() call doesn't try to parse our own echo as an
  // incoming frame. Upstream (unreality/FujiHeatPump) does the same thing
  // immediately after every write -- see upstream-comparison.md's "Echo suppression"
  // section. Bounded by a short deadline rather than a fixed byte-count wait, since
  // if the echo doesn't show up as expected that's itself useful information, not a
  // reason to hang.
  uint32_t echo_deadline = millis() + 50;
  size_t discarded = 0;
  while (discarded < FRAME_LENGTH && millis() < echo_deadline) {
    uint8_t echo_byte;
    if (uart_->read_byte(&echo_byte)) {
      discarded++;
    }
  }
  if (discarded < FRAME_LENGTH) {
    ESP_LOGW(TAG, "sendPendingFrame(): only discarded %d/%d echo bytes after TX -- unexpected, worth noting",
             (int) discarded, (int) FRAME_LENGTH);
  }

  // Logged at WARN (not gated behind debug_) -- Phase 2 testing is rare and
  // deliberate, and knowing exactly what was sent is essential for correlating
  // against what happens at the physical wall unit immediately afterward.
  ESP_LOGW(TAG, "Sent TX frame (Phase 2 test, unvalidated against real hardware):");
  for (int i = 0; i < FRAME_LENGTH; i++) {
    ESP_LOGW(TAG, "  TX[%d] = 0x%02X", i, tx_buffer_[i]);
  }

  has_pending_frame_ = false;
  return true;
}

uint8_t FujiHeatPump::calculateChecksum(const uint8_t *data, size_t len) {
  // Simple 8-bit sum (used by Fujitsu protocol)
  // The checksum is just the sum of all bytes, truncated to 8 bits
  uint16_t sum16 = 0;  // Use 16-bit to see full value

  if (debug_) {
    ESP_LOGD(TAG, "Checksum calculation: summing %d bytes:", len);
    for (size_t i = 0; i < len; i++) {
      ESP_LOGD(TAG, "  byte[%d] = 0x%02X (dec %d)", i, data[i], data[i]);
      sum16 += data[i];
    }
    ESP_LOGD(TAG, "  Sum16 = 0x%04X (dec %d)", sum16, sum16);
    ESP_LOGD(TAG, "  Sum8  = 0x%02X (dec %d)", (uint8_t)sum16, (uint8_t)sum16);
  } else {
    for (size_t i = 0; i < len; i++) {
      sum16 += data[i];
    }
  }

  return (uint8_t)sum16;  // Truncate to 8 bits
}

void FujiHeatPump::setOnOff(bool on) {
  if (on_off_ != on) {
    on_off_ = on;
    buildFrame();
    ESP_LOGI(TAG, "Set power: %s", on ? "ON" : "OFF");
  }
}

void FujiHeatPump::setMode(FujiMode mode) {
  if (mode_ != mode) {
    mode_ = mode;
    buildFrame();
    ESP_LOGI(TAG, "Set mode: %d", static_cast<int>(mode));
  }
}

void FujiHeatPump::setTemperature(float temp) {
  // Clamp to valid range
  if (temp < 16.0f) temp = 16.0f;
  if (temp > 30.0f) temp = 30.0f;

  // Guard against NAN comparison (std::abs(NAN - x) is NAN, always false)
  if (std::isnan(temperature_) || std::abs(temperature_ - temp) > 0.1f) {
    temperature_ = temp;
    buildFrame();
    ESP_LOGI(TAG, "Set temperature: %.1fdegC", temp);
  }
}

void FujiHeatPump::setFanMode(FujiFanMode fan) {
  if (fan_mode_ != fan) {
    fan_mode_ = fan;
    buildFrame();
    ESP_LOGI(TAG, "Set fan mode: %d", static_cast<int>(fan));
  }
}

}  // namespace fujitsu_climate
}  // namespace esphome
