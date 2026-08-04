"""
Self-check for bsf/bsr (bit scan) lifting.

Run: py -3 tools/recomp/test_bsf_bsr.py

Regression guard for the bug where bsf/bsr fell through to the "TODO" stub.
The bitmap allocator's block-scan helper (e.g. "bsf eax, ecx; ret") then
lifted to a function that returned without setting eax, so the caller
computed a garbage bucket pointer and the free-list unlink wrote through it.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.disasm import Disassembler  # noqa: E402
from tools.recomp.lifter import Lifter  # noqa: E402


def _lift(code_bytes):
    """Disassemble with the project Disassembler and lift each instruction."""
    ds = Disassembler()
    insns = ds.disassemble_function(code_bytes, 0x400000, 0x400000 + len(code_bytes))
    lifter = Lifter()
    return [lifter.lift_instruction(i) for i in insns]


def test_bsf_emits_bsf32():
    stmts = _lift(bytes.fromhex("0fbc c1"))  # bsf eax, ecx
    out = " ".join(s for block in stmts for s in block)
    assert "BSF32(ecx)" in out, out
    assert "TODO" not in out, out
    print("ok  bsf_emits_bsf32:", out.strip())


def test_bsr_emits_bsr32():
    stmts = _lift(bytes.fromhex("0fbd c1"))  # bsr eax, ecx
    out = " ".join(s for block in stmts for s in block)
    assert "BSR32(ecx)" in out, out
    assert "TODO" not in out, out
    print("ok  bsr_emits_bsr32:", out.strip())


def test_bsf_jcc_uses_source_zero():
    """The jcc after bsf must branch on the source being zero (ZF)."""
    from tools.recomp.lifter import _make_condition
    import types
    cond, _ = _make_condition(
        "jne", "bsf",
        [types.SimpleNamespace(type="reg", reg="ecx"),
         types.SimpleNamespace(type="reg", reg="edx")])
    assert cond == "(edx != 0)", cond
    cond, _ = _make_condition(
        "je", "bsf",
        [types.SimpleNamespace(type="reg", reg="ecx"),
         types.SimpleNamespace(type="reg", reg="edx")])
    assert cond == "(edx == 0)", cond
    print("ok  bsf_jcc_uses_source_zero")


def test_tiny_bsf_helper_is_no_longer_a_stub():
    """sub_00087950 ('bsf eax, ecx; ret') must lift to a real scan, not a TODO."""
    stmts = _lift(bytes.fromhex("0fbc c1 c3"))  # bsf eax, ecx; ret
    out = " ".join(s for block in stmts for s in block)
    assert "BSF32(ecx)" in out, out
    assert "TODO" not in out, out
    print("ok  tiny_bsf_helper_is_no_longer_a_stub")


if __name__ == "__main__":
    test_bsf_emits_bsf32()
    test_bsr_emits_bsr32()
    test_bsf_jcc_uses_source_zero()
    test_tiny_bsf_helper_is_no_longer_a_stub()
    print("\nall passed")
