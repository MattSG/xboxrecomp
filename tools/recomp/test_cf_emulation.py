"""
Self-check for carry-flag (CF) emulation across the ALU ops.

Run: py -3 tools/recomp/test_cf_emulation.py

Regression guard for the bug where only `neg` stored _cf, so any
`cmp; sbb`, `sub; sbb`, `add; adc`, or `and`-then-`adc` idiom read a stale
carry of 0. These idioms are pervasive in MM3 (64-bit arithmetic, pointer
math, boundary checks); a stale CF silently nulled computed values (observed
as sub_0027930D passing obj=0 into the DICE recursion).
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.disasm import Disassembler  # noqa: E402
from tools.recomp.lifter import Lifter  # noqa: E402


def _lift(code_bytes):
    ds = Disassembler()
    insns = ds.disassemble_function(code_bytes, 0x400000, 0x400000 + len(code_bytes))
    lifter = Lifter()
    return " ".join(s for block in [lifter.lift_instruction(i) for i in insns] for s in block)


def test_cmp_sets_cf_before_sbb():
    out = _lift(bytes.fromhex("39 d8 19 c9"))  # cmp eax, ebx; sbb ecx, ecx
    assert "_cf = ((uint32_t)_lhs < (uint32_t)_rhs)" in out, out
    assert "_result = _lhs - _lhs - _borrow" in out, out
    print("ok  cmp_sets_cf_before_sbb:", out.strip())


def test_sub_sets_cf_borrow():
    out = _lift(bytes.fromhex("29 d8 19 c9"))  # sub eax, ebx; sbb ecx, ecx
    assert "_cf = ((uint32_t)_lhs < (uint32_t)_rhs)" in out, out
    assert "eax = _result" in out, out
    assert "_result = _lhs - _lhs - _borrow" in out, out
    print("ok  sub_sets_cf_borrow:", out.strip())


def test_add_sets_cf_carry_into_adc():
    out = _lift(bytes.fromhex("03 c1 13 d2"))  # add eax, ecx; adc edx, edx
    assert "_cf = ((uint32_t)_result < (uint32_t)_lhs)" in out, out
    assert "_result = _lhs + _rhs + _carry" in out, out
    print("ok  add_sets_cf_carry_into_adc:", out.strip())


def test_xor_clears_cf():
    out = _lift(bytes.fromhex("33 c0 19 c9"))  # xor eax, eax; sbb ecx, ecx
    assert "_cf = 0;" in out, out
    assert "_result = _lhs - _lhs - _borrow" in out, out
    print("ok  xor_clears_cf:", out.strip())


def test_neg_sets_cf():
    out = _lift(bytes.fromhex("f7 d8 19 c0"))  # neg eax; sbb eax, eax
    assert "recomp_set_sub_flags(0, _src, 0, _result" in out, out
    assert "_result = _lhs - _lhs - _borrow" in out, out
    print("ok  neg_sets_cf:", out.strip())

def test_rotate_and_rep_are_not_placeholders():
    out = _lift(bytes.fromhex("d1 d0 d1 d8 f3 a4 f3 a6"))
    assert "_cf = _next" in out, out
    assert "while (ecx)" in out, out
    assert "recomp_set_sub_flags" in out, out
    print("ok  rotate_and_rep:", out.strip())


if __name__ == "__main__":
    test_cmp_sets_cf_before_sbb()
    test_sub_sets_cf_borrow()
    test_add_sets_cf_carry_into_adc()
    test_xor_clears_cf()
    test_neg_sets_cf()
    test_rotate_and_rep_are_not_placeholders()
    print("\nall cf-emulation checks passed")
