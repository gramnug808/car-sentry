# Car Sentry — ESP32 Firmware

Standalone alarm / presence-sensor node for **your own vehicle only**.
Defensive only: mmWave person detection near the car, car-alarm mode
(siren + strobe), sleep/wake, and a small ARM / DISARM / ALERT / WAKE /
STATUS protocol to the host over serial + MQTT + HTTP.

**Scope — read this first.** This firmware is deliberately separate from
any signal work. It contains **no wardriving, no SDR, no LPR, no signal
intelligence, no jamming, no spoofing, no intercepting, no attack tools**.
It is a sensor + alarm node that talks to the host only via the simple
protocol below. It lives in its own directory (`~/workspace/car-sentry/esp32/`)
with its own README and tests, and no signal code is merged into it.

The host is a **laptop** (Linux — DragonOS / Ubuntu), powered in the car
from 12 V via an inverter / USB-C. The ESP32 wakes that laptop as
described in [Waking the laptop](#waking-the-laptop) below.

## Files

| Path | What it is |
|---|---|
| `platformio.ini` | PlatformIO project: `esp32-ld2410` (default), `esp32-ld2450` |
| `src/main.cpp` | Firmware: radar, alarm, wake, sleep, serial/MQTT/HTTP, OTA |
| `src/alarm_logic.h` | Alarm + sleep state machines — pure logic, host-tested |
| `src/protocol.h` | Protocol parse/build + WoL packet — pure logic, host-tested |
| `src/ld2410.h` / `src/ld2450.h` | Radar parsers (HLK protocol; reused from the mmWave detector) |
| `include/config.h` | Pins, defaults, WiFi/MQTT settings |
| `test/test_logic.cpp` + `test/run_tests.sh` | Host unit tests (g++, no ESP32 needed) |
| `firmware.bin` | Built LD2410 firmware (compile only — see limits) |
| `firmware-ld2450.bin` | Built LD2450 firmware |
| `wiring.md` | Wiring diagram, 12 V power, low-voltage cutoff |
| `TEST-REPORT.md` | Test + build report |

## Hardware

- ESP32 dev board (ESP32-WROOM-32, classic ESP32 — no native USB)
- HLK-LD2410 24 GHz mmWave radar (default) or LD2450 (compile-time option)
- Active buzzer / siren + strobe or relay, driven via MOSFET/relay
  (do **not** drive a siren directly from a GPIO pin)
- Momentary push-button (disarm) between GPIO32 and GND
- 12 V → 5 V buck converter, 2 A or better (see `wiring.md`)

Wiring (full diagram in `wiring.md`):

```
LD2410 VCC (5V)  -> ESP32 VIN (5V)        NEVER 3V3 — the LD2410 needs 5 V
LD2410 GND       -> ESP32 GND
LD2410 TX        -> ESP32 GPIO16 (RX2)
LD2410 RX        -> ESP32 GPIO17 (TX2)    UART2, 256000 baud, 8N1
LD2410 OUT       -> ESP32 GPIO33 (optional, deep-sleep wake only)
Buzzer/siren driver -> GPIO26   Strobe/relay -> GPIO27
Wake pulse out      -> GPIO25   Disarm button -> GPIO32 to GND
Car 12 V sense divider -> GPIO34 (see wiring.md)
```

## Behaviour

### Detection

- Near-vehicle range, default **300 cm (3.0 m)**, configurable 50–800 cm.
  A person must be inside that range to trigger. LD2410 max gate defaults
  to gate 4 (= 3.0 m, gates are 0.75 m each), motion/static sensitivity
  default 50/100 each.
- Presence must persist for a confirmation time (default **1000 ms**)
  before it counts — a single passing frame does not set the alarm off.
- While **DISARMED**, presence is only reported in STATUS; no siren, no wake.

### Alarm mode

```
DISARMED --ARM--> GRACE (30 s) --grace ends--> ARMED
ARMED --confirmed in-range person--> ALARMING (siren 30 s + strobe + WAKE)
ALARMING --burst ends--> ARMED (retrigger allowed)
ALARMING x3 bursts --> LATCHED (quiet, still armed, needs manual disarm)
any state --DISARM (code / button)--> DISARMED
```

- **Grace period (default 30 s after arming):** detections are ignored
  so you can walk away from the car without triggering it yourself.
  Arming twice does **not** reset the grace period.
- **Siren pattern:** alternating 500 ms on/off for the alarm duration
  (default 30 s per burst), plus the strobe output in the same pattern.
- **Retrigger cap:** after 3 siren bursts (configurable 1–10) the node
  goes **LATCHED** — quiet, still armed — instead of sirening all night
  and draining the car battery. Disarm + re-arm to reset.
- **Disarm:** code over serial / MQTT / HTTP (`{"cmd":"DISARM","code":"1234"}`),
  or the physical button: a press disarms from GRACE / ALARMING / LATCHED;
  from ARMED it needs a 2 s hold (so a bump doesn't disarm it). A wrong
  code never stops a sounding siren.
- **Default disarm code is `1234`. Change it on first setup** — over serial:
  `{"cmd":"CONFIG","disarm_code":"YOURCODE"}` — and don't leave the default.

### Sleep and power

- **Light sleep (default):** when ARMED and idle, the ESP32 light-sleeps
  in 2 s cycles and wakes to scan for 500 ms. RAM and WiFi state are kept.
- **Deep sleep (opt-in, `deep_sleep:true`):** only when the LD2410 OUT pin
  is wired to GPIO33. The ESP32 deep-sleeps and the radar's OUT pin
  (HIGH on presence) wakes it via EXT0; armed state survives in RTC
  memory and it goes straight back to ARMED (no grace). Timer wake every
  10 min re-checks battery voltage. If OUT is **not** wired, deep sleep
  falls back to light-sleep cycles — it will not silently stop detecting.
- **Never sleeps** while ALARMING (siren must sound) or in GRACE.
- **Battery cutoff:** car voltage is sensed on GPIO34 via a divider.
  Below **11.5 V for 60 s** the siren stops and the node deep-sleeps to
  protect the car battery; it re-enables above 12.0 V. See `wiring.md`.

Approximate current draw (estimates, **not measured** on hardware):

| Mode | ESP32 | Radar (LD2410, powered) | Total, approx |
|---|---|---|---|
| Awake, WiFi on, scanning | 80–160 mA | ~30–70 mA | ~110–230 mA |
| Light sleep between scans | ~10 mA avg | ~30–70 mA | ~40–80 mA avg |
| Deep sleep (ESP32) | ~10 µA | ~30–70 mA | ~30–70 mA |

The radar dominates in every sleep mode: it must stay powered to detect
anything (or to raise its OUT pin). Deep sleep saves the ESP32's draw,
not the radar's. If the car sits for days, expect tens of mA continuous —
use the battery cutoff, and for multi-day parking power the node from a
switched (ignition) feed or disconnect it. Do not rely on deep sleep
alone to protect a car battery.

## Waking the laptop

The host is a laptop, not a Pi, and a sleeping laptop cannot run code —
so "wake" has to happen at the hardware/network level. This firmware
implements **exactly these three wake methods**, and nothing else:

### 1. Wake-on-LAN magic packet — PRIMARY, implemented

On every alarm trigger (and on `WAKE_TEST`) the ESP32 sends a WoL magic
packet (6× `0xFF` + laptop MAC ×16) as a UDP broadcast on port 9 over
the WiFi network it is connected to.

- **Set it up:** set the laptop's MAC once, over serial / HTTP / MQTT
  config: `laptop_mac` = e.g. `AA:BB:CC:DD:EE:FF` (the MAC of the
  interface that will receive the packet — see below). Without a
  configured MAC, WoL is skipped and the log says so.
- **What you must enable on the laptop (Linux / Ubuntu / DragonOS):**
  1. **Use wired Ethernet if you can.** Most WiFi adapters do **not**
     support Wake-on-LAN; WoL over WiFi ("Wake on Wireless LAN") is
     rarely implemented. The reliable path is a wired NIC — on a laptop
     that usually means a **USB-Ethernet adapter** (or a dock). Use
     *that* adapter's MAC, keep it plugged in, and put the ESP32 on the
     same LAN/subnet (same WiFi network + router, or the same switch).
  2. **BIOS/UEFI:** enable Wake-on-LAN — usually under Power /
     "Wake on LAN", "Power On by PCI-E / PCI", or "Wake on LAN from S4/S5".
     Exact name varies by vendor; if there is no such option, WoL from
     full shutdown will not work (suspend may still work).
  3. **OS:** enable magic-packet wake on the interface and make it stick:
     ```
     sudo ethtool -s eth0 wol g          # replace eth0 with your interface
     ethtool eth0 | grep -i wake-on      # must show "Wake-on: g"
     ```
     Persist it (example, NetworkManager):
     ```
     sudo nmcli connection modify "Wired connection 1" 802-3-ethernet.wake-on-lan magic
     ```
     or a systemd link file / udev rule. Without this, the setting is
     lost on reboot.
  4. **Suspend, don't hibernate or power off.** WoL reliably wakes from
     suspend-to-RAM (`systemctl suspend`, S3). Waking from hibernate
     (S4) or full shutdown (S5) needs BIOS support and often fails on
     laptops. Test from suspend first.
  5. **Keep standby power:** the laptop must stay on charger / inverter
     power while suspended — WoL needs the NIC powered. A laptop on
     battery with the lid closed may cut NIC power; check your
     power settings.
  6. **Test it before trusting it:** from another machine on the same
     LAN, suspend the laptop, then `wakeonlan AA:BB:CC:DD:EE:FF`
     (package `wakeonlan`). If that doesn't wake it, the ESP32's packet
     won't either — fix BIOS/OS first.
- **Limit, stated plainly:** WoL is best-effort. If the laptop is on
  WiFi only, or the BIOS/OS doesn't support it, or the ESP32 and laptop
  are on different subnets, the packet will not wake it. The alarm still
  sounds locally and the MQTT/serial ALERT still goes out — the siren
  does not depend on the laptop waking.

### 2. MQTT WAKE message — implemented, notification only

On trigger the node publishes `{"type":"WAKE","reason":"person_detected",
"method":"wol+mqtt+gpio"}` to `carsentry/<id>/wake`, and a WAKE line on
serial. **This cannot wake a sleeping laptop by itself** — a sleeping
machine runs no MQTT client and no broker. Its job is to tell an
**already-awake** host (or a broker on the LAN) to start the camera and
radios. If the laptop is asleep, only WoL (or the GPIO path) can wake it;
the MQTT message is what the host acts on *after* it wakes.

### 3. GPIO wake pulse — implemented, needs external hardware

On trigger, GPIO25 goes HIGH for 500 ms. A laptop has no GPIO input, so
this pin **does not wake a laptop directly**. It is for an optional
external wake adapter you add yourself, e.g. a small USB-HID dongle or a
relay wired across the laptop's power button. If nothing is connected to
GPIO25, the pulse is harmless and does nothing.

### NOT implemented — USB wake

- **Plain USB serial cannot wake a laptop.** The ESP32's USB-serial
  connection is a device, not a host wake source, and this classic-ESP32
  board has no native USB. Sending serial data to a sleeping laptop
  does nothing.
- **USB HID keyboard wake is NOT implemented here.** An ESP32-S3 board
  (native USB) acting as a USB keyboard could send a keypress to wake a
  laptop from suspend (if the laptop's BIOS/OS allows USB wake). That is
  a different board and a different firmware build — noted as a future
  option, not claimed as working in this build.

## Protocol

One JSON object per line over serial (115200 baud, `\n` terminated).
MQTT uses the same objects; HTTP exposes the same operations.

**Host → ESP32**

| Message | Effect |
|---|---|
| `{"cmd":"ARM"}` | Arm; grace period starts |
| `{"cmd":"DISARM","code":"1234"}` | Disarm (code required; wrong code rejected) |
| `{"cmd":"STATUS"}` | Request a STATUS reply |
| `{"cmd":"CONFIG","max_range_cm":250,"alarm_duration_s":30,"grace_s":30,"motion_sensitivity":50,"static_sensitivity":50,"deep_sleep":false,"disarm_code":"NEW"}` | Set config (any subset; saved to NVS) |
| `{"cmd":"WAKE_TEST"}` | Fire the wake outputs once (WoL + MQTT + GPIO) |
| `{"cmd":"SLEEP"}` | Host says sleep is OK (informational) |

**ESP32 → host**

| Message | When |
|---|---|
| `{"type":"STATUS","state":"ARMED","presence":false,"dist_cm":0,"sensor_ok":true,"uptime_s":123,"vbat_v":12.6,"grace_s":0,"alarm_s":0}` | Reply to STATUS; also published to MQTT `carsentry/<id>/status` (retained) every 5 s |
| `{"type":"ALERT","event":"person_detected","dist_cm":180,"state":"ALARMING","uptime_s":123}` | On trigger; also MQTT `carsentry/<id>/alert` |
| `{"type":"WAKE","reason":"person_detected","method":"wol+mqtt+gpio"}` | On trigger / WAKE_TEST; also MQTT `carsentry/<id>/wake` |
| `{"type":"ARMED","state":"GRACE"}` / `{"type":"DISARMED","state":"DISARMED"}` | After ARM / DISARM |
| `{"type":"ERROR","error":"bad disarm code"}` | Bad code / unknown command |

**MQTT topics** (`<id>` = last 4 hex chars of the ESP32 MAC):
`carsentry/<id>/status`, `carsentry/<id>/alert`, `carsentry/<id>/wake`
(published) and `carsentry/<id>/command` (subscribed, same `{"cmd":…}` objects).
Set the broker host/port in config (`mqtt_host`, `mqtt_port`, default 1883).
No MQTT broker is bundled — run one on the laptop (e.g. Mosquitto) or
leave `mqtt_host` empty to use serial/HTTP only.

**HTTP** (port 80, AP `CarSentry-XXXX` + `http://carsentry.local/`):

| Endpoint | What it does |
|---|---|
| `GET /` | Web UI: status, ARM/DISARM, wake test, WiFi + settings forms |
| `GET /api/status` | JSON status (state, presence, distance, battery, grace/alarm countdown) |
| `GET / POST /api/config` | Read / set config (incl. `laptop_mac`, `mqtt_host`, `radar_out_wired`) |
| `POST /api/arm` | Arm |
| `POST /api/disarm` `{"code":"1234"}` | Disarm (403 on wrong code) |
| `POST /api/wake-test` | Fire wake outputs once |
| `GET /api/version` | Firmware version, uptime, free heap, boot count |
| `POST /api/ota` + `GET /api/ota/status` | WiFi OTA firmware upload (`.bin`, magic `0xE9` checked) |

## First flash (USB), then OTA

1. Wire the board per `wiring.md`. Do the first flash on the bench
   (ESP32 on USB power, radar wired) — not in the car.
2. **Change the disarm code first** (see above) — or at least do it
   before the node goes in the car.
3. Flash over USB (the only step that needs a PC):
   ```
   ~/.venvs/pio/bin/pio run -d . -t upload
   # LD2450 build: ~/.venvs/pio/bin/pio run -d . -e esp32-ld2450 -t upload
   ```
   If no serial port is found, check the USB cable (must be data, not
   charge-only) and that you have permission to the port.
4. Watch the serial log at 115200 baud:
   ```
   ~/.venvs/pio/bin/pio device monitor -b 115200
   ```
   Expect `[boot] Car Sentry …`, `[radar] config applied`, `[http] :80 …`,
   and `[radar] present=… dist=… cm` lines as you walk in front of it.
5. Join the `CarSentry-XXXX` WiFi AP, open `http://192.168.4.1/`, enter
   the WiFi network the laptop is on, set the laptop MAC + MQTT host,
   and test: ARM, walk up to the car, confirm siren + WAKE + laptop wake.
   Test WoL from suspend **before** relying on it (see above).
6. After this first USB flash, updates are OTA: `POST /api/ota` with a
   new `firmware.bin`. **OTA limits (honest):** a failed upload aborts
   before commit, so the old firmware keeps running — but if power is
   lost mid-write *after* the commit starts, the ESP32 may not boot and
   the only recovery is a USB re-flash. The very first flash onto a
   blank ESP32 always needs USB.

## Build

```
~/.venvs/pio/bin/pio run -d .                  # LD2410 (default)
~/.venvs/pio/bin/pio run -d . -e esp32-ld2450  # LD2450 option
./test/run_tests.sh                             # host unit tests (g++)
```

## Test status — read this before trusting it on hardware

- Host unit tests: **64 tests, 64 passing, 0 failed** (see `TEST-REPORT.md`):
  alarm state machine (grace, confirm, retrigger cap, latch, disarm),
  sleep/wake decisions, protocol parsing/building, WoL packet, LD2410
  frame parsing incl. resync and bad-footer rejection.
- Firmware: both `esp32-ld2410` and `esp32-ld2450` compile clean with
  PlatformIO. LD2410: 900,624 bytes, sha256
  `fc8e3ac1dd96a02609742ebb4ce79333cc6c3eb92412df8eeac918034bdf1614`.
  LD2450: 900,560 bytes, sha256
  `41496d3afe42633aec3989f3a8bd8cf49927cce1be09848e9b58c143fc5e8fda`.
- **NOT tested on real hardware. No ESP32, LD2410/LD2450, buzzer, or
  laptop was attached here.** The radar parsing is verified against the
  HLK protocol and the unit tests, the alarm logic is verified on the
  host, but the firmware has never talked to a real module, the siren
  has never sounded, WoL has never woken a real laptop from this build,
  and the sleep/wake and battery-cutoff paths have never run on a real
  board. First live test is the bench procedure above.

## Privacy

Local only. No cloud account, no external network calls, no location
tracking, no recording. Presence data is reported live to the host and
is not stored except the node's own settings in NVS.
