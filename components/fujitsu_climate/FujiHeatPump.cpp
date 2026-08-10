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

    // Always log every raw byte at VERBOSE level for protocol capture/debug
    ESP_LOGV(TAG, "RX byte: 0x%02X", byte);

    // Experimental corrected-decode tracker (see FujiHeatPump.h) â€” runs on every raw
    // byte independently of the sync/parse logic below. Logs only; does not affect
    // on_off_/mode_/temperature_/fan_mode_.
    feedCorrectedSync(byte);

    if (rx_index_ == 0) {
      // Sync strategy: only 0xFE locks us onto a unit frame. The ctrl frame
      // immediately follows a valid unit frame, so we accept any start byte
      // only while expecting_ctrl_ is set. Everything else is discarded until
      // we see 0xFE again â€” this prevents the infinite offset-drift loop.
      if (byte == FRAME_START || expecting_ctrl_) {
        rx_buffer_[rx_index_++] = byte;
        // expecting_ctrl_ stays set until the full 8-byte ctrl frame is done
      } else {
        ESP_LOGV(TAG, "Pre-sync: 0x%02X", byte);
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
        // 30ms budget. State decoding below is NOT gated -- only the logging is.
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
        }
        last_frame_time_ = millis();
        return true;
      } else {
        // Invalid frame â€” if we were expecting a ctrl frame, drop it and
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

  // Frame structure â€” ART30LUAK / UTY-RNNUM (RSG series ~2010), confirmed by live capture:
  // [0] 0xFE  start marker
  // [1] 0xDF  fixed (unit address / frame type identifier)
  // [2] 0xDF  fixed
  // [3] 0x7F  fixed (always 0x7F, purpose unknown â€” NOT power/mode/fan for this unit)
  // [4] 0xFF  fixed (always 0xFF â€” NOT temperature for this unit)
  // [5] state byte A: bits[4:1] = temp_raw (Â°C âˆ’ 16); bits[7:4] = ~mode (inverted mode nibble)
  // [6] state byte B: purpose still being mapped (bit[1] = update-in-progress flag suspected)
  // [7] 0x6B  end marker (0xEB on some alternate frames)
  //
  // Temperature: ((frame[5] >> 1) & 0x0F) + 16  e.g. 0xC9 -> 4 + 16 = 20Â°C (confirmed)
  // Mode:        (~(frame[5] >> 4)) & 0x0F        e.g. 0xC9 -> ~0xC & 0xF = 3 = COOL (confirmed)
  //
  // Power and fan are in the CTRL frame start byte â€” see parseCTRLFrame().
  //
  // NOTE (10 Aug 2026): this is the original decode, kept as the primary/working path.
  // An experimental alternative decode (byte inversion + 2-byte sync shift, see
  // upstream-comparison.md) runs in parallel via feedCorrectedSync()/processCorrectedFrame()
  // below, logged under tag "CORR" â€” it is not wired to state here pending live validation.

  if (log_details) {
    ESP_LOGD(TAG, "  B3=0x%02X B4=0x%02X (fixed overhead)",
             frame[3], frame[4]);
    ESP_LOGD(TAG, "  B5=0x%02X  temp_raw=%d (->%.0fÂ°C) mode_nibble=%d",
             frame[5], (frame[5] >> 1) & 0x0F,
             (float)((frame[5] >> 1) & 0x0F) + TEMP_OFFSET,
             (~(frame[5] >> 4)) & 0x0F);
    ESP_LOGD(TAG, "  B6=0x%02X  (mapping TBD)", frame[6]);
  }

  // --- Decode fields from UNIT frame ---

  // Byte 5: target temperature, bits[4:1] = Â°C - TEMP_OFFSET
  uint8_t raw_temp = (frame[5] >> 1) & 0x0F;
  if (raw_temp <= TEMP_RAW_MAX) {
    temperature_ = static_cast<float>(raw_temp) + static_cast<float>(TEMP_OFFSET);
  } else {
    ESP_LOGW(TAG, "Target temp raw=%d out of range (byte5=0x%02X) â€” keeping %.1fÂ°C",
             raw_temp, frame[5], temperature_);
  }

  // Byte 5: mode, upper nibble = ~mode
  uint8_t mode_raw = (~(frame[5] >> 4)) & 0x0F;
  if (mode_raw <= static_cast<uint8_t>(FujiMode::MODE_AUTO)) {
    mode_ = static_cast<FujiMode>(mode_raw);
  }

  // Note: power and fan are decoded from CTRL frame â€” see parseCTRLFrame().
  // Log whatever state we have (on_off_ and fan_mode_ may still be from last CTRL frame).
  if (log_details) {
    ESP_LOGI(TAG, "State: pwr=%s mode=%d temp=%.0fÂ°C room=%.0fÂ°C fan=%d",
             on_off_ ? "ON" : "OFF", static_cast<int>(mode_),
             temperature_, current_temperature_, static_cast<int>(fan_mode_));
  }
}

void FujiHeatPump::parseCTRLFrame(const uint8_t *frame, size_t len, bool log_details) {
  if (len < FRAME_LENGTH) return;

  // CTRL frame structure â€” ART30LUAK confirmed by live capture:
  // [0] ctrl_start: upper nibble = 0xC (varies); bits[4:2] = fan mode; bit[1] = power on
  // [1] 0xFF  fixed
  // [2] 0xFF  fixed
  // [3] 0x5F  normally; 0x7E briefly during updates (change-in-progress flag)
  // [4] 0xFF  fixed
  // [5] same as UNIT frame B5 (temp + mode â€” redundant confirmation)
  // [6] same as UNIT frame B6
  // [7] 0x4B  end marker
  //
  // Power: (frame[0] >> 1) & 0x01   e.g. CC->0=OFF, CE->1=ON
  // Fan:   (frame[0] >> 2) & 0x07   e.g. 0xCC/0xCE -> 3 = MED (this project's original
  //        reading â€” the bits here never actually change across any capture, which is
  //        exactly why fan speed is flagged as unresolved; see processCorrectedFrame()
  //        for the experimental alternative that does find a moving fan field.)

  uint8_t ctrl0 = frame[0];
  on_off_ = ((ctrl0 >> 1) & 0x01) != 0;
  uint8_t fan_raw = (ctrl0 >> 2) & 0x07;
  if (fan_raw <= static_cast<uint8_t>(FujiFanMode::FAN_HIGH)) {
    fan_mode_ = static_cast<FujiFanMode>(fan_raw);
  }

  if (log_details) {
    ESP_LOGI(TAG, "CTRL decoded: pwr=%s fan=%d (CTRL0=0x%02X)",
             on_off_ ? "ON" : "OFF", static_cast<int>(fan_mode_), ctrl0);
  }
}

// --- Experimental corrected decode (added 10 Aug 2026, Session A) ---
// See FujiHeatPump.h for the full explanation. Summary: invert every raw byte, and read
// the meaningful 8-byte window starting 2 bytes after the raw (uninverted) 0xFE sync byte
// rather than at it. Verified by hand against every worked example in
// upstream-comparison.md (all five modes, all logged setpoints, all logged room
// temperatures) before writing this â€” but that verification was against a re-read of old
// log files, not live hardware. Treat log lines tagged "CORR" as a hypothesis to check
// against real button presses, not as ground truth yet.

void FujiHeatPump::feedCorrectedSync(uint8_t raw_byte) {
  switch (corr_state_) {
    case CorrSyncState::SEEK_FE:
      if (raw_byte == FRAME_START) {  // raw (uninverted) 0xFE
        corr_state_ = CorrSyncState::SKIP_ONE;
      }
      break;
    case CorrSyncState::SKIP_ONE:
      // Discard the byte immediately after the raw 0xFE â€” the corrected window starts
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
  // Field layout (validated by hand against upstream-comparison.md's worked examples â€”
  // NOT yet against live hardware):
  //   frame[3]  bit0=power  bits[3:1]=mode(1=Fan,2=Dry,3=Cool,4=Heat,5=Auto)
  //             bits[6:4]=fan (raw value; enum order â€” unreality vs fuji-iot â€” unconfirmed)
  //             bit7=error flag
  //   frame[4]  bits[6:0]=setpoint in Â°C directly, no offset (0 = no setpoint, e.g. FAN
  //             mode)  bit7=economy mode
  //   frame[6]  bit0=controller-present flag  (frame[6] >> 1)=room/controller temp in Â°C
  // frame[0], frame[1], frame[2], frame[5], frame[7] were constant in every example seen
  // so far (0x20, 0x80, 0x00, 0x94, 0x00) â€” logged raw below in case that changes.
  bool corr_power = frame[3] & 0x01;
  uint8_t corr_mode = (frame[3] >> 1) & 0x07;
  uint8_t corr_fan = (frame[3] >> 4) & 0x07;
  bool corr_error = (frame[3] >> 7) & 0x01;
  bool corr_economy = (frame[4] >> 7) & 0x01;
  uint8_t corr_setpoint = frame[4] & 0x7F;
  bool corr_ctrl_present = frame[6] & 0x01;
  uint8_t corr_room_temp = frame[6] >> 1;

  // Rate-limited to 1/sec (added 10 Aug 2026, post-crash-loop fix): with the RX pin
  // unwired/floating, noise can trigger this capture path far more often than the
  // real bus ever would, and unthrottled ESP_LOGD (plus API log forwarding) at that
  // rate was the leading suspect for the watchdog-reset crash loop seen this session.
  uint32_t now_ms = millis();
  if (now_ms - corr_last_log_ms_ >= 1000) {
    corr_last_log_ms_ = now_ms;
    ESP_LOGD(TAG,
             "CORR (experimental): pwr=%s mode=%d fan=%d err=%d econ=%d setpoint_raw=%d(0=none) "
             "room=%dC ctrl_present=%d  raw=[%02X %02X %02X %02X %02X %02X %02X %02X]",
             corr_power ? "ON" : "OFF", corr_mode, corr_fan, corr_error, corr_economy,
             corr_setpoint, corr_room_temp, corr_ctrl_present,
             frame[0], frame[1], frame[2], frame[3], frame[4], frame[5], frame[6], frame[7]);
  }
}

void FujiHeatPump::buildFrame() {
  // Build command frame
  // This is a GUESS - will need to be adjusted based on what we see from the bus
  
  memset(tx_buffer_, 0, sizeof(tx_buffer_));
  
  tx_buffer_[0] = FRAME_START;  // Frame start marker
  tx_buffer_[1] = secondary_ ? 0x01 : 0x00;  // Controller ID
  
  // Byte 2: Flags
  tx_buffer_[2] = 0;
  if (on_off_) tx_buffer_[2] |= 0x01;
  
  // Byte 3: Mode
  tx_buffer_[3] = static_cast<uint8_t>(mode_);
  
  // Byte 4: Temperature (offset by 16)
  int temp_byte = static_cast<int>(temperature_ - 16.0f);
  if (temp_byte < 0) temp_byte = 0;
  if (temp_byte > 14) temp_byte = 14;  // 16-30Â°C range
  tx_buffer_[4] = temp_byte;
  
  // Byte 5: Fan mode
  tx_buffer_[5] = static_cast<uint8_t>(fan_mode_);
  
  // Byte 6: Reserved
  tx_buffer_[6] = 0x00;
  
  // Byte 7: Checksum
  tx_buffer_[7] = calculateChecksum(tx_buffer_, 7);
  
  has_pending_frame_ = true;
  
  if (debug_) {
    ESP_LOGD(TAG, "Built frame:");
    for (int i = 0; i < FRAME_LENGTH; i++) {
      ESP_LOGD(TAG, "  [%d] = 0x%02X", i, tx_buffer_[i]);
    }
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
  
  ESP_LOGI(TAG, "Sent frame");
  if (debug_) {
    for (int i = 0; i < FRAME_LENGTH; i++) {
      ESP_LOGD(TAG, "  TX[%d] = 0x%02X", i, tx_buffer_[i]);
    }
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
    ESP_LOGI(TAG, "Set temperature: %.1fÂ°C", temp);
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
