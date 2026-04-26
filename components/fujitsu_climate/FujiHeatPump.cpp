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

    if (rx_index_ == 0) {
      // Waiting for frame start marker
      if (byte == FRAME_START) {
        rx_buffer_[rx_index_++] = byte;
      } else {
        if (debug_) {
          ESP_LOGD(TAG, "Pre-sync byte: 0x%02X", byte);
        }
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

      if (rx_buffer_[FRAME_LENGTH - 1] == 0xEB) {
        if (debug_) {
          ESP_LOGD(TAG, "Valid frame received");
        }
        parseFrame(rx_buffer_, FRAME_LENGTH);
        last_frame_time_ = millis();
        return true;
      } else {
        if (debug_) {
          ESP_LOGD(TAG, "Invalid end marker: 0x%02X", rx_buffer_[FRAME_LENGTH - 1]);
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

  // Frame structure (unreality/FujiHeatPump protocol):
  // [0] 0xFE  start marker
  // [1] src   source address  (0x21=secondary controller, 0x20=primary, 0x01=unit?)
  // [2] dst   dest address
  // [3] bits: power(0), mode(1-3), fan(4-6), error(7)
  // [4] bits: target_temp_raw(0-6) = °C-16, economy(7), 0x7F=no setpoint
  // [5] bits: swing_step(1), swing(2), update_magic(4-7)
  // [6] bits: ctrl_present(0), room_temp(1-6) = direct °C
  // [7] 0xEB  end marker

  ESP_LOGD(TAG, "  B3=0x%02X  pwr=%d mode=%d fan=%d err=%d",
           frame[3],
           frame[3] & 0x01, (frame[3] >> 1) & 0x07,
           (frame[3] >> 4) & 0x07, (frame[3] >> 7) & 0x01);
  ESP_LOGD(TAG, "  B4=0x%02X  temp_raw=%d (->%.0f°C) eco=%d",
           frame[4], frame[4] & 0x7F,
           (float)(frame[4] & 0x7F) + TEMP_OFFSET,
           (frame[4] >> 7) & 0x01);
  ESP_LOGD(TAG, "  B5=0x%02X  magic=%d swing=%d step=%d",
           frame[5], (frame[5] >> 4) & 0x0F,
           (frame[5] >> 2) & 0x01, (frame[5] >> 1) & 0x01);
  ESP_LOGD(TAG, "  B6=0x%02X  room=%.0f°C ctrl_present=%d",
           frame[6], (float)((frame[6] & 0x7E) >> 1), frame[6] & 0x01);

  // --- Decode fields ---

  // Byte 3: power, mode, fan
  on_off_ = (frame[3] & 0x01) != 0;
  mode_ = static_cast<FujiMode>((frame[3] & 0x0E) >> 1);
  fan_mode_ = static_cast<FujiFanMode>((frame[3] & 0x70) >> 4);

  // Byte 4: target temperature (stored as °C - TEMP_OFFSET, range 0–14 → 16–30°C)
  uint8_t raw_temp = frame[4] & 0x7F;
  if (raw_temp <= TEMP_RAW_MAX) {
    temperature_ = static_cast<float>(raw_temp) + static_cast<float>(TEMP_OFFSET);
  } else {
    ESP_LOGW(TAG, "Target temp raw=%d out of range (byte4=0x%02X) — keeping %.1f°C",
             raw_temp, frame[4], temperature_);
  }

  // Byte 6: room temperature (bits 1-6, direct °C) and controller-present flag
  if ((frame[6] & 0x01) != 0) {
    float ctrl_temp = static_cast<float>((frame[6] & 0x7E) >> 1);
    if (ctrl_temp <= ROOM_TEMP_MAX_C) {
      current_temperature_ = ctrl_temp;
    } else {
      ESP_LOGW(TAG, "Room temp %.1f°C out of range (byte6=0x%02X) — keeping %.1f°C",
               ctrl_temp, frame[6], current_temperature_);
    }
  }

  ESP_LOGI(TAG, "State: pwr=%s mode=%d temp=%.0f°C room=%.0f°C fan=%d",
           on_off_ ? "ON" : "OFF", static_cast<int>(mode_),
           temperature_, current_temperature_, static_cast<int>(fan_mode_));
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
  
  if (abs(temperature_ - temp) > 0.1f) {
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
