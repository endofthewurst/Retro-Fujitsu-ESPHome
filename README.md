# Retro-Fujitsu-ESPHome

Monitor, and eventually control, a **Fujitsu heat pump** from **Home Assistant** using an
ESP32 and ESPHome — without modifying or replacing the existing wired controller.

Developed and tested against a **~2010-era Fujitsu ART30LUAK (RSG series)** heat pump using
its LIN bus interface, with the ESP32 joining as a **secondary (slave) controller** alongside
the existing wired remote (Fujitsu **AR-3TA3 / UTB-TUB** — see correction note below).

**Current status:** decoding the bus is working and solid (power, mode, and target
temperature are read correctly into Home Assistant). Sending commands back to the unit is
**experimental and unvalidated** — see [Protocol](#protocol) and
[Project History & Phases](#project-history--phases) below before relying on it.

---

## Correction, 1 September 2026 — actual wired controller model, and the UTD-RS100 option

This repo's docs (and its earlier design notes) assumed the wired controller fitted to this
ART30LUAK was a Fujitsu **UTY-RNNUM**. That was wrong. The actual unit is a Fujitsu
**AR-3TA3 / UTB-TUB** simple wired remote controller (the exact model suffix is hard to pin
down and varies by region/distributor) — corroborated by the ART30/36LUAK technical manual's
spec table, which lists the remote controller type as "WIRED (AR-3TA*)". All the live
bus-decode and bus-capture work in this repo is unaffected by this correction, since it was
derived from real captures against the actual hardware, not from the UTY-RNNUM manual's
description — but any reasoning that leaned on the UTY-RNNUM manual's specific button/DIP-switch
layout (rather than the captured bus behaviour) should be treated with caution.

Separately: the ART30/36LUAK has an optional **Fujitsu UTD-RS100** remote temperature sensor.
If fitted, it physically replaces the connection to the indoor unit's own built-in thermistor
at connector **CN8** on the indoor unit's PCB — it does not sit alongside it. Whether a
UTD-RS100 is actually fitted on this specific installation has **not been physically
confirmed** (checking connector CN8 on the indoor unit would confirm it either way).

This matters for the wired controller's **Thermo Sensor** setting (see
[Open Question 3 in PROTOCOL.md](PROTOCOL.md)): that setting selects which room-temperature
source the system uses — the wired controller's own thermistor, or a remote source (the
indoor unit's own thermistor, or the UTD-RS100 if fitted at CN8 in its place). On the wired
controller's LCD, an icon resembling the wired controller itself is shown when its own
thermistor is in use, and is not shown when a remote source is in use. This wasn't understood
during this repo's earlier Thermo Sensor button testing (see the project history), so any
future retest of that setting should watch the physical icon directly, and confirm whether a
UTD-RS100 is fitted at CN8, rather than inferring the sensor source from bus behaviour alone.

---

## Design principle: the ESP32 is never the boss

The wired controller (Fujitsu **AR-3TA3 / UTB-TUB** — see correction note below) is the
**primary controller** and must always remain fully capable of
running the system on its own — regardless of whether the ESP32 is powered, on WiFi, able
to reach Home Assistant, or running firmware without bugs. A person must always be able to
walk up to the wired controller and operate the heat pump exactly as if the ESP32 didn't
exist. If the ESP32 and the wired controller ever disagree, the wired controller's setting
is the one that should win. This governs every design decision in the transmit (control)
path.

---

## Hardware

### Heat Pump Units (Tested)
| Component | Model |
|-----------|-------|
| Indoor Unit | Fujitsu **ART30LUAK** |
| Outdoor Unit | Fujitsu **AOT30LMBDL** |
| Wired Controller | Fujitsu **AR-3TA3 / UTB-TUB** (see correction note below) |
| Optional remote sensor | Fujitsu **UTD-RS100** (not confirmed fitted on this installation — see correction note below) |

### Electronics Required
| Component | Notes |
|-----------|-------|
| **ESP32-WROOM-32** | Any ESP32 dev board should work |
| **linttl3 LIN Module** | TJA1021/SIT1021T chip, handles 12V/24V LIN bus to 3.3V TTL conversion |

---

## Features

> **Note, 2 September 2026:** this section badly lagged the project's actual progress —
> it still described the original ~29 April bring-up. Fan speed, current (room)
> temperature, and the HEAT/AUTO target-temperature gap were all resolved back in August
> (see `PROTOCOL.md`'s revision notes), and control from Home Assistant has since been
> adopted from a vendored upstream engine and confirmed working live for several fields.
> Updated below to match; see the project's own `hardware-and-protocol.md` and
> `state-of-play.md` for the full, current picture.

### Confirmed and working
- **State decode** — Power, Mode, Target Temperature (all modes, including HEAT/AUTO),
  **Fan Speed**, current (room) temperature, and Economy mode all read correctly off the
  bus and published to Home Assistant as a climate entity (plus dedicated diagnostic
  sensors/entities for several of them).
- **Fan Speed** — decoded and live-confirmed: Auto=0, Low=2, Medium=3, High=4 (Quiet=1
  inferred from enum spacing, not independently button-tested). Exposed both via the
  climate entity's `fan_mode` and a dedicated HA `select` entity.
- **Control from Home Assistant** — power, mode, target temperature, fan speed, and
  Economy mode have all been exercised live via the HA API against the real unit, gated
  behind an explicit, default-off "Aircon Control Enabled" kill switch. See
  `state-of-play.md`'s "4A" sessions for the live-confirmation history of each field.
- **Diagnostic sensors** — IP address, WiFi signal strength, uptime, free heap, bus
  status/alive, status LED pattern.
- **Status LED** — Visual feedback on the ESP32 board (GPIO2); read-only diagnostic, not
  a controllable entity.
- **Web interface** — ESPHome's built-in web server for local access.
- **Non-invasive installation** — connects as a secondary (slave) controller; the existing
  wired controller keeps working normally, with no modification to the heat pump or the controller.

### Experimental / in progress
- **Swing** — decoded upstream in principle (see `PROTOCOL.md`) but not live-tested on
  this unit.
- **The wired controller's "Thermo Sensor" setting** — its general meaning is now
  understood (see the correction note above), but the specific protocol bit behind it is
  still not confirmed; see `PROTOCOL.md`'s Open Question 3.
- **OTA updates** — works, with one caveat: native ESPHome OTA fails if `logger: level:
  VERBOSE` is set (use the curl route below instead in that case).

### Climate Controls (see status above for what's actually reliable today)
| Control | Options | Status |
|---------|---------|--------|
| Power | On / Off | Decoded and control both live-confirmed |
| Mode | Auto, Cool, Heat, Dry, Fan Only | Decoded and control both live-confirmed |
| Target Temperature | 16°C – 30°C | Decoded and control both live-confirmed, all modes including HEAT/AUTO |
| Fan Speed | Auto, Quiet, Low, Medium, High | Decoded and live-confirmed (Quiet inferred); exposed via a dedicated select entity |
| Economy | On / Off | Decoded and control both live-confirmed, both as a climate preset and a standalone switch |

### Sensors
| Sensor | Description | Status |
|--------|-------------|--------|
| Target Temperature | Current setpoint | Working, all modes including HEAT/AUTO |
| Current Temperature | Room temperature (wired controller's own thermistor) | Working — see Protocol |
| Fan Speed | Auto/Quiet/Low/Medium/High | Working (Quiet inferred, not independently tested) |
| Economy | On/Off | Working, decoded and controllable |
| WiFi Signal | RSSI in dBm | Working |
| IP Address | Device IP on local network | Working |
| Uptime | Time since last reboot | Working |
| Free Heap | ESP32 memory (for diagnostics) | Working |

---

## How It Works

```
Fujitsu Heat Pump (Indoor Unit)
        <->  LIN Bus (500 baud, 8E1)
  linttl3 Module (TJA1021)
        <->  TTL UART (3.3V)
    ESP32-WROOM-32
        <->  WiFi
    Home Assistant
```

The Fujitsu indoor unit communicates with controllers using a **LIN bus** running at
**500 baud, 8E1** (switched from 8N1 as of firmware 3B.21 — see UART Configuration
below). The linttl3 module converts the 12V/24V LIN bus signals to 3.3V TTL
levels suitable for the ESP32's UART.

The ESP32 listens to frames on the bus and decodes them into Home Assistant. Injecting
control frames as a secondary controller is implemented but experimental (see Features) —
the existing wired controller continues to operate normally either way.

---

## Wiring

### Safety Warning
**Turn off the heat pump at the breaker before opening the indoor unit or making any
connections.**

### Connection Overview

```
Fujitsu Indoor Unit (ART30LUAK)
  Remote Controller Connector (CN-REM or similar)
    Pin 1: 12V or 24V Power  ---> linttl3 VIN
    Pin 2: LIN Bus (Data)    ---> linttl3 LIN
    Pin 3: Ground            ---> linttl3 GND (power side)

linttl3 Module (TJA1021)
    TX  ---> ESP32 GPIO16 (RX2)
    RX  <--- ESP32 GPIO17 (TX2)
    GND ---> ESP32 GND
    SLP, INH: leave unconnected

ESP32-WROOM-32
    GPIO16 (RX2) <- linttl3 TX
    GPIO17 (TX2) -> linttl3 RX
    GND          <- linttl3 GND
    GPIO2        -> Status LED
    USB          -> Laptop/Power (bring-up only)
```

### Pin Summary

| linttl3 Pin | Connects To | Notes |
|-------------|-------------|-------|
| VIN | Heat pump 12V/24V | Power for module |
| LIN | Heat pump LIN wire | Data line |
| GND (power side) | Heat pump GND | Common ground |
| TX | ESP32 GPIO16 | Data to ESP32 |
| RX | ESP32 GPIO17 | Data from ESP32 |
| GND (MCU side) | ESP32 GND | Common ground |
| SLP | Not connected | Leave floating |
| INH | Not connected | Leave floating |

### Critical: Common Ground
All three devices (heat pump, linttl3, ESP32) **must share a common ground**. Without this,
communication will not work.

### Critical: pull-up on GPIO16 (RX)
Without a pull-up, the floating pin will crash the ESP32 whenever it's powered but not yet
wired to the bus — exactly the bench-test condition below.

---

## UART Configuration

| Parameter | Value |
|-----------|-------|
| Baud Rate | **500 bps** |
| Data Bits | **8** |
| Parity | **Even (8E1)** — see note below |
| Stop Bits | **1** |
| Logic | Normal (not inverted) |

> **Updated, 2 Sep 2026:** this table previously said "Parity: None" (8N1). As of firmware
> 3B.21 the UART runs **8E1** — receiving works identically either way (the parity bit
> doesn't affect the data bits), but transmit needs even parity to be accepted by the bus.
> This was switched on upstream's explicit recommendation; see `PROTOCOL.md` and
> `upstream-comparison.md`.

---

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

## Software Setup

### Prerequisites
- [ESPHome](https://esphome.io/) installed on Windows (via pip)
- [Git](https://git-scm.com/download/win) installed
- Home Assistant with the ESPHome integration

### Installation

#### 1. Clone the Repository
```batch
git clone https://github.com/endofthewurst/Retro-Fujitsu-ESPHome.git
cd Retro-Fujitsu-ESPHome
```

#### 2. Create Secrets File
```batch
copy secrets.yaml.template secrets.yaml
notepad secrets.yaml
```

Edit with your WiFi credentials:
```yaml
wifi_ssid: "YourWiFiName"
wifi_password: "YourWiFiPassword"
```

#### 3. Validate Configuration
```batch
esphome config retrofujitsu.yaml
```

#### 4. Flash to ESP32

**First flash, and any time firmware is being trusted for the first time:** do it with the
board powered but **not yet wired to the LIN bus**. In that state OTA is just as safe as USB
— a failed flash is a USB cable away from recovery, since the board isn't installed yet.
Only wire it into the heat pump once you've confirmed the new firmware boots and reconnects
cleanly.

```batch
esphome run retrofujitsu.yaml --device <esp32-ip-or-hostname>
```

If the board isn't reachable on the network yet (e.g. very first flash ever), fall back to
USB:
```batch
esphome run retrofujitsu.yaml
```

**Later updates**, once the board is installed and on the bus, are OTA-only — don't reflash
over USB while it's wired into the wall unless you have to.

> Native ESPHome OTA (port 3232) fails if `logger: level: VERBOSE` is set. If that's the
> case, use `curl.exe -F "file=@firmware.bin" http://<esp32-ip>/update` instead.

#### 5. Add to Home Assistant
1. Go to **Settings** -> **Devices & Services** -> **ESPHome**
2. Device **"Aircon"** should appear automatically
3. Click **Configure** and add it

---

## LED Status Indicators

The onboard LED (GPIO2) shows the current status:

| Pattern | Meaning |
|---------|---------|
| 3 quick flashes | Boot sequence |
| Single flash every 2s | WiFi connected |
| Slow blink (500ms/1500ms) | WiFi OK, waiting for HA |
| Medium blink (300ms/700ms) | HA disconnected |
| Fast blink (200ms/200ms) | WiFi error |

---

## Home Assistant

Once connected, the **Aircon** device appears in Home Assistant. Entity IDs are prefixed
with the device name (`aircon`), e.g.:

```yaml
type: thermostat
entity: climate.aircon_fujitsu_heat_pump
```

---

## Troubleshooting

### No Frames Received
- Check linttl3 is powered (LED on if equipped)
- Verify common ground between all devices
- Check TX/RX aren't swapped (linttl3 TX -> ESP32 GPIO16)
- Confirm baud rate is 500, 8E1 (not 8N1 — see UART Configuration above)

### Room Temperature / Fan Speed Show Nothing
Both are decoded and should show real values as of firmware 3B.18 (see Protocol above). If
they're genuinely blank, that points to a real problem — check firmware version and bus
health (`binary_sensor.house_aircon_bus_alive`) rather than assuming it's expected.

### Target Temperature Wrong in HEAT or AUTO
Was a known issue under the original byte addressing — resolved 8 Aug 2026 under the
corrected addressing (see Protocol above). If you're still seeing this on current firmware,
it's a regression worth reporting, not expected behaviour.

### Device Not Appearing in Home Assistant
- Check the ESP32 is on WiFi (web interface accessible)
- Restart Home Assistant
- Try adding manually with the IP address

### ESP32 Crash Loop After Flashing
- Check the GPIO16 pull-up is in place if bench-testing unwired
- Check for compilation errors
- Ensure `setup()` doesn't block waiting for UART

---

## Debug Logging

Set `logger: level: DEBUG` (or `VERBOSE` for even more) in `retrofujitsu.yaml` and enable
the component's own debug flag:

```yaml
logger:
  level: DEBUG

fujitsu_climate:
  - id: aircon
    # ... other options ...
    debug: true
```

With debug logging on, each cycle logs the raw UNIT and CTRL frames as hex, plus a
field-by-field breakdown (`B5=...`, `B6=...`) and the decoded state line. Sending a command
additionally logs the frame the firmware built and the checksum calculation before
transmitting it — useful for checking transmit attempts against what's actually observed on
the bus, given transmit is still experimental.

---

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

The transmit path is live and has been exercised successfully for several fields (see
Climate Controls above), but hasn't been through the same volume of testing as decode —
treat it as working-but-young rather than as mature as decode. The wired controller's
Thermo Sensor setting (which physical thermistor the system uses) is understood in general
but its exact protocol bit is still unconfirmed. See the open issues on this repo for
current status.

---

## Credits & References

This project stands on the shoulders of:

- **[Myles Eftos / unreality](https://github.com/unreality/FujiHeatPump)** - Arduino library for Fujitsu heat pumps, protocol reverse engineering
- **[jaroslawprzybylowicz/fuji-iot](https://github.com/jaroslawprzybylowicz/fuji-iot)** - Library for Fuji Electric / Fujitsu AC units, Raspberry Pi implementation
- **[FujiHeatPump/esphome-fujitsu](https://github.com/FujiHeatPump/esphome-fujitsu)** - Original ESPHome custom component
- **[Hackaday: Frederic Germain & Myles Eftos (2017)](https://hackaday.io/project/19473-reverse-engineering-a-fujitsu-air-conditioner-unit)** - Original reverse engineering of the Fujitsu LIN bus protocol

These target different unit families with different frame markers to the ART30LUAK, so
their code isn't a byte-for-byte drop-in — but their field layout, after correcting for
this unit's byte inversion and frame sync offset, turned out to match exactly, and is what
this project's fan-speed and room-temperature decode (and the vendored transmit engine) are
actually built on. See Protocol above.

---

## Contributing

If you have a similar Fujitsu unit and this works (or doesn't work) for you, please open an
issue or pull request with:
- Your indoor/outdoor unit model numbers
- Your wired controller model
- What worked or what needed changing

This will help build a compatibility list for other users!

---

## Licence

Apache 2.0 - see [LICENSE](LICENSE)

---

## Disclaimer

This project involves opening electrical equipment and making connections to control
boards. Do this at your own risk. Always turn off power at the breaker before working on
the unit. This project is not affiliated with or endorsed by Fujitsu.
