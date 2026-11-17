"""Self-test / diagnostics — runnable as `python3 -m car_sentry --self-test`.

Each check reports ok / not-ok with an expected-result line, so
the user can see exactly what should be true on a working laptop.
No camera / GPS / SDR / ESP32 is required to run it; missing
hardware is reported as NOT PRESENT, not as a crash.
"""
from __future__ import annotations

import os
import shutil
import subprocess
from dataclasses import dataclass


@dataclass
class Check:
    name: str
    ok: bool
    detail: str
    expected: str

    def line(self) -> str:
        mark = "PASS" if self.ok else "FAIL"
        return f"[{mark}] {self.name}: {self.detail} (expected: {self.expected})"


def _run(cmd, timeout=10):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return r.returncode, r.stdout + r.stderr
    except (FileNotFoundError, subprocess.TimeoutExpired, OSError) as e:
        return -1, str(e)


def check_camera() -> Check:
    from .lpr import camera_available
    ok, detail = camera_available()
    return Check("camera", ok, detail, "/dev/video0 present (USB webcam plugged in)")

def check_tesseract() -> Check:
    ok = shutil.which("tesseract") is not None
    return Check("tesseract-ocr", ok,
                 "tesseract found" if ok else "tesseract not installed — OCR stub will be used, plates will not be read live",
                 "tesseract installed (`sudo apt install tesseract-ocr`)")

def check_opencv() -> Check:
    try:
        import cv2
        return Check("opencv", True, f"cv2 {cv2.__version__}", "cv2 importable")
    except ImportError:
        return Check("opencv", False, "cv2 not installed — ffmpeg/v4l2 fallback will be tried",
                     "pip install opencv-python (optional)")

def check_gps() -> Check:
    import glob
    cands = sorted(glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*"))
    ok = bool(cands)
    return Check("gps-serial", ok,
                 f"serial devices: {', '.join(cands)}" if ok else "no /dev/ttyUSB* or /dev/ttyACM* — no USB GPS attached",
                 "USB GPS on /dev/ttyUSB0 or /dev/ttyACM0 (or configure --gps-device)")

def check_wifi() -> Check:
    tools = [t for t in ("iw", "nmcli") if shutil.which(t)]
    return Check("wifi-scan", bool(tools),
                 f"scan tools: {', '.join(tools)}" if tools else "neither iw nor nmcli found",
                 "iw or nmcli installed; laptop WiFi interface present")

def check_ble() -> Check:
    ok = shutil.which("bluetoothctl") is not None
    return Check("ble-scan", ok,
                 "bluetoothctl found" if ok else "bluetoothctl not found (bluez not installed?)",
                 "bluetoothctl installed (optional)")

def check_sdr() -> Check:
    from .sdr import sdr_present
    ok, detail = sdr_present()
    return Check("sdr-passive", ok, detail,
                 "RTL-SDR attached + rtl-sdr tools installed (optional; passive receive only)")

def check_esp32() -> Check:
    from .esp32_protocol import serial_device_present
    ok, detail = serial_device_present()
    return Check("esp32-serial", ok, detail,
                 "ESP32 on /dev/ttyUSB0 (or /dev/ttyACM0) — see README if a different port")

def check_wol() -> Check:
    code, out = _run(["ethtool", "wlan0"])
    if code != 0:
        code, out = _run(["ethtool", "eth0"])
    if code != 0:
        return Check("wake-on-lan", False, "ethtool not available / no interface checked",
                     "BIOS WoL enabled + `ethtool <iface> wol g` (see README)")
    import re
    m = re.search(r"Wake-on:\s*(\S+)", out)
    wol = m.group(1) if m else "?"
    return Check("wake-on-lan", "g" in wol, f"Wake-on: {wol}",
                 "Wake-on includes 'g' (magic packet) — enable in BIOS + ethtool")

def check_usb_wakeup() -> Check:
    import glob
    enabled = [p for p in glob.glob("/sys/bus/usb/devices/*/power/wakeup")
               if open(p).read().strip() == "enabled"]
    return Check("usb-wakeup", bool(enabled),
                 f"{len(enabled)} USB device(s) with wakeup enabled" if enabled else
                 "no USB device has power/wakeup=enabled",
                 "BIOS Wake on USB on + echo enabled > /sys/bus/usb/devices/<dev>/power/wakeup")

def check_battery() -> Check:
    from .sentry import battery_status
    batt = battery_status()
    if not batt:
        return Check("battery", True, "no battery info (desktop / VM?) — power guidance in README",
                     "battery capacity readable under /sys/class/power_supply/BAT*")
    return Check("battery", batt.get("pct", 0) > 20,
                 f"{batt.get('pct')}% ({batt.get('status', '?')})",
                 "battery > 20% before arming on battery alone; use inverter in the car")

def run_self_test() -> list[Check]:
    return [check_camera(), check_tesseract(), check_opencv(), check_gps(),
            check_wifi(), check_ble(), check_sdr(), check_esp32(),
            check_wol(), check_usb_wakeup(), check_battery()]


def main() -> int:
    checks = run_self_test()
    for c in checks:
        print(c.line())
    n_pass = sum(1 for c in checks if c.ok)
    print(f"\n{n_pass}/{len(checks)} checks passed.")
    print("Missing hardware (camera/GPS/SDR/ESP32) is expected on a laptop without")
    print("those devices attached — it means that feature is unavailable, not broken.")
    return 0 if n_pass == len(checks) else 1
