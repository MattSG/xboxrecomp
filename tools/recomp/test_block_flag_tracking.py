"""
Self-check for flag tracking in the basic-block lift path.

Run: py -3 tools/recomp/test_block_flag_tracking.py

Regression guard for the bug where a jcc/setcc/cmovcc consumer re-derived its
condition from the *current* register/memory values at the consumer, instead
of the values present when the flags were set. `cmp eax, ebx; mov eax, X;
jne` must test the pre-mov eax against ebx; the block-lift path snapshots the
flag setter's operands into temporaries so intervening instructions cannot
corrupt the condition. Per-instruction lifting is unaffected (it does not use
the block-lift flag state).
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.disasm import Disassembler, BasicBlock  # noqa: E402
from tools.recomp.lifter import Lifter, lift_basic_block  # noqa: E402


def _block(code_bytes, start=0x400000):
    ds = Disassembler()
    insns = ds.disassemble_function(code_bytes, start, start + len(code_bytes))
    return BasicBlock(start=start, instructions=insns, successors=[])


def _lift(code_bytes, flag_state=None, snap_counter=None):
    lifter = Lifter()
    stmts, out_state = lift_basic_block(
        lifter, _block(code_bytes), flag_state=flag_state,
        snap_counter=snap_counter)
    return "\n".join(stmts), out_state


def test_jcc_uses_snapshot_not_clobbered_operand():
    out, _ = _lift(bytes.fromhex("39 d8 b8 10 00 00 00 75 f0"))
    # cmp eax, ebx; mov eax, 0x10; jne target
    assert "uint32_t _fcmp_" in out, out
    assert "CMP_NE(_fcmp_0_a, _fcmp_1_b)" in out, out
    assert "CMP_NE(eax, ebx)" not in out, out
    print("ok  jcc_uses_snapshot_not_clobbered_operand")


def test_setcc_uses_snapshot():
    out, _ = _lift(bytes.fromhex("39 d8 b8 10 00 00 00 0f 95 c1"))
    # cmp eax, ebx; mov eax, 0x10; setne cl
    assert "CMP_NE(_fcmp_0_a, _fcmp_1_b)) ? 1 : 0" in out, out
    print("ok  setcc_uses_snapshot")


def test_cmovcc_uses_snapshot():
    out, _ = _lift(bytes.fromhex("39 d8 b8 10 00 00 00 0f 45 ca"))
    # cmp eax, ebx; mov eax, 0x10; cmovne ecx, edx
    assert "if (CMP_NE(_fcmp_0_a, _fcmp_1_b))" in out, out
    print("ok  cmovcc_uses_snapshot")


def test_second_consumer_after_pair_uses_snapshot():
    out, _ = _lift(bytes.fromhex("85 c0 77 0a 72 14"))
    # test eax, eax; ja X; jb Y  (jb reuses flags from the test)
    assert "CMP_NE(_fcmp_" not in out or "_fcmp_0_a" in out, out
    assert "_fcmp_0_a" in out, out
    print("ok  second_consumer_after_pair_uses_snapshot")


def test_flag_state_crosses_block_boundary_with_snapshot():
    counter = [0]
    out1, fs = _lift(bytes.fromhex("39 d8"), snap_counter=counter)  # cmp eax, ebx
    assert "_fcmp_0_a" in out1, out1
    out2, _ = _lift(bytes.fromhex("b8 10 00 00 00 74 f0"), flag_state=fs,
                    snap_counter=counter)
    # mov eax, 0x10; je target -- must use block1's snapshot, not live eax
    assert "CMP_EQ(_fcmp_0_a, _fcmp_1_b)" in out2, out2
    assert "CMP_EQ(eax, ebx)" not in out2, out2
    print("ok  flag_state_crosses_block_boundary_with_snapshot")


if __name__ == "__main__":
    test_jcc_uses_snapshot_not_clobbered_operand()
    test_setcc_uses_snapshot()
    test_cmovcc_uses_snapshot()
    test_second_consumer_after_pair_uses_snapshot()
    test_flag_state_crosses_block_boundary_with_snapshot()
    print("\nall block flag-tracking checks passed")
