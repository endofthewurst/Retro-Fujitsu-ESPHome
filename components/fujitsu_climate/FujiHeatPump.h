#pragma once

#include "esphome/core/component.h"
#include "esphome/components/uart/uart.h"

namespace esphome {
namespace fujitsu_climate {

// Fujitsu protocol frame structure
// Based on live bus capture from ART30LUAK / UTY-RNNUM (RSG series ~2010).
// NOTE: This model uses DIFFERENT frame markers than reference implementations
//       (unreality/FujiHeatPump, jaroslawprzybylowicz) which targeted other models.
//
// Observed 16-byte repeating cycle on the bus:
//   FE DF DF 7F FF D6 EB 6B  ← Unit status frame (starts 0xFE, ends 0x6B)
//   D1 FF FF 5F FF D6 EB 4B  ← Controller frame  (starts 0xD0-0xDE, ends 0x4B)
//
// The controller start byte lower nibble appears to toggle/vary (0xD0, 0xD1 seen).
// Both frame types are 8 bytes.
static const uint8_t FRAME_START = 0xFE;           // Unit status frame start
static const uint8_t FRAME_END = 0x6B;             // Unit frame end marker (ART30LUAK)
static const uint8_t FRAME_END_ALT = 0xEB;         // Alt unit frame end (other models / keep for compat)
static const uint8_t FRAME_END_CTRL = 0x4B;        // Controller frame end marker
static const uint8_t FRAME_CTRL_START_NIBBLE = 0xD0; // Controller frame start: upper byte = 0xD, lower = varies
static const uint8_t FRAME_LENGTH = 8;

// Target temperature encoding: stored value = (°C - TEMP_OFFSET), range [0, TEMP_RAW_MAX]
static const uint8_t TEMP_OFFSET = 16;
static const uint8_t TEMP_RAW_MAX = 14;  // 14 + 16 = 30°C (upper visual limit)

// Sanity ceiling for room-temperature readings
static const float ROOM_TEMP_MAX_C = 50.0f;

// Controller types
enum class ControllerType : uint8_t {
  PRIMARY = 0x00,
  SECONDARY = 0x01,
};

// Operating modes (from unreality/FujiHeatPump)
enum class FujiMode : uint8_t {
  UNKNOWN = 0,
  FAN = 1,
  DRY = 2,
  COOL = 3,
  HEAT = 4,
  MODE_AUTO = 5,
};

// Fan modes (from unreality/FujiHeatPump)
enum class FujiFanMode : uint8_t {
  FAN_AUTO = 0,
  QUIET = 1,
  FAN_LOW = 2,
  MEDIUM = 3,
  FAN_HIGH = 4,
};

class FujiHeatPump {
 public:
  FujiHeatPump() = default;
  
  // Initialize connection
  void connect(uart::UARTComponent *uart, bool secondary);
  
  // Frame reading (non-blocking — call from loop())
  bool readFrame();
  
  // State setters (prepare frame for sending)
  void setOnOff(bool on);
  void setMode(FujiMode mode);
  void setTemperature(float temp);
  void setFanMode(FujiFanMode fan);
  
  // State getters (from received frames)
  bool getOnOff() const { return on_off_; }
  FujiMode getMode() const { return mode_; }
  float getTemperature() const { return temperature_; }
  float getCurrentTemperature() const { return current_temperature_; }
  FujiFanMode getFanMode() const { return fan_mode_; }
  
  // Send pending changes
  bool sendPendingFrame();
  bool hasPendingFrame() const { return has_pending_frame_; }
  
  // Checksum calculation
  uint8_t calculateChecksum(const uint8_t *data, size_t len);
  
  // Debug helpers
  void setDebug(bool debug) { debug_ = debug; }
  bool isConnected() const { return connected_; }
  
 protected:
  uart::UARTComponent *uart_{nullptr};
  bool secondary_{true};
  bool connected_{false};
  bool debug_{false};
  
  // Current state (from bus) — NAN until first frame received
  bool on_off_{false};
  FujiMode mode_{FujiMode::MODE_AUTO};
  float temperature_{NAN};
  float current_temperature_{NAN};
  FujiFanMode fan_mode_{FujiFanMode::FAN_AUTO};
  
  // Pending changes flag
  bool has_pending_frame_{false};

  // Protocol sync state: after a valid unit frame, the very next 8 bytes are
  // the ctrl frame regardless of start byte. This flag gates that acceptance.
  bool expecting_ctrl_{false};
  
  // Frame buffers
  uint8_t rx_buffer_[32];
  uint8_t tx_buffer_[32];
  size_t rx_index_{0};
  
  // Parse received frames
  void parseFrame(const uint8_t *frame, size_t len);      // UNIT frame (FE...6B)
  void parseCTRLFrame(const uint8_t *frame, size_t len);  // CTRL frame (??...4B)

  // Build transmit frame
  void buildFrame();
  
  // Timing
  uint32_t last_frame_time_{0};
  static const uint32_t FRAME_REPLY_DELAY_MS = 60;  // Reply 50-60ms after receiving
};

}  // namespace fujitsu_climate
}  // namespace esphome
