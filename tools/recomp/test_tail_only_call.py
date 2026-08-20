"""Self-check: indirect-tail callees still get a normal return slot.

sub_00095EB4 ends in `jmp [edx+0x14]`, but the return address pushed by the
original `call` is still on the simulated stack and the tail target returns
through it. Lowering the call without the dummy return slot shifts every
[esp+N] argument by 4 bytes (observed as sub_00095EB4 reading arg=1 instead
of the D3DX unwind table).

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


def test_indirect_tail_callee_pushes_return_slot():
    lifter = Lifter(func_db={
        0x00095EB4: {"name": "sub_00095EB4", "size": 0x79, "end": 0x00095F2D},
    })
    insn = _Insn(target=0x00095EB4, address=0x00088B3C, size=5)
    lines = lifter._lift_call(insn, insn.operands)
    text = "\n".join(lines)
    assert "PUSH32(esp, 0); sub_00095EB4();" in text, text
    assert "sub_00095EB4(); return;" not in text, text
    print("ok  indirect_tail_callee_pushes_return_slot")


if __name__ == "__main__":
    test_indirect_tail_callee_pushes_return_slot()
    print("\nall passed")
