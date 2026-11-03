# Project history and roadmap

> Moved out of the README on 3 Nov 2026 when it was restructured. Some passages carry dated update notes from earlier in the project. Where this page and the code disagree, the code wins.

## Project History & Phases

- **Feb 2026** — Baseline ESP32 + ESPHome, diagnostics, LED patterns, Fujitsu component
  compiling in listen-only mode with no hardware connected.
- **29 Apr 2026** — Hardware wired up for the first time. Six capture sessions against the
  live unit while cycling the wired controller through modes and settings. The 16-byte
  UNIT+CTRL structure, mode identification, and power bit were all cracked in a single
  afternoon and published to Home Assistant.
- **May-Jul 2026** — Dormant.
- **8-11 Aug 2026** — Recovered and consolidated the local working tree; found and fixed
  the fan-speed and room-temperature decode (both had been misreported as confirmed under
  the original byte addressing — the fix was inverting every byte and re-syncing 2 bytes
  later); both live-confirmed against the real unit. HEAT/AUTO setpoint resolved the same
  way.
- **Aug-Sep 2026** — Adopted a vendored upstream transmit/decode engine; control from Home
  Assistant (power, mode, target temperature, fan speed, Economy) confirmed live against
  the real unit, gated behind an explicit kill switch. Corrected the wired controller's
  model identification (AR-3TA3/UTB-TUB, not UTY-RNNUM) and documented the optional
  UTD-RS100 remote sensor.
- **Oct 2026** — Installed in the house, bus-powered. Found the Thermo Sensor icon bit
  (byte 5 bit 7 of frames addressed to us); firmware 4A.9 adds read-only monitoring and
  split raw-frame diagnostics. A write test had no effect. RoomMind configuration reviewed.
- **17 Oct 2026** — Firmware 4A.10: Defrost Active binary sensor (byte 2 bit 3 of the frame not
  addressed to us, found on a live cold-morning cycle); the noisy "unconfirmed" text sensors
  retired. 4A.11: Thermo Sensor Experiment select — writing the icon bit can only clear the
  icon, never set it.
- **Late Oct 2026** — Firmware 4A.12 / 4A.12a: control-authority attribution (Last Change
  Source / Detail). 4A.12 also added an event entity that crash-looped the installed board;
  4A.12a dropped it and uses text sensors only.
- **27–28 Oct 2026** — Home Assistant device page regrouped as Controls / Sensors /
  Diagnostic (nothing left in "Configuration"). Thermo Sensor Icon live-confirmed; the Swing
  sensor was tested against the wired remote, renamed "Swing Icon" and moved to Sensors.
  Release candidate **4A.12b-rc1**.
- **3 Nov 2026** — Ported to ESPHome 2026.x (issue #3, reported by a user): `climate_schema()`
  and feature-flag API changes, plus a reply-timing fix. Arduino-ESP32 3.x hands received
  bytes to the code ~31 ms sooner than 2.x did, so replies went out ~31 ms early and the
  wired controller reverted HA's changes ~0.8 s later; a 31 ms reply gap restores the old
  behaviour (verified on hardware). Added `quiet_fan_mode` (default off). The last
  ESPHome 2025.4.2 commit is tagged `esphome-2025.4.2`. Release candidate **4A.13-rc1**.

The transmit path is live and has been exercised successfully for several fields (see
Climate Controls above), but hasn't been through the same volume of testing as decode —
treat it as working-but-young rather than as mature as decode. The wired controller's Thermo
Sensor icon is decoded and monitored read-only (live-confirmed 28 Oct 2026; the ESP32 can clear
it but not set it); Defrost Active exists but rests on one live cycle. See the Roadmap below.

---

## Roadmap / To do

Full detail: `docs/session-5A-checkin-2026-10-03.md`, `docs/session-5B-defrost-2026-10-17.md`, `docs/session-5C-entity-layout-2026-10-28.md`.

### A. Thermo Sensor icon — monitoring confirmed, two-way control is a future task
- [x] **A1. Monitor.** The icon is byte 5 bit 7 (0x80) of the frame addressed to us (`84` = shown, `04` = off); exposed as `binary_sensor` "Thermo Sensor Icon" (fw 4A.9). **Live-confirmed 28 Oct 2026:** HA matches the wired display at all times.
- [x] **A2.** Second confirmation — covered by the 28 Oct 2026 test.
- [x] **A3.** Retire the noisy `thermo_sensor_unconfirmed` / `unknown_bit_unconfirmed` / `raw_frame_unconfirmed` diagnostics — done in 4A.10.
- [ ] **A4.** Establish what the icon actually changes (the 21.0 vs 19.7 °C room-temperature gap is *not* explained by it).
- [ ] **A5. Two-way control (future task).** From the ESP32 the icon can only be *cleared* (ORing 0x8 into the `updateMagic` nibble of a STATUS reply; one-shot). Six variants tried in 4A.11 never set it. Turning it on needs another field or message type, probably reachable only by emulating the wired controller, which is **not** recommended (ESP32 is never the boss). Alternative: accept off-only plus an HA-side advisory.
- [ ] **A6.** Keep or remove the "Thermo Sensor Write Test" switch; the "Thermo Sensor Experiment" select is no longer needed.

### B. Defrost / oil-recovery active flag — shipped, validation open
- [x] **B1–B3.** Bit identified (byte 2 bit 3 of the frame not addressed to us) and exposed as `binary_sensor` "Defrost Active" (fw 4A.10, 3 s debounce), from one live cycle on 17 Oct 2026. A `hvac_action: defrosting` mapping is not done.
- [ ] **B4.** Validate on a second cold cycle; check whether Oil Recovery also sets the bit.
- [ ] **B5.** Use the flag in RoomMind (exclude defrost plus a ~2 min hold-off from learning and control).

### C. Other
- API/WiFi link drops (five brief `unavailable` events in two days) not yet root-caused.
- RoomMind short-cycling (4–8 minute runs observed 1–2 Oct); five rooms share the single climate entity.
- Swing Icon reads On while the unit is off/standby (the wired remote hides the icon then); cause not investigated.
- Possible tidy-ups: move Sync Mismatch / Out of Sync Since to Diagnostic; make Restart a button instead of a switch.
- The local web page cannot follow the HA Controls / Sensors / Diagnostic grouping while on `web_server` version 2.

---
