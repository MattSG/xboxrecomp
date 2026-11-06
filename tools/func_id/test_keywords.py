"""Application keyword policy is explicit and isolated between analyses."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from .clustering import propagate_labels

class KeywordPolicyTest(unittest.TestCase):
    def classify(self, keywords=None):
        return propagate_labels(
            [{"start": "0x1000"}], {}, {}, {0x2000: [0x1000]},
            [{"address": "0x2000", "string": "Throttle orbit"}],
            game_keywords=keywords)

    def test_default_disabled(self):
        self.assertEqual(self.classify(), {})
        self.assertEqual(self.classify({}), {})

    def test_explicit_policy_isolated(self):
        policy = {"space": ["orbit"]}
        result = self.classify(policy)[0x1000]
        self.assertEqual(result["category"], "game_space")
        self.assertEqual(result["method"], "string_keyword")
        self.assertEqual(result["confidence"], 0.60)
        self.assertEqual(policy, {"space": ["orbit"]})
        self.assertEqual(self.classify(), {})
        self.assertEqual(self.classify({"other": ["absent"]}), {})

    def test_malformed_policy(self):
        for policy in ([], "orbit", {"": ["orbit"]}, {3: ["orbit"]},
                       {"space": "orbit"}, {"space": [42]},
                       {"space": [""]}, {"space": ["Orbit"]}):
            with self.subTest(policy=policy), self.assertRaises(ValueError):
                self.classify(policy)

    def test_cli_forwards_json(self):
        from . import __main__ as cli
        policy = {"space": ["orbit"]}
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "keywords.json"
            path.write_text(json.dumps(policy), encoding="utf-8")
            with patch.object(cli, "run", return_value={}) as run:
                with patch("sys.argv", ["func_id", "input.xbe", "--game-keywords", str(path)]):
                    cli.main()
                self.assertEqual(run.call_args.kwargs["game_keywords"], policy)
                with patch("sys.argv", ["func_id", "input.xbe"]):
                    cli.main()
                self.assertIsNone(run.call_args.kwargs["game_keywords"])

if __name__ == "__main__":
    unittest.main()
