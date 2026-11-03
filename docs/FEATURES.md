# Features and Home Assistant entities

> Moved out of the README on 3 Nov 2026 when it was restructured. Some passages carry dated update notes from earlier in the project. Where this page and the code disagree, the code wins.

## Features

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
  behind an "Aircon Control Enabled" switch that is **on by default** (restored on every boot);
  turn it off at any time from HA or the web UI to drop back to read-only monitoring. See
  `state-of-play.md`'s "4A" sessions for the live-confirmation history of each field.
- **Diagnostic sensors** — IP address, WiFi signal strength, uptime, free heap, bus
  status/alive, status LED pattern.
- **Thermo Sensor Icon** — read-only binary sensor for the wired controller's "own sensor in
  use" icon (byte 5 bit 7 of the frame addressed to us). Live-confirmed 28 Oct 2026: HA matches
  the wired display at all times. Two-way control is a future task.
- **Swing Icon** — read-only On/Off for the swing icon (byte 5 bit 2). Matches the wired
  remote's icon while the unit is running; with the unit off/standby the remote hides the icon
  but the entity still reads On (known quirk, tested 28 Oct 2026).
- **Defrost Active** — binary sensor from byte 2 bit 3 of the frame not addressed to us
  (fw 4A.10); identified on one live defrost cycle, a second is still to do.
- **Sync and attribution** — In Sync / Sync Mismatch / Out of Sync Since (requested vs actual
  state) and Last Change Source / Detail (who changed what: HA, Human or Boot).
- **Tidy Home Assistant page** — entities grouped as Controls / Sensors / Diagnostic with
  nothing in "Configuration" (see [Home Assistant](#home-assistant)).
- **Status LED** — Visual feedback on the ESP32 board (GPIO2); read-only diagnostic, not
  a controllable entity.
- **Web interface** — ESPHome's built-in web server for local access.
- **Non-invasive installation** — connects as a secondary (slave) controller; the existing
  wired controller keeps working normally, with no modification to the heat pump or the controller.

### Experimental / in progress
- **Two-way control of the Thermo Sensor icon** — future task. The ESP32 can only clear the
  icon, never set it (`docs/session-5B-defrost-2026-10-17.md`; `PROTOCOL.md` Open Question 3).
- **Defrost / oil recovery detection** — Defrost Active exists but rests on one live cycle;
  whether Oil Recovery sets the same bit is untested. See the Roadmap below.
- **OTA updates** — works, with one caveat: native ESPHome OTA fails if `logger: level:
  VERBOSE` is set (use the curl route below instead in that case).

### Climate Controls (see status above for what's actually reliable today)
| Control | Options | Status |
|---------|---------|--------|
| Power | On / Off | Decoded and control both live-confirmed |
| Mode | Auto, Cool, Heat, Dry, Fan Only | Decoded and control both live-confirmed |
| Target Temperature | 16°C – 30°C | Decoded and control both live-confirmed, all modes including HEAT/AUTO |
| Fan Speed | Auto, Low, Medium, High (Quiet optional, off by default) | Decoded and live-confirmed; exposed via a dedicated select entity |
| Economy | On / Off | Decoded and control both live-confirmed, both as a climate preset and a standalone switch |

> **Quiet fan speed is off by default.** On the tested ART30LUAK with its AR-3TA3 / UTB-TUB wired controller it does not work: the
> indoor unit briefly obeys it, then the wired controller (which has no Quiet state) overwrites
> it with High a second later, so the display shows High while the fan runs at Low. Quiet also
> cannot be selected from the wired remote. If your unit supports Quiet, set
> `quiet_fan_mode: true` on the `climate:` block in `retrofujitsu.yaml` and add `- "Quiet"` to
> the Fan Speed select.

### Sensors
| Sensor | Description | Status |
|--------|-------------|--------|
| Target Temperature | Current setpoint | Working, all modes including HEAT/AUTO |
| Current Temperature | Room temperature (wired controller's own thermistor) | Working — see Protocol |
| Fan Speed | Auto/Low/Medium/High (Quiet optional) | Working |
| Economy | On/Off | Working, decoded and controllable |
| Thermo Sensor Icon | Wired controller is using its own sensor | Working, live-confirmed 28 Oct 2026; read-only (two-way control is a future task) |
| Swing Icon | Wired remote's swing icon (On/Off) | Working while the unit is running; reads On when off/standby (known quirk) |
| Defrost Active | Defrost / oil-recovery icon | Working (fw 4A.10), from one live cycle |
| In Sync / Sync Mismatch / Out of Sync Since | Requested vs actual unit state | Working |
| Last Change Source / Detail | Who last changed the state (HA / Human / Boot) and what changed | Working (fw 4A.12a) |
| Raw Frame To Us / Other | Last raw frame addressed to us / to others (diagnostic) | Working (fw 4A.9) |
| WiFi Signal | RSSI in dBm | Working |
| IP Address | Device IP on local network | Working |
| Uptime | Time since last reboot | Working |
| Free Heap | ESP32 memory (for diagnostics) | Working |

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

### Device page layout

The **Aircon** device page is grouped as **Controls / Sensors / Diagnostic**. Home Assistant
builds these groups from each entity's `entity_category` (custom groups are not possible), and
the firmware never uses `config`, so there is no "Configuration" group.

| Group | Entities |
|---|---|
| Controls | Fujitsu Heat Pump (climate), Fan Speed, Economy Mode, Aircon Control Enabled |
| Sensors | Room Temp, Thermo Sensor Icon, Swing Icon, Defrost Active, In Sync, Sync Mismatch, Out of Sync Since, Last Change Source, Last Change Detail |
| Diagnostic | Bus Alive, Bus Status, Raw Frame To Us / Other, Setpoint, Set Temperature, Thermo Sensor Experiment, Thermo Sensor Write Test, Restart, Safe Mode Boot, Status, Status LED Pattern, ESPHome Version, GitHub Repository, Uptime, Free Heap Memory, WiFi Signal / SSID, IP / MAC Address |

The local ESPHome web page stays a flat list: ESPHome's `sorting_groups` need `web_server`
version 3, and this project uses version 2 (the v3 UI only rendered entities that republished
after the page loaded).

---
