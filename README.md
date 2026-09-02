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

### Confirmed and working
- **State decode** — Power, Mode, and Target Temperature read correctly off the bus and
  published to Home Assistant as a climate entity (see caveats below for HEAT/AUTO).
- **Diagnostic sensors** — IP address, WiFi signal strength, uptime, free heap.
- **Status LED** — Visual feedback on the ESP32 board (GPIO2).
- **Web interface** — ESPHome's built-in web server for local access.
- **Non-invasive installation** — connects as a secondary (slave) controller; the existing
  wired controller keeps working normally, with no modification to the heat pump or the controller.

### Experimental / in progress
- **Control from Home Assistant** — the climate entity accepts commands (power, mode,
  temperature, fan) and the firmware has scaffolding to build and transmit a frame in
  response, but the frame-building logic is **unvalidated and explicitly marked as a guess
  in the source**. Don't rely on it to actually change the heat pump's behaviour yet.
- **Fan speed** — not currently decoded or published.
- **Current (room) temperature** — not currently decoded or published.
- **OTA updates** — works, with one caveat: native ESPHome OTA fails if `logger: level:
  VERBOSE` is set (use the curl route below instead in that case).

### Climate Controls (target state — see status above for what's actually reliable today)
| Control | Options | Status |
|---------|---------|--------|
| Power | On / Off | Decoded reliably; control experimental |
| Mode | Auto, Cool, Heat, Dry, Fan Only | Decoded reliably; control experimental |
| Target Temperature | 16°C – 30°C | Decoded for COOL/DRY; HEAT/AUTO setpoint decode unresolved |
| Fan Speed | Auto, Quiet, Low, Medium, High | Not yet decoded |

### Sensors
| Sensor | Description | Status |
|--------|-------------|--------|
| Target Temperature | Current setpoint | Working (COOL/DRY confirmed; HEAT/AUTO unresolved) |
| Current Temperature | Room temperature | Not yet decoded — see Protocol |
| WiFi Signal | RSSI in dBm | Working |
| IP Address | Device IP on local network | Working |
| Uptime | Time since last reboot | Working |
| Free Heap | ESP32 memory (for diagnostics) | Working |

---

## How It Works

```
Fujitsu Heat Pump (Indoor Unit)
        <->  LIN Bus (500 baud, 8N1)
  linttl3 Module (TJA1021)
        <->  TTL UART (3.3V)
    ESP32-WROOM-32
        <->  WiFi
    Home Assistant
```

The Fujitsu indoor unit communicates with controllers using a **LIN bus** running at
**500 baud, 8N1**. The linttl3 module converts the 12V/24V LIN bus signals to 3.3V TTL
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
| Parity | **None** |
| Stop Bits | **1** |
| Logic | Normal (not inverted) |

> Some published Fujitsu LIN implementations use even parity (8E1) instead. 8N1 is what
> this unit's receive path uses today; 8E1 is being evaluated for the transmit path (see
> Protocol below) and hasn't shipped yet.

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
COOL and DRY. **Unresolved for HEAT/AUTO** — both modes read the same `B5` value regardless
of the setpoint, so either the offset differs in those modes or the field isn't live there.

### Not yet decoded

- **Fan speed.** Every byte thought to carry it is constant across all 1112 captured
  frames — fan speed is not where earlier notes assumed it was. Needs a dedicated capture
  with fan cycled through every position while everything else is held fixed.
- **Current (room) temperature.** Never successfully read; the byte long assumed to carry
  it turns out to carry mode information instead.

A promising but **not yet validated** hypothesis, informed by comparing this project against
other published Fujitsu LIN implementations: inverting every received byte (`^= 0xFF`) and
reading the frame two bytes later than the naive sync point may reveal both fields, since it
would put this unit's traffic in line with those other implementations' field layout. This
has only been checked against old capture logs re-read offline, not against a live unit with
a corrected parser — treat it as a lead, not an answer, until it's been through a hardware
session.

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
- Confirm baud rate is 500 with parity NONE

### Room Temperature / Fan Speed Show Nothing
Expected right now — neither field is decoded yet (see Protocol above). This isn't a wiring
or config problem.

### Target Temperature Wrong in HEAT or AUTO
Known unresolved issue — see Protocol above. COOL and DRY are reliable.

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
- **May-Aug 2026** — Dormant.
- **Aug 2026** — Recovered and consolidated the local working tree; corrected the fan-speed
  and room-temperature decode (both were previously misreported as confirmed); began
  investigating the transmit path.

Fan speed, room temperature, and the HEAT/AUTO setpoint offset are the remaining protocol
gaps; a validated, tested transmit path is the remaining feature gap. See the open issues
on this repo for current status.

---

## Credits & References

This project stands on the shoulders of:

- **[Myles Eftos / unreality](https://github.com/unreality/FujiHeatPump)** - Arduino library for Fujitsu heat pumps, protocol reverse engineering
- **[jaroslawprzybylowicz/fuji-iot](https://github.com/jaroslawprzybylowicz/fuji-iot)** - Library for Fuji Electric / Fujitsu AC units, Raspberry Pi implementation
- **[FujiHeatPump/esphome-fujitsu](https://github.com/FujiHeatPump/esphome-fujitsu)** - Original ESPHome custom component
- **[Hackaday: Frederic Germain & Myles Eftos (2017)](https://hackaday.io/project/19473-reverse-engineering-a-fujitsu-air-conditioner-unit)** - Original reverse engineering of the Fujitsu LIN bus protocol

These target different unit families with different frame markers to the ART30LUAK, so
their code is a reference for method rather than a drop-in byte layout — though their field
layout (after correcting for this unit's byte inversion and frame sync offset) is the
current lead on the still-undecoded fan and room-temperature fields, see Protocol above.

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
