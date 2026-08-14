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
//   FE DF DF 7F FF D6 EB 6B  <- Unit status frame (starts 0xFE, ends 0x6B)
//   D1 FF FF 5F FF D6 EB 4B  <- Controller frame  (starts 0xD0-0xDE, ends 0x4B)
//
// The controller start byte lower nibble appears to toggle/vary (0xD0, 0xD1 seen).
// Both frame types are 8 bytes.
static const uint8_t FRAME_START = 0xFE;           // Unit status frame start
static const uint8_t FRAME_END = 0x6B;             // Unit frame end marker (ART30LUAK)
static const uint8_t FRAME_END_ALT = 0xEB;         // Alt unit frame end (other models / keep for compat)
static const uint8_t FRAME_END_CTRL = 0x4B;        // Controller frame end marker
static const uint8_t FRAME_CTRL_START_NIBBLE = 0xD0; // Controller frame start: upper byte = 0xD, lower = varies
static const uint8_t FRAME_LENGTH = 8;

// Target temperature encoding: stored value = (degC - TEMP_OFFSET), range [0, TEMP_RAW_MAX]
// NOTE: retained for the old/deprecated decode below. The corrected decode (which now
// drives the live climate entity, as of 11 Aug 2026 / 3B.18) reads setpoint directly in
// whole degrees C with no offset -- see processCorrectedFrame().
static const uint8_t TEMP_OFFSET = 16;
static const uint8_t TEMP_RAW_MAX = 14;  // 14 + 16 = 30degC (upper visual limit)

// Sanity ceiling for room-temperature readings
static const float ROOM_TEMP_MAX_C = 50.0f;

// Controller types
enum class ControllerType : uint8_t {
  PRIMARY = 0x00,
  SECONDARY = 0x01,
};

// Operating modes (from unreality/FujiHeatPump). Values are deliberately aligned with
// the corrected decode's frame[3] bits[3:1] field (1=Fan..5=Auto) so that
// processCorrectedFrame() can cast its decoded raw mode straight into this enum --
// see the corrected-decode section below.
enum class FujiMode : uint8_t {
  UNKNOWN = 0,
  FAN = 1,
  DRY = 2,
  COOL = 3,
  HEAT = 4,
  MODE_AUTO = 5,
};

// Fan modes (from unreality/FujiHeatPump). Values are deliberately aligned with the
// corrected decode's frame[3] bits[6:4] field. Confirmed live against a full
// High->Medium->Low->Auto cycle on the physical remote, 11 Aug 2026 (Session B):
// Auto=0, Low=2, Medium=3, High=4 all matched the display in lockstep. Quiet=1 has
// not yet been exercised live -- kept in the enum on the strength of the observed
// AUTO/LOW/MED/HIGH spacing, not yet independently confirmed.
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

  // Frame reading (non-blocking -- call from loop())
  bool readFrame();

  // State setters (prepare frame for sending). NOTE: as of 3B.18 these are no longer
  // called from FujitsuClimate::control() (still read-only pending broader Phase 2
  // validation) -- but as of the Phase 2 TX-test work (11 Aug 2026), they ARE called
  // from FujitsuClimate::test_setpoint_step(), a single deliberate, manually-triggered
  // test entry point (see that method and buildFrame() below). buildFrame()'s output
  // has never been sent to the real bus before this -- see buildFrame() for the
  // current, corrected-decode-based approach and its own caveats.
  void setOnOff(bool on);
  void setMode(FujiMode mode);
  void setTemperature(float temp);
  void setFanMode(FujiFanMode fan);

  // State getters (from received frames). As of 3B.18, these are populated from the
  // corrected decode (see processCorrectedFrame()) rather than the old parseFrame()/
  // parseCTRLFrame() below -- promoted to primary after Session B validated the
  // corrected decode live against real button presses for every field here (power,
  // mode, fan, setpoint) and found the old decode wrong or stuck on all of them.
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

  // Bus-activity timestamps for a higher-level alive/dead diagnostic (added 10 Aug
  // 2026). last_frame_time_ only advances on a structurally valid frame (the proven
  // sync/parse logic, not the experimental corrected-decode); last_any_byte_time_
  // advances on every byte regardless of validity -- together they distinguish
  // `no signal at all` vs `noise but no valid frames` vs `bus OK`.
  uint32_t getLastFrameTime() const { return last_frame_time_; }
  uint32_t getLastByteTime() const { return last_any_byte_time_; }

  // Live mirror of the corrected decode (added 10 Aug 2026, Session B kickoff;
  // promoted to the primary decode 11 Aug 2026, 3B.18) -- exposed as diagnostic HA
  // entities in parallel with the values now feeding the main climate entity above,
  // so the two can be cross-checked. 0xFF means "no corrected frame decoded yet".
  uint8_t getCorrModeRaw() const { return corr_mode_raw_; }
  uint8_t getCorrFanRaw() const { return corr_fan_raw_; }
  uint32_t getCorrLastUpdateTime() const { return corr_last_update_ms_; }
  uint8_t getCorrSetpointRaw() const { return corr_setpoint_raw_; }
  uint8_t getCorrRoomTempRaw() const { return corr_room_temp_raw_; }
  bool getCorrEconomy() const { return corr_economy_; }
  // Candidate bit hypothesized (per the Fujitsu manual's description of the "Thermo
  // Sensor" Local/Remote setting) to be frame[6] bit0. Live testing on 11 Aug 2026
  // did NOT cleanly confirm this -- the bit was observed not moving at all across two
  // separate button presses, then moving once in a way whose direction didn't
  // obviously match the stated action. Renamed from "Corrected Thermo Sensor" to
  // "Mystery Bit" pending further investigation -- do not treat this as a trusted
  // Thermo Sensor readout yet.
  bool getCorrMysteryBit() const { return corr_mystery_bit_; }

  // Whether a real CTRL frame has ever been captured off the bus -- buildFrame()
  // refuses to build a command frame without one to use as a template (added for
  // Phase 2 TX work, 11 Aug 2026).
  bool hasLastCtrlRaw() const { return have_last_ctrl_raw_; }

  // --- Boot/discovery-probe instrumentation (added 14 Aug 2026, Phase 2 item 2 --
  // see protocol-review-and-next-experiments.md) ---
  // Looks for the ~4-second one-shot secondary-controller discovery probe that
  // unreality/FujiHeatPump's README describes, across a normal power cycle. Tracks
  // two established candidate signals directly (raw CTRL byte[3] deviating from its
  // 0x5F rest value -- see hardware-and-protocol.md's "change-in-progress flag" /
  // upstream-comparison.md's "destination-1" reading of the same byte; and raw UNIT
  // bytes[1]/[2] deviating from their 0xDF rest value -- the address-byte candidate
  // from upstream-comparison.md's "source reads 32 and 0, where upstream expects 32
  // and 1" open item) -- plus a bounded raw-frame capture as a safety net, since
  // neither candidate is fully confirmed as THE discovery signal. All timestamps
  // below are raw millis() (time since actual chip boot/reset), deliberately NOT
  // time since this component's setup() ran -- see FujitsuClimate.h's
  // get_setup_priority() note for why that distinction matters here. -1 means "not
  // seen yet within the capture window".
  int32_t getBootFirstFrameMs() const { return boot_first_frame_ms_; }
  int32_t getBootCtrl3AltMs() const { return boot_ctrl3_alt_ms_; }
  uint16_t getBootCtrl3AltCount() const { return boot_ctrl3_alt_count_; }
  int32_t getBootUnitAddrAltMs() const { return boot_unit_addr_alt_ms_; }
  uint16_t getBootUnitAddrAltCount() const { return boot_unit_addr_alt_count_; }
  size_t getBootCaptureCount() const { return boot_capture_count_; }
  bool isBootCaptureDumped() const { return boot_capture_dumped_; }
  // Call roughly once a second (e.g. from FujitsuClimate::update()) -- a cheap no-op
  // check until the boot window closes, then performs exactly one bounded log dump.
  void maybeDumpBootCapture();

 protected:
  uart::UARTComponent *uart_{nullptr};
  bool secondary_{true};
  bool connected_{false};
  bool debug_{false};

  // Current state (from bus) -- NAN until first frame received. As of 3B.18, written
  // by processCorrectedFrame() below, not by parseFrame()/parseCTRLFrame().
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

  // Last real, unmodified CTRL frame captured off the bus (raw wire bytes, NOT
  // inverted) -- added for Phase 2 TX work, 11 Aug 2026. buildFrame() copies this as
  // its starting template rather than constructing a frame from scratch, per
  // plan-to-completion.md's Phase 2 approach ("the CTRL frame is the wired
  // controller's own frame, so as a secondary controller the ESP32 should emit that
  // shape"). Populated in readFrame() whenever a structurally valid CTRL frame is
  // seen -- independent of hardware_present_/decode state, so it's available as soon
  // as any real CTRL frame has been observed.
  uint8_t last_ctrl_raw_[FRAME_LENGTH]{};
  bool have_last_ctrl_raw_{false};

  // --- Boot/discovery-probe instrumentation (added 14 Aug 2026) -- see the public
  // getters above and FujiHeatPump.cpp's recordBootProbe_()/maybeDumpBootCapture()
  // for the full explanation. Bounded to BOOT_CAPTURE_WINDOW_MS so this can never
  // become a sustained hot-path logging cost like the crash-loop/overrun bugs this
  // codebase already had to fix once each (see readFrame()'s log_details comments).
  static const uint32_t BOOT_CAPTURE_WINDOW_MS = 12000;  // 12s -- comfortable margin over the ~4s probe
  static const size_t BOOT_CAPTURE_MAX = 500;            // ~12s at the observed ~32 valid-frames/sec rate
  struct BootCaptureEntry {
    uint32_t t_ms;
    bool is_ctrl;
    uint8_t bytes[FRAME_LENGTH];
  };
  BootCaptureEntry boot_capture_[BOOT_CAPTURE_MAX];
  size_t boot_capture_count_{0};
  bool boot_capture_dumped_{false};
  int32_t boot_first_frame_ms_{-1};
  int32_t boot_ctrl3_alt_ms_{-1};
  uint16_t boot_ctrl3_alt_count_{0};
  int32_t boot_unit_addr_alt_ms_{-1};
  uint16_t boot_unit_addr_alt_count_{0};
  void recordBootProbe_(const uint8_t *frame, bool is_ctrl);

  // Parse received frames -- retained for frame-structure sync (used by the Bus
  // Alive/Bus Status diagnostics) and for legacy debug logging. As of 3B.18 these no
  // longer write on_off_/mode_/temperature_/fan_mode_ -- see processCorrectedFrame().
  // log_details gates the expensive per-frame ESP_LOGD/LOGI dumps (see readFrame()) --
  // added 10 Aug 2026 after real live-bus traffic showed these firing on every single
  // frame (several times/sec) caused sustained 65-85ms component-loop overruns, once a
  // 527ms spike. Frame-structure sync itself always runs regardless of log_details.
  void parseFrame(const uint8_t *frame, size_t len, bool log_details);      // UNIT frame (FE...6B)
  void parseCTRLFrame(const uint8_t *frame, size_t len, bool log_details);  // CTRL frame (??...4B)

  // Build transmit frame
  void buildFrame();

  // Timing
  uint32_t last_frame_time_{0};
  uint32_t last_any_byte_time_{0};  // set on every raw byte, regardless of validity (added 10 Aug 2026)
  uint32_t debug_log_last_ms_{0};  // throttles the per-frame debug/info dumps to 1/sec (added 10 Aug 2026)
  static const uint32_t FRAME_REPLY_DELAY_MS = 60;  // Reply 50-60ms after receiving

  // --- Corrected decode (added 10 Aug 2026, Session A; promoted to primary 11 Aug
  // 2026, 3B.18) ---
  // Hypothesis from comparing this project against other published Fujitsu LIN
  // implementations (see project notes: upstream-comparison.md): every byte on the
  // wire is inverted relative to how the old decode above reads it, and the meaningful
  // 8-byte field window starts 2 bytes after the raw 0xFE sync byte, not at it. Session
  // B (10-11 Aug 2026) validated this live against real button presses for power,
  // mode (all 5), fan speed (4 of 5), setpoint, and economy -- all correct, while the
  // old decode above was wrong or stuck on every one of them. It now feeds
  // on_off_/mode_/temperature_/current_temperature_/fan_mode_ directly (see
  // processCorrectedFrame()) and is also mirrored to standalone diagnostic HA entities
  // for cross-checking.
  //
  // Byte mapping to the raw 16-byte UNIT+CTRL wire cycle (worked out 11 Aug 2026 for
  // Phase 2 TX -- this window straddles the frame boundary, it is NOT just the UNIT
  // frame): corr frame[0..5] = inverted(UNIT[2..7]), corr frame[6..7] =
  // inverted(CTRL[0..1]). So UNIT[5] ("B5") == inverted frame[3], UNIT[6] ("B6") ==
  // inverted frame[4], and CTRL[0] ("C0") == inverted frame[6]. This is how
  // buildFrame() below maps its writes back onto raw CTRL bytes 5 and 6.
  enum class CorrSyncState : uint8_t { SEEK_FE, SKIP_ONE, CAPTURE };
  CorrSyncState corr_state_{CorrSyncState::SEEK_FE};
  uint8_t corr_buf_[8];
  uint8_t corr_index_{0};
  uint32_t corr_last_log_ms_{0};  // throttle experimental logging (added 10 Aug 2026, post-crash-loop fix)
  uint8_t corr_mode_raw_{0xFF};    // last decoded corrected-mode value; 0xFF = none yet
  uint8_t corr_fan_raw_{0xFF};     // last decoded corrected-fan value; 0xFF = none yet
  uint32_t corr_last_update_ms_{0};  // millis() of the last corrected-frame decode
  uint8_t corr_setpoint_raw_{0};   // last decoded corrected setpoint, degC (0 = none, e.g. FAN mode)
  uint8_t corr_room_temp_raw_{0};  // last decoded corrected room/controller temp, degC
  bool corr_economy_{false};       // last decoded corrected economy-mode flag
  bool corr_mystery_bit_{false};  // frame[6] bit0 -- see getCorrMysteryBit() comment above
  void feedCorrectedSync(uint8_t raw_byte);
  void processCorrectedFrame(const uint8_t *frame);
};

}  // namespace fujitsu_climate
}  // namespace esphome
