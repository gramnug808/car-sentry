"""Logging — local SQLite + CSV export, alerts, red/white lists,
following-signal logic.

Own-vehicle defensive use only: plates/signals are logged because
*this* vehicle saw them, with GPS + timestamp, so the owner can
tell whether the same plate/signal keeps reappearing at different
places. No lookup of plate owners, no sharing.

Following rule: same plate (or same BSSID) seen at >=3 distinct
locations (>=100 m apart) within 60 minutes -> FOLLOWING alert.
Red list -> immediate RED alert. White list -> logged, never alerted.
"""
from __future__ import annotations

import csv
import json
import os
import sqlite3
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone

from .gps import haversine_m

SCHEMA = """
CREATE TABLE IF NOT EXISTS sightings(
  id INTEGER PRIMARY KEY, plate TEXT, kind TEXT, score INTEGER,
  lat REAL, lon REAL, ts TEXT, source TEXT);
CREATE TABLE IF NOT EXISTS signals(
  id INTEGER PRIMARY KEY, kind TEXT, ident TEXT, ssid TEXT,
  signal_dbm INTEGER, channel INTEGER, lat REAL, lon REAL, ts TEXT);
CREATE TABLE IF NOT EXISTS alerts(
  id INTEGER PRIMARY KEY, level TEXT, type TEXT, detail TEXT,
  lat REAL, lon REAL, ts TEXT);
CREATE TABLE IF NOT EXISTS events(
  id INTEGER PRIMARY KEY, type TEXT, detail TEXT, ts TEXT);
"""


@dataclass
class Alert:
    level: str   # INFO | RED | FOLLOWING
    type: str
    detail: str
    lat: float | None = None
    lon: float | None = None
    ts: str = ""


class Store:
    def __init__(self, db_path: str, red_list=None, white_list=None):
        self.db_path = db_path
        os.makedirs(os.path.dirname(os.path.abspath(db_path)) or ".", exist_ok=True)
        self.conn = sqlite3.connect(db_path)
        self.conn.executescript(SCHEMA)
        self.conn.commit()
        self.red_list = {p.upper() for p in (red_list or [])}
        self.white_list = {p.upper() for p in (white_list or [])}

    @classmethod
    def from_config(cls, db_path: str, config: dict | None = None):
        cfg = config or {}
        return cls(db_path, cfg.get("red_list", []), cfg.get("white_list", []))

    @staticmethod
    def _now() -> str:
        return datetime.now(timezone.utc).isoformat()

    # -- plates ---------------------------------------------------------
    def check_plate(self, plate: str) -> str:
        """'red' | 'white' | 'unknown'."""
        p = (plate or "").upper()
        if p in self.red_list:
            return "red"
        if p in self.white_list:
            return "white"
        return "unknown"

    def log_sighting(self, plate: str, kind: str, score: int,
                     lat: float | None, lon: float | None,
                     source: str = "webcam", ts: str | None = None) -> Alert | None:
        ts = ts or self._now()
        self.conn.execute(
            "INSERT INTO sightings(plate,kind,score,lat,lon,ts,source) VALUES (?,?,?,?,?,?,?)",
            (plate, kind, score, lat, lon, ts, source))
        self.conn.commit()
        status = self.check_plate(plate)
        if status == "white":
            return None  # logged, never alerted
        if status == "red":
            return self._add_alert("RED", "RED_LIST",
                                   f"Red-list plate {plate} seen", lat, lon, ts)
        following = self.check_following(plate, now_ts=ts)
        if following:
            return self._add_alert("FOLLOWING", "FOLLOWING",
                                   f"Plate {plate} seen at {following} distinct "
                                   f"locations in 60 min — possible following", lat, lon, ts)
        return None

    def check_following(self, plate: str, window_minutes: int = 60,
                        min_locations: int = 3, min_distance_m: float = 100.0,
                        now_ts: str | None = None) -> int:
        """Distinct GPS clusters for this plate in the window. 0 if < threshold."""
        now = datetime.fromisoformat(now_ts or self._now())
        since = (now - timedelta(minutes=window_minutes)).isoformat()
        rows = self.conn.execute(
            "SELECT lat,lon FROM sightings WHERE plate=? AND ts>=? AND lat IS NOT NULL",
            (plate, since)).fetchall()
        clusters: list[tuple[float, float]] = []
        for lat, lon in rows:
            if all(haversine_m(lat, lon, clat, clon) >= min_distance_m
                   for clat, clon in clusters):
                clusters.append((lat, lon))
        return len(clusters) if len(clusters) >= min_locations else 0

    # -- signals --------------------------------------------------------
    def log_signal(self, kind: str, ident: str, ssid: str | None = None,
                   signal_dbm: int | None = None, channel: int | None = None,
                   lat: float | None = None, lon: float | None = None,
                   ts: str | None = None) -> None:
        ts = ts or self._now()
        self.conn.execute(
            "INSERT INTO signals(kind,ident,ssid,signal_dbm,channel,lat,lon,ts) "
            "VALUES (?,?,?,?,?,?,?,?)",
            (kind, ident, ssid, signal_dbm, channel, lat, lon, ts))
        self.conn.commit()

    def check_signal_following(self, kind: str, ident: str,
                               window_minutes: int = 60, min_locations: int = 3,
                               min_distance_m: float = 100.0) -> int:
        since = (datetime.now(timezone.utc) - timedelta(minutes=window_minutes)).isoformat()
        rows = self.conn.execute(
            "SELECT lat,lon FROM signals WHERE kind=? AND ident=? AND ts>=? AND lat IS NOT NULL",
            (kind, ident, since)).fetchall()
        clusters: list[tuple[float, float]] = []
        for lat, lon in rows:
            if all(haversine_m(lat, lon, clat, clon) >= min_distance_m
                   for clat, clon in clusters):
                clusters.append((lat, lon))
        return len(clusters) if len(clusters) >= min_locations else 0

    # -- alerts / events ------------------------------------------------
    def _add_alert(self, level: str, type_: str, detail: str,
                   lat: float | None, lon: float | None, ts: str | None) -> Alert:
        ts = ts or self._now()
        self.conn.execute("INSERT INTO alerts(level,type,detail,lat,lon,ts) VALUES (?,?,?,?,?,?)",
                          (level, type_, detail, lat, lon, ts))
        self.conn.commit()
        return Alert(level, type_, detail, lat, lon, ts)

    def add_alert(self, level: str, type_: str, detail: str,
                  lat: float | None = None, lon: float | None = None) -> Alert:
        return self._add_alert(level, type_, detail, lat, lon, None)

    def log_event(self, type_: str, detail: str) -> None:
        self.conn.execute("INSERT INTO events(type,detail,ts) VALUES (?,?,?)",
                          (type_, detail, self._now()))
        self.conn.commit()

    def list_alerts(self, limit: int = 50) -> list[Alert]:
        rows = self.conn.execute(
            "SELECT level,type,detail,lat,lon,ts FROM alerts ORDER BY id DESC LIMIT ?",
            (limit,)).fetchall()
        return [Alert(*r) for r in rows]

    def export_csv(self, out_path: str) -> str:
        with open(out_path, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["table", "id", "a", "b", "c", "lat", "lon", "ts"])
            for table, cols in (("sightings", "id,plate,kind,score,lat,lon,ts"),
                                ("signals", "id,kind,ident,signal_dbm,lat,lon,ts"),
                                ("alerts", "id,level,type,detail,lat,lon,ts")):
                for row in self.conn.execute(f"SELECT {cols} FROM {table}"):
                    w.writerow([table, *row])
        return out_path

    def close(self):
        self.conn.close()


def load_config(path: str) -> dict:
    if not path or not os.path.exists(path):
        return {}
    with open(path) as f:
        return json.load(f)
