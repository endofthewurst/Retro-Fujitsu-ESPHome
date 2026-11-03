# Protocol summary

> Moved out of the README on 3 Nov 2026 when it was restructured. Some passages carry dated update notes from earlier in the project. Where this page and the code disagree, the code wins.

The full reference is in [PROTOCOL.md](../PROTOCOL.md).

## Protocol

The bus runs a repeating **16-byte cycle**: one 8-byte **UNIT frame** (the indoor unit's
status) immediately followed by one 8-byte **CTRL frame** (the wired controller's frame).
This is confirmed structurally across 1112 captured frames with zero sync errors — it is
**not** the single 8-byte frame model used by some other Fujitsu ESPHome projects; those
target different frame markers for different unit families.

### UNIT frame

| Byte | Content |
|------|---------|
| 0 | `0xFE` — start marker, fixed |
| 1 | `0xDF` — fixed |
| 2 | `0xDF` — fixed |
| 3 | `0x7F` — fixed |
| 4 | `0xFF` — fixed |
| 5 | `B5` — state byte A: setpoint + mode nibble |
| 6 | `B6` — state byte B: mode bits (upper bits not yet mapped) |
| 7 | `0x6B` — end marker, fixed |

### CTRL frame

| Byte | Content |
|------|---------|
| 0 | `C0` — fan/power/mode bit; the only byte with controller state |
| 1 | `0xFF` — fixed |
| 2 | `0xFF` — fixed |
| 3 | `0x5F` at rest, `0x7E` briefly during a settings change |
| 4 | `0xFF` — fixed |
| 5 | `B5` — mirrors UNIT B5 |
| 6 | `B6` — mirrors UNIT B6 |
| 7 | `0x4B` — end marker, fixed |

### Decoded fields (confirmed, ground-truthed against the physical controller)

**Power** — CTRL byte 0 (`C0`), bit 1: `0xCE` = ON, `0xCC`/`0xCD` = OFF.

**Mode** — a *pair* of fields, not one: `{B6 bits[2:0], C0 bit 0}`.

| Mode | B6[2:0] | C0 bit 0 |
|------|---------|----------|
| COOL | 3 | 0 |
| FAN  | 3 | 1 |
| DRY  | 7 | 1 |
| HEAT | 0 | 1 |
| AUTO | 0 | 0 |

**Target temperature** — `B5` bits [4:1]: `temp_c = ((B5 >> 1) & 0x0F) + 16`. Confirmed for
COOL and DRY. HEAT/AUTO looked unresolved under this addressing (both modes read the same
`B5` value regardless of setpoint) — **resolved 8 Aug 2026 under the corrected/upstream
addressing below**, where HEAT and AUTO are separate fields and both read correctly.

### Fan speed and room temperature — resolved (8 Aug 2026), live-confirmed (11 Aug 2026)

Both of these were briefly believed undecodable under the byte addressing above (every byte
thought to carry fan speed was constant across all 1112 captured frames; the byte assumed to
carry room temperature turned out to carry mode information instead).

**The fix: invert every received byte (`^= 0xFF`) and read the frame starting two bytes
later than the naive `0xFE` sync point.** This aligns the traffic with the field layout used
by other published Fujitsu LIN implementations, and once applied:

- **Fan speed** is bits [6:4] of the corrected byte 3. Live-confirmed by cycling the
  physical Fan Speed button (High→Medium→Low→Auto): **Auto=0, Low=2, Medium=3, High=4**
  (Quiet=1 inferred from spacing, not independently button-tested). This has been the
  `fan_mode` driving the climate entity — and, since firmware 4A.2, a dedicated HA `select`
  entity — since firmware 3B.18.
- **Room/controller temperature** is bits [6:1] of the corrected byte 6, and is live and
  feeding the climate entity's current temperature.

This is not a hypothesis anymore — it's the layout the current firmware actually implements
(`components/fujitsu_climate/FujiHeatPump.cpp`). See the project's own `hardware-and-protocol.md`
("Current field decoding" section) and `PROTOCOL.md`'s revision notes for the full field
table and derivation; the byte tables above in this README describe the original,
now-superseded addressing and are kept for historical/structural reference (frame length,
sync markers, etc. are still accurate).

---
