# Session 5A — deployed-board check-in, RoomMind config, Thermo Sensor icon decoded (3–4 October 2026)

> Companion to `state-of-play.md`, same standalone-session pattern as `session-4A8-update.md`.
> Covers: (1) the bus-powered, installed ESP32 as seen from Home Assistant, (2) how RoomMind is
> configured against it, (3) the Thermo Sensor icon bit found by a passive button test,
> (4) firmware 4A.9 (read-only Thermo Sensor icon sensor, raw frames split by type, default-off
> write-test switch) and the negative write-test result, and (5) the to-do list for the two items
> that remain open: **controlling the Thermo Sensor icon** and a **Defrost cycle active flag**.
> Times: UTC unless marked NZDT (UTC+13).

## 1. Board health in HA (bus-powered, installed in the house)

- Online, firmware 4A.8 at the time of the check (compiled 11 Sep 2026 10:37), IP 192.168.1.40,
  WiFi -53 dBm, free heap ~186KB (flat).
- Uptime was ~7.1 days at the first check (booted ~25 Sep 23:05 UTC) and ~8 days at flash time:
  no reboot, no crash, on bus power, through a week of real use including RoomMind-driven
  compressor cycling. This largely answers the 4A.1 brownout / cold-boot worry in practice (not
  as a controlled test).
- `binary_sensor.house_aircon_bus_alive`: no off-blips since 25 Sep. `in_sync` on.
  `switch.house_aircon_aircon_control_enabled` is on (needed for RoomMind).
- Watch item: `climate.aircon_fujitsu_heat_pump` went `unavailable` briefly at 01 Oct 00:46,
  01 Oct 22:18, 02 Oct 19:54, 19:56 and 21:23 (last ~53 s) with no reboot — API/WiFi link drops,
  not crashes. Not root-caused; same class as the 3 Sep observation that the thermo diagnostic
  churn flaps the API link.
- `climate.heating_control` is still gone (disabled 11 Sep). `switch.ac` last changed 25 Sep and is
  no longer a meaningful oracle of anything.
- HA recorder retention is ~10 days: the 11 Sep icon-recorded test data had already aged out.
  Anything worth keeping from a capture must be copied into the repo promptly.

## 2. How RoomMind is configured (websocket `roommind/rooms/list`, `roommind/settings/get`)

- Global: `control_mode: mpc`, `climate_control_active: true`, prediction on, presence on
  (person.kim, person.james, away action off), schedule_off_action eco, vacation off,
  `compressor_groups: []`, outdoor sensor `sensor.outdoor_temperature`, weather `weather.houpara`.
- Six rooms defined (living_room, media_room, bedroom, max_s_room, study, garden; garden hidden,
  no devices). All five real rooms point at the SAME device, `climate.aircon_fujitsu_heat_pump`:
  type `ac`, role `auto`, `setpoint_mode: proportional`, `idle_action: off`, `idle_fan_mode: low`,
  coil-dry inherit / 0 min.
- Per-room `climate_control_enabled` was FALSE in every room at the time (living room switched
  off by a user at 19:24 UTC on 2 Oct), so RoomMind was not driving the unit.
- living_room: `heat_only`, MPC active (the only room with a trained model in use), temperature
  sensor = `sensor.house_aircon_room_temp` (the ESP32's own decode), schedule
  `schedule.schedule_heating_comfort`, comfort 22/24, eco 16/27. Other rooms use their own
  sensors (study_temp, media_temp_2, maxs_temp, lounge_temp for "bedroom").
- Observed behaviour 1–2 Oct (service calls with no user context, consistent with RoomMind): heat
  at setpoint 25 (proportional/boost) alternating with off (setpoint reads 16); every command
  adopted, `in_sync` stayed on. Runs were short — e.g. 2 Oct 07:07–07:13, 07:17–07:21,
  07:23–07:31 UTC — i.e. an inverter compressor short-cycled; no compressor protection configured.

Concerns:
1. `sensor.house_aircon_room_temp` flaps ±1°C every 1–3 s (whole-degree quantisation at a rounding
   boundary) and feeds the living-room MPC model directly. Needs a smoothed copy or a real sensor.
2. The unit reads 21.0 while `sensor.lounge_temp_temperature` reads 19.7. Not explained by the
   Thermo Sensor setting (the room-temp field did not change between icon states in the passive
   test, though only over ~11 s). Cause unknown; possibly just where the wired controller is mounted.
3. Defrost/oil recovery is invisible to HA (`hvac_action` stays `heating`), so RoomMind's learning
   (2,923 observations for living_room) absorbs defrost periods as ordinary heating.

## 3. Thermo Sensor icon — decoded (passive test, 3 Oct ~15:36 NZDT / 02:36 UTC)

James: unit in standby with the Thermo Sensor icon showing; turned the unit on; ~10 s later pressed
Thermo Sensor (icon off); ~10 s later pressed again (icon on); ~10 s later turned the unit off. HA
history of `sensor.house_aircon_raw_frame_unconfirmed` for the window:

| Time (UTC) | Event | Byte 5 of the frame addressed to us |
|---|---|---|
| before 02:36:42 | standby, icon showing | `84` |
| 02:36:42 | unit turned on (byte 3 `38`→`39`) | `84` |
| 02:36:47 | press 1, icon off | `04` |
| 02:36:58.7 | press 2, icon on | `84` |
| 02:37:11 | unit turned off (byte 3 `39`→`38`) | `84` |

**Byte 5 bit 7 (0x80) of the frame addressed to our SECONDARY address tracks the wired controller's
Thermo Sensor icon: set = icon shown (controller's own sensor in use), clear = icon off.** Exactly
two transitions, matching the two presses, right direction; baseline matches the icon at start and
end. Nothing else in that frame changed except power (byte 3 bit 0, plus a byte 2 `08` blip at each
power transition). No crash or reboot.

Frame types (decoded bytes after inversion; this is what `raw_frame` was alternating between and
why the old "thermo oscillation" was an artefact):
- **B / "to us"**: source `0x20` (PRIMARY) → dest `0x21` (SECONDARY, our address), e.g.
  `20 A1 00 39 19 84 2C 00`. This is the frame the ESP32 replies to. Carries power/mode/setpoint,
  and the Thermo Sensor bit in byte 5 bit 7.
- **A / "other"**: source `0x00` → dest `0x20` (PRIMARY), e.g. `00 A0 00 39 19 A4 03 20`. Byte 5
  bit 7 flickers (`A4`→`04`) for ~0.8 s at each power transition only — unrelated to the icon.

In the vendored engine's terms byte 5 bits 7:4 are the `updateMagic` nibble: the frame to us reads
`8` with the icon on and `0` with it off. Our own replies send `updateMagic` 0 or 2, i.e. bit 7
always clear.

Corrects earlier notes: the 3 Sep lead (bit 5, `A4` vs `84`) was just frame type A vs B; the 11 Sep
refutation of `frame[6] bit0` stands; the earlier suggestion that the 21.0 vs 19.7 gap came from the
Thermo Sensor setting is not supported.

## 4. Firmware 4A.9 (4 Oct 2026) and the write test

**What changed** (commit `b5b45bb`, pushed, `HEAD == origin/main`):
- `FujiHeatPump`: keeps `lastToUsFrame` / `lastOtherFrame` separately (split on
  `messageDest == controllerAddress`), `getToUsThermoBit()` (`lastToUsFrame[5] & 0x80`), and an
  experimental `thermoWriteTest` flag that ORs `updateMagic |= 0x08` into STATUS replies.
- `FujitsuClimate` / `binary_sensor.py` / `text_sensor.py` / `retrofujitsu.yaml` (version `4A.9`):
  new `binary_sensor` Thermo Sensor Icon, two text sensors Raw Frame To Us / Raw Frame Other, and a
  `switch` Thermo Sensor Write Test (`restore_mode: ALWAYS_OFF`, config category). All new
  publishes use the same change-gate + 250 ms throttle as the existing diagnostics.
- Old `thermo_sensor_unconfirmed`, `unknown_bit_unconfirmed`, `raw_frame_unconfirmed` and
  `swing_unconfirmed` sensors left in place (still flapping/noisy; retirement is a to-do).
- Deliberately NOT built: the separate "bit-change journal" floated earlier. Because the two raw
  frame types now only change on real changes, their HA history is the journal.

**Build/deploy:** compiled clean (RAM 13.0% / 42,440 B, Flash 54.8% / 1,005,353 B). OTA to
192.168.1.40 succeeded first try (15.18 s upload). Post-flash: fresh boot, Bus OK / Bus Alive /
In Sync on, heap ~186KB, Thermo Sensor Icon read `on` (matching the wired controller's icon), both
raw-frame sensors populated with the expected A/B values. Launch method: WMI `Win32_Process.Create`
(the PowerShell tool call times out at ~60 s, so compile and upload were launched detached and
polled). A stale `.git/HEAD.lock` left by committing through the Linux bridge had to be removed from
Windows before pushing.

**New entity IDs** — HA gave the device an area prefix, so these differ from the older
`house_aircon_` / `aircon_` slugs:
`binary_sensor.living_room_aircon_thermo_sensor_icon`,
`sensor.living_room_aircon_raw_frame_to_us`, `sensor.living_room_aircon_raw_frame_other`,
`switch.living_room_aircon_thermo_sensor_write_test`.

**Write test — negative.** 4 Oct 22:10 UTC (11:10 NZDT): unit on in heat; James pressed Thermo Sensor
so the icon went off (icon sensor `off`, frame byte 5 `04`). Write Test switch turned on and held
~45 s. Result: icon sensor stayed `off`, frame to us stayed `… 04 …`, bus stayed alive, no fault, no
`E:EE`; switch turned off again. **Setting byte 5 bit 7 in the secondary controller's replies has no
effect on the Thermo Sensor setting.** Caveats: one trial, unit on in heat mode only, no
independent log confirmation that the bit actually left the wire (the code path is a one-line
`updateMagic |= 0x08` before `encodeFrame()`), and the physical icon on the wall was not separately
re-confirmed by James after the test. Side note: `in_sync` read off afterwards purely because the unit
had been switched on at the wall (`power: requested=off, actual=on`) — expected behaviour.

## 5. To-do

### A. Thermo Sensor icon — monitor (done) and control (open)

| # | Item | Status |
|---|---|---|
| A1 | Read-only icon sensor from byte 5 bit 7 of the frame to us | **Done, 4A.9** — `binary_sensor.living_room_aircon_thermo_sensor_icon` |
| A2 | Second confirmation cycle: press twice in heat mode and once with the unit in standby, watching the new sensor | Open, cheap, no flash |
| A3 | Retire the refuted/noisy diagnostics (`thermo_sensor_unconfirmed` ~2.4 Hz flap, `unknown_bit_unconfirmed`, mixed `raw_frame_unconfirmed`); rename/tidy the new entities' HA slugs | Open — next flash |
| A4 | Establish what the icon actually changes: does `room_temp` / the unit's own thermostat behaviour change between icon on/off over a longer window; explain the 21.0 vs 19.7 °C gap to the lounge sensor | Open |
| A5 | **Control (set) the icon from HA.** Result so far: writing the bit in our reply does nothing (4A.9 test). Remaining avenues, in order: (a) repeat the write test in other conditions (unit in standby, one-shot vs sustained, other message types) to close out the negative properly; (b) look for any other field/frame in the secondary's vocabulary that moves the setting (full-frame capture, section B); (c) accept read-only and add an HA-side advisory (notify when the icon state differs from what an automation expects). Emulating the wired controller's own frame is the only route left and is NOT recommended (violates the "ESP32 is never the boss" principle). | Open — decision needed from James between (a)/(b) effort and (c) |
| A6 | Decide whether to keep the default-off Write Test switch in the shipped firmware or remove it once A5 is settled | Open |

### B. Defrost / oil-recovery cycle active flag (open)

The wired controller's Defrost icon is fed by bus traffic, so the information must be in the frames
we already receive; the job is to find the bit.

| # | Item | Status |
|---|---|---|
| B1 | Capture: both frame types are now published separately (4A.9) and the recorder keeps ~10 days. Arm a watch so the capture isn't missed: defrost needs a cold, humid morning (outdoor sensor read 16°C on 3 Oct — too mild). Candidate: an HA automation that notifies when outdoor temp is low and the unit is heating, prompting James to note the time the icon appears; export the frames from history within the retention window. | Open |
| B2 | Identify the bit(s): diff `raw_frame_to_us` / `raw_frame_other` against baseline at the moment the icon shows; expect the unit→us frame. Use the unit's Stand-by (hot-start) icon as a more triggerable stand-in to locate the status-byte region. Check whether Defrost and Oil Recovery share one bit (same icon) or two. | Open |
| B3 | Firmware: `binary_sensor.<…>_defrost_active` (read-only, change-gated + throttled like the other diagnostics). Note: ESPHome 2025.4.2's climate actions have no DEFROSTING value, so `hvac_action: defrosting` likely needs a template/HA-side wrapper rather than the climate entity itself — verify before promising. | Open |
| B4 | Validate live on a real cold morning with James watching the icon: transitions on/off timestamped and matched to the flag. | Open |
| B5 | Use the flag: pause/discount defrost periods in RoomMind (learning and control) — check what hooks RoomMind actually offers; otherwise an HA automation. Also stop short-cycling the compressor around defrost. | Open |

### C. Other open items carried forward
- RoomMind side: smoothed temperature feed for the living-room model; compressor short-cycle protection
  (`compressor_groups` / minimum run time); decide when to re-enable living-room control.
- Setpoint (`temperature`) attribute blank in Auto and Fan-only modes (from 4A.8).
- Quiet fan: display/actual-speed split (from 4A.8).
- Swing off-transition still untestable (no swing button on this controller).
- API/WiFi link drops (section 1) — root cause unknown.
- Repo hygiene: many untracked `*.log` / `tx_*` files in the working tree; consider `.gitignore`.
