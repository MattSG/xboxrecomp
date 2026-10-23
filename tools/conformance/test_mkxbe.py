"""The checked-in memory/conformance fixture must remain reproducible synthetic data."""
import unittest
from pathlib import Path

from . import mkxbe


class SyntheticXbeTest(unittest.TestCase):
    def test_checked_in_fixture_matches_generator(self):
        self.assertEqual(Path(mkxbe.__file__).with_name("test.xbe").read_bytes(),
                         mkxbe.build())


if __name__ == "__main__":
    unittest.main()
