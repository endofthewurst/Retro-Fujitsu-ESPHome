# Session 5B — Defrost / Oil Recovery flag found (17 October 2026)

> Companion to `session-5A-checkin-2026-10-03.md`. Covers to-do item **B** (Defrost active
> flag) from 5A, and firmware 4A.10. Times: UTC unless marked NZDT (UTC+13).

## Live capture (17 Oct 2026, cold morning, outdoor 6.5 °C, unit heating, setpoint 25)

James watched the wired remote and reported the Defrost/Oil Recovery icon and the outdoor
fan stopping. HA history of the two raw-frame sensors (`sensor.living_room_aircon_raw_frame_*`):

| Time (UTC) | Frame | Value | Event |
|---|---|---|---|
| 19:22:22.7 | other | `00 A0 08 49 19 A4 03 20` | byte 2 `00` -> `08`; defrost icon on (James: "just started") |
| 19:28:53.4 | other | `00 A0 00 49 19 A4 03 20` | byte 2 back to `00`; icon off (James reported ~40 s later) |

- **Byte 2 bit 3 (0x08) of the frame NOT addressed to us (unit -> primary, "other" frame)
  tracked the icon for the whole cycle: 6 min 31 s.** One cycle only -- strongly indicated, not
  yet repeated. Defrost and oil recovery share the wired controller's icon, so this can't tell
  them apart.
- Nothing else changed on the bus during the cycle. The to-us frame was untouched; `hvac_action`
  stayed `heating`; the fan field kept reading `high` (commanded setting) while the fan was
  physically stopped, so no fan information is available as a defrost cue.
- The outdoor fan restarted roughly a minute or more AFTER the bit cleared (James's report,
  timing approximate). A post-defrost warm-up/hot-start period is therefore not covered by
  this bit. No frame change was visible when the fan restarted. Suggest a ~2 min hold-off in
  RoomMind before trusting heat output/trends again (to be tuned after another cycle).
- Unrelated event in the same window: byte 3 high nibble `3` -> `4` at 19:13:57 in both frames,
  coinciding with HA `fan_mode` medium -> high. Consistent with Fujitsu fan encoding
  (3 = medium, 4 = high); inferred, not separately tested.
- The 3 Oct session noted a byte 2 `08` blip at each power transition, so the sensor debounces.

## Firmware 4A.10

- NEW `binary_sensor` **Defrost Active** (`defrost_active`): `lastOtherFrame[2] & 0x08`,
  state flips only after the raw bit is stable for 3 s; forced off while the bus is not alive
  (ESPHome 2025.4.2 has no `invalidate_state()`); change-gated publish; INFO log on transitions.
- RETIRED text sensors `thermo_sensor`, `unknown_bit`, `raw_frame` (the "(unconfirmed)" ones;
  noisy/refuted, superseded by Thermo Sensor Icon and the split raw-frame sensors). **Swing
  (unconfirmed) kept** at James's request. `FujiHeatPump` decode left alone (TX uses it).
- Not in this flash: icon control (A5/A6 -- Write Test switch kept, default off), RoomMind use
  of the flag (B5), a `hvac_action: defrosting` template, setpoint-in-Auto/Fan-only, quiet fan.
- Hypothesis to check after the flash: removing the ~2.4 Hz thermo diagnostic churn may reduce
  the brief `climate.aircon_fujitsu_heat_pump` `unavailable` blips (last seen 2 Oct).
- Orphaned HA entities from the retired sensors need deleting by hand in HA.

## To-do after this session

| # | Item | Status |
|---|---|---|
| A3 | Retire noisy diagnostics | Done in 4A.10 (Swing intentionally kept) |
| B1/B2 | Capture + identify the bit | Done (one cycle) |
| B3 | `defrost_active` sensor | Done in 4A.10 (pending flash verification) |
| B4 | Validate on a second cold cycle with the sensor timestamping it; note whether Oil Recovery also sets it | Open |
| B5 | Use the flag in RoomMind / HA (exclude defrost + ~2 min hold-off from learning and control) | Open |