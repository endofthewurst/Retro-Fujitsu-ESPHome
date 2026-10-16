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
## Addendum — Thermo Sensor Write Test now turns the icon OFF (17 Oct 2026, ~19:46-19:47 UTC)

First positive write result for A5. With 4A.10 running and the wired remote's Thermo Sensor icon
showing, James turned on `switch.living_room_aircon_thermo_sensor_write_test` (sets byte 5 bit 7
in our STATUS replies). HA history (icon sensor, frame to us, switch):

| Switch on (UTC) | Icon at the time | Result |
|---|---|---|
| 19:46:49.8 | on | icon off 19:46:51.1 (+1.35 s); frame byte 5 `84` -> `04` |
| 19:47:02.0 | off | no change |
| 19:47:21.3 | on | icon off 19:47:22.7 (+1.39 s) |
| 19:47:34.0 | on | icon off 19:47:35.2 (+1.16 s) |

James confirmed he restored the icon each time with the wired remote's Thermo Sensor button
(19:47:16, :27, :38). The unit never turned it back on by itself.

- **One direction only:** HA can switch the icon off but cannot switch it on (same as the 4 Oct
  negative result, which was run with the icon already off).
- **Behaves like a one-shot, not a held override:** the icon stayed on after a remote press at
  19:47:27.7 while the switch was still on, so a level-sensitive "bit set = force off" is
  refuted. The ~1.2-1.4 s latency suggests one bus exchange.
- Not a toggle (a toggle would have turned the icon on in the icon-off cases).
- Meaning of the setting still per 3 Oct: icon shown = wired controller's own sensor in use. What
  the setting actually changes (A4) is still open.
- Open for A5: how to turn the icon ON from the secondary. Candidate experiments (one variable
  at a time, switch default-off): single-frame pulse instead of a held bit; the other bits of the
  same nibble (updateMagic 0x01/0x02/0x04) in our reply; other message types. Alternative is to
  accept "off only" and add an HA-side advisory.

## Addendum 2 — Firmware 4A.11: trying to turn the icon ON (17 Oct 2026, ~20:03-20:06 UTC)

4A.11 adds the "Thermo Sensor Experiment" select (default Off, never restored): replaces/ORs
values into the updateMagic nibble (byte 5 bits 7:4) of our STATUS replies. Our normal reply
nibble is `0x2`; the unit's own frame-to-us carries `0x8` with the icon on, `0x0` off. Flashed by
OTA (compile stamp 08:56:51 NZDT). All runs started with the wired remote's icon OFF and the unit
heating; results read from the icon sensor + `raw_frame_to_us` at ~0.4 s resolution:

| Option | Nibble sent | Result |
|---|---|---|
| Pulse x1 (OR 0x8) | `0xA`, 1 reply (select reset after ~1 s) | icon stayed off |
| Pulse x5 (OR 0x8) | `0xA`, 5 replies (reset after ~4 s) | icon stayed off |
| Held: nibble = 0x8 | `0x8`, 12 s | icon stayed off |
| Held: nibble = 0x0 | `0x0`, 12 s | icon stayed off |
| Held: OR 0x4 | `0x6`, 12 s | icon stayed off |
| Held: OR 0x1 | `0x3`, 12 s | icon stayed off |

Bus status, Bus Alive, In Sync and the climate state were unchanged throughout; no fault on the
wired controller. STATUS replies go out about once a second.

**Conclusion for A5:** writing the updateMagic nibble in our STATUS replies can only CLEAR the
Thermo Sensor icon (0x8 bit set while the icon is showing, ~1.3 s, one-shot); none of the six
variants above sets it. Turning it on from HA is not possible through this field. Remaining
avenues are other message types or frame fields (only reachable by emulating the wired
controller, which is NOT recommended -- "never the boss"), so the working plan is: keep the
off-only control (Thermo Sensor Write Test switch) and, if wanted, an HA-side advisory when the
icon state differs from what an automation expects.

Open: A6 -- keep the Write Test switch (useful: remote "own sensor off"); the Experiment select
is no longer needed and can be removed in the next flash.

## Addendum 3 — Experiments repeated with the icon ON (James, 17 Oct 2026, ~11:12 NZDT)

James repeated the six 4A.11 experiments with the icon set ON via the wired remote's Thermo
Sensor button (hand-run from the HA select; not re-timed from HA history).

| Option | Nibble sent | Icon ON -> |
|---|---|---|
| Pulse x1 (OR 0x8) | `0xA`, 1 reply | **turned OFF**, did not return |
| Pulse x5 (OR 0x8) | `0xA`, 5 replies | **turned OFF**, did not return |
| Held: nibble = 0x8 | `0x8` | **turned OFF**, did not return |
| Held: nibble = 0x0 | `0x0` | no change (confirmed by James) |
| Held: OR 0x4 | `0x6` | no change (confirmed by James) |
| Held: OR 0x1 | `0x3` | no change (confirmed by James) |

- Only the options whose reply nibble contains `0x8` (byte 5 bit 7) clear the icon. Bit 7 alone
  is the trigger: nibble `0x8` works without our usual `0x2`, and a single reply (Pulse x1) is
  enough, so it is not a held override.
- Combined with the icon-OFF runs earlier (Addendum 2: all six variants left the icon off), the
  field is one-way: bit 7 in our STATUS reply clears the Thermo Sensor icon; nothing in the
  nibble sets it. The unit never restores the icon itself; only the wired remote button does.
- A6 stands: keep the Write Test switch; the Experiment select can be removed next flash.
