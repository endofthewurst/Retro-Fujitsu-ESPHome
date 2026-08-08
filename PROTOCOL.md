# Fujitsu ART30LUAK LIN Bus Protocol Reference

Model: ART30LUAK (RSG series ~2010)
Controller: UTY-RNNUM
Bus: Single-wire LIN, 500 baud, 8N1, NO parity
ESP32 role: Secondary slave, GPIO16 RX / GPIO17 TX via TJA1021/SIT1021T transceiver

> **Revision note (7 Aug 2026):** all six capture logs are now committed under `captures/`
> and were re-analysed in bulk (1112 frames). That analysis **invalidated two fields that
> this document previously recorded as confirmed** — fan speed and room temperature. Both
> are marked ❌ below. Everything else held up.

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

### Fan Speed ❌ NOT DECODED

**Previously recorded here as `(C0 >> 2) & 0x07` with "3 = MEDIUM confirmed". That was wrong
and the code implementing it should not be trusted.**

C0 takes exactly three values across all 1112 captured frames — `0xCC`, `0xCD`, `0xCE` —
which differ only in bits 0 and 1. **Bits [4:2] are `011` in every frame ever captured.**
The decoder reports "fan=3" because those bits are constant, not because the fan was on
medium. It would report MEDIUM regardless of the actual fan setting.

The previous open question — "user cycled LOW -> AUTO -> HIGH -> MED -> LOW, CTRL0 bytes not
yet retrieved from logs" — is now answered: **the CTRL0 bytes are in the logs and they do
not change.** Therefore either that fan-cycling session was never captured to a surviving
log, or fan speed is communicated outside this 16-byte cycle.

**This is now the largest open question in the project.** See Next Steps 1.

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

### Room temperature ❌ NOT DECODED

The old decode read B6 and produced ~52C. The logs contain **189 rejection warnings**:

```
Room temp 52.0C out of range (byte6=0xE9) - keeping nan
Room temp 53.0C out of range (byte6=0xEB) - keeping nan
```

B6's upper bits carry mode information, not temperature. **Current temperature has never
been successfully read** — every capture shows `room=nanC`, so the HA climate entity has no
current-temperature source. See Open Question 3.

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

### 1. Fan speed — where is it? (highest priority)

Not in C0 bits[4:2]; not anywhere else in the 16-byte cycle, since no other byte varies.

**To resolve:** fix mode and temperature, then change *only* fan (AUTO -> QUIET -> LOW ->
MED -> HIGH), pausing ~15s on each. Critically, **log the raw byte stream, not just parsed
frames** — the parser accepts the 8 bytes after a UNIT frame unconditionally as the CTRL
frame, so a third frame type would be silently consumed and never seen. Without a raw dump
a negative result is uninterpretable.

### 2. Temperature offset for HEAT/AUTO

For COOL and DRY, offset=16 works. For HEAT:
- User set 20C, B5=0xC4 -> raw=2 -> 2+16=18C (wrong by 2)
- If offset=18: 2+18=20C ✓

For AUTO at 23C, B5=0xC4 -> raw=2 -> neither offset gives 23C. Since HEAT and AUTO both show
B5=0xC4, the likeliest explanation is that **B5 does not carry a live setpoint in HEAT/AUTO**,
or both captures were taken mid-transition.

**To resolve:** set HEAT to 24C. Offset=16 predicts raw=8; offset=18 predicts raw=6. Compare B5.

### 3. Room temperature / thermo sensor source

The UTY-RNNUM has a "thermo sensor" button switching between the wall controller's sensor and
the indoor air handler's sensor. B6 bits[7:3] likely encode which is active.

**To resolve:** toggle the thermo sensor button and compare B6 before/after against a real
thermometer reading. Also let the room drift 2C over ten minutes and watch for any byte tracking it.

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

1. **Fan speed capture with raw byte logging** — see Open Question 1. Blocks Phase 4 fan control.
2. **Confirm temperature offset for HEAT** — set HEAT to 24C and read B5.
3. **Map thermo sensor / room temp** — toggle sensor button, compare B6.
4. **Wire decoded state to HA climate entity** — `on_off_`, `mode_`, `fan_mode_` decode
   correctly in parseCTRLFrame(); still need pushing to the ESPHome climate component publish.
5. **Phase 4: transmit.** The CTRL frame is the wired controller's own frame, so as a
   secondary controller the ESP32 should emit that shape rather than invent a command frame.
   Copy the last received CTRL frame, modify C0 (power / mode bit), B5 (temp), B6 (mode bits),
   set B3=0x7E, send after the UNIT frame within the reply window
   (`FRAME_REPLY_DELAY_MS = 60`, unverified against measured inter-frame timing).
