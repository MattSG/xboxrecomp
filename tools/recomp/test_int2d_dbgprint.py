"""
Regression guard for the Xbox DbgPrint idiom:

    mov eax, 1 ; int 0x2d ; int3

The pair is one debugger command (service 1 = DEBUG_PRINT). The original
returns normally after the trailing int3; it is not a real breakpoint. Before
the fix the lifter translated int3 to __debugbreak(), so MM3 sub_00084324
trapped immediately after its valid debug print and never reached the render
pump.

Run: py -3 tools/recomp/test_int2d_dbgprint.py
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


def test_int2d_followed_by_int3_has_no_debugbreak():
    # mov eax,1 ; int 0x2d ; int3 ; leave ; ret 4
    out = _lift(bytes.fromhex("b8 01 00 00 00 cd 2d cc c9 c2 04 00"))
    assert "xbox_int_0x2d" in out, out
    assert "__debugbreak" not in out, out
    assert "consumed by debug service" in out, out


def test_standalone_int3_still_debugbreaks():
    # int3 ; ret
    out = _lift(bytes.fromhex("cc c3"))
    assert "__debugbreak" in out, out


if __name__ == "__main__":
    test_int2d_followed_by_int3_has_no_debugbreak()
    test_standalone_int3_still_debugbreaks()
    print("all int2d dbgprint checks passed")
