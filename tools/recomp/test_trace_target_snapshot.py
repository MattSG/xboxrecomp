"""Regression guard for bounded indirect-call trace target snapshots."""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.disasm import Disassembler  # noqa: E402
from tools.recomp.lifter import Lifter  # noqa: E402


def test_bcbcc0_trace_reuses_target_after_callback():
    """The END trace must not re-read EAX after the callback clobbers it."""
    insn = Disassembler().disassemble_function(
        bytes.fromhex("ff 50 0c c3"), 0x001BCC84, 0x001BCC88)[0]
    lifter = Lifter()
    lifter.func_start = 0x001BCBC0
    out = "\n".join(lifter._lift_call(insn, insn.operands))

    assert "_bcbcc0_target_001BCC84" in out, out
    assert "RECOMP_ICALL_SAFE(_bcbcc0_target_001BCC84" in out, out
    assert "recomp_trace_bcbcc0_icall(1, _bcbcc0_target_001BCC84" in out, out
    assert "recomp_trace_bcbcc0_icall(1, MEM32(eax + 0xC)" not in out, out


if __name__ == "__main__":
    test_bcbcc0_trace_reuses_target_after_callback()
    print("ok bcbcc0_trace_reuses_target_after_callback")
