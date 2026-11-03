# Hardware notes

> Moved out of the README on 3 Nov 2026 when it was restructured. Some passages carry dated update notes from earlier in the project. Where this page and the code disagree, the code wins.

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
