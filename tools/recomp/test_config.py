import json
import os
import unittest
from pathlib import Path

from . import config


class ConfigTest(unittest.TestCase):
    def test_analysis_sections_are_all_mapped(self):
        configured = os.environ.get("XBOXRECOMP_ANALYSIS_JSON")
        analysis_path = Path(configured) if configured else Path(__file__).parents[4] / "game_files" / "midtown_analysis.json"
        if not analysis_path.exists():
            self.skipTest("set XBOXRECOMP_ANALYSIS_JSON to run the title-fixture mapping test")
        analysis = json.loads(analysis_path.read_text(encoding="utf-8"))
        config.configure(analysis_path)

        self.assertEqual(len(config.SECTIONS), len(analysis["sections"]))
        self.assertEqual(config.TEXT_VA_END, 0x00259180)
        self.assertTrue(config.is_code_address(0x00259180))  # D3DX
        self.assertFalse(config.is_code_address(0x00361F00))  # .rdata
        self.assertTrue(config.is_data_address(0x00391F00))  # .data


if __name__ == "__main__":
    unittest.main()
