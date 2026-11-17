"""Signal intel — WiFi wardriving from the laptop's built-in card.

Parsers are pure functions over command output (testable with
fixtures, no hardware needed). The live scan() prefers `iw`,
falls back to `nmcli`, and reports honestly when neither exists.

Collected per network: BSSID, SSID if broadcast, signal dBm,
channel, frequency, security. No connecting, no deauth, no
cracking — passive scan only.
"""
from __future__ import annotations

import re
import shutil
import subprocess
from dataclasses import dataclass


@dataclass
class WifiNetwork:
    bssid: str
    ssid: str | None      # None = hidden / not broadcast
    signal_dbm: int | None
    channel: int | None
    freq_mhz: int | None
    security: str = ""

    def as_dict(self):
        return {"bssid": self.bssid, "ssid": self.ssid,
                "signal_dbm": self.signal_dbm, "channel": self.channel,
                "freq_mhz": self.freq_mhz, "security": self.security}


def _freq_to_channel(freq_mhz: int | None) -> int | None:
    if not freq_mhz:
        return None
    if 2412 <= freq_mhz <= 2484:
        return 14 if freq_mhz == 2484 else (freq_mhz - 2407) // 5
    if 5000 <= freq_mhz <= 5900:
        return (freq_mhz - 5000) // 5
    if 5925 <= freq_mhz <= 7125:
        return (freq_mhz - 5950) // 5
    return None


def parse_iw_scan(text: str) -> list[WifiNetwork]:
    """Parse `iw dev <iface> scan` output."""
    networks, cur = [], None
    for line in (text or "").splitlines():
        line = line.rstrip()
        m = re.match(r"\s*BSS\s+([0-9a-fA-F:]{17})", line)
        if m:
            if cur:
                networks.append(cur)
            cur = {"bssid": m.group(1).lower(), "ssid": None,
                   "signal_dbm": None, "channel": None,
                   "freq_mhz": None, "security": ""}
            continue
        if cur is None:
            continue
        m = re.search(r"signal:\s*(-?\d+)", line)
        if m:
            cur["signal_dbm"] = int(m.group(1))
        m = re.search(r"freq:\s*(\d+)", line)
        if m:
            cur["freq_mhz"] = int(m.group(1))
            cur["channel"] = _freq_to_channel(cur["freq_mhz"])
        m = re.search(r"SSID:\s*(.*)$", line)
        if m:
            ssid = m.group(1).strip()
            cur["ssid"] = ssid if ssid else None
        if "RSN:" in line or "WPA:" in line:
            cur["security"] = (cur["security"] + " WPA/RSN").strip()
        if re.search(r"capability:.*Privacy", line):
            cur["security"] = cur["security"] or "WEP?"
    if cur:
        networks.append(cur)
    return [WifiNetwork(**n) for n in networks]


def parse_nmcli(text: str) -> list[WifiNetwork]:
    """Parse `nmcli -t -f SSID,BSSID,SIGNAL,CHAN,FREQ,SECURITY dev wifi list`.

    SIGNAL here is 0-100 quality, not dBm — converted approximately
    (quality 100 ~= -30 dBm, 0 ~= -90 dBm) and flagged as approximate
    by keeping the raw quality in the security field? No: we store the
    estimate and document it. Prefer `iw` for true dBm.
    """
    networks = []
    for line in (text or "").splitlines():
        # BSSIDs contain colons, so a naive split(":") breaks. Match
        # the BSSID itself, then parse the fields around it:
        #   SSID:BSSID:SIGNAL:CHAN:FREQ:SECURITY
        m = re.match(r"^(.*?):([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5}):(\d*):(\d*):(\d*):(.*)$", line)
        if not m:
            continue
        ssid, bssid, signal, chan, freq, security = m.groups()
        try:
            quality = int(signal)
            dbm = -30 - int((100 - quality) * 0.6)  # approximate
        except ValueError:
            dbm = None
        try:
            chan_i = int(chan) if chan else None
        except ValueError:
            chan_i = None
        try:
            freq_i = int(freq) if freq else None
        except ValueError:
            freq_i = None
        networks.append(WifiNetwork(
            bssid=bssid.lower(), ssid=ssid or None, signal_dbm=dbm,
            channel=chan_i, freq_mhz=freq_i, security=security or ""))
    return networks


def _run(cmd: list[str], timeout: int = 30) -> str | None:
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return r.stdout if r.returncode == 0 else None
    except (FileNotFoundError, subprocess.TimeoutExpired, OSError):
        return None


def scan_wifi(interface: str = "wlan0") -> tuple[list[WifiNetwork], str]:
    """Live scan. Returns (networks, method_or_reason)."""
    if shutil.which("iw"):
        out = _run(["iw", "dev", interface, "scan"])
        if out:
            return parse_iw_scan(out), f"iw {interface}"
    if shutil.which("nmcli"):
        out = _run(["nmcli", "-t", "-f", "SSID,BSSID,SIGNAL,CHAN,FREQ,SECURITY",
                    "dev", "wifi", "list", "--rescan", "yes"])
        if out:
            return parse_nmcli(out), "nmcli (signal is approx. dBm from quality)"
    return [], "no iw/nmcli found — no WiFi scan tool on this laptop"
