"""Sentry / sleep mode for the LAPTOP host + power guidance.

WAKE — what is actually supported, stated exactly:
  The host process itself cannot wake a laptop that is in deep
  suspend (S3/S4) over MQTT — the WiFi radio and CPU are off.
  Three wake paths exist, in order of reliability:

  1. IDLE WAKE (default, implemented here): the laptop is NOT in
     S3 suspend. Screen off, CPU throttled, host process running.
     The ESP32 sends WAKE over USB serial or MQTT; the host sees
     it immediately and starts camera + radios. This is the
     recommended mode — it always works and costs little power.
  2. WAKE-ON-LAN (WoL): the ESP32 sends a magic packet to the
     laptop's MAC on the same WiFi/LAN. Requires: BIOS "Wake on
     LAN" / "Wake on WLAN" enabled, `ethtool <iface> wol g`,
     laptop on AC (inverter) power, not fully powered off.
     The host checks WoL status in --self-test and can send a
     magic packet itself for testing (send_wol()).
  3. USB WAKE: the ESP32 (via USB-serial) asserts wake. Requires
     BIOS "Wake on USB" / "USB Wake Support" enabled AND
     /sys/bus/usb/devices/.../power/wakeup = enabled for that port.
     Checked in --self-test. Behaviour varies by laptop model —
     verify on your hardware before relying on it.

  What the user must enable (documented in README.md too):
    BIOS: Wake on LAN / Wake on WLAN, Wake on USB.
    OS:   `sudo ethtool -s <iface> wol g` (persist via a systemd
          unit or NetworkManager dispatcher script — see README),
          enable USB wakeup for the ESP32 port, and prefer
          "suspend-then-hibernate: no" while armed.

POWER (laptop on car 12V inverter / USB-C):
  * Armed + idle: screen off, WiFi scan throttled to 1 sweep/min,
    camera off until WAKE. Typical draw is a few watts; the
    inverter + car battery handle this for hours, but NOT
    indefinitely with the engine off — see battery guidance below.
  * Host sleeps (screen off / idle) when armed and nothing is
    happening; it never runs flat-out. `should_sleep()` is the
    policy hook the runner uses.
  * Battery guidance: if the laptop battery falls below 20% while
    on battery (no inverter), the host logs a LOW_BATTERY alert
    and stops scanning; below 10% it shuts the sentry down so the
    laptop is not drained flat. Never leave it armed overnight on
    battery alone.
"""
from __future__ import annotations

import os
import socket
import struct
import time
from dataclasses import dataclass, field

LOW_BATTERY_PCT = 20
CRITICAL_BATTERY_PCT = 10


def battery_status() -> dict:
    """Read /sys/class/power_supply/BAT*. Returns {} if no battery info."""
    base = "/sys/class/power_supply"
    try:
        bats = [d for d in os.listdir(base) if d.startswith("BAT")]
    except OSError:
        return {}
    if not bats:
        return {}
    out = {}
    try:
        with open(os.path.join(base, bats[0], "capacity")) as f:
            out["pct"] = int(f.read().strip())
        with open(os.path.join(base, bats[0], "status")) as f:
            out["status"] = f.read().strip()  # Charging/Discharging/Full
    except (OSError, ValueError):
        return {}
    return out


def should_sleep(armed: bool, battery_pct: int | None, active: bool) -> bool:
    """Idle policy: sleep (screen off, radios throttled) when armed and idle."""
    if active:
        return False
    if battery_pct is not None and battery_pct <= CRITICAL_BATTERY_PCT:
        return True
    return armed


def send_wol(mac: str, broadcast: str = "255.255.255.255", port: int = 9) -> bool:
    """Send a Wake-on-LAN magic packet (for testing WoL setup)."""
    try:
        clean = mac.replace(":", "").replace("-", "")
        mac_bytes = bytes.fromhex(clean)
        packet = b"\xff" * 6 + mac_bytes * 16
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.sendto(packet, (broadcast, port))
        sock.close()
        return True
    except (ValueError, OSError):
        return False


@dataclass
class Sentry:
    store: object = None          # Store, optional
    idle_timeout_s: int = 300     # return to idle this long after a wake
    armed: bool = False
    active_until: float = 0.0
    wake_log: list = field(default_factory=list)

    def arm(self):
        self.armed = True
        self._event("ARM", "sentry armed — idle, screen off, camera off until WAKE")

    def disarm(self):
        self.armed = False
        self.active_until = 0.0
        self._event("DISARM", "sentry disarmed")

    def _event(self, type_: str, detail: str):
        self.wake_log.append((type_, detail, time.time()))
        if self.store is not None:
            self.store.log_event(type_, detail)

    @property
    def is_active(self) -> bool:
        return time.time() < self.active_until

    def on_wake(self, reason: str = "") -> dict:
        """ESP32 WAKE received: start camera + radios, log, set idle timer."""
        self.active_until = time.time() + self.idle_timeout_s
        detail = f"WAKE received ({reason or 'unspecified'}) — camera + radios on for {self.idle_timeout_s}s"
        self._event("WAKE", detail)
        if self.store is not None:
            self.store.add_alert("INFO", "WAKE", detail)
        return {"active": True, "reason": reason, "active_for_s": self.idle_timeout_s}

    def on_alert(self, alert: str, detail: str = "") -> dict:
        """ESP32 ALERT received: wake fully and log as an alert."""
        result = self.on_wake(f"ALERT {alert}")
        if self.store is not None:
            self.store.add_alert("RED", f"ESP32_{alert}", detail or f"ESP32 alert: {alert}")
        return result

    def tick(self) -> str:
        """Call periodically. Returns 'active' | 'idle' | 'low-battery'."""
        batt = battery_status()
        pct = batt.get("pct")
        if pct is not None and pct <= CRITICAL_BATTERY_PCT and self.armed:
            return "low-battery"
        return "active" if self.is_active else "idle"
