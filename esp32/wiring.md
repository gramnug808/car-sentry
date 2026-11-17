# Car Sentry — Wiring

Standalone alarm/sensor node for your own vehicle. Read `README.md`
first for behaviour, wake methods, and limits.

## Full wiring diagram

```
                         CAR 12 V BATTERY
                        (+) ── fuse 2 A ──────────────┐
                        (-) ── chassis ground ────────┤
                                                       │
                              12 V -> 5 V BUCK CONVERTER (2 A min)
                              IN+ ── car 12 V (fused)     OUT+ 5 V ──► ESP32 VIN
                              IN- ── ground                OUT- GND ──► ESP32 GND
                                                       │
              ┌────────────────────────────────────────┴──────────────────┐
              │ ESP32 DEV BOARD (ESP32-WROOM-32)                          │
              │                                                           │
              │  VIN (5V) ◄── buck 5 V        GND ◄── common ground       │
              │                                                           │
              │  GPIO16 (RX2) ◄── LD2410 TX                               │
              │  GPIO17 (TX2) ──► LD2410 RX (OT)                          │
              │  GPIO33 (RTC) ◄── LD2410 OUT  (optional, deep-sleep wake) │
              │  GPIO34 (ADC) ◄── 12 V sense divider (below)              │
              │                                                           │
              │  GPIO26 ──► buzzer/siren DRIVER (MOSFET/relay, see below) │
              │  GPIO27 ──► strobe / relay DRIVER                         │
              │  GPIO25 ──► wake pulse out (500 ms HIGH on alarm)         │
              │             (external wake adapter only — see README)     │
              │  GPIO32 ──► disarm push-button ──► GND (INPUT_PULLUP)     │
              └───────────────────────────────────────────────────────────┘

  HLK-LD2410
  ┌──────────┐
  │ VCC (5V) │──► ESP32 VIN (5 V)     *** NEVER 3V3 — the LD2410 needs 5 V ***
  │ GND      │──► ESP32 GND
  │ TX       │──► ESP32 GPIO16 (RX2)
  │ RX (OT)  │──► ESP32 GPIO17 (TX2)
  │ OUT      │──► ESP32 GPIO33 (optional: HIGH on presence, deep-sleep wake)
  └──────────┘
  UART2, 256000 baud, 8N1. LD2450 wiring is identical (same pins).

  BUZZER / SIREN (do NOT drive directly from a GPIO pin)
  ┌─────────────────────────────────────────────────────────┐
  │ GPIO26 ── 1 kΩ ──► N-MOSFET gate (e.g. IRLZ44N / 2N7002) │
  │                   MOSFET drain ──► siren (-)             │
  │                   siren (+) ──► 12 V (fused) or 5 V     │
  │                   MOSFET source ──► GND                  │
  │                   Flyback diode across the siren if it  │
  │                   is inductive (relay/siren coil).       │
  │ A small active buzzer (5 V, <100 mA) can use the same    │
  │ MOSFET circuit; a piezo buzzer can go direct with a      │
  │ series resistor, but the MOSFET circuit works for both.  │
  └─────────────────────────────────────────────────────────┘
  Strobe / relay on GPIO27: same MOSFET/relay circuit as above.

  DISARM BUTTON
  GPIO32 ── push-button ── GND. Firmware uses INPUT_PULLUP (active LOW).
  Short press disarms from GRACE / ALARMING / LATCHED; 2 s hold disarms
  from ARMED. Mount it where the owner can reach it but a person outside
  the car cannot (inside the cabin, not on the bumper).

  CAR 12 V SENSE (GPIO34, ADC1 — input-only pin, no pull-up/down)
  car 12 V (+) ── 100 kΩ ──●── 27 kΩ ── GND
                            │
                            └──► ESP32 GPIO34
  15.0 V in -> ~3.19 V at the pin (divider ratio 4.7037, set in config.h
  as VIN_DIVIDER_RATIO). Keep the divider resistors close to the ESP32,
  fuse the 12 V feed, and never connect car 12 V directly to a GPIO pin.
```

## 12 V power — how to feed it in the car

- **Buck converter:** 12 V → 5 V, 2 A or better, automotive-rated if
  possible (car rails spike well above 14 V on crank/load-dump; a cheap
  converter can pass spikes to the ESP32). Fuse the 12 V input at 2 A,
  as close to the tap point as possible.
- **Which feed:** for a sentry that must watch while parked, tap an
  **always-on** feed (not ignition-switched) — otherwise it is dead when
  the car is off, which is exactly when you want it. The trade-off is
  continuous draw (see README power table: tens of mA even in deep
  sleep, dominated by the radar). For multi-day parking, use the
  battery cutoff and consider a switched feed or disconnecting the node.
- **Ground:** common ground between buck output, ESP32, radar, siren
  driver, and the car chassis. A floating ground is the most common
  cause of "works on the bench, flaky in the car".
- **Laptop power:** the laptop itself runs from the car 12 V via an
  inverter / USB-C car charger. That is separate from the ESP32's buck
  converter — do not power the ESP32 from the laptop's USB port and
  expect it to keep watching while the laptop sleeps (USB ports are
  often powered down in suspend, and the ESP32 would die with it).

## Low-voltage cutoff — how it works

- The divider above feeds GPIO34; the firmware reads it every 10 s
  (ADC, 11 dB attenuation, approximate — expect ±0.3 V error; it is a
  battery guard, not a voltmeter).
- **Cutoff 11.5 V:** if the reading stays below 11.5 V for 60 s, the
  siren stops, the node deep-sleeps, and it only wakes on the 10 min
  timer to re-check voltage. **Recover 12.0 V:** above 12.0 V it
  re-enables normally.
- A healthy car battery at rest is ~12.6 V; 11.5 V is already deeply
  discharged. The cutoff protects you from a dead battery the next
  morning — it is not a substitute for watching total draw on a car
  parked for days.
- **Calibration:** if your divider resistors differ from 100 k / 27 k,
  update `VIN_DIVIDER_RATIO` in `include/config.h` to
  `(Rtop + Rbottom) / Rbottom`, and update the cutoff voltages if your
  battery chemistry needs it. Verify with a multimeter on first install:
  compare the reported `vbat_v` in `/api/status` against the meter.

## Pin summary

| ESP32 pin | Connects to | Notes |
|---|---|---|
| VIN (5 V) | Buck converter 5 V out; LD2410 VCC | LD2410 needs 5 V, NOT 3V3 |
| GND | Common ground | Buck, radar, siren driver, button |
| GPIO16 (RX2) | LD2410 TX | UART2 RX, 256000 baud |
| GPIO17 (TX2) | LD2410 RX (OT) | UART2 TX |
| GPIO33 | LD2410 OUT (optional) | RTC pin; deep-sleep wake only |
| GPIO34 | 12 V sense divider | ADC1, input-only |
| GPIO26 | Siren/buzzer driver (MOSFET/relay) | HIGH = sounding |
| GPIO27 | Strobe / relay driver | HIGH = on |
| GPIO25 | Wake pulse out | 500 ms HIGH on alarm; external adapter only |
| GPIO32 | Disarm button to GND | INPUT_PULLUP, active LOW |
| EN / BOOT | (flashing only) | Standard ESP32 USB flash wiring |
