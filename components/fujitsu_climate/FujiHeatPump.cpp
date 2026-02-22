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

bool FujiHeatPump::waitForFrame(uint32_t timeout_ms) {
  uint32_t start = millis();
  rx_index_ = 0;
  bool sync_found = false;
  
  while (millis() - start < timeout_ms) {
    yield();  // Prevent watchdog timeout
    
    if (uart_->available()) {
      uint8_t byte;
      if (uart_->read_byte(&byte)) {
        
        // If we haven't found sync yet, look for 0xFE
        if (!sync_found) {
          if (byte == FRAME_START) {  // 0xFE
            rx_buffer_[0] = byte;
            rx_index_ = 1;
            sync_found = true;
            ESP_LOGV(TAG, "Found sync byte 0xFE");
          } else {
            // Log what comes before sync for debugging
            ESP_LOGV(TAG, "Before sync: 0x%02X", byte);
          }
          continue;  // Keep looking for sync
        }
        
        // We have sync, collect remaining bytes
        rx_buffer_[rx_index_++] = byte;
        
        // Check if we have a complete frame
        if (rx_index_ >= FRAME_LENGTH) {
          if (debug_) {
            ESP_LOGD(TAG, "Received frame (%d bytes):", rx_index_);
            for (size_t i = 0; i < rx_index_; i++) {
              ESP_LOGD(TAG, "  [%d] = 0x%02X", i, rx_buffer_[i]);
            }
          }
          
          // Check for valid end marker (0xEB appears to be constant frame end, not checksum)
          if (rx_buffer_[7] == 0xEB) {
            ESP_LOGI(TAG, "Valid frame received (end marker 0xEB found)");
            parseFrame(rx_buffer_, rx_index_);
            last_frame_time_ = millis();
            rx_index_ = 0;
            sync_found = false;
            return true;
          } else {
            ESP_LOGW(TAG, "Invalid end marker! Expected 0xEB, got 0x%02X", rx_buffer_[7]);
            // Reset and look for next sync
            rx_index_ = 0;
            sync_found = false;
          }
        }
      }
    }
    delay(1);
  }
  
  return false;
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

  // Frame structure based on unreality/FujiHeatPump protocol:
  // Byte 0: 0xFE (sync)
  // Byte 1: Address/Source
  // Byte 2: Address/Dest
  // Byte 3: Power (bit 0), Mode (bits 1-3), Fan (bits 4-6), Error (bit 7)
  // Byte 4: Target temperature (bits 0-6, stored as temp-16), Economy (bit 7)
  // Byte 5: Update magic (bits 4-7), Swing (bit 2), Swing step (bit 1)
  // Byte 6: Controller present (bit 0), Controller temp (bits 1-6, direct °C)
  // Byte 7: 0xEB (end marker)

  if (debug_) {
    // Raw hex dump — stack-allocated, no heap allocation
    char hex_buf[3 * FRAME_LENGTH + 1];
    for (size_t i = 0; i < FRAME_LENGTH; i++) {
      snprintf(hex_buf + i * 3, 3, "%02X", frame[i]);
      hex_buf[i * 3 + 2] = (i < FRAME_LENGTH - 1) ? ' ' : '\0';
    }
    hex_buf[3 * FRAME_LENGTH] = '\0';  // ensure null-terminated
    ESP_LOGD(TAG, "Raw frame: %s", hex_buf);
    ESP_LOGD(TAG, "  Byte[3]=0x%02X  power=%d mode=%d fan=%d err=%d",
             frame[3],
             frame[3] & 0x01,
             (frame[3] >> 1) & 0x07,
             (frame[3] >> 4) & 0x07,
             (frame[3] >> 7) & 0x01);
    ESP_LOGD(TAG, "  Byte[4]=0x%02X  temp_raw=%d economy=%d",
             frame[4],
             frame[4] & 0x7F,
             (frame[4] >> 7) & 0x01);
    ESP_LOGD(TAG, "  Byte[6]=0x%02X  ctrl_temp_raw=%d ctrl_present=%d",
             frame[6],
             (frame[6] >> 1) & 0x3F,
             frame[6] & 0x01);
  }

  // Extract power state (byte 3, bit 0)
  on_off_ = (frame[3] & 0x01) != 0;

  // Extract mode (byte 3, bits 1-3)
  uint8_t mode_bits = (frame[3] & 0x0E) >> 1;
  mode_ = static_cast<FujiMode>(mode_bits);

  // Extract fan mode (byte 3, bits 4-6)
  uint8_t fan_bits = (frame[3] & 0x70) >> 4;
  fan_mode_ = static_cast<FujiFanMode>(fan_bits);

  // Extract target temperature (byte 4, bits 0-6).
  // The protocol stores target temp as (°C - TEMP_OFFSET); valid stored range is
  // [0, TEMP_RAW_MAX] (representing 16-30°C).  0x7F / all-bits-set is a sentinel.
  uint8_t raw_temp = frame[4] & 0x7F;
  if (raw_temp <= TEMP_RAW_MAX) {
    temperature_ = static_cast<float>(raw_temp) + static_cast<float>(TEMP_OFFSET);
  } else {
    ESP_LOGW(TAG, "Target temp raw value out of range: %d (byte4=0x%02X) — keeping previous %.1f°C",
             raw_temp, frame[4], temperature_);
  }

  // Extract current/controller temperature (byte 6, bits 1-6, right-shifted by 1).
  // Valid room-temperature range: 0–ROOM_TEMP_MAX_C.
  if ((frame[6] & 0x01) != 0) {  // Controller present bit
    float ctrl_temp = static_cast<float>((frame[6] & 0x7E) >> 1);
    if (ctrl_temp <= ROOM_TEMP_MAX_C) {
      current_temperature_ = ctrl_temp;
    } else {
      ESP_LOGW(TAG, "Current temp out of range: %.1f°C (byte6=0x%02X) — keeping previous %.1f°C",
               ctrl_temp, frame[6], current_temperature_);
    }
  }

  ESP_LOGI(TAG, "State: Power=%s, Mode=%d, Temp=%.1f°C, CurrentTemp=%.1f°C, Fan=%d",
           on_off_ ? "ON" : "OFF",
           static_cast<int>(mode_),
           temperature_,
           current_temperature_,
           static_cast<int>(fan_mode_));
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
