import os, sys, unittest
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from car_sentry.wifi_scan import parse_iw_scan, parse_nmcli
from car_sentry.ble_scan import parse_bluetoothctl
from car_sentry.sdr import parse_rtl_power_csv

IW = """BSS aa:bb:cc:dd:ee:01(on wlan0)
\tfreq: 2412
\tsignal: -45.00 dBm
\tSSID: HomeWifi
\tRSN:\t * Version: 1
BSS aa:bb:cc:dd:ee:02(on wlan0)
\tfreq: 5180
\tsignal: -70.00 dBm
\tSSID:
"""
NMCLI = "HomeWifi:AA:BB:CC:DD:EE:01:80:1:2412:WPA2\nHidden:AA:BB:CC:DD:EE:02:40:36:5180:WPA2\n"
BLE = "Device AA:BB:CC:DD:EE:11 My Headphones\nDevice AA:BB:CC:DD:EE:12\n[CHG] Device AA:BB:CC:DD:EE:11 RSSI: -62\n"
RTL = "2026-11-16, 20:00:00, 88000000, 108000000, 200000, 101, -50.0, -52.0, -51.0\n"

class TestWifi(unittest.TestCase):
    def test_iw(self):
        nets = parse_iw_scan(IW)
        self.assertEqual(len(nets), 2)
        self.assertEqual(nets[0].ssid, "HomeWifi"); self.assertEqual(nets[0].signal_dbm, -45)
        self.assertEqual(nets[0].channel, 1); self.assertEqual(nets[0].freq_mhz, 2412)
        self.assertIsNone(nets[1].ssid)  # hidden
        self.assertEqual(nets[1].channel, 36)
    def test_nmcli(self):
        nets = parse_nmcli(NMCLI)
        self.assertEqual(len(nets), 2)
        self.assertEqual(nets[0].bssid, "aa:bb:cc:dd:ee:01")
        self.assertIsNotNone(nets[0].signal_dbm)
        self.assertEqual(nets[1].channel, 36)
    def test_empty(self):
        self.assertEqual(parse_iw_scan(""), []); self.assertEqual(parse_nmcli(""), [])

class TestBleSdr(unittest.TestCase):
    def test_ble(self):
        devs = parse_bluetoothctl(BLE)
        self.assertEqual(len(devs), 2)
        by_addr = {d.address: d for d in devs}
        self.assertEqual(by_addr["aa:bb:cc:dd:ee:11"].name, "My Headphones")
        self.assertEqual(by_addr["aa:bb:cc:dd:ee:11"].rssi_dbm, -62)
        self.assertIsNone(by_addr["aa:bb:cc:dd:ee:12"].name)
    def test_rtl_power(self):
        bins = parse_rtl_power_csv(RTL)
        self.assertEqual(len(bins), 3)
        self.assertAlmostEqual(bins[0].freq_mhz, 88.0)
        self.assertAlmostEqual(bins[1].freq_mhz, 88.2)
        self.assertAlmostEqual(bins[0].power_db, -50.0)
        self.assertEqual(parse_rtl_power_csv(""), [])

if __name__ == "__main__":
    unittest.main()
