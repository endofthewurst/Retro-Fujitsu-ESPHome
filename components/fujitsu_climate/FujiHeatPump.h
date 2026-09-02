#pragma once

// ---------------------------------------------------------------------------
// Vendored from unreality/FujiHeatPump (https://github.com/unreality/FujiHeatPump),
// MIT licensed, Copyright 2021 Raal Goff. Brought in close to verbatim on 19 Aug 2026
// (Phase 4 rebuild) after a hard code review
// (tx-architecture-review-and-adoption-plan.md) found this project's own hand-rolled
// re-implementation had independently re-derived the same field layout, addressing,
// and writeBit semantics upstream already had -- the real gap was architectural (no
// continuous reply loop, bus I/O sharing ESPHome's cooperative loop() instead of a
// dedicated task), not protocol-level. Field-for-field, this is the exact class that
// was already proven correct against this hardware across Session B and 3B.29-3B.38's
// ground-truth captures -- see that doc for the full comparison.
//
// Deliberate deviations from upstream, each called out where it happens below:
//   1. sendPendingFrame()'s reply gate: upstream waits >50ms since the last received
//      frame before sending. This project measured the real CTRL->UNIT gap on this
//      specific bus directly (3B.23) and found it to be ~0ms in Normal DIP mode and at
//      most ~10.9ms in Dual mode -- a 50ms wait would never land in a gap that small,
//      it would always land mid-frame on top of live traffic. kMinReplyGapMs replaces
//      the hardcoded 50 with a named, much smaller constant reflecting that finding.
//   2. connect()'s rxPin/txPin overload already existed upstream for ESP32 -- kept
//      unmodified, just noting it's how FujitsuClimate::setup() wires GPIO16/17.
//   3. Added lastRawControllerPresent/getLastRawControllerPresent() (20 Aug 2026) --
//      a passive, read-only capture of the incoming frame's raw controllerPresent bit
//      (frame[6] bit0) BEFORE waitForFrame()'s STATUS/LOGIN branches overwrite `ff` to
//      build our own reply. Upstream doesn't preserve this anywhere once decoded --
//      it's immediately reused as an outgoing field. This project's own history
//      (test-and-dev-workflow.md's "Thermo Sensor / Mystery Bit investigation" and
//      state-of-play.md's protocol review) found this exact bit was the leading
//      candidate for the UTY-RNNUM's Thermo Sensor Local/Remote setting, but four
//      rounds of live button testing never confirmed it, and a later upstream-source
//      review concluded it more likely reflects "a secondary controller has logged
//      in" than Local/Remote -- unconfirmed either way. This getter exists purely so a
//      diagnostic sensor can surface the raw bit for a future dedicated live test
//      (single button press, precise timestamp, no standby cycling -- see
//      test-and-dev-workflow.md's recommended next test); it does not change any
//      protocol behaviour and nothing here claims the mapping is solved.
// Everything else -- decodeFrame()/encodeFrame()'s bit layout, the waitForFrame()
// state machine (reply-only, gated on messageDest == controllerAddress, LOGIN vs
// STATUS branching), the FujiMode/FujiFanMode/FujiMessageType/FujiAddress enums -- is
// unmodified from upstream.
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <HardwareSerial.h>

const byte kModeIndex = 3;
const byte kModeMask = 0b00001110;
const byte kModeOffset = 1;

const byte kFanIndex = 3;
const byte kFanMask = 0b01110000;
const byte kFanOffset = 4;

const byte kEnabledIndex = 3;
const byte kEnabledMask = 0b00000001;
const byte kEnabledOffset = 0;

const byte kErrorIndex = 3;
const byte kErrorMask = 0b10000000;
const byte kErrorOffset = 7;

const byte kEconomyIndex = 4;
const byte kEconomyMask = 0b10000000;
const byte kEconomyOffset = 7;

const byte kTemperatureIndex = 4;
const byte kTemperatureMask = 0b01111111;
const byte kTemperatureOffset = 0;

const byte kUpdateMagicIndex = 5;
const byte kUpdateMagicMask = 0b11110000;
const byte kUpdateMagicOffset = 4;

const byte kSwingIndex = 5;
const byte kSwingMask = 0b00000100;
const byte kSwingOffset = 2;

const byte kSwingStepIndex = 5;
const byte kSwingStepMask = 0b00000010;
const byte kSwingStepOffset = 1;

const byte kControllerPresentIndex = 6;
const byte kControllerPresentMask = 0b00000001;
const byte kControllerPresentOffset = 0;

const byte kControllerTempIndex = 6;
const byte kControllerTempMask = 0b01111110;
const byte kControllerTempOffset = 1;

typedef struct FujiFrames {
  byte onOff = 0;
  byte temperature = 16;
  byte acMode = 0;
  byte fanMode = 0;
  byte acError = 0;
  byte economyMode = 0;
  byte swingMode = 0;
  byte swingStep = 0;
  byte controllerPresent = 0;
  byte updateMagic = 0;  // unsure what this value indicates
  byte controllerTemp = 16;

  bool writeBit = false;
  bool loginBit = false;
  bool unknownBit = false;  // unsure what this bit indicates

  byte messageType = 0;
  byte messageSource = 0;
  byte messageDest = 0;
} FujiFrame;

class FujiHeatPump {
 private:
  HardwareSerial *_serial;
  byte readBuf[8];
  byte writeBuf[8];

  byte controllerAddress;
  bool controllerIsPrimary = true;
  bool seenSecondaryController = false;
  bool controllerLoggedIn = false;
  unsigned long lastFrameReceived;

  byte updateFields;
  FujiFrame updateState;
  FujiFrame currentState;

  // Deviation #3 -- see the file-header comment above. Set once per valid incoming
  // frame, immediately after decodeFrame(), before anything in waitForFrame() mutates
  // the local `ff` copy into an outgoing reply. Read-only outside this class.
  byte lastRawControllerPresent = 0;

  // 21 Aug 2026 -- same passive-capture treatment, for FujiFrame::unknownBit
  // (readBuf[1] bit 7, doc'd above as unsure what this bit indicates). Never
  // exposed anywhere before today; added as a second live-test candidate for the
  // UTY-RNNUM's Thermo Sensor setting after controllerPresent (frame[6] bit0) was
  // ruled out by two USB-tethered live tests on 21 Aug 2026 that showed zero
  // correlation with the physical button. Same capture point/reasoning as above.
  byte lastRawUnknownBit = 0;
  // 21 Aug 2026 -- full raw incoming frame (all 8 bytes), captured at the same
  // point as lastRawControllerPresent/lastRawUnknownBit above. Added after finding
  // byte2 (5 bits), byte5 (2 bits), byte6 bit7, and the entirety of byte7 have no
  // named field anywhere in this decode -- rather than add one diagnostic per
  // candidate bit, this exposes everything at once so a single live test can spot
  // whichever byte/bit actually moves. Throttled at the FujitsuClimate layer (not
  // here) before publish_state, since this changes on almost every valid frame.
  byte lastRawFrame[8] = {0};

  FujiFrame decodeFrame();
  void encodeFrame(FujiFrame ff);
  void printFrame(byte buf[8], FujiFrame ff);

  bool pendingFrame = false;

  // Deviation #1 from upstream -- see the file-header comment above. Upstream hardcodes
  // 50 here; this project's own measured bus timing (3B.23: 0-10.9ms real CTRL->UNIT
  // gap, never 50ms) means the reply has to go out essentially immediately once built,
  // not after an artificial wait. sendPendingFrame() is called right after
  // waitForFrame() returns true in FujitsuClimate's dedicated bus task -- by the time
  // that call happens, `millis() - lastFrameReceived` is already >= this many ms just
  // from decode/build overhead, so 0 is deliberately permissive rather than a real
  // delay.
  static const unsigned long kMinReplyGapMs = 0;

 public:
  void connect(HardwareSerial *serial, bool secondary);
  void connect(HardwareSerial *serial, bool secondary, int rxPin, int txPin);

  bool waitForFrame();
  void sendPendingFrame();
  bool isBound();
  bool updatePending();

  void setOnOff(bool o);
  void setTemp(byte t);
  void setMode(byte m);
  void setFanMode(byte fm);
  void setEconomyMode(byte em);
  void setSwingMode(byte sm);
  void setSwingStep(byte ss);

  bool getOnOff();
  byte getTemp();
  byte getMode();
  byte getFanMode();
  byte getEconomyMode();
  byte getSwingMode();
  byte getSwingStep();
  byte getControllerTemp();

  // Added (not in upstream) so FujitsuClimate can implement its own, wider bus-alive
  // threshold on top of isBound()'s fixed 1000ms -- this project's own history
  // (state-of-play.md, 3B.19) found 1-3s quiet gaps are normal on this bus and a
  // 1000ms-or-less threshold flickers on them. Read-only; doesn't change isBound()'s
  // own behaviour or anything upstream.
  unsigned long getLastFrameReceived() { return lastFrameReceived; }

  // Deviation #3 -- see the file-header comment above. Raw, passive, unconfirmed --
  // NOT the same as FujiFrame::controllerPresent on currentState (which, for a
  // secondary controller, reflects OUR OWN reply's value, not the incoming frame's).
  byte getLastRawControllerPresent() { return lastRawControllerPresent; }

  // 21 Aug 2026 -- see lastRawUnknownBit above. Same raw/passive/unconfirmed caveat.
  byte getLastRawUnknownBit() { return lastRawUnknownBit; }
  // 21 Aug 2026 -- see lastRawFrame above. Returns a pointer to the internal 8-byte
  // buffer; read-only, single-threaded (fujitsu_bus_task), same as every other
  // getter here -- caller should copy out promptly rather than hold the pointer.
  byte *getLastRawFrame() { return lastRawFrame; }

  FujiFrame *getCurrentState();
  FujiFrame *getUpdateState();
  byte getUpdateFields();

  bool debugPrint = false;
};

enum class FujiMode : byte { UNKNOWN = 0, FAN = 1, DRY = 2, COOL = 3, HEAT = 4, AUTO = 5 };

enum class FujiMessageType : byte {
  STATUS = 0,
  ERROR = 1,
  LOGIN = 2,
  UNKNOWN = 3,
};

enum class FujiAddress : byte {
  START = 0,
  UNIT = 1,
  PRIMARY = 32,
  SECONDARY = 33,
};

enum class FujiFanMode : byte {
  FAN_AUTO = 0,
  FAN_QUIET = 1,
  FAN_LOW = 2,
  FAN_MEDIUM = 3,
  FAN_HIGH = 4
};

const byte kOnOffUpdateMask = 0b10000000;
const byte kTempUpdateMask = 0b01000000;
const byte kModeUpdateMask = 0b00100000;
const byte kFanModeUpdateMask = 0b00010000;
const byte kEconomyModeUpdateMask = 0b00001000;
const byte kSwingModeUpdateMask = 0b00000100;
const byte kSwingStepUpdateMask = 0b00000010;
