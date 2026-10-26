# Control Authority Attribution

**Status:** firmware 4A.12a (26 Oct 2026) - attribution implemented via the two text sensors; live validation pending.
**The `Control Change` event entity described below is NOT in this build:** the build that included it crash-looped the board (reboot every ~60-80 s into safe mode) and was reverted; the same code without the event ran stable. Treat the event row as planned, pending a serial-captured diagnosis.

## Purpose

HA (RoomMind or any automation) needs to know when a human has changed the unit at the wired
controller, so it can defer rather than fight them. This is the "ESP32 is never the boss"
principle made operational.

- **ESP32:** reports *who* made each state change and *what* changed. Firmware never reacts.
- **HA:** owns the deference timer and all policy.

How it relates to the sync entities (4A.6): `in_sync` says "reality differs from the last HA
request". Attribution says "a human (or HA) just changed X". They share the `requested_*`
snapshot; attribution adds which fields HA set.

## Entities

| Entity | Purpose |
|---|---|
| `event` "Control Change" | Fires once per attributed change; types `human` and `ha`. Use this as the automation trigger: a repeated human change (temp-up pressed twice) fires twice, which a text sensor that stays "Human" would not. |
| `text_sensor` "Last Change Source" | `HA` / `Human` / `Boot`. Published just before each event fires. |
| `text_sensor` "Last Change Detail" | e.g. `setpoint 22 -> 23`, `power off -> on`, `mode HEAT -> COOL`, `fan LOW -> HIGH`. |

Not `entity_category: diagnostic`: HA automations consume these directly. There is deliberately
no ESP-side "override active" flag, because HA cannot clear an ESP entity. Use an HA timer
helper or `input_boolean`.

Example HA trigger: `event` entity `event.living_room_aircon_control_change`, `event_type: human`.

## Mechanism (`update_attribution_()` in `FujitsuClimate.cpp`)

1. `control()` records WHICH fields the HA call set (`attr_pending_mask_`: power, mode, setpoint,
   fan) next to the existing complete `requested_*` snapshot.
2. Each decoded frame is diffed against the last state, **field by field**:
   - A pending field reaching its requested value within the adoption window -> `ha` (bit cleared).
   - Any other change -> `human` (that field's pending bit cleared).
3. The first frame after boot only baselines (`Boot`); it is never `Human`.

Judging per field means a multi-field HA command adopted over several frames is credited to HA
field by field, and a change to a field HA never touched is human by construction.

### Constants (tune from live data)

| Constant | Value | Meaning |
|---|---|---|
| `ATTR_ADOPTION_WINDOW_MS` | 8000 | how long after an HA command a commanded field may still be credited to HA |
| `ATTR_POWER_QUIET_MS` | 3000 | after a power change, other-field changes are re-baselined silently |
| `ATTR_MODE_QUIET_MS` | 3000 | after a mode change, setpoint/fan changes are re-baselined silently |

### Why the quiet windows

- While the unit is off, mode/setpoint/fan readings are unreliable (setpoint has been seen
  reading 16 when off), so they are tracked silently.
- A power or mode change makes the unit republish other fields (per-mode setpoint memory).
  Those side effects are not user actions.
- Setpoint 0 / FAN mode carries no setpoint and is never tracked.

## Known limitations

- **Unit-initiated changes** (defrost, thermostat-off, a wired-remote timer) cannot be told apart
  from a human and report as `human`. Possibly acceptable for deference purposes.
- **A human change to a setpoint or fan inside a quiet window** (3 s after a mode change) is not
  reported. A human mode or power change is never suppressed except in the 3 s after a power change.
- **HA command that changes nothing** leaves its pending bit set until the window expires. A
  human change to a different value in that window still reports `human`.
- `Aircon Control Enabled` off: `control()` returns early, nothing is marked pending, every change
  reads `human`.
- `writeBit` (CTRL B3 = 0x7E) was considered as a sharper timestamp. 3B.35 showed it does not appear
  in frames to us, so it is not used.

## Validation protocol (live, with someone at the wall)

Watch `Last Change Source` / `Last Change Detail` and the `Control Change` event history.

1. HA: set the setpoint +1 -> `HA`, `setpoint a -> b`.
2. HA: change mode and setpoint together -> one `HA` event per field.
3. Wall: press temp-up once -> `Human`, `setpoint a -> b`. Press again -> a second `human` event.
4. Wall: change fan speed -> `Human`, `fan X -> Y`.
5. Wall: power off, then on -> `Human` each time, no spurious mode/setpoint/fan events afterwards.
6. HA: change something, then within ~1 s change something else at the wall -> HA then Human.
7. Reboot the board -> `Boot`, no `Human` event.

Tune the three constants from the observed adoption timing.
