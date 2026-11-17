"""GPS logging — NMEA parsing from a USB/serial GPS.

Supports $GPGGA (fix, altitude, satellites) and $GPRMC
(position, speed, date/time). Pure stdlib, testable with fixtures.
Checksum is verified when present; a bad checksum marks the
sentence invalid rather than silently trusting it.
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from datetime import datetime, timezone


@dataclass
class GpsFix:
    lat: float | None = None
    lon: float | None = None
    alt_m: float | None = None
    speed_kmh: float | None = None
    timestamp: datetime | None = None
    fix_quality: int = 0          # GGA: 0=no fix, 1=GPS, 2=DGPS
    satellites: int = 0
    valid: bool = False
    source: str = ""              # "GGA" or "RMC"

    @property
    def has_fix(self) -> bool:
        return self.valid and self.lat is not None and self.lon is not None


def _checksum_ok(sentence: str) -> bool:
    """Verify *HH checksum if present; True if absent (lenient)."""
    m = re.search(r"\*([0-9A-Fa-f]{2})\s*$", sentence)
    if not m:
        return True
    body = sentence.strip().lstrip("$").split("*")[0]
    calc = 0
    for ch in body:
        calc ^= ord(ch)
    return calc == int(m.group(1), 16)


def _deg_min_to_decimal(value: str, hemi: str) -> float | None:
    """NMEA ddmm.mmmm / dddmm.mmmm -> decimal degrees."""
    if not value or not hemi:
        return None
    try:
        v = float(value)
    except ValueError:
        return None
    degrees = int(v // 100)
    minutes = v - degrees * 100
    dec = degrees + minutes / 60.0
    if hemi in ("S", "W"):
        dec = -dec
    return dec


def _parse_gga(fields: list[str]) -> GpsFix:
    # $GPGGA,time,lat,N/S,lon,E/W,quality,sats,hdop,alt,M,...
    fix = GpsFix(source="GGA")
    try:
        fix.fix_quality = int(fields[6]) if fields[6] else 0
        fix.satellites = int(fields[7]) if fields[7] else 0
        fix.alt_m = float(fields[9]) if fields[9] else None
    except (ValueError, IndexError):
        pass
    fix.lat = _deg_min_to_decimal(fields[2], fields[3]) if len(fields) > 3 else None
    fix.lon = _deg_min_to_decimal(fields[4], fields[5]) if len(fields) > 5 else None
    fix.valid = fix.fix_quality > 0 and fix.lat is not None and fix.lon is not None
    return fix


def _parse_rmc(fields: list[str]) -> GpsFix:
    # $GPRMC,time,status,lat,N/S,lon,E/W,speed_knots,course,date,...
    fix = GpsFix(source="RMC")
    if len(fields) < 10:
        return fix
    if fields[2] != "A":  # A=active, V=void
        return fix
    fix.lat = _deg_min_to_decimal(fields[3], fields[4])
    fix.lon = _deg_min_to_decimal(fields[5], fields[6])
    try:
        fix.speed_kmh = float(fields[7]) * 1.852 if fields[7] else None
    except ValueError:
        fix.speed_kmh = None
    try:
        # time hhmmss.sss + date ddmmyy
        t, d = fields[1], fields[9]
        fix.timestamp = datetime(
            2000 + int(d[4:6]), int(d[2:4]), int(d[0:2]),
            int(t[0:2]), int(t[2:4]), int(t[4:6]), tzinfo=timezone.utc)
    except (ValueError, IndexError):
        fix.timestamp = None
    fix.valid = fix.lat is not None and fix.lon is not None
    return fix


def parse_nmea(sentence: str) -> GpsFix | None:
    """Parse one NMEA sentence. None if not GGA/RMC or checksum bad."""
    s = (sentence or "").strip()
    if not s.startswith("$"):
        return None
    if not _checksum_ok(s):
        return None
    body = s.lstrip("$").split("*")[0]
    fields = body.split(",")
    talker_type = fields[0][2:] if len(fields[0]) >= 3 else ""
    if talker_type == "GGA":
        return _parse_gga(fields)
    if talker_type == "RMC":
        return _parse_rmc(fields)
    return None


def parse_nmea_stream(text: str) -> list[GpsFix]:
    """Parse a block of NMEA lines; returns only valid fixes."""
    fixes = []
    for line in (text or "").splitlines():
        fix = parse_nmea(line)
        if fix is not None and fix.has_fix:
            fixes.append(fix)
    return fixes


def haversine_m(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    """Distance in metres between two decimal-degree points."""
    import math
    r = 6371000.0
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp, dl = math.radians(lat2 - lat1), math.radians(lon2 - lon1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * r * math.asin(math.sqrt(a))
