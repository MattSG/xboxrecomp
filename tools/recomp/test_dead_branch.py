"""
Self-check for the dead-branch marker.

Run: py -3 tools/recomp/test_dead_branch.py

A conditional jump whose target label is outside its function used to be
emitted as `(void)0; /* goto ... dead code */`, so a reached branch fell
through with no trace. It is now RECOMP_DEAD_BRANCH(0x...), which the runtime
header defines to log the first hit per target and still fall through.
"""

import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.test_fallthrough import _setup, _func_db, _translate, BASE  # noqa: E402

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
HEADER = os.path.join(ROOT, "templates", "runtime", "recomp_types.h")

# 3C 01        cmp al, 1
# 74 01        je  -> BASE+5, the middle of the mov below (not a label in this function)
# B8 00 00 00 00  mov eax, 0
# C3           ret
IMAGE = bytes.fromhex("3C 01 74 01 B8 00 00 00 00 C3")


def test_translator_emits_dead_branch_marker():
    _setup(IMAGE)
    db = _func_db([(BASE, len(IMAGE))])
    c = _translate(IMAGE, db, BASE)
    assert "RECOMP_DEAD_BRANCH(0x00010005u); /* goto loc_00010005 - label not in function */" in c, c
    assert "(void)0; /* goto" not in c, c
    print("ok  translator_emits_dead_branch_marker")


def test_runtime_header_defines_overridable_macro():
    with open(HEADER, encoding="utf-8") as f:
        text = f.read()
    assert len(re.findall(r"^#define RECOMP_DEAD_BRANCH\(", text, re.M)) == 1, "macro not defined once"
    guard = text.index("#ifndef RECOMP_DEAD_BRANCH")
    assert guard < text.index("#define RECOMP_DEAD_BRANCH(target)"), "default must sit inside the #ifndef"
    assert "static inline void recomp_dead_branch_log(uint32_t target)" in text
    print("ok  runtime_header_defines_overridable_macro")


if __name__ == "__main__":
    test_translator_emits_dead_branch_marker()
    test_runtime_header_defines_overridable_macro()
    print("\nall passed")
