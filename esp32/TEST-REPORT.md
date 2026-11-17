# Car Sentry — ESP32 Firmware Test Report

Date: 2026-11-16 (build run). Builder: PlatformIO 6.2.0
(`~/.venvs/pio/bin/pio`), Arduino framework, `espressif32@6.9.0`.
**No real ESP32, radar, buzzer, or laptop was attached during this build
— compile only + host unit tests. Nothing here has run on hardware.**

## Host unit tests — 64 passed, 0 failed

Run: `./test/run_tests.sh` (compiles `test/test_logic.cpp` with plain
`g++ -std=c++17`, no ESP32/PlatformIO needed). The tests exercise the
pure-logic headers (`src/alarm_logic.h`, `src/protocol.h`) and the
LD2410 parser (`src/ld2410.h`, via a minimal `test/Arduino.h` stub).

| Group | Tests | Result |
|---|---|---|
| Alarm state machine | 28 | 28 pass |
| Sleep / wake decisions | 8 | 8 pass |
| Protocol parsing / building | 18 | 18 pass |
| LD2410 parser | 10 | 10 pass |
| **Total** | **64** | **64 pass, 0 fail** |

What is covered (each is one CHECK in `test/test_logic.cpp`):

- **Alarm:** disarmed presence ignored; arm → GRACE; arming twice does
  not reset grace; grace detections ignored; grace countdown; grace
  expiry → ARMED; brief presence (< 1000 ms confirm) does not trigger;
  presence lost before confirm resets; sustained in-range presence →
  ALARMING + wake; alarm countdown; siren 500 ms on/off pattern;
  wrong disarm code rejected (siren keeps sounding); correct code
  disarms; out-of-range (500 cm > 300 cm) never triggers; unknown
  distance (0) treated as in-range; burst end → ARMED; retrigger works;
  max cycles (2 in test) → LATCHED, quiet; latched presence does not
  re-siren; physical button clears latch; config clamping; empty disarm
  code rejected.
- **Sleep/wake:** disarmed + no presence history → AWAKE; disarmed +
  idle → LIGHT_SLEEP; GRACE → AWAKE; ARMED + deep sleep off →
  LIGHT_SLEEP; ARMED + deep sleep + OUT wired → DEEP_SLEEP; ARMED +
  deep sleep but OUT not wired → LIGHT_SLEEP (fallback); ALARMING →
  AWAKE; battery cutoff → DEEP_SLEEP regardless of state.
- **Protocol:** parse ARM / DISARM+code / STATUS / WAKE_TEST / CONFIG
  (all fields incl. `deep_sleep` and `disarm_code`); unknown command and
  garbage and empty line rejected; STATUS / ALERT / WAKE / ERROR JSON
  shapes; WoL packet = 6× `0xFF` + MAC ×16; MAC parse (colons and
  dashes); bad/short MAC rejected.
- **LD2410 parser:** basic moving / both-targets / no-target frames;
  field values; resync through garbage bytes; bad footer rejected
  (previous reading kept); bad report type rejected.

What is **not** covered by these tests (honest gap): the Arduino
`main.cpp` glue itself — WiFi/MQTT/HTTP handlers, the actual GPIO
outputs, the real sleep calls, WoL transmission, OTA, and the LD2450
parser — is only compile-checked, not executed. The LD2450 parser was
reused unchanged from the mmWave detector build.

## Firmware compile — SUCCESS (both environments)

```
~/.venvs/pio/bin/pio run -d . -e esp32-ld2410   -> SUCCESS (28.8 s)
~/.venvs/pio/bin/pio run -d . -e esp32-ld2450   -> SUCCESS (32.9 s)
```

| Build | Size | SHA-256 | Magic |
|---|---|---|---|
| `firmware.bin` (LD2410, default) | 900,624 bytes | `fc8e3ac1dd96a02609742ebb4ce79333cc6c3eb92412df8eeac918034bdf1614` | `0xE9` ✓ |
| `firmware-ld2450.bin` (LD2450) | 900,560 bytes | `41496d3afe42633aec3989f3a8bd8cf49927cce1be09848e9b58c143fc5e8fda` | `0xE9` ✓ |

LD2410 build memory: RAM 14.8% (48,432 / 327,680 bytes),
Flash 68.2% (893,909 / 1,310,720 bytes). No compiler warnings were
shown for the project sources in the build tail (framework warnings, if
any, were not surfaced as errors; the build status is the check).

One fix made during the build: the global `AlarmMachine alarm;` in
`main.cpp` collided with the libc `alarm()` function, so it was renamed
to `alarmMachine` (no logic change).

## Not tested on real hardware — first live test

1. Bench-flash over USB, watch serial at 115200 baud for `[boot]`,
   `[radar] config applied`, `[radar] present=… dist=… cm` lines.
2. Walk in front of the radar: STATUS must show presence + distance.
3. ARM, walk away, wait out the 30 s grace, walk up: siren + strobe
   must sound for 30 s and a WAKE line must print.
4. DISARM with the code, then with the button.
5. Set `laptop_mac`, suspend the laptop, trigger: confirm WoL wakes it
   (test WoL standalone first with `wakeonlan` — see README).
6. Battery cutoff: only verifiable with a variable supply or a low
   battery — do not drain a real car battery to test it.
