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

    // Always log every raw byte at VERBOSE level for protocol capture/debug
    ESP_LOGV(TAG, "RX byte: 0x%02X", byte);

    if (rx_index_ == 0) {
      // Sync strategy: only 0xFE locks us onto a unit frame. The ctrl frame
      // immediately follows a valid unit frame, so we accept any start byte
      // only while expecting_ctrl_ is set. Everything else is discarded until
      // we see 0xFE again — this prevents the infinite offset-drift loop.
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
        // Log frame type clearly for protocol capture
        char hex_buf[3 * FRAME_LENGTH + 1];
        for (size_t i = 0; i < FRAME_LENGTH; i++) {
          snprintf(hex_buf + i * 3, 4, "%02X ", rx_buffer_[i]);
        }
        hex_buf[3 * FRAME_LENGTH - 1] = '\0';

        if (valid_unit) {
          ESP_LOGD(TAG, "UNIT  frame: %s", hex_buf);
          parseFrame(rx_buffer_, FRAME_LENGTH);
          expecting_ctrl_ = true;  // Next 8 bytes are the ctrl frame
        } else if (valid_ctrl) {
          ESP_LOGD(TAG, "CTRL  frame: %s", hex_buf);
          parseCTRLFrame(rx_buffer_, FRAME_LENGTH);
          expecting_ctrl_ = false;  // Ctrl frame consumed
        }
        last_frame_time_ = millis();
        return true;
      } else {
        // Invalid frame — if we were expecting a ctrl frame, drop it and
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

void FujiHeatPump::parseFrame(const uint8_t *frame, size_t len) {
  if (len < FRAME_LENGTH) {
    ESP_LOGW(TAG, "Frame too short: %d bytes", len);
    return;
  }

  // --- Always dump the raw frame at DEBUG level for capture/analysis ---
  // Format: RAW [src→dst]: FE 21 10 09 06 00 33 EB
  {
    char hex_buf[3 * FRAME_LENGTH + 1];
    for (size_t i = 0; i < FRAME_LENGTH; i++) {
      snprintf(hex_buf + i * 3, 4, "%02X ", frame[i]);
    }
    hex_buf[3 * FRAME_LENGTH - 1] = '\0';
    ESP_LOGD(TAG, "RAW [0x%02X->0x%02X]: %s", frame[1], frame[2], hex_buf);
  }

  // Frame structure — ART30LUAK / UTY-RNNUM (RSG series ~2010), confirmed by live capture:
  // [0] 0xFE  start marker
  // [1] 0xDF  fixed (unit address / frame type identifier)
  // [2] 0xDF  fixed
  // [3] 0x7F  fixed (always 0x7F, purpose unknown — NOT power/mode/fan for this unit)
  // [4] 0xFF  fixed (always 0xFF — NOT temperature for this unit)
  // [5] state byte A: bits[4:1] = temp_raw (°C − 16); bits[7:4] = ~mode (inverted mode nibble)
  // [6] state byte B: purpose still being mapped (bit[1] = update-in-progress flag suspected)
  // [7] 0x6B  end marker (0xEB on some alternate frames)
  //
  // Temperature: ((frame[5] >> 1) & 0x0F) + 16  e.g. 0xC9 → 4 + 16 = 20°C ✓
  // Mode:        (~(frame[5] >> 4)) & 0x0F        e.g. 0xC9 → ~0xC & 0xF = 3 = COOL ✓
  //
  // Power and fan are in the CTRL frame start byte — see parseCTRLFrame().

  ESP_LOGD(TAG, "  B3=0x%02X B4=0x%02X (fixed overhead)",
           frame[3], frame[4]);
  ESP_LOGD(TAG, "  B5=0x%02X  temp_raw=%d (->%.0f°C) mode_nibble=%d",
           frame[5], (frame[5] >> 1) & 0x0F,
           (float)((frame[5] >> 1) & 0x0F) + TEMP_OFFSET,
           (~(frame[5] >> 4)) & 0x0F);
  ESP_LOGD(TAG, "  B6=0x%02X  (mapping TBD)", frame[6]);

  // --- Decode fields from UNIT frame ---

  // Byte 5: target temperature, bits[4:1] = °C - TEMP_OFFSET
  uint8_t raw_temp = (frame[5] >> 1) & 0x0F;
  if (raw_temp <= TEMP_RAW_MAX) {
    temperature_ = static_cast<float>(raw_temp) + static_cast<float>(TEMP_OFFSET);
  } else {
    ESP_LOGW(TAG, "Target temp raw=%d out of range (byte5=0x%02X) — keeping %.1f°C",
             raw_temp, frame[5], temperature_);
  }

  // Byte 5: mode, upper nibble = ~mode
  uint8_t mode_raw = (~(frame[5] >> 4)) & 0x0F;
  if (mode_raw <= static_cast<uint8_t>(FujiMode::MODE_AUTO)) {
    mode_ = static_cast<FujiMode>(mode_raw);
  }

  // Note: power and fan are decoded from CTRL frame — see parseCTRLFrame().
  // Log whatever state we have (on_off_ and fan_mode_ may still be from last CTRL frame).
  ESP_LOGI(TAG, "State: pwr=%s mode=%d temp=%.0f°C room=%.0f°C fan=%d",
           on_off_ ? "ON" : "OFF", static_cast<int>(mode_),
           temperature_, current_temperature_, static_cast<int>(fan_mode_));
}

void FujiHeatPump::parseCTRLFrame(const uint8_t *frame, size_t len) {
  if (len < FRAME_LENGTH) return;

  // CTRL frame structure — ART30LUAK confirmed by live capture:
  // [0] ctrl_start: upper nibble = 0xC (varies); bits[4:2] = fan mode; bit[1] = power on
  // [1] 0xFF  fixed
  // [2] 0xFF  fixed
  // [3] 0x5F  normally; 0x7E briefly during updates (change-in-progress flag)
  // [4] 0xFF  fixed
  // [5] same as UNIT frame B5 (temp + mode — redundant confirmation)
  // [6] same as UNIT frame B6
  // [7] 0x4B  end marker
  //
  // Power: (frame[0] >> 1) & 0x01   e.g. CC→0=OFF, CE→1=ON
  // Fan:   (frame[0] >> 2) & 0x07   e.g. 0xCC/0xCE → 3 = MED ✓

  uint8_t ctrl0 = frame[0];
  on_off_ = ((ctrl0 >> 1) & 0x01) != 0;
  uint8_t fan_raw = (ctrl0 >> 2) & 0x07;
  if (fan_raw <= static_cast<uint8_t>(FujiFanMode::FAN_HIGH)) {
    fan_mode_ = static_cast<FujiFanMode>(fan_raw);
  }

  ESP_LOGI(TAG, "CTRL decoded: pwr=%s fan=%d (CTRL0=0x%02X)",
           on_off_ ? "ON" : "OFF", static_cast<int>(fan_mode_), ctrl0);
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
  if (temp_byte > 14) temp_byte = 14;  // 16-30°C range
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
    ESP_LOGI(TAG, "Set temperature: %.1f°C", temp);
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
