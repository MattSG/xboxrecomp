"""Self-check: direct calls to tail-only callees are lowered as tail calls.

sub_00095EB4 ends in an unconditional `jmp [edx+0x14]`, so the original
`call 0x95eb4` never returns to the call site.  In C, RECOMP_ITAIL returns to
the C caller, so the call site must immediately return instead of falling
through into the branch that original execution could only reach when the
call was skipped.

Run: py -3 tools/recomp/test_tail_only_call.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.lifter import Lifter  # noqa: E402


class _Insn:
    def __init__(self, target, address, size):
        self.call_target = target
        self.address = address
        self.size = size
        self.mnemonic = "call"
        self.operands = []


def test_tail_only_call_emits_return():
    lifter = Lifter(func_db={
        0x00095EB4: {"name": "sub_00095EB4", "size": 0x79, "end": 0x00095F2D},
    })
    insn = _Insn(target=0x00095EB4, address=0x00088B3C, size=5)
    lines = lifter._lift_call(insn, insn.operands)
    text = "\n".join(lines)
    assert "sub_00095EB4(); return;" in text, text
    assert "PUSH32(esp, 0); sub_00095EB4();" not in text, text
    print("ok  tail_only_call_emits_return")


if __name__ == "__main__":
    test_tail_only_call_emits_return()
    print("\nall passed")
