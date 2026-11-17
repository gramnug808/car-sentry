# Car Sentry — Laptop Host App (v1.0)

Defensive, own-vehicle only. Linux laptop in the car (DragonOS / Ubuntu).
Python 3 stdlib only — no Gradle, no Android. OpenCV / Tesseract / `iw` /
`rtl-sdr` are optional; everything degrades to a reported "not present"
instead of crashing.

**Host only.** No ESP32 firmware lives here. The ESP32 is a separate
standalone alarm/sensor node in `../esp32/`. The host talks to it only
over the simple serial / MQTT protocol below.

## What it does

| Module | File | Notes |
|---|---|---|
| LPR (USB webcam) | `car_sentry/lpr.py`, `plates.py` | OpenCV or ffmpeg/v4l2 capture; Tesseract OCR if installed, else stub interface. Hawaii ABC123 / 123ABC / custom (KEVIN1, ALOHA) with newtool repair rules: 0/O, 1/I, 5/S, 8/B, 2/Z positional; split tokens joined; reversed 123ABC first-class; letter-slot `8` → `B`, never `C`. |
| WiFi wardriving | `car_sentry/wifi_scan.py` | Laptop built-in WiFi via `iw` (true dBm) or `nmcli` fallback. Passive scan only — no connecting, no deauth. |
| BLE | `car_sentry/ble_scan.py` | `bluetoothctl` listing. Passive only, no pairing/connecting. |
| SDR | `car_sentry/sdr.py` | **Passive receive / spectrum presence only** via `rtl_power`. Never transmits, never jams, never decodes anyone's communications. No SDR → reports not present. |
| GPS | `car_sentry/gps.py` | NMEA `$GPGGA` / `$GPRMC` parser (checksum-checked), USB/serial GPS. Every capture/signal gets GPS + timestamp. |
| Offline map | `car_sentry/mapgen.py` | Self-contained HTML + SVG pins. **No internet tiles, no Leaflet CDN.** Works with data off. |
| Sentry / sleep | `car_sentry/sentry.py` | Idle when armed (screen off, camera off, WiFi 1 sweep/min), wakes on ESP32 WAKE, back to idle after timeout. |
| Logging | `car_sentry/store.py` | Local SQLite + CSV export, alerts list. Red list → immediate alert; white list → logged, never alerted; same plate/signal at ≥3 distinct locations (≥100 m apart) in 60 min → FOLLOWING alert. |
| ESP32 protocol | `car_sentry/esp32_protocol.py` | Host-side parser/builder only. |
| Self-test | `car_sentry/selftest.py` | `python3 -m car_sentry --self-test` |

## Run

```bash
cd ~/workspace/car-sentry/host
python3 -m car_sentry --self-test     # diagnostics (no hardware needed)
python3 -m car_sentry --ui            # Tkinter dashboard on the laptop screen
python3 -m car_sentry --run           # headless: arm, idle, 1 WiFi sweep/min
python3 -m car_sentry --scan-wifi     # one WiFi sweep
python3 -m car_sentry --scan-ble      # one BLE listing
python3 -m car_sentry --sdr           # one passive SDR sweep (needs RTL-SDR)
python3 -m car_sentry --export-map    # data/map.html from stored alerts
python3 -m car_sentry --export-csv out.csv
python3 -m unittest discover -s tests -v   # unit tests
cp config.example.json config.json   # then edit red_list / white_list etc.
```

Optional installs on the laptop: `sudo apt install tesseract-ocr iw
bluez rtl-sdr` and `pip install opencv-python` (camera) — all optional.

## ESP32 protocol (agreed contract)

Serial line (`\n`-terminated) or MQTT payload; JSON form also accepted.

```
ARM / DISARM / STATUS / PING / PONG
STATUS {"armed":true,"battery":82,"motion":false}
ALERT MOTION <detail> | ALERT PERSON <detail> | ALERT TAMPER <detail>
WAKE <reason>            e.g. WAKE MOTION
```

MQTT topics: `car-sentry/esp32/alert` and `car-sentry/esp32/status`
(ESP32 → host), `car-sentry/host/cmd` (host → ESP32: ARM/DISARM/STATUS).
Host implements the parser (`parse_message`, `parse_mqtt`) and the
ARM/DISARM builders; the serial/MQTT transport is configured in
`config.json` (`esp32_serial`, `mqtt_broker`).

## Wake — exactly what is supported

The host cannot wake a laptop in deep suspend (S3/S4) over MQTT —
the radio and CPU are off. Three paths, in reliability order:

1. **IDLE WAKE (default, implemented):** laptop is *not* in S3.
   Screen off, CPU throttled, host process running. ESP32 sends
   WAKE over USB serial or MQTT; host starts camera + radios
   immediately. This always works — use this mode.
2. **Wake-on-LAN:** ESP32 sends a magic packet to the laptop's MAC
   on the same WiFi/LAN. Requires BIOS **Wake on LAN / Wake on
   WLAN** enabled, `sudo ethtool -s <iface> wol g`, laptop on AC
   (inverter) power, not fully powered off. `--self-test` checks
   the `Wake-on:` line; `sentry.send_wol(mac)` sends a test packet.
3. **USB wake:** ESP32 via USB-serial asserts wake. Requires BIOS
   **Wake on USB / USB Wake Support** enabled AND
   `echo enabled > /sys/bus/usb/devices/<dev>/power/wakeup` for
   that port. Behaviour varies by laptop — verify on your hardware
   before relying on it. `--self-test` reports the wakeup state.

Persist WoL: `sudo ethtool -s <iface> wol g` does not survive reboot
on most distros — add a systemd unit or a NetworkManager dispatcher
script that runs it at boot (example in `sentry.py` docstring area
is intentionally minimal: use your distro's documented method).

## Power — laptop on car 12V inverter / USB-C

* Armed + idle: screen off, camera off until WAKE, WiFi 1 sweep/min.
  A few watts — fine for hours on inverter + car battery with the
  engine running, **not** indefinitely with the engine off.
* Battery guard: below 20% on battery the host logs LOW_BATTERY and
  stops scanning; below 10% it stops the sentry so the laptop is
  not drained flat. Never leave it armed overnight on battery alone.
* Use a 12V inverter (or USB-C PD car charger) rated above the
  laptop's charger wattage, and prefer the laptop's own battery
  as the buffer — if the inverter drops, the host keeps running
  on battery and the guard above kicks in.

## Honest limits

* **Not tested live in a car.** No camera, GPS, SDR, or ESP32 was
  attached on this VM. All live paths are code-reviewed + unit-tested
  with fixtures/stubs; the first real test is: plug in a USB webcam
  + USB GPS, run `--self-test`, confirm PASS on camera/GPS, then
  `--run` and drive.
* OCR accuracy depends on Tesseract + camera angle/lighting; the
  repair rules fix common confusions but cannot invent a plate.
* GPS is only as good as the USB GPS fix; indoor / no-sky = no fix.
* WiFi `nmcli` signal is an approximation (quality → dBm); use `iw`
  for true dBm.
* SDR is spectrum-presence only — no demodulation, no decoding.
* WoL / USB wake depend on BIOS + NIC support; verify on your laptop.
