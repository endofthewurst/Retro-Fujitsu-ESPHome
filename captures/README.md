# Capture Logs — 29 April 2026

Raw ESPHome logs from the live-bus capture session against the ART30LUAK, with the user
pressing buttons on the UTY-RNNUM wired controller. These are the empirical basis for
`PROTOCOL.md`. **Do not delete** — re-deriving them means another session at the heat pump.

All captured 29 Apr 2026 between 13:21 and 13:47 NZST.

| File | Firmware | Time | Size | Content |
|---|---|---|---|---|
| `3B9_capture.log` | 3B.9 | 13:19–13:23 | 23 KB | First frames off the bus |
| `3B10_capture.log` | 3B.10 | 13:15–13:18 | 146 KB | General bus traffic |
| `3B10_buttons.log` | 3B.10 | 13:21–13:22 | 139 KB | **Temperature button presses.** Contains the 27C->28C step and all four `CTRL[3]=0x7E` change-flag occurrences |
| `3B11_verify.log` | 3B.11 | 13:32 | 42 KB | Decode verification, COOL 20C |
| `3B11_modes.log` | 3B.11 | 13:39–13:42 | 407 KB | **Mode cycling.** DRY -> FAN -> HEAT transitions |
| `3B11_auto.log` | 3B.11 | 13:46 | 30 KB | AUTO mode |

Each has a matching `.log.err` in the original folder (stderr from the ESPHome CLI, not
protocol data — not committed).

## Aggregate analysis (7 Aug 2026)

1112 valid frames parsed across all six files. Only four byte positions ever vary:

- UNIT B5 / CTRL B5 — 7 distinct values
- UNIT B6 / CTRL B6 — 4 distinct values
- CTRL B0 — 3 distinct values (`CC`, `CD`, `CE`)
- CTRL B3 — `5F` (535x), `7E` (4x)

Everything else is constant. Zero sync/resync errors. This analysis is what invalidated the
fan-speed and room-temperature decodes — see the ❌ sections in `PROTOCOL.md`.

## Log line format

```
[13:39:39][D][fujitsu.heatpump:067]: UNIT  frame: FE DF DF 7F FF FA E9 6B
[13:39:39][D][fujitsu.heatpump:071]: CTRL  frame: CE FF FF 5F FF FA E9 4B
[13:39:39][I][fujitsu.heatpump:152]: State: pwr=ON mode=0 temp=29C room=nanC fan=3
[13:39:39][I][fujitsu.heatpump:179]: CTRL decoded: pwr=ON fan=3 (CTRL0=0xCE)
```

Note `fan=3` in the State and CTRL lines is an artefact of constant bits, not a measurement.
