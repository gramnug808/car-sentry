"""BLE scan — passive discovery via bluetoothctl (if present).

Passive only: list nearby BLE/BT devices (address, name, RSSI
when reported). No pairing, no connecting, no GATT reads.
Parser is a pure function over `bluetoothctl devices` / scan
output so it is testable without hardware.
"""
from __future__ import annotations

import re
import shutil
import subprocess
from dataclasses import dataclass


@dataclass
class BleDevice:
    address: str
    name: str | None
    rssi_dbm: int | None = None

    def as_dict(self):
        return {"address": self.address, "name": self.name, "rssi_dbm": self.rssi_dbm}


def parse_bluetoothctl(text: str) -> list[BleDevice]:
    """Parse `bluetoothctl devices` lines, plus optional RSSI lines.

    Lines:  'Device AA:BB:CC:DD:EE:FF Some Name'
    RSSI lines from `bluetoothctl scan on` info dumps are of the
    form '[CHG] Device AA:.. RSSI: -62' — handled too.
    """
    devices: dict[str, BleDevice] = {}
    for line in (text or "").splitlines():
        m = re.match(r"\s*(?:\[CHG\]\s+)?Device\s+([0-9A-Fa-f:]{17})\s*(.*)$", line)
        if m:
            addr, rest = m.group(1).lower(), m.group(2).strip()
            rssi_m = re.search(r"RSSI:\s*(-?\d+)", rest)
            if rssi_m:
                dev = devices.setdefault(addr, BleDevice(addr, None))
                dev.rssi_dbm = int(rssi_m.group(1))
                continue
            name = rest or None
            dev = devices.setdefault(addr, BleDevice(addr, name))
            if name:
                dev.name = name
            continue
        m = re.search(r"RSSI:\s*(-?\d+)", line)
        # RSSI without an address on the same line cannot be attributed.
    return list(devices.values())


def scan_ble(timeout_s: int = 10) -> tuple[list[BleDevice], str]:
    """Live BLE scan via bluetoothctl. Returns (devices, method/reason)."""
    if not shutil.which("bluetoothctl"):
        return [], "bluetoothctl not found — no BLE scan on this laptop"
    try:
        # `devices` lists already-seen devices; a real scan needs an
        # interactive session, so we report known devices and say so.
        r = subprocess.run(["bluetoothctl", "devices"], capture_output=True,
                           text=True, timeout=timeout_s)
        if r.returncode != 0:
            return [], f"bluetoothctl failed: {r.stderr.strip()[:200]}"
        return parse_bluetoothctl(r.stdout), \
            "bluetoothctl devices (already-seen devices; live scan needs interactive bluetoothctl)"
    except (FileNotFoundError, subprocess.TimeoutExpired, OSError) as e:
        return [], f"BLE scan failed: {e}"
