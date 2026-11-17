import os, sys, tempfile, unittest
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from car_sentry.esp32_protocol import (parse_message, parse_mqtt, build_arm,
    build_disarm, TOPIC_ALERT)
from car_sentry.store import Store
from car_sentry.sentry import Sentry, should_sleep
from car_sentry.lpr import read_plate_from_image, StubOcr

class TestProtocol(unittest.TestCase):
    def test_text_forms(self):
        self.assertEqual(parse_message("ARM").kind, "ARM")
        self.assertEqual(parse_message("DISARM").kind, "DISARM")
        self.assertEqual(parse_message("PING").kind, "PING")
        m = parse_message("ALERT MOTION front bumper")
        self.assertEqual((m.kind, m.alert, m.detail), ("ALERT", "MOTION", "front bumper"))
        m = parse_message("WAKE MOTION")
        self.assertEqual((m.kind, m.reason), ("WAKE", "MOTION"))
        m = parse_message('STATUS {"armed":true,"battery":82}')
        self.assertEqual(m.kind, "STATUS"); self.assertTrue(m.data["armed"])
    def test_json_forms(self):
        m = parse_message('{"type":"ALERT","alert":"PERSON","detail":"near car"}')
        self.assertEqual((m.kind, m.alert), ("ALERT", "PERSON"))
        m = parse_message('{"type":"WAKE","reason":"TAMPER"}')
        self.assertEqual((m.kind, m.reason), ("WAKE", "TAMPER"))
    def test_unknown_and_blank(self):
        self.assertEqual(parse_message("GIBBERISH").kind, "UNKNOWN")
        self.assertIsNone(parse_message("   "))
        self.assertIsNone(parse_message(""))
    def test_mqtt(self):
        m = parse_mqtt(TOPIC_ALERT, "ALERT TAMPER window")
        self.assertEqual((m.kind, m.alert), ("ALERT", "TAMPER"))
        m = parse_mqtt(TOPIC_ALERT, "MOTION front")
        self.assertEqual((m.kind, m.alert), ("ALERT", "MOTION"))
    def test_builders(self):
        self.assertEqual(build_arm(), "ARM"); self.assertEqual(build_disarm(), "DISARM")

class TestStore(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.store = Store(os.path.join(self.tmp.name, "t.db"),
                           red_list=["ABC123"], white_list=["KEVIN1"])
    def tearDown(self):
        self.store.close(); self.tmp.cleanup()
    def test_red_white(self):
        self.assertEqual(self.store.check_plate("ABC123"), "red")
        self.assertEqual(self.store.check_plate("KEVIN1"), "white")
        self.assertEqual(self.store.check_plate("XYZ789"), "unknown")
    def test_red_alert_white_silent(self):
        a = self.store.log_sighting("ABC123", "standard", 100, 21.3, -157.8)
        self.assertIsNotNone(a); self.assertEqual(a.level, "RED")
        a = self.store.log_sighting("KEVIN1", "custom", 65, 21.3, -157.8)
        self.assertIsNone(a)  # white list: logged, never alerted
        self.assertEqual(len(self.store.list_alerts()), 1)
    def test_following_three_locations(self):
        # same plate, 3 spots >100 m apart within 60 min
        for lat in (21.3000, 21.3020, 21.3040):
            a = self.store.log_sighting("XYZ789", "standard", 100, lat, -157.8)
        self.assertIsNotNone(a); self.assertEqual(a.level, "FOLLOWING")
    def test_following_same_spot_no_alert(self):
        for _ in range(3):
            a = self.store.log_sighting("XYZ789", "standard", 100, 21.3000, -157.8)
        self.assertIsNone(a)
    def test_csv_export(self):
        self.store.log_sighting("ABC123", "standard", 100, 21.3, -157.8)
        out = os.path.join(self.tmp.name, "out.csv")
        self.store.export_csv(out)
        with open(out) as f:
            self.assertIn("ABC123", f.read())

class TestSentryLpr(unittest.TestCase):
    def test_sentry_wake_and_idle(self):
        s = Sentry(store=None, idle_timeout_s=300)
        s.arm(); self.assertTrue(s.armed)
        r = s.on_wake("MOTION"); self.assertTrue(r["active"]); self.assertTrue(s.is_active)
        self.assertTrue(should_sleep(True, 80, active=False))
        self.assertFalse(should_sleep(True, 80, active=True))
        s.disarm(); self.assertFalse(s.armed)
    def test_lpr_stub_ocr(self):
        r = read_plate_from_image("/nonexistent.jpg", ocr=StubOcr("123A8C"))
        self.assertEqual(r.plate, "123ABC")
        self.assertIsNone(read_plate_from_image("/nonexistent.jpg", ocr=StubOcr("")))

if __name__ == "__main__":
    unittest.main()
