# Car Sentry — laptop + ESP32 mmWave car alarm / wardriving system

Defensive, own-vehicle only. Two clean, separate pieces that talk only over a simple protocol. No transmitting, jamming, spoofing, or intercepting other people's communications. LPR is for plates seen in public from your own vehicle, same limits as newtool.

## The two pieces — separate, on purpose

| Piece | Where | What it does | What it does NOT do |
|---|---|---|---|
| **Host app (laptop)** | `host/` | LPR from USB webcam, WiFi/BLE wardriving, SDR passive spectrum logging, GPS, offline map, SQLite/CSV log, sentry/idle + battery guard, Tkinter UI + self-test | No ESP32 firmware, no alarm hardware control beyond ARM/DISARM messages |
| **ESP32 firmware (alarm node)** | `esp32/` | mmWave person detection, car-alarm mode (buzzer/siren, grace period, disarm), sleep, wake the laptop | No wardriving, no SDR, no LPR, no signal-intel code at all |

## How they connect

```
ESP32 + LD2410 (mmWave) ── serial / MQTT ──> Laptop (DragonOS/Ubuntu)
  detects person near car        ARM/DISARM/ALERT/WAKE/STATUS
  sounds siren, wakes laptop ────────────────────────────────>
  Laptop wakes, starts webcam + WiFi/BLE/SDR, logs plate + signals + GPS
```

Protocol (both sides implement the same contract, see `host/car_sentry/esp32_protocol.py` and `esp32/src/protocol.h`):
- Text + JSON lines over serial, MQTT topics `car-sentry/esp32/alert|status` / `car-sentry/host/cmd`
- Messages: `ARM`, `DISARM`, `ALERT`, `WAKE`, `STATUS`, `PING`
- A WAKE/ALERT from the ESP32 tells an awake-or-idle host to start camera + radios immediately.

## Hardware assumed

- **Host: a laptop** running Linux (DragonOS / Ubuntu) — built-in WiFi for wardriving, laptop screen for UI, USB webcam, USB GPS and/or SDR if attached.
- **ESP32** (DevKit) + HLK-LD2410 (default) or LD2450 (option). Wiring: LD2410 VCC → ESP32 VIN (5V, NOT 3V3), GND → GND, TX → GPIO16, RX → GPIO17, UART 256000. Full wiring, siren driver, and battery sense in `esp32/wiring.md`.
- **Alarm output:** buzzer/siren via MOSFET driver, strobe output, disarm button, GPIO25 wake pulse. Default disarm code `1234` — change it on first setup (see `esp32/README.md`).
- **Power:** laptop on car 12V inverter / USB-C. ESP32 on 12V → 5V buck, with battery sense on GPIO34 and low-voltage cutoff 11.5V for 60s → deep sleep, recover at 12.0V. Never leave armed overnight on battery alone, engine off.

## Laptop sleep / wake — exactly what is implemented

1. **Idle wake (default, recommended):** laptop is NOT in S3 suspend — screen off, CPU throttled, host running. ESP32 WAKE over USB serial/MQTT starts camera + radios immediately. Always works.
2. **Wake-on-LAN (ESP32 sends the magic packet):** ESP32 sends UDP broadcast port 9 to the laptop MAC on trigger. Requires wired Ethernet (USB-Ethernet adapter — most WiFi adapters don't support WoL), BIOS WoL enabled, `ethtool wol g` persisted, suspend (S3, not hibernate/shutdown), same subnet, laptop on inverter power. Test with `wakeonlan` before trusting it.
3. **MQTT WAKE message:** notification only — it cannot wake a laptop in deep S3/S4, radio and CPU are off.
4. **GPIO wake pulse (GPIO25, 500ms):** for an external wake adapter only; does not wake a laptop directly.
5. **NOT implemented:** USB serial wake (can't wake a host) and USB HID keyboard wake (classic ESP32 has no native USB; ESP32-S3 is a future option, not claimed).

Host battery guard: stop scanning <20%, stop sentry <10%.

## What was built and verified

### Host app — `host/` — 33/33 unit tests pass
Re-verified by the coordinator: `cd host && python3 -m unittest discover -s tests` → 33 tests, OK.
- `car_sentry/plates.py` — Hawaii repair: ABC123/123ABC first-class, custom, positional repair, letter-slot `8` → `B` never `C`
- `car_sentry/lpr.py` — webcam (OpenCV → ffmpeg/v4l2 → stub), Tesseract if installed else stub
- `car_sentry/wifi_scan.py` — `iw` (true dBm) / `nmcli` parsers, passive only
- `car_sentry/ble_scan.py` — `bluetoothctl` parser, passive, no pairing
- `car_sentry/sdr.py` — passive `rtl_power` spectrum-presence only
- `car_sentry/gps.py` — NMEA GGA/RMC, checksum-checked, haversine
- `car_sentry/mapgen.py` — self-contained offline HTML/SVG map, no internet tiles
- `car_sentry/store.py` — SQLite + CSV; red list → immediate alert, white list → logged never alerted, same plate/signal at ≥3 locations ≥100m apart in 60 min → FOLLOWING alert
- `car_sentry/sentry.py`, `selftest.py`, `ui.py` (Tkinter), `esp32_protocol.py`
- Run: `python3 -m car_sentry --self-test` / `--ui` / `--run` / `--scan-wifi` / `--export-map` / `--export-csv`

### ESP32 firmware — `esp32/` — 64/64 tests pass, both builds compile
Re-verified by the coordinator: `./test/run_tests.sh` → 64 passed, 0 failed.
- LD2410 build: `firmware.bin`, 900,624 bytes, sha256 `fc8e3ac1dd96a02609742ebb4ce79333cc6c3eb92412df8eeac918034bdf1614`
- LD2450 build: `firmware-ld2450.bin`, 900,560 bytes, sha256 `41496d3afe42633aec3989f3a8bd8cf49927cce1be09848e9b58c143fc5e8fda`
- Copy at `~/workspace/your_files/car-sentry-firmware.bin` (LD2410 build)
- Alarm: range default 300cm, grace 30s, siren/strobe 500ms alternating, 30s/burst, 3-cycle cap then LATCHED; disarm code + button; light-sleep scan cycles, opt-in deep sleep woken by LD2410 OUT on GPIO33; serial JSON + MQTT + HTTP config/API + OTA with magic-byte check
- Tests cover: grace period, 1000ms confirm, out-of-range rejection, wrong-code disarm, retrigger cap → LATCHED, button disarm, deep vs light sleep, battery-cutoff override, WoL packet shape, LD2410 parser resync

## Bench test — do this before installing in the car

1. **ESP32, bench:** flash over USB (`pio run -t upload`), monitor 115200 baud, expect `[radar] present=… dist=… cm`. Join `CarSentry-XXXX` AP, set WiFi + laptop MAC + MQTT host. Test ARM → walk up → siren + WAKE. Test WoL from suspend before trusting it.
2. **Laptop, bench:** plug in USB webcam + USB GPS, run `python3 -m car_sentry --self-test`, confirm camera/GPS PASS, then `--run` and drive. On this VM the self-test is 1/11 PASS — expected, no camera/GPS/SDR/ESP32 here.
3. **Together, bench:** arm the ESP32, walk up to the sensor, confirm the laptop wakes/starts camera + radios and logs the event with GPS + time. Confirm disarm + grace period so you don't trigger it yourself.
4. **Power, bench:** check the 12V buck + cutoff with a meter before leaving it in the car; confirm the laptop battery guard stops scanning <20%.

## Honest limits

- **Not tested on real hardware or in a car.** No ESP32, radar, buzzer, webcam, GPS, SDR, or laptop was attached here. Firmware is compile-checked + logic-tested; host live paths are fixture-tested only.
- Detection is heuristic, not proof — for plates, towers, and person detection alike.
- Current-draw figures are estimates, not measurements; battery cutoff is ADC-approximate (±0.3V).
- WoL/USB wake depend on the exact laptop BIOS + NIC — verify on that laptop.
- Map tiles cover Honolulu only in the bundled set; outside that, the map draws a grid + pins.
