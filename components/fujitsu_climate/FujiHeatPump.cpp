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
        // Timestamp the first byte of this candidate frame for the inter-frame timing
        // instrumentation below (added 14 Aug 2026) -- micros() rather than millis()
        // since the gaps being measured could plausibly be sub-millisecond.
        frame_start_us_ = micros();
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

          // NEW 18 Aug 2026 (3B.25): if a frame is armed (buildFrame() already called,
          // e.g. from test_setpoint_step()), send it right here -- immediately after a
          // real CTRL frame ends, at the start of the measured CTRL->UNIT gap -- rather
          // than waiting for a separate, unsynchronized call from outside readFrame().
          // See sendPendingFrame()'s own comment for the full reasoning. This does mean
          // a test button press doesn't transmit instantly; it arms has_pending_frame_
          // and waits (at most one bus cycle, sub-second) for the next real CTRL frame
          // to trigger the actual send at the right moment.
          if (has_pending_frame_) {
            sendPendingFrame();

            // NEW 18 Aug 2026 (3B.27): login-handshake burst re-arm. sendPendingFrame()
            // just cleared has_pending_frame_ after sending; if a burst is still in
            // progress, decrement and rebuild for the next CTRL->UNIT gap so the burst
            // lands one frame per bus cycle rather than all at once.
            if (login_burst_remaining_ > 0) {
              login_burst_remaining_--;
              if (login_burst_remaining_ > 0) {
                buildLoginFrame();
              }
            }
          }
        }

        // Boot/discovery-probe instrumentation (added 14 Aug 2026) -- runs on every
        // valid frame unconditionally, but recordBootProbe_() itself is a no-op once
        // the bounded capture window has closed, so this never becomes a sustained
        // per-frame cost. See FujiHeatPump.h for the full explanation.
        recordBootProbe_(rx_buffer_, valid_ctrl);

        // Inter-frame timing instrumentation (added 14 Aug 2026) -- see FujiHeatPump.h
        // for the full explanation. Unconditionally called; recordFrameTiming_()
        // itself is a no-op once its own bounded capture window has closed.
        recordFrameTiming_(valid_ctrl);

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

// --- Boot/discovery-probe instrumentation (added 14 Aug 2026, Phase 2 item 2) ---
// See FujiHeatPump.h for the full explanation and protocol-review-and-next-
// experiments.md for the background. Goal: across a normal power cycle (both the
// Fujitsu unit and the ESP32 coming up together on the shared 12V rail), find out
// whether/when the primary's one-shot ~4-second secondary-controller discovery
// probe arrives, and whether this project's current boot sequence would even be
// listening in time to catch it -- neither has ever actually been observed.

void FujiHeatPump::recordBootProbe_(const uint8_t *frame, bool is_ctrl) {
  uint32_t elapsed = millis();  // deliberately time-since-chip-boot, not time-since-setup() -- see header note
  if (elapsed > BOOT_CAPTURE_WINDOW_MS) return;  // window closed -- stop capturing, never a sustained cost

  if (boot_first_frame_ms_ < 0) {
    boot_first_frame_ms_ = static_cast<int32_t>(elapsed);
  }

  // Candidate 1: raw CTRL byte[3] deviating from its 0x5F rest value. Established in
  // hardware-and-protocol.md as the "change-in-progress flag" and reinterpreted in
  // upstream-comparison.md's addendum as a destination/flags byte (0x5F = dest 32
  // i.e. the wired controller; 0x7E = dest 1) -- either reading makes an early,
  // pre-any-button-press sighting of this a strong discovery-probe candidate.
  if (is_ctrl && frame[3] != 0x5F) {
    boot_ctrl3_alt_count_++;
    if (boot_ctrl3_alt_ms_ < 0) boot_ctrl3_alt_ms_ = static_cast<int32_t>(elapsed);
  }

  // Candidate 2: raw UNIT bytes[1]/[2] deviating from their 0xDF rest value --
  // upstream-comparison.md's still-open "source reads 32 and 0, where upstream
  // expects 32 and 1" item. Not confirmed, but cheap to watch for alongside
  // candidate 1 rather than betting everything on one hypothesis.
  if (!is_ctrl && (frame[1] != 0xDF || frame[2] != 0xDF)) {
    boot_unit_addr_alt_count_++;
    if (boot_unit_addr_alt_ms_ < 0) boot_unit_addr_alt_ms_ = static_cast<int32_t>(elapsed);
  }

  // Raw safety-net capture: in case neither established candidate above is actually
  // the discovery signal, keep the full raw bytes so a human can look at everything
  // that happened in the boot window, not just what these two heuristics flagged.
  if (boot_capture_count_ < BOOT_CAPTURE_MAX) {
    BootCaptureEntry &e = boot_capture_[boot_capture_count_++];
    e.t_ms = elapsed;
    e.is_ctrl = is_ctrl;
    memcpy(e.bytes, frame, FRAME_LENGTH);
  }
}

void FujiHeatPump::maybeDumpBootCapture() {
  if (boot_capture_dumped_) return;
  // Small margin past the window close so any last frame right at the boundary is
  // captured before we dump -- this is a one-time event, cost doesn't matter.
  if (millis() <= BOOT_CAPTURE_WINDOW_MS + 500) return;
  boot_capture_dumped_ = true;

  ESP_LOGI(TAG, "=== Boot capture dump: %d frames captured in first %ums ===",
           (int) boot_capture_count_, (unsigned) BOOT_CAPTURE_WINDOW_MS);
  ESP_LOGI(TAG, "  first frame seen at: %dms", boot_first_frame_ms_);
  ESP_LOGI(TAG, "  CTRL[3]!=0x5F (candidate 1): count=%u first_at=%dms",
           boot_ctrl3_alt_count_, boot_ctrl3_alt_ms_);
  ESP_LOGI(TAG, "  UNIT[1/2]!=0xDF (candidate 2): count=%u first_at=%dms",
           boot_unit_addr_alt_count_, boot_unit_addr_alt_ms_);
  for (size_t i = 0; i < boot_capture_count_; i++) {
    BootCaptureEntry &e = boot_capture_[i];
    char hex_buf[3 * FRAME_LENGTH + 1];
    for (size_t j = 0; j < FRAME_LENGTH; j++) {
      snprintf(hex_buf + j * 3, 4, "%02X ", e.bytes[j]);
    }
    hex_buf[3 * FRAME_LENGTH - 1] = '\0';
    bool flagged = (e.is_ctrl && e.bytes[3] != 0x5F) || (!e.is_ctrl && (e.bytes[1] != 0xDF || e.bytes[2] != 0xDF));
    ESP_LOGI(TAG, "  [%5ums] %s: %s%s", (unsigned) e.t_ms, e.is_ctrl ? "CTRL" : "UNIT", hex_buf,
             flagged ? "  *** CANDIDATE ***" : "");
  }
  ESP_LOGI(TAG, "=== end boot capture dump ===");
}

// --- Inter-frame timing instrumentation (added 14 Aug 2026, Phase 2 item 5) ---
// See FujiHeatPump.h for the full explanation and protocol-review-and-next-
// experiments.md for the background. Goal: measure the real gap between frame
// boundaries in the live 16-byte UNIT+CTRL cycle during normal steady-state running,
// to check the untested FRAME_REPLY_DELAY_MS=60ms guess against reality -- no power
// cycle required, unlike the boot/discovery-probe capture above.

void FujiHeatPump::recordFrameTiming_(bool is_ctrl) {
  if (millis() > TIMING_CAPTURE_WINDOW_MS) return;  // window closed -- never a sustained cost

  if (have_prev_frame_end_) {
    // Unsigned subtraction handles micros() wraparound correctly as long as the gap
    // itself is under ~71 minutes, which every gap here will be by many orders of
    // magnitude.
    uint32_t gap_us = frame_start_us_ - prev_frame_end_us_;
    GapStats *stats = nullptr;
    if (prev_frame_was_ctrl_ && !is_ctrl) {
      // CTRL-end -> UNIT-start: the gap that actually matters for Phase 2 TX timing.
      stats = &ctrl_to_unit_gap_;
      if (ctrl_to_unit_sample_count_ < TIMING_SAMPLE_MAX) {
        ctrl_to_unit_samples_us_[ctrl_to_unit_sample_count_++] = gap_us;
      }
    } else if (!prev_frame_was_ctrl_ && is_ctrl) {
      // UNIT-end -> CTRL-start: expected ~0 (same 16-byte cycle), a sanity baseline.
      stats = &unit_to_ctrl_gap_;
    }
    // (ctrl->ctrl or unit->unit shouldn't happen given the sync logic above, but if it
    // ever does -- e.g. after a resync -- stats stays null and we simply don't record
    // a gap for that transition, rather than mixing it into either bucket.)
    if (stats != nullptr) {
      stats->count++;
      if (gap_us < stats->min_us) stats->min_us = gap_us;
      if (gap_us > stats->max_us) stats->max_us = gap_us;
      stats->sum_us += gap_us;
    }
  }

  prev_frame_end_us_ = micros();
  prev_frame_was_ctrl_ = is_ctrl;
  have_prev_frame_end_ = true;
}

void FujiHeatPump::maybeDumpTimingCapture() {
  if (timing_capture_dumped_) return;
  if (millis() <= TIMING_CAPTURE_WINDOW_MS + 500) return;  // small margin past window close
  timing_capture_dumped_ = true;

  ESP_LOGI(TAG, "=== Frame timing capture dump (first %ums of running) ===",
           (unsigned) TIMING_CAPTURE_WINDOW_MS);
  ESP_LOGI(TAG, "  UNIT->CTRL (same-cycle baseline, expect ~0): n=%u min=%.2fms max=%.2fms avg=%.2fms",
           unit_to_ctrl_gap_.count, unit_to_ctrl_gap_.count ? unit_to_ctrl_gap_.min_us / 1000.0f : 0.0f,
           unit_to_ctrl_gap_.max_us / 1000.0f,
           unit_to_ctrl_gap_.count ? (unit_to_ctrl_gap_.sum_us / (float) unit_to_ctrl_gap_.count) / 1000.0f : 0.0f);
  ESP_LOGI(TAG, "  CTRL->UNIT (the reply-window question): n=%u min=%.2fms max=%.2fms avg=%.2fms",
           ctrl_to_unit_gap_.count, ctrl_to_unit_gap_.count ? ctrl_to_unit_gap_.min_us / 1000.0f : 0.0f,
           ctrl_to_unit_gap_.max_us / 1000.0f,
           ctrl_to_unit_gap_.count ? (ctrl_to_unit_gap_.sum_us / (float) ctrl_to_unit_gap_.count) / 1000.0f : 0.0f);
  ESP_LOGI(TAG, "  CTRL->UNIT individual samples (ms), %d captured:", (int) ctrl_to_unit_sample_count_);
  for (size_t i = 0; i < ctrl_to_unit_sample_count_; i++) {
    ESP_LOGI(TAG, "    [%2d] %.2fms", (int) i, ctrl_to_unit_samples_us_[i] / 1000.0f);
  }
  ESP_LOGI(TAG, "=== end frame timing capture dump ===");
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
  //   frame[1]  UPDATED 18 Aug 2026, continued -- previously described here only as
  //             "constant 0x80". Per the real upstream FujiFrame struct this is
  //             unknownBit (bit7, always set) + messageDest (bits[6:0]). Confirmed
  //             non-constant: reads 0x80 (dest=0=START) in Normal mode, 0x80->0xA1
  //             (dest=33=SECONDARY) once the master DIP is set to Dual -- see
  //             getCorrMessageDest() in the header.
  // frame[0], frame[2], frame[5], frame[7] were constant in every example seen so far
  // (0x20, 0x00, 0x94, 0x00) -- logged raw below in case that changes.
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

  // messageDest diagnostic (added 18 Aug 2026, continued) -- see FujiHeatPump.h's
  // getCorrMessageDest() comment for the full reasoning. frame[1] is the same byte
  // previously described here only as "constant 0x80" -- per upstream's real
  // FujiFrame struct, bit7 is unknownBit (always set) and bits[6:0] are messageDest.
  // Tracked on every single corrected-frame decode (~30/sec), not just once at boot,
  // specifically to check whether SECONDARY(33) shows up continuously in Dual mode
  // (the leading hypothesis, per the 3B.24 UNIT[3] 0x7F->0x5E finding) rather than as
  // a rare one-shot probe.
  uint8_t corr_unknown_bit = (frame[1] >> 7) & 0x01;
  uint8_t corr_message_dest = frame[1] & 0x7F;

  // Mirror into the diagnostic raw fields (unchanged since 10-11 Aug 2026).
  corr_mode_raw_ = corr_mode;
  corr_fan_raw_ = corr_fan;
  corr_setpoint_raw_ = corr_setpoint;
  corr_room_temp_raw_ = corr_room_temp;
  corr_economy_ = corr_economy;
  corr_mystery_bit_ = corr_ctrl_present;
  corr_last_update_ms_ = millis();
  corr_unknown_bit_ = corr_unknown_bit;
  corr_message_dest_ = corr_message_dest;
  message_dest_total_count_++;
  if (corr_message_dest == 33) {  // FujiAddress::SECONDARY, per real unreality/FujiHeatPump source
    message_dest_secondary_count_++;
    if (message_dest_first_secondary_ms_ < 0) {
      message_dest_first_secondary_ms_ = static_cast<int32_t>(millis());
    }

    // Address-gated LOGIN-ack test (added 18 Aug 2026, continued) -- see
    // armLoginAckTest()'s header comment for the full reasoning. This is the first
    // TX trigger in this project's history gated on actually having just been
    // addressed, rather than fired on manual command alone. Only builds -- the
    // existing, proven 3B.25 send path (readFrame(), right after the real CTRL frame
    // ends) still does the actual transmit, at the same measured-safe timing every
    // other TX test has used.
    if (login_ack_test_armed_) {
      login_ack_test_armed_ = false;
      buildLoginAckFrame();
    }

    // Address-gated STATUS command test (added 18 Aug 2026, continued, 3B.31) -- see
    // armStatusCommandTest()'s header comment. Same gating, different content: a
    // real field-changing command instead of a handshake acknowledgment.
    if (status_command_test_armed_) {
      status_command_test_armed_ = false;
      buildStatusCommandFrame(status_command_delta_c_);
    }
  }

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
             "room=%dC mystery_bit=%d dest=%d(unk=%d) sec_count=%u/%u  "
             "raw=[%02X %02X %02X %02X %02X %02X %02X %02X]",
             corr_power ? "ON" : "OFF", corr_mode, corr_fan, corr_error, corr_economy,
             corr_setpoint, corr_room_temp, corr_ctrl_present, corr_message_dest, corr_unknown_bit,
             message_dest_secondary_count_, message_dest_total_count_,
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
  // and only overwrite specific raw bytes now known, via the validated corrected
  // decode plus (18 Aug 2026) upstream's real address enum, to carry controller
  // state and identity:
  //
  //   raw CTRL byte[5] ("B5")  <- power (bit0) / mode (bits[3:1]) / fan (bits[6:4])
  //   raw CTRL byte[6] ("B6")  <- setpoint degC (bits[6:0], 0=none) / economy (bit7)
  //   raw CTRL byte[2]         <- messageSource = SECONDARY(33), new 18 Aug 2026
  //   raw CTRL byte[3]         <- messageDest = UNIT(1), unchanged since 11 Aug 2026
  //
  // These are the SAME bytes the corrected decode reads back out of the UNIT frame
  // (UNIT[5]/UNIT[6] mirror CTRL[5]/CTRL[6] byte-for-byte -- confirmed across all 1112
  // frames in the 29 Apr captures) -- just inverted, per processCorrectedFrame()'s
  // frame[3]/frame[4]. So: raw_byte = logical_byte ^ 0xFF.
  //
  // raw CTRL byte[3] (0x5F at rest, 0x7E in the 4 instances observed during real
  // button presses in the 29 Apr captures) is set to 0x7E to mark this as an active
  // command frame.
  //
  // CORRECTED, 18 Aug 2026: fetched unreality/FujiHeatPump's actual source (the real
  // upstream reference this whole decode is ported from) to settle this rather than
  // keep guessing. It defines:
  //   enum class FujiAddress : byte { START=0, UNIT=1, PRIMARY=32, SECONDARY=33 };
  // and a single FujiFrame struct with messageSource/messageDest/unknownBit fields,
  // decoded as: messageSource = buf[0] (whole byte, no mask); unknownBit = buf[1]
  // bit7; messageDest = buf[1] & 0x7F (bits 0-6). Applying the same validated 2-byte
  // shift used for the UNIT-derived frame[], by analogy, to the CTRL raw array
  // (frame_c[0..5] = inverted(CTRL[2..7])) puts messageDest at inverted(CTRL[3]) --
  // this raw byte's two observed values decode cleanly to real FujiAddress constants:
  // 0x5F -> 0xA0 = unknownBit(0x80) | UNIT... no: 0xA0 & 0x7F = 0x20 = 32 = PRIMARY;
  // 0x7E -> 0x81, 0x81 & 0x7F = 0x01 = UNIT. So byte[3] is messageDest, not a generic
  // flag: 0x5F = addressed to nobody-in-particular/PRIMARY(idle default), 0x7E =
  // addressed to UNIT(1) -- i.e. "this is a real command, for the indoor unit"
  // specifically. That's a real resolution of the old "destination/flags byte of the
  // indoor unit's frame" (upstream-comparison.md) vs "change-in-progress flag"
  // (original PROTOCOL.md) disagreement: both were half right. 0x7E is correct here
  // and needs no change.
  //
  // What DOES need a change: messageSOURCE. By the same shift analogy, messageSource
  // for a CTRL-slot frame is inverted(CTRL[2]) -- CTRL[2] has read fixed 0xFF (source
  // = inverted 0x00 = START) in every real frame ever captured. START is a real, named
  // upstream constant (0), not "no data" -- but it is very unlikely to be the right
  // self-declaration for OUR frame. Cloning the master's frame verbatim (the previous
  // behaviour) meant our transmitted frame declared messageSource=START, same as every
  // real CTRL frame -- never SECONDARY(33), the actual identity upstream defines for a
  // second controller. Address "1" (this project's and upstream-comparison.md's
  // earlier working guess for what a secondary should declare) was wrong: per the real
  // enum, 1 = UNIT, i.e. the indoor unit's own address -- declaring that would have
  // made this frame claim to BE the indoor unit, not introduce itself as a second
  // controller. The corrected, single-variable test: raw CTRL byte[2] <- SECONDARY(33)
  // inverted = 0x21 ^ 0xFF = 0xDE. Nothing else changes in this round.
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

  // raw byte[3]: 0x7E marks messageDest=UNIT(1) -- "this command is for the indoor
  // unit" -- see the comment above buildFrame(). Confirmed correct against the real
  // upstream FujiAddress enum, no change needed.
  tx_buffer_[3] = 0x7E;

  // raw byte[2]: messageSource. Declares this frame as coming from SECONDARY(33)
  // rather than the cloned master's implicit START(0), per the corrected byte-mapping
  // in the comment above. Confirmed safe to transmit (3B.24/3B.25): produces no bus
  // disruption once send timing is correct (3B.25) -- the earlier `E:EE` (3B.24) only
  // showed up with the old, unsynchronized fixed-delay send, not from this byte alone.
  tx_buffer_[2] = 0xDE;

  // raw byte[4]: messageType. NEW, 18 Aug 2026 (3B.26) -- next single-variable test,
  // now that 3B.25 removes the timing confound. Per the real upstream source, byte[2]
  // of the FujiFrame struct (== raw CTRL[4] via the same shift) carries messageType in
  // bits[5:4]: 0=STATUS, 1=ERROR, 2=LOGIN, 3=unexplored (register-type field, also
  // documented in upstream-comparison.md). CTRL[4] has read fixed 0xFF (logical 0x00 =
  // STATUS) in every real frame ever captured -- every prior TX test, including
  // 3B.24/3B.25's SECONDARY(33) attempts, still presented as an ordinary STATUS-type
  // command frame, just with a new source address bolted on. A brand new secondary
  // controller introducing itself for the first time seems more likely to need a
  // LOGIN-type frame than a STATUS-type one -- this tests that directly. Logical value
  // = 0b00100000 (messageType bits set to 2, writeBit and everything else left 0,
  // matching every real CTRL[4] observed) = 0x20, inverted for the wire = 0xDF.
  // Address (SECONDARY/33) and destination (UNIT/1) unchanged from the last test.
  tx_buffer_[4] = 0xDF;

  has_pending_frame_ = true;

  // CHANGED 18 Aug 2026 (3B.28) -- collapsed from a per-byte 9-line ESP_LOGW loop to a
  // single formatted line. See sendPendingFrame()'s comment for why: three consecutive
  // 3B.27 handshake-burst tests each coincided almost exactly with a live-log-client
  // connection drop (WinError 64), and this synchronous per-byte logging -- called
  // directly inside readFrame()'s valid_ctrl branch, the same timing-critical path
  // 3B.25 built to hit the measured CTRL->UNIT gap -- is the leading suspect. One
  // ESP_LOGW call instead of nine cuts the per-invocation logging cost roughly 9x with
  // no loss of the actual diagnostic content.
  {
    char hex_buf[3 * FRAME_LENGTH + 1];
    for (size_t i = 0; i < FRAME_LENGTH; i++) {
      snprintf(hex_buf + i * 3, 4, "%02X ", tx_buffer_[i]);
    }
    hex_buf[3 * FRAME_LENGTH - 1] = '\0';
    ESP_LOGW(TAG, "buildFrame(): built CTRL command frame: %s (was: %02X %02X %02X %02X %02X %02X %02X %02X)",
             hex_buf, last_ctrl_raw_[0], last_ctrl_raw_[1], last_ctrl_raw_[2], last_ctrl_raw_[3],
             last_ctrl_raw_[4], last_ctrl_raw_[5], last_ctrl_raw_[6], last_ctrl_raw_[7]);
  }
}

void FujiHeatPump::buildLoginFrame() {
  // Added 18 Aug 2026 (3B.27) -- see armLoginHandshake()'s header comment. Unlike
  // buildFrame(), this does NOT touch the state payload (raw bytes [5]/[6]) at all --
  // it's a pure "here I am" announcement, mirroring whatever the real controller's
  // last frame said, with only identity/type fields changed. Rationale: a login
  // handshake shouldn't plausibly carry a command; bundling one in was never tested
  // as a separate variable and might itself be why 3B.26's single LOGIN attempt did
  // nothing (or might not matter at all -- this at least removes it as a variable).
  if (!have_last_ctrl_raw_) {
    has_pending_frame_ = false;
    ESP_LOGW(TAG, "buildLoginFrame(): no CTRL frame captured from the bus yet, refusing");
    return;
  }
  memcpy(tx_buffer_, last_ctrl_raw_, FRAME_LENGTH);
  tx_buffer_[2] = 0xDE;  // messageSource = SECONDARY(33), inverted
  tx_buffer_[3] = 0x7E;  // messageDest = UNIT(1), inverted
  tx_buffer_[4] = 0xDF;  // messageType = LOGIN(2), inverted
  has_pending_frame_ = true;

  // CHANGED 18 Aug 2026 (3B.28) -- see buildFrame()'s matching comment: collapsed to a
  // single log line. This one matters most of the three, since a 3-frame burst calls
  // this up to 3 times within a handful of bus cycles -- the tightest concentration of
  // logging calls in the whole 3B.27 feature.
  {
    char hex_buf[3 * FRAME_LENGTH + 1];
    for (size_t i = 0; i < FRAME_LENGTH; i++) {
      snprintf(hex_buf + i * 3, 4, "%02X ", tx_buffer_[i]);
    }
    hex_buf[3 * FRAME_LENGTH - 1] = '\0';
    ESP_LOGW(TAG, "buildLoginFrame(): built LOGIN frame (burst_remaining=%d): %s",
             (int) login_burst_remaining_, hex_buf);
  }
}

void FujiHeatPump::armLoginHandshake(int count) {
  if (count < 1) count = 1;
  login_burst_remaining_ = count;
  buildLoginFrame();
  ESP_LOGW(TAG, "armLoginHandshake(): arming %d LOGIN frame(s), one per CTRL->UNIT gap starting now", count);
}

void FujiHeatPump::buildLoginAckFrame() {
  // Added 18 Aug 2026, continued -- see armLoginAckTest()'s header comment and
  // protocol-review-and-next-experiments.md's "real mechanism" addendum for the full
  // reasoning. This is upstream's actual LOGIN-reply shape, quoted from real source:
  //   ff.messageSource     = controllerAddress;       // SECONDARY(33), same as always
  //   ff.messageDest       = FujiAddress::SECONDARY;  // NEW -- every prior LOGIN test
  //                                                    // (3B.26-28) used UNIT(1) here
  //   ff.loginBit          = true;
  //   ff.controllerPresent = 1;                       // NEW -- force this bit on
  //   ff.unknownBit        = true;
  //   ff.writeBit          = 0;                        // not a command
  //   ff.onOff/.temperature/.acMode/.fanMode/... = currentState.*  // mirror, don't invent
  //
  // Mirroring is achieved the same way buildFrame()/buildLoginFrame() already do it:
  // start from the last real CTRL frame captured off the bus (on/off, mode, fan,
  // setpoint, economy, room temp all come along unchanged), then only touch the
  // specific bytes this reply actually needs to declare.
  if (!have_last_ctrl_raw_) {
    has_pending_frame_ = false;
    ESP_LOGW(TAG, "buildLoginAckFrame(): no CTRL frame captured from the bus yet, refusing");
    return;
  }
  memcpy(tx_buffer_, last_ctrl_raw_, FRAME_LENGTH);

  // raw byte[2]: messageSource = SECONDARY(33), inverted -- unchanged from every
  // addressed test since 3B.24.
  tx_buffer_[2] = 0xDE;

  // raw byte[3]: messageDest = SECONDARY(33) + unknownBit, inverted. Logical value
  // 0x80 (unknownBit) | 0x21 (dest=33) = 0xA1, inverted for the wire = 0x5E. THIS is
  // the new variable this test exists to try: every prior LOGIN attempt used 0x7E
  // (dest=UNIT(1), "announce myself to the indoor unit"), which was never what
  // upstream's real LOGIN-ack reply actually declares.
  tx_buffer_[3] = 0x5E;

  // raw byte[4]: messageType = LOGIN(2), inverted -- same value buildLoginFrame()
  // already uses (0xDF). writeBit=0 is implicit: no other bits in this byte have
  // ever been observed set in any real captured frame, so leaving them at their
  // established rest value keeps writeBit (wherever exactly it lives in this byte)
  // at 0, matching "this is not a command."
  tx_buffer_[4] = 0xDF;

  // raw byte[0]: controllerPresent (frame[6] bit0, via frame[6]=inverted(CTRL[0])).
  // Force bit0 of the LOGICAL byte to 1, preserving whatever room/controller
  // temperature is already mirrored in the upper 7 bits from the cloned template --
  // "controllerPresent=1, everything else mirrored" per upstream's snippet above.
  uint8_t logical_b0 = static_cast<uint8_t>(last_ctrl_raw_[0] ^ 0xFF);
  logical_b0 |= 0x01;
  tx_buffer_[0] = static_cast<uint8_t>(logical_b0 ^ 0xFF);

  has_pending_frame_ = true;

  {
    char hex_buf[3 * FRAME_LENGTH + 1];
    for (size_t i = 0; i < FRAME_LENGTH; i++) {
      snprintf(hex_buf + i * 3, 4, "%02X ", tx_buffer_[i]);
    }
    hex_buf[3 * FRAME_LENGTH - 1] = '\0';
    ESP_LOGW(TAG, "buildLoginAckFrame(): built LOGIN-ack (dest=SECONDARY, controllerPresent=1): %s", hex_buf);
  }
}

void FujiHeatPump::armLoginAckTest() {
  login_ack_test_armed_ = true;
  ESP_LOGW(TAG, "armLoginAckTest(): armed -- will send exactly one LOGIN-ack on the next observed "
                "messageDest==SECONDARY frame (per the diagnostic, expect this within ~1 bus cycle in Dual mode)");
}

void FujiHeatPump::buildStatusCommandFrame(int delta_c) {
  // Added 18 Aug 2026, continued (3B.31) -- see armStatusCommandTest()'s header
  // comment. This is the actual command, sent only in reply to being addressed --
  // every prior command test (3B.20-3B.26) sent unprompted, on manual command alone.
  if (!have_last_ctrl_raw_) {
    has_pending_frame_ = false;
    ESP_LOGW(TAG, "buildStatusCommandFrame(): no CTRL frame captured from the bus yet, refusing");
    return;
  }
  if (std::isnan(temperature_)) {
    has_pending_frame_ = false;
    ESP_LOGW(TAG, "buildStatusCommandFrame(): no current setpoint decoded (mode may not support one), refusing");
    return;
  }
  float target = temperature_ + static_cast<float>(delta_c);
  if (target < 16.0f) target = 16.0f;
  if (target > 30.0f) target = 30.0f;

  memcpy(tx_buffer_, last_ctrl_raw_, FRAME_LENGTH);

  // raw byte[2]: messageSource = SECONDARY(33), inverted -- unchanged from every
  // addressed test since 3B.24.
  tx_buffer_[2] = 0xDE;

  // raw byte[3]: messageDest = SECONDARY(33) + unknownBit, inverted (0x5E). NEW for a
  // command frame -- every prior command test (3B.20-3B.26) declared dest=UNIT(1)
  // (0x7E), on the assumption a command is "addressed to the indoor unit." The
  // messageDest diagnostic (3B.29) and the LOGIN-ack test (3B.30) both confirm this
  // controller is only ever addressed via dest=SECONDARY -- so its own replies,
  // including a real content-changing command, use that same slot/addressing
  // convention rather than the old UNIT(1) guess.
  tx_buffer_[3] = 0x5E;

  // raw byte[4]: messageType. Deliberately left UNTOUCHED here -- cloned from the
  // real template, which reads 0xFF (logical 0x00 = STATUS) in every captured frame.
  // This reverts 3B.26's messageType=LOGIN experiment for this specific test: per
  // upstream's real source, LOGIN is for the handshake reply only (see
  // buildLoginAckFrame() above); a real field-changing command is STATUS-type with
  // writeBit=1. The exact bit position of writeBit within this byte isn't
  // independently confirmed, so it's left at whatever the real controller's own
  // STATUS frames already carry rather than guessed at.

  // raw byte[6]: setpoint (bits[6:0]) + economy (bit7) -- same encoding buildFrame()
  // already uses, preserving economy from the real last frame.
  uint8_t setpoint_raw = static_cast<uint8_t>(target);
  uint8_t last_logical_b6 = static_cast<uint8_t>(last_ctrl_raw_[6] ^ 0xFF);
  bool economy_bit = (last_logical_b6 & 0x80) != 0;
  uint8_t logical_b6 = (setpoint_raw & 0x7F) | (economy_bit ? 0x80 : 0x00);
  tx_buffer_[6] = static_cast<uint8_t>(logical_b6 ^ 0xFF);

  has_pending_frame_ = true;

  {
    char hex_buf[3 * FRAME_LENGTH + 1];
    for (size_t i = 0; i < FRAME_LENGTH; i++) {
      snprintf(hex_buf + i * 3, 4, "%02X ", tx_buffer_[i]);
    }
    hex_buf[3 * FRAME_LENGTH - 1] = '\0';
    ESP_LOGW(TAG, "buildStatusCommandFrame(): built STATUS command (dest=SECONDARY, setpoint %.0f->%.0fdegC): %s",
             temperature_, target, hex_buf);
  }
}

void FujiHeatPump::armStatusCommandTest(int delta_c) {
  status_command_delta_c_ = delta_c;
  status_command_test_armed_ = true;
  ESP_LOGW(TAG, "armStatusCommandTest(): armed (delta=%d) -- will send exactly one STATUS command on the next "
                "observed messageDest==SECONDARY frame", delta_c);
}

bool FujiHeatPump::sendPendingFrame() {
  if (!has_pending_frame_ || !connected_) {
    return false;
  }

  // CHANGED 18 Aug 2026 (3B.25): dropped the old "pad up to FRAME_REPLY_DELAY_MS
  // (60ms) since last_frame_time_, then send" logic. 3B.23's timing capture measured
  // the real CTRL->UNIT gap at 0.0ms in Normal mode and up to only ~10.9ms in Dual
  // mode -- 60ms was never going to land inside either. Worse, this function used to
  // be called directly from test_setpoint_step(), synchronously, at whatever random
  // point in the bus cycle the HA button happened to be pressed -- the delay() padding
  // gave the illusion of "timed", but really just blocked execution (and stopped
  // readFrame() from processing incoming bytes) for up to 60ms before blindly
  // transmitting, with no relationship to where we actually were in the cycle. That's
  // consistent with every collision-artifact result seen in every TX test to date,
  // Normal and Dual mode alike, address-modified or not.
  //
  // sendPendingFrame() is now called from exactly one place: readFrame(), immediately
  // after it finishes processing a real CTRL frame (see the valid_ctrl branch below).
  // That positions transmission at the START of the CTRL->UNIT gap this project has
  // actually measured -- the best-grounded timing available, rather than a borrowed
  // constant that was never verified against this bus's real cycle. No added delay
  // here; the whole point is to send as soon as possible after the CTRL frame ends.

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
  //
  // CHANGED 18 Aug 2026 (3B.28) -- collapsed from a per-byte 9-line ESP_LOGW loop to a
  // single formatted line. Root-cause note: three consecutive 3B.27 login-handshake
  // test attempts each coincided almost exactly (WinError 64, live-log client dropped)
  // with the moment this burst of logging + echo-suppression ran -- 0/3 clean captures
  // despite the physical unit itself showing no lasting disruption each time (HA state
  // recovered to Bus OK / normal readings within ~15-30s). This function runs
  // synchronously inside readFrame()'s valid_ctrl branch -- the exact timing-critical
  // path 3B.25 built to hit the measured (0-10.9ms) CTRL->UNIT gap -- and during a
  // 3-frame burst it (and buildLoginFrame()) fire up to 3 times within a handful of
  // bus cycles, each previously costing 9 separate ESP_LOGW calls. That's the leading
  // suspect for stalling loop() long enough to disrupt the WiFi/TCP stack -- not the
  // bus/hardware itself. Cutting to 1 line per call is a cheap, safe first mitigation
  // to test before concluding anything about the handshake's actual bus effect.
  {
    char hex_buf[3 * FRAME_LENGTH + 1];
    for (size_t i = 0; i < FRAME_LENGTH; i++) {
      snprintf(hex_buf + i * 3, 4, "%02X ", tx_buffer_[i]);
    }
    hex_buf[3 * FRAME_LENGTH - 1] = '\0';
    ESP_LOGW(TAG, "Sent TX frame: %s", hex_buf);
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
