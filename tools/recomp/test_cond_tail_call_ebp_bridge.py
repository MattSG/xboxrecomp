"""
Self-check for the conditional tail-call ebp bridge.

Run: py -3 tools/recomp/test_cond_tail_call_ebp_bridge.py

Regression guard for sub_001E839C -> sub_001E83BB: a conditional jump to an
external target is lifted as `{ sub_X(); return; }` (a tail call). The target
is often a split piece of the same original function and starts with
"ebp = g_seh_ebp", so it must inherit the *current* function's frame. Without
"g_seh_ebp = ebp;" the target read a stale outer frame, its `esp = ebp`
epilog restored garbage, and guest esp wrapped (0x00F7FE44 -> 0xFFFFFF80).
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.disasm import Disassembler, BasicBlock  # noqa: E402
from tools.recomp.lifter import Lifter, lift_basic_block  # noqa: E402

FUNC_START = 0x00400000
FUNC_END = 0x00400010
EXT_TARGET = 0x00500000


def _lift(code_bytes):
    ds = Disassembler()
    insns = ds.disassemble_function(
        code_bytes, FUNC_START, FUNC_START + len(code_bytes))
    lifter = Lifter()
    lifter.func_start = FUNC_START
    lifter.func_end = FUNC_END
    stmts, _ = lift_basic_block(lifter, BasicBlock(
        start=FUNC_START, instructions=insns, successors=[]))
    return "\n".join(stmts)


def test_cmp_jcc_external_bridges_ebp():
    # cmp eax, 3; je EXT_TARGET   (target outside the function)
    out = _lift(bytes.fromhex("83 f8 03 0f 84" +
                              (EXT_TARGET - (FUNC_START + 9)).to_bytes(4, "little", signed=True).hex()))
    assert "g_seh_ebp = ebp; sub_00500000(); return;" in out, out
    print("ok  cmp_jcc_external_bridges_ebp")


def test_jecxz_external_bridges_ebp():
    # jecxz 0x400080 (e3 rel8), target outside the function
    rel = (0x400080 - (FUNC_START + 2)) & 0xFF
    out = _lift(bytes.fromhex("e3 %02x" % rel))
    assert "g_seh_ebp = ebp; sub_00400080(); return;" in out, out
    print("ok  jecxz_external_bridges_ebp")


def test_internal_jcc_stays_goto():
    # je +4 (inside the function): must remain a goto, no tail call
    out = _lift(bytes.fromhex("83 f8 03 74 02 90 90"))
    assert "goto loc_00400007" in out, out
    assert "sub_" not in out.replace("sub_", ""), out
    print("ok  internal_jcc_stays_goto")


if __name__ == "__main__":
    test_cmp_jcc_external_bridges_ebp()
    test_jecxz_external_bridges_ebp()
    test_internal_jcc_stays_goto()
    print("all ok")
