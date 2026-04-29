# Fujitsu ART30LUAK LIN Bus Protocol Reference

Model: ART30LUAK (RSG series ~2010)  
Controller: UTY-RNNUM  
Bus: Single-wire LIN, 500 baud, 8N1, NO parity  
ESP32 role: Secondary slave, GPIO16 RX / GPIO17 TX via TJA1021/SIT1021T transceiver  

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
[5] B5    — STATE BYTE A: temperature + mode (see decode below)
[6] B6    — STATE BYTE B: mode bits + possibly room temp (partially decoded)
[7] 0x6B  — end marker (0xEB seen on some frames — treat as equivalent)
```

### CTRL Frame (?? ... 4B)

```
[0] C0    — CTRL_START: fan speed + power on/off + mode bit (see decode below)
[1] 0xFF  — fixed
[2] 0xFF  — fixed
[3] 0x5F  — fixed; briefly 0x7E during a settings change (change-in-progress flag)
[4] 0xFF  — fixed
[5] B5    — same as UNIT frame B5 (redundant copy)
[6] B6    — same as UNIT frame B6 (redundant copy)
[7] 0x4B  — end marker
```

---

## Field Decoding

### Temperature (set point)

Encoded in B5 bits[4:1]. Formula:

```cpp
uint8_t raw = (B5 >> 1) & 0x0F;
float temp_c = raw + 16.0f;   // range 16–30°C
```

**Ground-truthed:**
- B5=0xC9 → raw=4 → **20°C** (COOL mode, confirmed by user)
- B5=0xCC → raw=6 → **22°C** (DRY mode, confirmed by user)

> ⚠️ HEAT mode may use a different offset (see notes below).

### Power On/Off

Encoded in CTRL_START (C0) bit[1]:

```cpp
bool on = (C0 >> 1) & 0x01;
```

- 0xCE → bit[1]=1 → **ON** ✓
- 0xCC → bit[1]=0 → **OFF** ✓

### Fan Speed

Encoded in CTRL_START (C0) bits[4:2]:

```cpp
uint8_t fan = (C0 >> 2) & 0x07;
```

Confirmed value:
- 3 = **MEDIUM** ✓ (from 0xCC and 0xCE)

Enum mapping (from FujiHeatPump.h — not all confirmed against hardware yet):

| Value | Speed |
|-------|-------|
| 0     | AUTO  |
| 1     | QUIET |
| 2     | LOW   |
| 3     | MEDIUM ✓ |
| 4     | HIGH  |

> ⚠️ LOW, HIGH, AUTO, QUIET not yet confirmed — logs from fan-speed cycle needed.

### Operating Mode

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
- DRY is unique: B6=0xFF (all ones), the only mode where B6 upper bits are 0xF rather than 0xE

---

## Full Observed Frame Table

Captured during live mode cycling with unit at various set points:

| State   | B5   | B6   | C0   | Decoded temp | pwr | fan | notes |
|---------|------|------|------|-------------|-----|-----|-------|
| Standby | 0xF7 | 0xEB | 0xCE | —           | ON  | 3   | No active mode |
| COOL    | 0xC9 | 0xEB | 0xCE | 20°C ✓      | ON  | 3   | Ground-truthed |
| DRY     | 0xCC | 0xFF | 0xCD | 22°C ✓      | OFF | 3   | Ground-truthed |
| FAN     | 0xC6 | 0xEB | 0xCD | 19°C        | OFF | 3   | (fan mode has no set temp) |
| HEAT    | 0xC4 | 0xE8 | 0xCD | 18°C*       | OFF | 3   | *user set 20°C — possible offset=18 |
| AUTO    | 0xC4 | 0xE8 | 0xCE | 18°C*       | ON  | 3   | *user set 23°C — decode unclear |

> The power=OFF readings for DRY/FAN/HEAT may reflect that the unit was captured during a mode-change transition, or that mode selection persists in the CTRL frame even when the unit is off.

---

## Open Questions

### 1. Temperature offset for HEAT/AUTO

For COOL and DRY, offset=16 works perfectly. For HEAT:
- User set 20°C, B5=0xC4 → raw=2 → 2+16=18°C (wrong by 2)
- If offset=18: 2+18=20°C ✓

For AUTO at 23°C, B5=0xC4 → raw=2 → neither offset gives 23°C. This suggests the AUTO capture may not have reflected the 23°C set point (captured during transition), or temperature encoding is different for some modes.

**To resolve:** Set HEAT to a temperature that gives a unique raw value, e.g. 24°C (raw=8 if offset=16, raw=6 if offset=18). Compare B5.

### 2. Fan speed values for LOW / HIGH / QUIET / AUTO

User cycled LOW → AUTO → HIGH → MED → LOW while in HEAT 22°C. CTRL0 bytes not yet retrieved from logs. When logs are available, decode `(C0 >> 2) & 0x07` for each setting.

### 3. Room temperature / Thermo sensor

The UTY-RNNUM has a "thermo sensor" button that switches between:
- Using the **room temperature sensor in the wall controller** (UTY-RNNUM)
- Using the **sensor in the indoor air handler**

B6 upper bits (bits[7:3]) likely encode which sensor is active and/or the sensor reading. The old decode of room temp from B6 was giving ~52°C (wrong). B6 clearly encodes mode information in bits[2:0]; the upper bits are still unmapped.

**To resolve:** Toggle thermo sensor button and compare B6 before/after with known room temperature.

### 4. B3 = 0x5F vs 0x7E in CTRL frame

CTRL[3] is normally 0x5F and briefly becomes 0x7E during a settings update on the controller. Likely a "change in progress" handshake flag.

---

## Firmware Notes

**Current version:** 3B.11 (running on device at 192.168.1.40)

**OTA:** Native ESPHome OTA (port 3232) fails with VERBOSE logging. Use:
```
curl.exe -F "file=@firmware.bin" http://192.168.1.40/update
```

**WiFi:** "moose" network has AP client isolation. OTA from laptop only works via curl HTTP POST. USB fallback on COM4.

**Sync strategy:** `expecting_ctrl_` flag — parser only accepts 0xFE as a frame start at rx_index_=0. After a valid UNIT frame, the flag allows any byte to start the immediately-following CTRL frame. Prevents offset-drift.

---

## Next Steps

1. **Retrieve fan-speed log** — get CTRL0 bytes for LOW/AUTO/HIGH from the mode-cycle capture to confirm fan speed encoding.
2. **Confirm temperature offset for HEAT** — set HEAT to 24°C and read B5.
3. **Map thermo sensor bit** — toggle sensor button, compare B6.
4. **Wire decoded state to HA climate entity** — `on_off_`, `mode_`, `fan_mode_` now decoded correctly in parseCTRLFrame(); need to push these to the ESPHome climate component publish.
5. **Phase 4: transmit** — build proper CTRL frame to send commands. Copy last received CTRL frame, modify C0 (fan/power/mode-bit), B5 (temp), B6 (mode bits). Send after UNIT frame.
