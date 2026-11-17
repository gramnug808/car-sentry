import os, sys, unittest
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from car_sentry.plates import repair_plate, candidates, is_structured

class TestPlates(unittest.TestCase):
    def test_standard_exact(self):
        r = repair_plate("ABC123"); self.assertEqual((r.plate, r.kind, r.score), ("ABC123", "standard", 100))
    def test_reversed_exact_first_class(self):
        r = repair_plate("123ABC"); self.assertEqual((r.plate, r.kind), ("123ABC", "reversed"))
    def test_reversed_8_repairs_to_B_not_C(self):
        self.assertEqual(repair_plate("123A8C").plate, "123ABC")
        self.assertEqual(repair_plate("123AB8").plate, "123ABB")
    def test_split_tokens_joined(self):
        self.assertEqual(repair_plate("ABC 123").plate, "ABC123")
        self.assertEqual(repair_plate("123 ABC").plate, "123ABC")
        self.assertEqual(repair_plate("A B C 1 2 3").plate, "ABC123")
    def test_positional_repairs(self):
        self.assertEqual(repair_plate("ABCI23").plate, "ABC123")   # I->1 digit slot
        self.assertEqual(repair_plate("A8C123").plate, "ABC123")   # 8->B letter slot
        self.assertEqual(repair_plate("ABC12O").plate, "ABC120")   # O->0 digit slot
        self.assertEqual(repair_plate("ABC1Z3").plate, "ABC123")   # Z->2 digit slot
        self.assertEqual(repair_plate("A5C123").plate, "ASC123")   # 5->S letter slot
    def test_custom(self):
        r = repair_plate("KEVIN1"); self.assertEqual((r.plate, r.kind), ("KEVIN1", "custom"))
        r = repair_plate("ALOHA"); self.assertEqual((r.plate, r.kind), ("ALOHA", "custom"))
        self.assertLess(repair_plate("ALOHA").score, repair_plate("KEVIN1").score)
    def test_structured_outranks_custom(self):
        self.assertEqual(repair_plate("ABC123").kind, "standard")
        self.assertGreater(repair_plate("ABC123").score, repair_plate("KEVIN1").score)
    def test_garbage_none(self):
        self.assertIsNone(repair_plate(""))
        self.assertIsNone(repair_plate("!!!"))
        self.assertIsNone(repair_plate("1234567"))  # all digits, no letter -> no custom
    def test_is_structured(self):
        self.assertTrue(is_structured("ABC123")); self.assertTrue(is_structured("123ABC"))
        self.assertFalse(is_structured("KEVIN1"))

if __name__ == "__main__":
    unittest.main()
