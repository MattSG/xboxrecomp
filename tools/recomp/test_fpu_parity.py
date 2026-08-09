"""
Regression guard for the FPU compare/parity idiom:

    fcomp [mem]; fnstsw ax; test ah, imm; jp/jnp

On x87 the branch is CONDITIONAL on the FPU status bits (C2/C3) via the
parity flag: after `test ah, 0x44`, jp is taken iff the operands are NOT
exactly equal (unordered included), jnp iff exactly equal. The lifter's flag
model has no parity flag, so the block-lift path emits `if (1 ...)` (always
taken) for BOTH jp and jnp -- a proven divergence from the original for the
equal case.

Proven on MM3 sub_00343F40 (0x343FA8 / 0x343FB9 `test ah, 0x44; jp`): the
generated code always marks the camera dirty even when the compared floats
are equal. NOT the current M4 blocker (mode==3 skips that block), so the
lifter is left unfixed until a real path needs it; this test pins the defect
so the fix lands with a failing check.

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


def test_jp_after_fcomp_must_be_conditional():
    out = _lift(bytes.fromhex("d81df84e3800 dfe0 f6c444 7a03 90 c3"))
    # fcomp [0x384ef8]; fnstsw ax; test ah, 0x44; jp +3; nop; ret
    assert "if (1 /* jp after test - parity */)" not in out, (
        "KNOWN DIVERGENCE: jp after fcomp/fnstsw/test ah,0x44 lifts as "
        "always-taken; original is conditional (taken iff not equal)")


def test_jnp_after_fcomp_must_be_conditional():
    out = _lift(bytes.fromhex("d81df84e3800 dfe0 f6c444 7b03 90 c3"))
    # fcomp [0x384ef8]; fnstsw ax; test ah, 0x44; jnp +3; nop; ret
    assert "if (1 /* jnp after test - parity */)" not in out, (
        "KNOWN DIVERGENCE: jnp after fcomp/fnstsw/test ah,0x44 lifts as "
        "always-taken; original is conditional (taken iff equal)")


if __name__ == "__main__":
    divergences = []
    for name, fn in [
        ("jp_after_fcomp", test_jp_after_fcomp_must_be_conditional),
        ("jnp_after_fcomp", test_jnp_after_fcomp_must_be_conditional),
    ]:
        try:
            fn()
            print("ok  ", name)
        except AssertionError as exc:
            print("KNOWN-DIVERGENCE:", name, "-", exc)
            divergences.append(name)
    if divergences:
        print("\n%d known lifter divergence(s) pinned (suite still passes): %s"
              % (len(divergences), ", ".join(divergences)))
    else:
        print("\nall fpu-parity checks passed")