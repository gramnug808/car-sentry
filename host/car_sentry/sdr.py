"""SDR — PASSIVE spectrum logging only.

Rules (hard, by design):
  * Receive only. Never transmit, never jam, never decode or
    intercept other people's communications.
  * Only spectrum *presence* is logged: frequency bin + power
    level from `rtl_power` (RTL-SDR) if an SDR is attached.
    No audio, no demodulation, no protocol decoding.
  * If no SDR is attached, every function reports that plainly
    and returns empty results — never fakes data.

`rtl_power` CSV format (one line per sweep):
  date, time, Hz low, Hz high, Hz step, samples, dB, dB, ...
"""
from __future__ import annotations

import csv
import io
import shutil
import subprocess
from dataclasses import dataclass


@dataclass
class SpectrumBin:
    freq_mhz: float
    power_db: float

    def as_dict(self):
        return {"freq_mhz": self.freq_mhz, "power_db": self.power_db}


def sdr_present() -> tuple[bool, str]:
    """Is an RTL-SDR (or compatible) present? Checks tools, not claims."""
    if shutil.which("rtl_test"):
        try:
            r = subprocess.run(["rtl_test", "-t"], capture_output=True,
                               text=True, timeout=10)
            if "Found" in r.stdout + r.stderr and "No supported devices" not in r.stdout + r.stderr:
                return True, "rtl_test found a device"
        except (FileNotFoundError, subprocess.TimeoutExpired, OSError):
            pass
    if shutil.which("rtl_power"):
        return False, "rtl_power installed but no SDR confirmed (rtl_test unavailable/negative)"
    return False, "no rtl-sdr tools found (rtl_test / rtl_power missing)"


def parse_rtl_power_csv(text: str) -> list[SpectrumBin]:
    """Parse rtl_power CSV into (freq, power) bins."""
    bins: list[SpectrumBin] = []
    reader = csv.reader(io.StringIO(text or ""))
    for row in reader:
        if len(row) < 7:
            continue
        try:
            low = float(row[2]); high = float(row[3]); step = float(row[4])
            powers = [float(x) for x in row[6:]]
        except ValueError:
            continue
        if step <= 0 or not powers:
            continue
        for i, p in enumerate(powers):
            bins.append(SpectrumBin(freq_mhz=(low + i * step) / 1e6, power_db=p))
    return bins


def log_spectrum(freq_range: str = "88M:108M:200k", duration_s: int = 10,
                 out_csv: str | None = None) -> tuple[list[SpectrumBin], str]:
    """One passive rtl_power sweep. Returns (bins, note). No SDR -> []."""
    present, note = sdr_present()
    if not present:
        return [], f"SDR not present: {note}"
    if not shutil.which("rtl_power"):
        return [], "rtl_power not installed"
    cmd = ["rtl_power", "-f", freq_range, "-i", str(duration_s)]
    if out_csv:
        cmd += ["-e", out_csv]
    else:
        cmd += ["-"]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=duration_s + 30)
    except (FileNotFoundError, subprocess.TimeoutExpired, OSError) as e:
        return [], f"rtl_power failed: {e}"
    if r.returncode != 0:
        return [], f"rtl_power failed: {(r.stderr or '').strip()[:200]}"
    text = r.stdout
    if out_csv:
        try:
            text = open(out_csv).read()
        except OSError:
            pass
    return parse_rtl_power_csv(text), f"passive rtl_power sweep {freq_range} ({len(text)} chars)"
