"""Laptop UI — simple Tkinter dashboard on the laptop screen.

Shows armed state, recent alerts, recent plates, battery, and
buttons for Arm / Disarm / Self-test / Export map + CSV.
Fails gracefully (prints a message) if no display is available.
"""
from __future__ import annotations


def run_ui(store, sentry):
    try:
        import tkinter as tk
        from tkinter import ttk, messagebox
    except ImportError as e:
        print(f"Tkinter unavailable: {e}")
        return 1
    try:
        root = tk.Tk()
    except Exception as e:
        print(f"No display available for Tkinter UI: {e}")
        print("Use --self-test / --run instead, or run with a display (DISPLAY set).")
        return 1
    root.title("Car Sentry — laptop host")
    root.geometry("640x480")

    status_var = tk.StringVar(value="DISARMED")
    ttk.Label(root, textvariable=status_var, font=("TkDefaultFont", 16)).pack(pady=8)

    alerts_box = tk.Text(root, height=12)
    alerts_box.pack(fill="both", expand=True, padx=8, pady=4)

    def refresh():
        status_var.set("ARMED — idle" if sentry.armed and not sentry.is_active
                       else "ARMED — ACTIVE" if sentry.armed else "DISARMED")
        alerts_box.delete("1.0", "end")
        for a in store.list_alerts(30):
            alerts_box.insert("end", f"{a.ts} [{a.level}] {a.type}: {a.detail}\n")
        root.after(5000, refresh)

    def arm():
        sentry.arm(); refresh()
    def disarm():
        sentry.disarm(); refresh()
    def do_selftest():
        from .selftest import run_self_test
        lines = "\n".join(c.line() for c in run_self_test())
        messagebox.showinfo("Self-test", lines)
    def do_export():
        from .mapgen import generate_map
        pts = [{"lat": a.lat, "lon": a.lon, "label": a.detail, "kind": "alert"}
               for a in store.list_alerts(100) if a.lat is not None]
        path = generate_map(pts, "data/map.html")
        messagebox.showinfo("Export", f"Map written to {path}\nCSV: use --export-csv")

    bar = ttk.Frame(root); bar.pack(pady=6)
    ttk.Button(bar, text="Arm", command=arm).pack(side="left", padx=4)
    ttk.Button(bar, text="Disarm", command=disarm).pack(side="left", padx=4)
    ttk.Button(bar, text="Self-test", command=do_selftest).pack(side="left", padx=4)
    ttk.Button(bar, text="Export map", command=do_export).pack(side="left", padx=4)
    refresh()
    root.mainloop()
    return 0
