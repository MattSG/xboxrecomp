"""The MM3 startup worker must leave its caller's EBX intact."""

import unittest

from . import config
from .translator import FunctionTranslator


BASE = 0x001E7B41
RETURN = bytes.fromhex("C3")

_CONFIG_GLOBALS = (
    "_SECTIONS", "SECTIONS", "_configured_from",
    "TEXT_VA_START", "TEXT_VA_END", "RDATA_VA_START", "RDATA_VA_END",
    "DATA_VA_START", "DATA_VA_END", "KERNEL_THUNK_ADDR", "ENTRY_POINT",
)


class StartupWorkerEbxPreserveTest(unittest.TestCase):
    def setUp(self):
        self._saved = {key: getattr(config, key) for key in _CONFIG_GLOBALS}
        config._install(
            [config.Section(".text", BASE, len(RETURN), 0, len(RETURN), True)],
            entry_point=BASE, kernel_thunk_addr=BASE,
            origin="startup-worker-ebx-test")

    def tearDown(self):
        for key, value in self._saved.items():
            setattr(config, key, value)

    def test_every_return_restores_entry_ebx(self):
        info = {"start": f"0x{BASE:08X}", "end": BASE + len(RETURN),
                "_addr": BASE, "size": len(RETURN)}
        code = FunctionTranslator(RETURN, {BASE: info}).translate_function(
            BASE, info)

        self.assertIn("uint32_t _saved_ebx = ebx;", code)
        self.assertIn("ebx = _saved_ebx; return;", code)


if __name__ == "__main__":
    unittest.main()
