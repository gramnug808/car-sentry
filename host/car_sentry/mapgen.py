"""Offline map — local HTML, NO live internet tiles.

Generates a self-contained HTML file: an SVG scatter plot of
GPS points (plates + signals + alerts), scaled from the recorded
coordinates, with a table underneath. Works with data off, in
the car, with no tile server. If the user later bundles OSM
tiles, a Leaflet page can point at the local tile directory —
but this generator never fetches tiles from the internet.
"""
from __future__ import annotations

import html
import os


def generate_map(points: list[dict], out_path: str, title: str = "Car Sentry — offline map") -> str:
    """points: [{lat, lon, label, kind}]. Returns out_path."""
    pts = [p for p in points if p.get("lat") is not None and p.get("lon") is not None]
    w, h, pad = 900, 600, 40
    if pts:
        lats = [p["lat"] for p in pts]; lons = [p["lon"] for p in pts]
        lat0, lat1 = min(lats), max(lats)
        lon0, lon1 = min(lons), max(lons)
        if lat1 - lat0 < 1e-6:
            lat1 += 0.001; lat0 -= 0.001
        if lon1 - lon0 < 1e-6:
            lon1 += 0.001; lon0 -= 0.001
    else:
        lat0, lat1, lon0, lon1 = 0.0, 1.0, 0.0, 1.0

    def xy(p):
        x = pad + (p["lon"] - lon0) / (lon1 - lon0) * (w - 2 * pad)
        y = h - pad - (p["lat"] - lat0) / (lat1 - lat0) * (h - 2 * pad)
        return x, y

    colors = {"plate": "#d62728", "signal": "#1f77b4", "alert": "#ff7f0e", "gps": "#2ca02c"}
    circles = []
    for p in pts:
        x, y = xy(p)
        c = colors.get(p.get("kind", "gps"), "#555")
        label = html.escape(str(p.get("label", "")))
        circles.append(
            f'<circle cx="{x:.1f}" cy="{y:.1f}" r="7" fill="{c}" stroke="#fff">'
            f'<title>{label} ({p["lat"]:.5f}, {p["lon"]:.5f})</title></circle>'
            f'<text x="{x + 9:.1f}" y="{y + 4:.1f}" font-size="11">{label}</text>')
    rows = "".join(
        f'<tr><td>{html.escape(str(p.get("kind", "")))}</td>'
        f'<td>{html.escape(str(p.get("label", "")))}</td>'
        f'<td>{p["lat"]:.5f}</td><td>{p["lon"]:.5f}</td></tr>' for p in pts)
    doc = f"""<!DOCTYPE html><html><head><meta charset="utf-8">
<title>{html.escape(title)}</title>
<style>body{{font-family:sans-serif;margin:16px}} svg{{border:1px solid #999;background:#eef}}
table{{border-collapse:collapse;margin-top:12px}} td,th{{border:1px solid #ccc;padding:4px 8px}}</style>
</head><body><h1>{html.escape(title)}</h1>
<p>Offline map — no internet tiles. {len(pts)} point(s). Red=plate, blue=signal, orange=alert, green=GPS.</p>
<svg width="{w}" height="{h}" viewBox="0 0 {w} {h}">{"".join(circles) if circles else '<text x="40" y="40">No GPS points yet.</text>'}</svg>
<table><tr><th>kind</th><th>label</th><th>lat</th><th>lon</th></tr>{rows}</table>
</body></html>"""
    os.makedirs(os.path.dirname(os.path.abspath(out_path)) or ".", exist_ok=True)
    with open(out_path, "w") as f:
        f.write(doc)
    return out_path
