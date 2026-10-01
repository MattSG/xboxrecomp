"""Run with python -m tools.seed_from_log.test_scan_log."""
import unittest
from .__main__ import scan_log


class ScanLogTest(unittest.TestCase):
    def test_both_runtime_failure_spellings(self):
        for wording in ("Failed resolve", "Failed to resolve"):
            with self.subTest(wording=wording):
                result = scan_log(
                    f"[ICALL] {wording} VA 0x002F9170 caller=0x002FA4DA")
                self.assertEqual(set(result), {0x002F9170})

    def test_data_target_is_not_seeded(self):
        self.assertEqual(scan_log(
            "[ICALL] non-code target 0x00392834 caller=0x001DB947"), {})


if __name__ == "__main__":
    unittest.main()
