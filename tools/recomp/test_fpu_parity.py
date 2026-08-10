"""
Regression guard for the FPU compare/parity idiom:

    fcomp [mem]; fnstsw ax; test ah, imm; jcc

The branch is CONDITIONAL on the x87 status bits stored in AH by fnstsw
(C0=AH0, C1=AH1, C2=AH2, C3=AH6). The lifter has no parity flag and no AH
model, so before the fix the block-lift path emitted `if (1 ...)` (always
taken) for both jp and jnp, and je/jne read a stale C register via
`TEST_*(HI8(eax), imm)` -- all diverging from the original.

Correct semantics (fcomp ST0 vs operand; PF = parity of AH & imm; jp/jnp
jump on even/odd parity):
  - mask 0x44 (C3|C2): PF=1 iff C3==C2, i.e. ST>op, ST<op or unordered --
    jp is taken iff NOT equal; jnp iff equal. The pre-2026-08-11 pinned
    tests had jp/jnp swapped (claimed jp == equal), which inverted the
    MM3 sub_00343F40 flip-gate: `fcomp [0x384ef8(=1.0)]; test ah,0x44; jp`
    set dev+8 bit 0x4000 when the frame was READY (ST==1.0) and cleared it
    otherwise -- the stuck-0x4000 gate. Proven from the raw XBE: the
    constant at 0x384ef8 is 1.0f.
  - mask 0x41 (C3|C0): jp taken iff ST>op or unordered; jne iff
    (AH&0x41)!=0, i.e. less-or-equal (unordered included) -- the shape seen
    in MM3 sub_00214EB9.

The lifter resolves these against _fpu_cmp (0 = equal/unordered, +/-1 =
ordered less/greater). jp/jnp after 0x41/0x44 diverge from the original for
unordered/NaN only (both status bits set -> even parity -> PF=1), the same
limitation the fcomi path already has.

Proven on MM3 sub_00343F40 (0x343FA8 / 0x343FB9 `test ah, 0x44; jp`) and
sub_00214EB9 (0x00214EC9 `test ah, 0x41; jne`).

Run: py -3 tools/recomp/test_fpu_parity.py
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.disasm import Disassembler, BasicBlock  # noqa: E402
from tools.recomp.lifter import Lifter, lift_basic_block  # noqa: E402


def _lift(code_bytes):
    ds = Disassembler()
    insns = ds.disassemble_function(code_bytes, 0x400000, 0x400000 + len(code_bytes))
    blk = BasicBlock(start=0x400000, instructions=insns, successors=[])
    lifter = Lifter()
    stmts, _ = lift_basic_block(lifter, blk, flag_state=None, snap_counter=None)
    return "\n".join(stmts)


def test_jp_after_fcomp_0x44_is_not_equal():
    out = _lift(bytes.fromhex("d81df84e3800 dfe0 f6c444 7a03 90 c3"))
    # fcomp [0x384ef8]; fnstsw ax; test ah, 0x44; jp +3; nop; ret
    assert "if (_fpu_cmp != 0)" in out, (
        "jp after fcomp/fnstsw/test ah,0x44 must be taken iff not equal")
    assert "if (1 " not in out, "no unconditional parity branches allowed"


def test_jnp_after_fcomp_0x44_is_equal():
    out = _lift(bytes.fromhex("d81df84e3800 dfe0 f6c444 7b03 90 c3"))
    # fcomp [0x384ef8]; fnstsw ax; test ah, 0x44; jnp +3; nop; ret
    assert "if (_fpu_cmp == 0)" in out, (
        "jnp after fcomp/fnstsw/test ah,0x44 must be taken iff equal")


def test_jp_after_fcomp_0x41_is_greater():
    out = _lift(bytes.fromhex("d81df84e3800 dfe0 f6c441 7a03 90 c3"))
    # fcomp [0x384ef8]; fnstsw ax; test ah, 0x41; jp +3; nop; ret
    assert "if (_fpu_cmp > 0)" in out, (
        "jp after fcomp/fnstsw/test ah,0x41 must be taken iff ST > operand")


def test_jnp_after_fcomp_0x41_is_less_equal():
    out = _lift(bytes.fromhex("d81df84e3800 dfe0 f6c441 7b03 90 c3"))
    # fcomp [0x384ef8]; fnstsw ax; test ah, 0x41; jnp +3; nop; ret
    assert "if (_fpu_cmp <= 0)" in out, (
        "jnp after fcomp/fnstsw/test ah,0x41 must be taken iff ST <= operand")
    assert "if (1 " not in out, "no unconditional parity branches allowed"


def test_jne_after_fcomp_0x41_is_less_equal():
    out = _lift(bytes.fromhex("d81d40613800 dfe0 f6c441 7503 90 c3"))
    # fcomp [0x386140]; fnstsw ax; test ah, 0x41; jne +3; nop; ret
    # MM3 sub_00214EB9 shape: original jne is taken iff (AH&0x41) != 0,
    # i.e. less-or-equal (or unordered); must NOT read stale HI8(eax).
    assert "if (_fpu_cmp <= 0)" in out, (
        "jne after fcomp/fnstsw/test ah,0x41 must resolve via _fpu_cmp")
    assert "if (HI8(eax)" not in out, (
        "branch must not read the stale C register for the FPU status")


if __name__ == "__main__":
    failed = []
    for name, fn in [
        ("jp_after_fcomp_0x44", test_jp_after_fcomp_0x44_is_not_equal),
        ("jnp_after_fcomp_0x44", test_jnp_after_fcomp_0x44_is_equal),
        ("jp_after_fcomp_0x41", test_jp_after_fcomp_0x41_is_greater),
        ("jnp_after_fcomp_0x41", test_jnp_after_fcomp_0x41_is_less_equal),
        ("jne_after_fcomp_0x41", test_jne_after_fcomp_0x41_is_less_equal),
    ]:
        try:
            fn()
            print("ok  ", name)
        except AssertionError as exc:
            print("FAIL:", name, "-", exc)
            failed.append(name)
    if failed:
        print("\n%d fpu-parity check(s) FAILED: %s" % (len(failed), ", ".join(failed)))
        sys.exit(1)
    print("\nall fpu-parity checks passed")
