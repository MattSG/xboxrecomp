"""Self-check: direct calls to return-address readers push the real guest RA.

sub_00095B8C saves its own return address from [esp] into a D3DX jump
context (+0x14). The generic direct-call lowering pushes a dummy zero, so the
context stores 0 and the later sub_00095EB4 tail jumps to address zero.

Run: py -3 tools/recomp/test_return_address_call.py
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


def test_call_to_return_address_reader_pushes_real_ra():
    lifter = Lifter(func_db={
        0x00095B8C: {"name": "sub_00095B8C", "size": 0x7B, "end": 0x00095C07},
    })
    insn = _Insn(target=0x00095B8C, address=0x0025E298, size=5)
    lines = lifter._lift_call(insn, insn.operands)
    text = "\n".join(lines)
    assert "PUSH32(esp, 0x0025E29D); sub_00095B8C();" in text, text
    assert "PUSH32(esp, 0); sub_00095B8C();" not in text, text
    print("ok  call_to_return_address_reader_pushes_real_ra")


def test_ordinary_call_still_pushes_dummy_zero():
    lifter = Lifter(func_db={
        0x00123456: {"name": "sub_00123456", "size": 0x10, "end": 0x00123466},
    })
    insn = _Insn(target=0x00123456, address=0x00100000, size=5)
    lines = lifter._lift_call(insn, insn.operands)
    text = "\n".join(lines)
    assert "PUSH32(esp, 0); sub_00123456();" in text, text
    print("ok  ordinary_call_still_pushes_dummy_zero")


if __name__ == "__main__":
    test_call_to_return_address_reader_pushes_real_ra()
    test_ordinary_call_still_pushes_dummy_zero()
    print("\nall passed")
