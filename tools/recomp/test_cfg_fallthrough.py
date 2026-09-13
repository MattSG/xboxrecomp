"""Regression guard for conditional fall-through blocks.

0020F7EB's failure branch lands on ``xor al, al`` immediately before the
shared ``leave; ret`` block.  The C emitter must preserve that ordinary x86
fall-through when CFG emission order places the epilogue first.
"""

import os
import sys
import inspect

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.translator import FunctionTranslator  # noqa: E402


def test_20f7eb_failure_falls_through_to_epilogue():
    start = 0x0020F7EB
    code = bytes.fromhex(
        "55 8B EC 83 EC 1C FF 75 08 56 E8 83 FF FF FF 84 C0 74 37 "
        "68 68 38 3B 00 8D 4D E4 E8 DF 2D E0 FF FF 36 8D 45 E4 "
        "E8 EB 56 00 00 6A 01 8D 4D E4 E8 8A 2F E0 FF FF 36 "
        "66 33 C0 66 33 D2 E8 B1 56 00 00 66 85 C0 75 04 B0 01 "
        "EB 02 32 C0 C9 C2 04 00"
    )
    translator = FunctionTranslator(b"", {}, {})
    translator._read_func_bytes = lambda _start, _end: code
    out = translator.translate_function(
        start, {"end": start + len(code), "name": "sub_0020F7EB"}
    )

    assert out is not None
    failure = out.index("loc_0020F835:")
    assert "goto loc_0020F837; /* CFG fall-through */" in out[failure:]


def test_cfg_fallthrough_special_cases_include_888cf():
    """The Lua-stack helper has the same reordered fall-through shape."""
    source = inspect.getsource(FunctionTranslator.translate_function)
    assert "0x000888CF" in source
    assert "0x0024CA76" in source
    assert "0x00195341" in source


if __name__ == "__main__":
    test_20f7eb_failure_falls_through_to_epilogue()
    test_cfg_fallthrough_special_cases_include_888cf()
    print("ok  20f7eb_failure_falls_through_to_epilogue")
