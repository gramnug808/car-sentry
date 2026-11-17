"""Car Sentry host CLI.

Usage (from ~/workspace/car-sentry/host/):
  python3 -m car_sentry --self-test        diagnostics, no hardware needed
  python3 -m car_sentry --ui               Tkinter dashboard on laptop screen
  python3 -m car_sentry --run              headless run: arm, idle, scan loop
  python3 -m car_sentry --scan-wifi        one WiFi sweep, print networks
  python3 -m car_sentry --scan-ble         one BLE listing
  python3 -m car_sentry --sdr             one passive SDR sweep (if SDR attached)
  python3 -m car_sentry --export-map       write data/map.html from stored alerts
  python3 -m car_sentry --export-csv out.csv
  python3 -m car_sentry --config config.json   (see config.example.json)
"""
from __future__ import annotations

import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DATA_DIR = os.path.join(os.path.dirname(HERE), "data")


def _store(config):
    from .store import Store, load_config
    cfg = load_config(config) if config else {}
    db = os.path.join(DATA_DIR, cfg.get("db", "car-sentry.db"))
    return Store.from_config(db, cfg), cfg


def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="car_sentry", description="Car Sentry laptop host")
    p.add_argument("--self-test", action="store_true")
    p.add_argument("--ui", action="store_true")
    p.add_argument("--run", action="store_true")
    p.add_argument("--scan-wifi", action="store_true")
    p.add_argument("--scan-ble", action="store_true")
    p.add_argument("--sdr", action="store_true")
    p.add_argument("--export-map", action="store_true")
    p.add_argument("--export-csv", metavar="PATH")
    p.add_argument("--config", default=None)
    p.add_argument("--gps-device", default=None, help="NMEA serial device, e.g. /dev/ttyUSB0")
    args = p.parse_args(argv)

    if args.self_test:
        from .selftest import main as st
        return st()

    store, cfg = _store(args.config)
    from .sentry import Sentry
    sentry = Sentry(store=store, idle_timeout_s=int(cfg.get("idle_timeout_s", 300)))

    if args.scan_wifi:
        from .wifi_scan import scan_wifi
        nets, method = scan_wifi(cfg.get("wifi_interface", "wlan0"))
        print(f"method: {method} — {len(nets)} network(s)")
        for n in nets:
            print(f"  {n.bssid} {n.ssid or '(hidden)'} {n.signal_dbm} dBm ch{n.channel} {n.security}")
        return 0
    if args.scan_ble:
        from .ble_scan import scan_ble
        devs, method = scan_ble()
        print(f"method: {method} — {len(devs)} device(s)")
        for d in devs:
            print(f"  {d.address} {d.name or ''} {d.rssi_dbm}")
        return 0
    if args.sdr:
        from .sdr import log_spectrum
        bins, note = log_spectrum()
        print(note)
        print(f"{len(bins)} spectrum bin(s)")
        for b in bins[:10]:
            print(f"  {b.freq_mhz:.3f} MHz  {b.power_db} dB")
        return 0
    if args.export_csv:
        print("wrote", store.export_csv(args.export_csv))
        return 0
    if args.export_map:
        from .mapgen import generate_map
        pts = [{"lat": a.lat, "lon": a.lon, "label": a.detail, "kind": "alert"}
               for a in store.list_alerts(200) if a.lat is not None]
        print("wrote", generate_map(pts, os.path.join(DATA_DIR, "map.html")))
        return 0
    if args.ui:
        from .ui import run_ui
        return run_ui(store, sentry)
    if args.run:
        sentry.arm()
        print("Car Sentry armed (idle). Ctrl-C to stop. Camera off until WAKE; "
              "WiFi sweep 1/min; battery guard on.")
        try:
            while True:
                state = sentry.tick()
                if state == "low-battery":
                    print("LOW BATTERY — stopping scans to protect the laptop battery.")
                    time.sleep(60)
                    continue
                if state == "idle":
                    from .wifi_scan import scan_wifi
                    nets, _ = scan_wifi(cfg.get("wifi_interface", "wlan0"))
                    for n in nets:
                        store.log_signal("wifi", n.bssid, n.ssid, n.signal_dbm, n.channel)
                time.sleep(60)
        except KeyboardInterrupt:
            print("\nstopped.")
            sentry.disarm()
        return 0

    p.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
