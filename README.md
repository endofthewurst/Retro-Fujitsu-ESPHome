# Retro-Fujitsu-ESPHome

Control and monitor a **Fujitsu heat pump** from **Home Assistant** using an ESP32 and ESPHome without modifying or replacing the existing wired controller.

This project was developed and tested for a **~2010 era Fujitsu RSG series** heat pump using the LIN bus interface, connecting as a secondary (slave) controller alongside the existing wired remote.

---

## Hardware

### Heat Pump Units (Tested)
| Component | Model |
|-----------|-------|
| Indoor Unit | Fujitsu **ART30LUAK** |
| Outdoor Unit | Fujitsu **AOT30LMBDL** |
| Wired Controller | Fujitsu **UTY-RNNUM** |

### Electronics Required
| Component | Notes |
|-----------|-------|
| **ESP32-WROOM-32** | Any ESP32 dev board should work |
| **linttl3 LIN Module** | TJA1021/SIT1021T chip, handles 12V/24V LIN bus to 3.3V TTL conversion |

---

## Features

### Home Assistant Integration
- **Climate entity** - Full control via standard HA climate card
- **Diagnostic sensors** - IP address, WiFi signal strength, uptime, free heap
- **Status LED** - Visual feedback on ESP32 board (GPIO2)
- **Web interface** - ESPHome built-in web server for local access
- **OTA updates** - Reflash wirelessly from ESPHome on Windows

### Climate Controls
| Control | Options |
|---------|---------|
| Power | On / Off |
| Mode | Auto, Cool, Heat, Dry, Fan Only |
| Target Temperature | 16°C - 30°C |
| Fan Speed | Auto, Quiet, Low, Medium, High |

### Sensors
| Sensor | Description |
|--------|-------------|
| Current Temperature | Room temperature from host Fujitsu wired controller sensor |
| Target Temperature | Current setpoint |
| WiFi Signal | RSSI in dBm |
| IP Address | Device IP on local network |
| Uptime | Time since last reboot |
| Free Heap | ESP32 memory (for diagnostics) |

### Important: Non-Invasive Installation
This project connects as a **secondary (slave) controller** on the LIN bus. Your existing **UTY-RNNUM** wired controller continues to work normally. No modifications to the heat pump or existing controller are required.

---

## How It Works

```
Fujitsu Heat Pump (Indoor Unit)
        ↕  LIN Bus (500 baud, 8N1)
  linttl3 Module (TJA1021)
        ↕  TTL UART (3.3V)
    ESP32-WROOM-32
        ↕  WiFi
    Home Assistant
```

The Fujitsu indoor unit communicates with controllers using a **LIN bus** running at **500 baud, 8N1**. The linttl3 module converts the 12V LIN bus signals to 3.3V TTL levels suitable for the ESP32's UART.

The ESP32 listens to frames on the bus and can also inject control frames as a secondary controller, allowing Home Assistant to control the heat pump while the existing wired controller continues to operate normally.

---

## Wiring

### ⚠️ Safety Warning
**Turn off the heat pump at the breaker before opening the indoor unit or making any connections.**

### Connection Overview

```
┌─────────────────────────────────────────────────────┐
│          Fujitsu Indoor Unit (ART30LUAK)            │
│                                                     │
│  Remote Controller Connector (CN-REM or similar)   │
│  ┌─────┬──────────────────────────────────┐         │
│  │ Pin │ Function                          │         │
│  ├─────┼──────────────────────────────────┤         │
│  │  1  │ 12V or 24V Power                 │─────┐   │
│  │  2  │ LIN Bus (Data)                   │──┐  │   │
│  │  3  │ Ground                           │─┐│  │   │
│  └─────┴──────────────────────────────────┘ ││  │   │
└─────────────────────────────────────────────╪╪══╪═══┘
                                              ││  │
                              ┌───────────────╪╪──╪──────────────┐
                              │  linttl3 Module (TJA1021)        │
                              │                                   │
                              │  [VIN]  ←─────────────────────── │ 12/24V
                              │  [LIN]  ←──────────────────────  │ LIN Data
                              │  [GND]  ←────────────────────    │ Ground
                              │                                   │
                              │  [TX]   ──────────────────────►  │ GPIO16 (RX2)
                              │  [RX]   ◄──────────────────────  │ GPIO17 (TX2)
                              │  [GND]  ──────────────────────►  │ GND
                              │                                   │
                              │  [SLP]  (leave unconnected)      │
                              │  [INH]  (leave unconnected)      │
                              └───────────────────────────────────┘
                                              │
                              ┌───────────────┴──────────────────┐
                              │         ESP32-WROOM-32           │
                              │                                   │
                              │  GPIO16 (RX2) ← linttl3 TX      │
                              │  GPIO17 (TX2) → linttl3 RX      │
                              │  GND          ← linttl3 GND     │
                              │  GPIO2        → Status LED       │
                              │  USB          → Laptop/Power     │
                              └──────────────────────────────────┘
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
All three devices (heat pump, linttl3, ESP32) **must share a common ground**. Without this, communication will not work.

---

## UART Configuration

After extensive testing, the correct UART settings for the ART30LUAK are:

| Parameter | Value |
|-----------|-------|
| Baud Rate | **500 bps** |
| Data Bits | **8** |
| Parity | **None** |
| Stop Bits | **1** |
| Logic | Normal (not inverted) |

> **Note:** Many older Fujitsu units use 500 baud 8N1. Some sources mention even/odd parity or different baud rates - these did not work for this unit. If you have a different model, you may need to experiment.

---

## Protocol

The Fujitsu LIN bus protocol uses **8-byte frames**:

| Byte | Content | Notes |
|------|---------|-------|
| 0 | `0xFE` | Sync/Start marker |
| 1 | Source address | Controller that sent the frame |
| 2 | Destination address | Target controller |
| 3 | Power, Mode, Fan, Error | Packed bits (see below) |
| 4 | Temperature, Economy | Target temp in bits 0-6 |
| 5 | Update magic, Swing | Control flags |
| 6 | Controller temp | Room temp in bits 1-6 |
| 7 | `0xEB` | End marker (constant) |

### Byte 3 Bit Layout
```
Bit 7: Error flag
Bits 4-6: Fan mode (0=Auto, 1=Quiet, 2=Low, 3=Medium, 4=High)
Bits 1-3: Mode (1=Fan, 2=Dry, 3=Cool, 4=Heat, 5=Auto)
Bit 0: Power (0=Off, 1=On)
```

### Byte 4 Bit Layout
```
Bit 7: Economy mode
Bits 0-6: Target temperature encoded as (°C - 16), valid range 0-14 → 16-30°C
          Value 0x7F (all bits set) is a sentinel meaning "no setpoint"
```

### Byte 6 Bit Layout
```
Bits 1-6: Controller/room temperature (°C, right shift by 1)
Bit 0: Controller present flag
```

> Protocol reverse engineered with reference to:
> - [Hackaday: Reverse Engineering a Fujitsu Air Conditioner (2017)](https://hackaday.io/project/19473-reverse-engineering-a-fujitsu-air-conditioner-unit)
> - [unreality/FujiHeatPump](https://github.com/unreality/FujiHeatPump)
> - [jaroslawprzybylowicz/fuji-iot](https://github.com/jaroslawprzybylowicz/fuji-iot)

---

## Software Setup

### Prerequisites
- [ESPHome](https://esphome.io/) installed on Windows (via pip)
- [Git](https://git-scm.com/download/win) installed
- Home Assistant with ESPHome integration

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
**First flash (USB):**
```batch
esphome run retrofujitsu.yaml
```

**Subsequent updates (OTA):**
```batch
esphome run retrofujitsu.yaml --device 192.168.x.x
```

#### 5. Add to Home Assistant
1. Go to **Settings** → **Devices & Services** → **ESPHome**
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

Once connected, the **Aircon** device appears in Home Assistant with:

### Climate Card
```yaml
type: thermostat
entity: climate.fujitsu_heat_pump
```



## Troubleshooting

### No Frames Received
- Check linttl3 is powered (LED on if equipped)
- Verify common ground between all devices
- Check TX/RX not swapped (linttl3 TX → ESP32 GPIO16)
- Confirm baud rate is 500 with parity NONE

### Temperatures Wrong
- Should read 16-30°C for target temp
- Should read actual room temperature for current temp
- If values are wildly wrong, byte parsing may need adjustment for your model

### Device Not Appearing in Home Assistant
- Check ESP32 is on WiFi (web interface accessible)
- Restart Home Assistant
- Try adding manually with IP address

### ESP32 Crash Loop After Flashing
- Flash Phase 1 baseline to recover
- Check for compilation errors
- Ensure `setup()` doesn't block waiting for UART

---

## Debug Logging

### Enabling Verbose Frame Logs

The Fujitsu component emits raw frame data and bit-field breakdowns at the `DEBUG` log level, gated behind the `debug` flag on the component.  To enable:

```yaml
# retrofujitsu.yaml
logger:
  level: DEBUG   # or VERBOSE for even more detail

fujitsu_climate:
  - id: aircon
    # ... other options ...
    debug: true
```

With `debug: true` every valid received frame produces three log lines:

```
[D][fujitsu.heatpump]: Raw frame: FE 21 10 09 06 00 33 EB
[D][fujitsu.heatpump]:   Byte[3]=0x09  power=1 mode=4 fan=0 err=0
[D][fujitsu.heatpump]:   Byte[4]=0x06  temp_raw=6 economy=0
[D][fujitsu.heatpump]:   Byte[6]=0x33  ctrl_temp_raw=25 ctrl_present=1
```

### Interpreting Raw Frames

| Field | Calculation | Example above |
|-------|-------------|---------------|
| Target temp | `(byte4 & 0x7F) + 16` °C | `6 + 16 = 22°C` |
| Room temp | `(byte6 & 0x7E) >> 1` °C | `(0x33 & 0x7E) >> 1 = 25°C` |
| Mode | `(byte3 >> 1) & 0x07` | `4 = Heat` |
| Fan | `(byte3 >> 4) & 0x07` | `0 = Auto` |

A `byte4` value of `0x7F` (all seven bits set) is a sentinel meaning the heat pump has not yet reported a valid setpoint; the component will keep the previous target temperature and log a warning.

---

## Project History & Phases

This project was built incrementally:

1. **Phase 1** - Baseline ESP32 with ESPHome (no Fujitsu code)
2. **Phase 2** - Added diagnostics, LED patterns, UART test tools
3. **Phase 3A** - Integrated Fujitsu component (software only, no hardware)
4. **Phase 3B** - Hardware connection and protocol decoding
5. **Phase 4** *(upcoming)* - Full bidirectional control
6. **Phase 5** *(upcoming)* - Polish and optimisation

---

## Credits & References

This project stands on the shoulders of:

- **[Myles Eftos / unreality](https://github.com/unreality/FujiHeatPump)** - Arduino library for Fujitsu heat pumps, protocol reverse engineering
- **[jaroslawprzybylowicz/fuji-iot](https://github.com/jaroslawprzybylowicz/fuji-iot)** - Library for Fuji Electric / Fujitsu AC units, Raspberry Pi implementation
- **[FujiHeatPump/esphome-fujitsu](https://github.com/FujiHeatPump/esphome-fujitsu)** - Original ESPHome custom component
- **[Hackaday: Frederic Germain & Myles Eftos (2017)](https://hackaday.io/project/19473-reverse-engineering-a-fujitsu-air-conditioner-unit)** - Original reverse engineering of the Fujitsu LIN bus protocol

---

## Contributing

If you have a similar Fujitsu unit and this works (or doesn't work) for you, please open an issue or pull request with:
- Your indoor/outdoor unit model numbers
- Your wired controller model
- What worked or what needed changing

This will help build a compatibility list for other users!

---

## Licence

Apache 2.0 - see [LICENSE](LICENSE)

---

## Disclaimer

This project involves opening electrical equipment and making connections to control boards. Do this at your own risk. Always turn off power at the breaker before working on the unit. This project is not affiliated with or endorsed by Fujitsu.
