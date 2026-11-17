import os, sys, unittest
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from car_sentry.gps import parse_nmea, parse_nmea_stream, haversine_m

# Honolulu ~21.3099, -157.8581. No checksums: parser is lenient when absent.
GGA = "$GPGGA,123519,2118.5940,N,15751.4860,W,1,08,0.9,10.0,M,0.0,M,,"
RMC = "$GPRMC,123519,A,2118.5940,N,15751.4860,W,10.0,90.0,161126,0.0,E"
GGA_NOFIX = "$GPGGA,123519,2118.5940,N,15751.4860,W,0,00,0.9,10.0,M,0.0,M,,"

def _with_checksum(sentence):
    body = sentence.lstrip("$").split("*")[0]
    c = 0
    for ch in body: c ^= ord(ch)
    return f"${body}*{c:02X}"

class TestGps(unittest.TestCase):
    def test_gga(self):
        f = parse_nmea(GGA)
        self.assertTrue(f.has_fix); self.assertEqual(f.fix_quality, 1); self.assertEqual(f.satellites, 8)
        self.assertAlmostEqual(f.lat, 21.3099, places=3); self.assertAlmostEqual(f.lon, -157.8581, places=3)
        self.assertAlmostEqual(f.alt_m, 10.0)
    def test_rmc(self):
        f = parse_nmea(RMC)
        self.assertTrue(f.has_fix); self.assertAlmostEqual(f.speed_kmh, 18.52, places=1)
        self.assertEqual((f.timestamp.year, f.timestamp.month, f.timestamp.day), (2026, 11, 16))
    def test_no_fix(self):
        f = parse_nmea(GGA_NOFIX); self.assertFalse(f.has_fix); self.assertEqual(f.fix_quality, 0)
    def test_checksum_good_and_bad(self):
        self.assertIsNotNone(parse_nmea(_with_checksum(GGA)))
        bad = _with_checksum(GGA)[:-2] + "00"
        self.assertIsNone(parse_nmea(bad))
    def test_unknown_and_garbage(self):
        self.assertIsNone(parse_nmea("$GPGSV,1,1,1,1,1,1,1"))
        self.assertIsNone(parse_nmea("not nmea"))
        self.assertIsNone(parse_nmea(""))
    def test_stream(self):
        fixes = parse_nmea_stream(GGA + "\n" + RMC + "\n" + GGA_NOFIX + "\n")
        self.assertEqual(len(fixes), 2)
    def test_haversine(self):
        # ~1 km north
        self.assertAlmostEqual(haversine_m(21.3099, -157.8581, 21.3189, -157.8581), 1000, delta=15)
        self.assertAlmostEqual(haversine_m(21.3, -157.8, 21.3, -157.8), 0.0)

if __name__ == "__main__":
    unittest.main()
