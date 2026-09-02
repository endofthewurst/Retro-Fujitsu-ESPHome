# Fujitsu ART30LUAK LIN Bus Protocol Reference

Model: ART30LUAK (RSG series ~2010)
Controller: Fujitsu AR-3TA3 / UTB-TUB (previously misidentified in this repo as UTY-RNNUM — see revision note below)
Bus: Single-wire LIN, 500 baud, 8N1, NO parity (receive path; TRANSMIT now uses 8E1
as of firmware 3B.21 — see revision note below)
ESP32 role: Secondary slave, GPIO16 RX / GPIO17 TX via TJA1021/SIT1021T transceiver

> **Revision note (7 Aug 2026):** all six capture logs are now committed under `captures/`
> and were re-analysed in bulk (1112 frames). That analysis **invalidated two fields that
> this document previously recorded as confirmed** — fan speed and room temperature. Both
> are marked ❌ below. Everything else held up.

> **Revision note (1 Sep 2026):** the wired controller on this unit is a Fujitsu
> **AR-3TA3 / UTB-TUB**, not the UTY-RNNUM assumed above and elsewhere in this document
> (corroborated by the ART30/36LUAK technical manual's spec table: "WIRED (AR-3TA*)"). This
> doesn't affect any of the decoded bus fields below, which were derived from real captures.
> Separately, the meaning of the "thermo sensor" button/icon referenced in Open Question 3
> is now understood — see the note added there.

> **Revision note (2 Sep 2026):** fan speed IS decoded, live-confirmed, and has been the
> production `fan_mode` value (plus its own HA select entity) since firmware 3B.18 — this
> document's "Fan Speed NOT DECODED" section and Open Question 1 below are stale and were
> resolved back on 8 Aug 2026 (see `upstream-comparison.md`) and live-confirmed 10-11 Aug
> 2026. This whole document describes the **original bespoke decode against raw,
> uninverted bytes**, which is superseded by the "upstream" layout (every byte XOR 0xFF,
> frame boundary 2 bytes later) actually implemented in
> `components/fujitsu_climate/FujiHeatPump.cpp` today. See the project's
> `hardware-and-protocol.md` ("Current field decoding" section) for the authoritative,
> current field table — this file is kept mainly for its empirical byte-variance data
> (still accurate) and its historical open-question framing (now answered for fan speed
> and room temperature; still open for the thermo-sensor/CN8 question and CTRL B3).

---

## Frame Structure

The bus runs a 16-byte repeating cycle: one **UNIT frame** followed immediately by one **CTRL frame**, both 8 bytes.

### UNIT Frame (FE ... 6B)

```
[0] 0xFE  — start marker (fixed)
[1] 0xDF  — fixed
[2] 0xDF  — fixed
[3] 0x7F  — fixed
[4] 0xFF  — fixed
[5] B5    — STATE BYTE A: temperature + mode nibble (see decode below)
[6] B6    — STATE BYTE B: mode bits in [2:0]; upper bits unmapped
[7] 0x6B  — end marker
```

### CTRL Frame (?? ... 4B)

```
[0] C0    — CTRL_START: power + mode bit (see decode below)
[1] 0xFF  — fixed
[2] 0xFF  — fixed
[3] 0x5F  — fixed; briefly 0x7E during a settings change (change-in-progress flag)
[4] 0xFF  — fixed
[5] B5    — same as UNIT frame B5 (redundant copy)
[6] B6    — same as UNIT frame B6 (redundant copy)
[7] 0x4B  — end marker
```

---

## Measured byte variance — all 1112 captured frames

This is the empirical base for everything below. Across every frame in `captures/`, only
four byte positions ever changed value:

| Position | Distinct values observed |
|---|---|
| UNIT B5 / CTRL B5 | `F7` `F8` `FA` `C4` `C6` `C9` `CC` (7) |
| UNIT B6 / CTRL B6 | `E8` `E9` `EB` `FF` (4) |
| CTRL B0 (C0) | `CC` `CD` `CE` (3) |
| CTRL B3 | `5F` (535×), `7E` (4×) |

**Every other byte in both frame types was constant across all 1112 frames.** No pre-sync
or resync errors appear anywhere in the logs — the `expecting_ctrl_` sync strategy works.

---

## Field Decoding

### Temperature (set point) ✓

Encoded in B5 bits[4:1]:

```cpp
uint8_t raw = (B5 >> 1) & 0x0F;
float temp_c = raw + 16.0f;   // range 16-30C
```

**Ground-truthed:**
- B5=0xC9 -> raw=4 -> **20C** (COOL mode, confirmed at controller)
- B5=0xCC -> raw=6 -> **22C** (DRY mode, confirmed at controller)
- B5=0xF7 -> raw=11 -> **27C**, stepping to B5=0xF8 -> raw=12 -> **28C** on a single
  temperature-up press (`3B10_buttons.log`, 13:21:52). Clean monotonic step.

⚠️ HEAT/AUTO may differ — see Open Question 1.

### Power On/Off ✓

Encoded in CTRL_START (C0) bit[1]:

```cpp
bool on = (C0 >> 1) & 0x01;
```

- 0xCE -> bit[1]=1 -> **ON** ✓
- 0xCC / 0xCD -> bit[1]=0 -> **OFF** ✓

### Fan Speed ✓ DECODED, LIVE-CONFIRMED (resolved 8 Aug 2026, live-confirmed 11 Aug 2026)

**Previously recorded here as `(C0 >> 2) & 0x07` with "3 = MEDIUM confirmed". That was wrong
and the code implementing it should not be trusted — see below for why, kept for history.**

C0 takes exactly three values across all 1112 captured frames — `0xCC`, `0xCD`, `0xCE` —
which differ only in bits 0 and 1. **Bits [4:2] are `011` in every frame ever captured.**
The old decoder reported "fan=3" because those bits are constant, not because the fan was
on medium. It would have reported MEDIUM regardless of the actual fan setting. Fan speed
is not in C0 at all under this document's raw/uninverted byte addressing.

**Resolved:** fan speed lives in a different byte, under a different (upstream) addressing
scheme that requires inverting every byte (`XOR 0xFF`) and reading the frame starting 2
bytes later than this document's `0xFE` sync point — see `upstream-comparison.md` for the
full derivation. Once corrected, fan speed is **bits [6:4] of the byte this document's old
scheme never separately named** (`readBuf[3]` in the current firmware, `kFanMask =
0b01110000`), and it is not constant — it took two distinct values in the original 29 April
captures alone (`AUTO`, `MEDIUM`), immediately falsifying the "always 3" reading above.

**Live-confirmed 11 Aug 2026** (`test-and-dev-workflow.md`'s Session B): cycling the
physical Fan Speed button High→Medium→Low→Auto on the real unit produced raw values
`4→3→2→0` in lockstep. James independently confirmed the mapping: **0=Auto, 2=Low,
3=Medium, 4=High**. Quiet=1 is inferred from the enum's spacing, not independently
button-tested. This has been the actual `fan_mode` driving `climate.aircon_fujitsu_heat_pump`
(and a dedicated HA `select` entity since 4A.2) since firmware 3B.18 — not a diagnostic-only
reading.

### Operating Mode ✓

Mode is **not** in a single field. It is uniquely identified by the combination of **B6 bits[2:0]** and **C0 bit[0]**:

```cpp
uint8_t b6_mode = B6 & 0x07;
uint8_t c0_bit0 = C0 & 0x01;
```

| Mode   | B6[2:0] | C0 bit[0] | B5 example | B6 example |
|--------|---------|-----------|------------|------------|
| COOL   | 3 (011) | 0         | 0xC9       | 0xEB       |
| FAN    | 3 (011) | 1         | 0xC6       | 0xEB       |
| DRY    | 7 (111) | 1         | 0xCC       | 0xFF       |
| HEAT   | 0 (000) | 1         | 0xC4       | 0xE8       |
| AUTO   | 0 (000) | 0         | 0xC4       | 0xE8       |

All five modes are uniquely identified by this pair. No two modes share the same combination.

**Notable observations:**
- COOL and FAN share B6=0xEB; distinguished only by C0 bit[0]
- HEAT and AUTO share B5=0xC4 and B6=0xE8; distinguished only by C0 bit[0]
- DRY is unique: B6=0xFF, the only mode where B6 upper nibble is 0xF rather than 0xE

### Room temperature ✓ DECODED, LIVE-CONFIRMED (resolved 8 Aug 2026)

The old decode read B6 and produced ~52C, which is impossible. The logs contain **189
rejection warnings**:

```
Room temp 52.0C out of range (byte6=0xE9) - keeping nan
Room temp 53.0C out of range (byte6=0xEB) - keeping nan
```

B6's upper bits carry mode information, not temperature, **under this document's raw byte
addressing** — that specific finding still holds. **Resolved:** under the upstream
(inverted, 2-byte-shifted) addressing, room/controller temperature decodes correctly from
a different byte entirely — see `upstream-comparison.md`. This is now live, feeding the HA
climate entity's current temperature since firmware 3B.18. Note this is specifically the
**wired controller's own thermistor** reading (not necessarily the indoor unit's, or a
UTD-RS100's, if one is fitted) — see Open Question 3 for what "Thermo Sensor" selects
between.

---

## Full Observed Frame Table

| State   | B5   | B6   | C0   | Decoded temp | pwr | notes |
|---------|------|------|------|--------------|-----|-------|
| Standby | 0xF7 | 0xEB | 0xCE | 27C          | ON  | |
| COOL    | 0xC9 | 0xEB | 0xCE | 20C ✓        | ON  | Ground-truthed |
| DRY     | 0xCC | 0xFF | 0xCD | 22C ✓        | OFF | Ground-truthed |
| FAN     | 0xC6 | 0xEB | 0xCD | 19C          | OFF | (fan mode has no set temp) |
| HEAT    | 0xC4 | 0xE8 | 0xCD | 18C*         | OFF | *user set 20C — possible offset=18 |
| AUTO    | 0xC4 | 0xE8 | 0xCE | 18C*         | ON  | *user set 23C — decode unclear |

The power=OFF readings for DRY/FAN/HEAT may reflect capture during a mode-change transition,
or that mode selection persists in the CTRL frame while the unit is off.

---

## Open Questions

### 1. Fan speed — RESOLVED, see the Fan Speed section above

Not in C0 bits[4:2] under this document's raw byte addressing — correct, that part still
holds. But it is decoded and live-confirmed under the upstream (inverted, 2-byte-shifted)
addressing — see the "Fan Speed" entry in Field Decoding above, `upstream-comparison.md`,
and `test-and-dev-workflow.md`'s Session B. No further hardware session is needed for this.

### 2. Temperature offset for HEAT/AUTO

For COOL and DRY, offset=16 works. For HEAT:
- User set 20C, B5=0xC4 -> raw=2 -> 2+16=18C (wrong by 2)
- If offset=18: 2+18=20C ✓

For AUTO at 23C, B5=0xC4 -> raw=2 -> neither offset gives 23C. Since HEAT and AUTO both show
B5=0xC4, the likeliest explanation is that **B5 does not carry a live setpoint in HEAT/AUTO**,
or both captures were taken mid-transition.

**To resolve:** set HEAT to 24C. Offset=16 predicts raw=8; offset=18 predicts raw=6. Compare B5.

### 3. Room temperature / thermo sensor source

The wired controller (Fujitsu AR-3TA3 / UTB-TUB) has a "Thermo Sensor" button/setting
switching the room-temperature source between the wired controller's own thermistor and a
remote source — either the indoor unit's own built-in thermistor, or a Fujitsu **UTD-RS100**
remote sensor if one has been fitted in its place at connector **CN8** on the indoor unit
(not yet confirmed either way on this installation). B6 bits[7:3] likely encode which source
is active.

**Update, 1 Sep 2026:** the wired controller's LCD shows an icon resembling the controller
itself when its own thermistor is in use, and hides that icon when a remote source is in
use — so the physical icon is a reliable ground truth for which state is active. This wasn't
watched during this repo's earlier Thermo Sensor button testing (three live presses produced
real bus anomalies and, on the third, an ESP32 crash — see the project history), so it's
still not known which icon state corresponds to which observed bus behaviour.

**To resolve:** toggle the Thermo Sensor button and compare B6 before/after against the
physical icon state (not just a real thermometer reading), and confirm whether a UTD-RS100
is fitted at CN8 first. Do this USB-tethered given the crash history above.

### 4. CTRL B3 = 0x5F vs 0x7E

CTRL[3] is normally 0x5F and becomes 0x7E during a settings update. Observed exactly 4 times,
all in `3B10_buttons.log` between 13:21:53 and 13:22:04 — during temperature button presses.
Almost certainly a change-in-progress handshake flag, and likely required on transmitted
command frames.

---

## Firmware Notes

**Current version:** 3B.11 (ESPHome 2025.4.2, compiled 29 Apr 2026, device at 192.168.1.40)

**OTA:** Native ESPHome OTA (port 3232) fails with VERBOSE logging. Use:
```
curl.exe -F "file=@firmware.bin" http://192.168.1.40/update
```

**WiFi:** "moose" network has AP client isolation. OTA from laptop only works via curl HTTP
POST. USB fallback on COM4.

**Sync strategy:** `expecting_ctrl_` flag — parser only accepts 0xFE as a frame start at
rx_index_=0. After a valid UNIT frame, the flag allows any byte to start the immediately
following CTRL frame. Prevents offset drift. Confirmed working: zero sync errors in 1112 frames.

---

## Next Steps

1. ~~Fan speed capture with raw byte logging~~ — **done.** Resolved 8 Aug 2026, live-confirmed 11 Aug 2026. See the Fan Speed section above.
2. **Confirm temperature offset for HEAT** — set HEAT to 24C and read B5.
3. **Map thermo sensor / room temp** — toggle sensor button, compare B6.
4. **Wire decoded state to HA climate entity** — `on_off_`, `mode_`, `fan_mode_` decode
   correctly in parseCTRLFrame(); still need pushing to the ESPHome climate component publish.
5. **Phase 4: transmit.** The CTRL frame is the wired controller's own frame, so as a
   secondary controller the ESP32 should emit that shape rather than invent a command frame.
   Copy the last received CTRL frame, modify C0 (power / mode bit), B5 (temp), B6 (mode bits),
   set B3=0x7E, send after the UNIT frame within the reply window
   (`FRAME_REPLY_DELAY_MS = 60`, unverified against measured inter-frame timing).
