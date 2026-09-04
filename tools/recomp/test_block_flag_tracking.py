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


def _lift(code_bytes, flag_state=None, snap_counter=None, start=0x400000):
    lifter = Lifter()
    stmts, out_state = lift_basic_block(
        lifter, _block(code_bytes, start=start), flag_state=flag_state,
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


def test_cmp_jcc_snapshot_precedes_branch_for_taken_path():
    counter = [0]
    out1, fs = _lift(bytes.fromhex("85 c0 7d 0a"), snap_counter=counter)
    # test eax, eax; jge +0xa: the snapshot must be emitted before the
    # conditional goto so the taken-path successor sees initialized flag
    # temporaries. If it is emitted after, a jcc in the successor reads
    # uninitialized locals whenever the jge is taken.
    assert out1.index("_fcmp_0_a") < out1.index("CMP_GE"), out1
    out2, _ = _lift(bytes.fromhex("75 13"), start=0x40000C, flag_state=fs,
                    snap_counter=counter)
    assert "TEST_NZ(_fcmp_0_a, _fcmp_1_b)" in out2, out2
    print("ok  cmp_jcc_snapshot_precedes_branch_for_taken_path")


def test_cmp_si_neg1_masks_imm_to_reg_width():
    out, _ = _lift(bytes.fromhex("66 83 fe ff 75 04"))
    # cmp si, -1; jne +4: Capstone sign-extends the imm8 to 0xFFFFFFFF,
    # but the flag math runs at 16-bit width. The condition must compare
    # against 0xFFFF and the snapshot temp must hold 0xFFFF, or the
    # CMP_NE is always true and the equal path never fires.
    assert "CMP_NE(LO16(esi), 0xFFFF)" in out, out
    assert "= 0xFFFF;" in out, out
    assert "0xFFFFFFFFu" not in out, out
    print("ok  cmp_si_neg1_masks_imm_to_reg_width")


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


def _successor_lift(blocks_by_addr, succ_map, start):
    """Reproduce FunctionTranslator.translate_function's CFG-successor flag
    propagation (translator.py, incoming_flag_states/pending loop) without
    the full XBE/func_db plumbing that method needs. Same contract: each
    block's incoming flag_state comes from whichever block's `successors`
    actually names it, not from whatever block precedes it in `blocks_by_addr`
    iteration order."""
    lifter = Lifter()
    incoming = {start: None}
    lifted = {}
    pending = [start]
    while pending:
        addr = pending.pop()
        if addr in lifted or addr not in blocks_by_addr:
            continue
        bb = blocks_by_addr[addr]
        stmts, out_state = lift_basic_block(lifter, bb, flag_state=incoming.get(addr))
        lifted[addr] = "\n".join(stmts)
        for succ in reversed(succ_map.get(addr, [])):
            if succ not in incoming:
                incoming[succ] = out_state
            pending.append(succ)
    return lifted


def test_successor_driven_propagation_ignores_address_order_sibling():
    """Regression guard for the bug fixed alongside the CFG-successor
    rewrite in translate_function: flag state used to propagate from
    whichever block was emitted immediately before another in `blocks`
    (address order), not from its real CFG predecessor. A block address-
    between a branch and its real target, but not actually on that edge,
    must not donate its flags to the target.

    Layout: A (cmp eax,ebx) branches to C directly. B sits at an address
    between A and C, sets *different* flags (cmp ecx,edx), and is not on
    any path to C. C has no flag-setting instruction of its own and must
    see A's eax/ebx comparison, never B's ecx/edx one."""
    a_addr, b_addr, c_addr = 0x1000, 0x1010, 0x2000
    a_bb = _block(bytes.fromhex("39 d8"), start=a_addr)          # cmp eax, ebx
    b_bb = _block(bytes.fromhex("39 d1"), start=b_addr)          # cmp ecx, edx (dead end)
    c_bb = _block(bytes.fromhex("b8 10 00 00 00 75 f0"), start=c_addr)  # mov eax,0x10; jne

    blocks_by_addr = {a_addr: a_bb, b_addr: b_bb, c_addr: c_bb}
    succ_map = {a_addr: [c_addr], b_addr: []}  # B does not lead to C

    lifted = _successor_lift(blocks_by_addr, succ_map, a_addr)
    c_out = lifted[c_addr]
    assert "CMP_NE(_fcmp_0_a, _fcmp_1_b)" in c_out, c_out  # A's snapshot
    assert "CMP_NE(ecx, edx)" not in c_out, c_out
    assert "CMP_NE(eax, ebx)" not in c_out, c_out  # must use snapshot, not live regs
    print("ok  successor_driven_propagation_ignores_address_order_sibling")


if __name__ == "__main__":
    test_jcc_uses_snapshot_not_clobbered_operand()
    test_setcc_uses_snapshot()
    test_cmovcc_uses_snapshot()
    test_second_consumer_after_pair_uses_snapshot()
    test_cmp_jcc_snapshot_precedes_branch_for_taken_path()
    test_flag_state_crosses_block_boundary_with_snapshot()
    test_cmp_si_neg1_masks_imm_to_reg_width()
    test_successor_driven_propagation_ignores_address_order_sibling()
    print("\nall block flag-tracking checks passed")
