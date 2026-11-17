"""Car Sentry — laptop host app (defensive, own-vehicle only).

Linux laptop host: LPR from USB webcam, WiFi/BLE wardriving,
passive SDR spectrum logging, GPS logging, offline map, and a
sentry / sleep mode that talks to a separate ESP32 alarm node
over a simple serial / MQTT protocol.

No ESP32 firmware lives in this package. See ../esp32/ for the
standalone ESP32 node and README.md for the protocol contract.
"""
__version__ = "1.0.0"
__all__ = ["__version__"]
