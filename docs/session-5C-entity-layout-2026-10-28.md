# Session 5C — Entity layout, Swing Icon / Thermo Sensor Icon confirmed, release candidate (27–28 October 2026)

> Companion to `session-5A-checkin-2026-10-03.md` and `session-5B-defrost-2026-10-17.md`.
> Covers: the Home Assistant device-page layout, James's live tests of the Thermo Sensor Icon
> and Swing icon entities, and firmware **4A.12b-rc1** (release candidate). Times NZDT.

## 1. Home Assistant device-page layout

Goal: the **Aircon** device page grouped as **Controls / Sensors / Diagnostic**, with nothing left
in Home Assistant's "Configuration" group.

HA/ESPHome has no custom groups. The device page is built from each entity's `entity_category`:

| `entity_category` | Device-page group |
|---|---|
| none (primary), writable | **Controls** |
| none (primary), read-only | **Sensors** |
| `diagnostic` | **Diagnostic** |
| `config` | Configuration (now unused) |

So the layout is achieved by never using `config`. Changes (flashed by OTA on 27 Oct 2026, build
stamp 21:15; layout then checked on the HA page):

| Entity | Before | After |
|---|---|---|
| Fan Speed, Economy Mode, Aircon Control Enabled | `config` | none — Controls |
| Room Temp | `diagnostic` (component schema default) | none (`entity_category: ""` in YAML) — Sensors |
| Set Temperature | `config` | `diagnostic` |
| Thermo Sensor Experiment, Thermo Sensor Write Test | `config` | `diagnostic` |
| Restart | `config` (ESPHome's default for the restart switch) | `diagnostic` |

Notes:
- **Set Temperature is deliberately kept** even though it duplicates the climate entity's target
  temperature (the YAML comment notes HA hides the climate target in Auto mode). It sits in
  Diagnostic so it does not clutter Controls.
- Entities inside each group are listed alphabetically by name; HA offers no way to reorder them
  short of renaming entities, which was judged not worth it.
- ESPHome's sensor schema accepts `""` (not `none`) for "primary".
- **Local web page: unchanged, flat list.** ESPHome's `sorting_groups` only work on `web_server`
  **version 3**. This project is on version 2 on purpose (commit `1ede70a`: the v3 UI only rendered
  entities that republished after page load). Decision, 28 Oct 2026: stay on v2. Revisit if v3's
  rendering issue is fixed.

## 2. Live-test findings (James, 28 Oct 2026)

**Thermo Sensor Icon** (`binary_sensor`, byte 5 bit 7 of the frame addressed to us): HA shows the
wired remote's icon **correctly at all times**. Confirmed. It remains read-only; **two-way control
of this icon is a future task** (see 5B: writing bit 7 in our STATUS reply can only clear the icon,
nothing tried so far sets it).

**Swing icon** (`text_sensor`, byte 5 bit 2 via the vendored engine's `getSwingMode()`): while the
unit is running, HA matches the wired remote's swing icon. **Known quirk:** when the unit is
off/standby the remote does not display the icon but HA still shows **On**. Treat the entity as
meaningful only while the unit is running (e.g. gate any automation on the climate state not being
off). This is a note, not a fix; the cause has not been investigated.

## 3. Firmware 4A.12b-rc1

- **"Swing (unconfirmed)" renamed "Swing Icon"**; published states are now plain `On` / `Off`
  (previously `On (unconfirmed)` / `Off (unconfirmed)`); icon changed from the "?" placeholder.
- **Swing Icon and Thermo Sensor Icon moved to Sensors** (no longer `diagnostic`; set in the
  component schemas `text_sensor.py` / `binary_sensor.py`).
- Version string `4A.12a` -> `4A.12b-rc1`. No protocol, TX or timing changes.
- Entity layout changes from section 1 (already live in 4A.12a).

**Consequences of the rename (do by hand in HA after flashing):**
- ESPHome derives the entity ID from the name, so HA will create a **new** entity (`..._swing_icon`);
  the old `Swing (unconfirmed)` entity becomes orphaned (delete it in HA; its history does not carry over).
- The local web page URL for it changes to `/text_sensor/swing_icon`.
- Anything matching the old `On (unconfirmed)` / `Off (unconfirmed)` strings needs updating. None is
  known in this repo; check RoomMind/automations.

**Build status:** compiles cleanly (ESPHome 2025.4.2, `esphome config` and `esphome compile`
both pass). On-board verification of the rename and regrouping is pending the flash of this build.

## 4. Considered, not adopted

Proposed in the layout review but not requested: moving **Sync Mismatch** and **Out of Sync Since**
to Diagnostic (they are blank while healthy), and turning **Restart** into a button instead of a
switch. Both are small follow-ups.

## 5. Open items carried into the release candidate (none are blockers)

| Item | Status |
|---|---|
| A2 second confirmation cycle for the Thermo Sensor icon | Effectively covered by the 28 Oct "correct at all times" test |
| A4 what the Thermo Sensor setting actually changes | Open |
| A5 two-way control of the Thermo Sensor icon | Future task (off-only today) |
| A6 keep/remove the Write Test switch; remove the Thermo Sensor Experiment select | Open (5B: select no longer needed) |
| B4 second cold-morning Defrost cycle; Oil Recovery sharing the bit | Open |
| B5 use the Defrost flag in RoomMind | Open |
| Swing Icon reads On while the unit is off/standby | Noted, uninvestigated |
| Intermittent API/WiFi link drops | Not root-caused |