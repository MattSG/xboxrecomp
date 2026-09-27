"""BatchTranslator must refresh the lifter's derived SEH helper set."""

import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from . import translator


PROLOG = 0x00094FC0
EPILOG = 0x00094FFB


class FakeFunctionTranslator:
    def __init__(self, *args, **kwargs):
        self.lifter = SimpleNamespace(
            SEH_PROLOGS=frozenset(), SEH_PROLOG=None,
            SEH_EPILOG=None, SEH_HELPERS=frozenset())
        self.protected_function_starts = set()

    def discover_static_indirect_targets(self):
        pass

    def discover_cfg_ownership(self):
        pass


class SehHelpersTest(unittest.TestCase):
    def test_batch_translator_refreshes_prolog_and_epilog_helpers(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            xbe = root / "game.xbe"
            functions = root / "functions.json"
            xbe.write_bytes(b"xbe")
            functions.write_text(json.dumps([{
                "start": "0x00011000", "end": "0x00011001", "size": 1,
            }]), encoding="utf-8")

            with patch.object(translator, "FunctionTranslator",
                              FakeFunctionTranslator), \
                    patch.object(translator, "xbe_title", return_value="test"), \
                    patch.object(translator, "detect_setjmp_helpers",
                                 return_value=(None, None)):
                batch = translator.BatchTranslator(
                    str(xbe), str(functions),
                    seh_prolog=PROLOG, seh_epilog=EPILOG)

        self.assertEqual(batch.translator.lifter.SEH_HELPERS,
                         frozenset((PROLOG, EPILOG)))


if __name__ == "__main__":
    unittest.main()
