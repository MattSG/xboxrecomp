"""Self-check: guest setjmp/longjmp lowering for sub_95B8C/sub_95EB4.

The D3DX unwind pair uses guest setjmp (sub_95B8C) and longjmp
(sub_95EB4).  The generated C must wrap setjmp in host setjmp/longjmp so the
longjmp target returns to the saved continuation instead of the generated
caller of sub_95EB4.

Run: py -3 tools/recomp/test_guest_setjmp_longjmp.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.lifter import Lifter  # noqa: E402


class _Mem:
    def __init__(self):
        self.type = "mem"
        self.mem_base = "edx"
        self.mem_index = None
        self.mem_disp = 0x14
        self.mem_size = 4
        self.mem_segment = None


class _Insn:
    def __init__(self, target=None, address=0, size=5):
        self.call_target = target
        self.jump_target = None
        self.address = address
        self.size = size
        self.mnemonic = "call" if target is not None else "jmp"
        self.operands = []


def test_setjmp_call_is_host_wrapped():
    lifter = Lifter(func_db={
        0x00095B8C: {"name": "sub_00095B8C", "size": 0x7B, "end": 0x00095C07},
    })
    lifter.uses_ebp = True
    insn = _Insn(target=0x00095B8C, address=0x000886DB, size=5)
    lines = lifter._lift_call(insn, insn.operands)
    text = "\n".join(lines)
    assert "setjmp(_mm3_jb_000886DB)" in text, text
    assert "PUSH32(esp, 0x000886E0); sub_00095B8C();" in text, text
    assert "recomp_setjmp_register(MEM32(esp), &_mm3_jb_000886DB);" in text, text
    assert "ebp = g_seh_ebp;" in text, text
    print("ok  setjmp_call_is_host_wrapped")


def test_longjmp_tail_uses_host_longjmp():
    lifter = Lifter(func_db={
        0x00095EB4: {"name": "sub_00095EB4", "size": 0x79, "end": 0x00095F2D},
    })
    lifter.func_start = 0x00095EB4
    insn = _Insn(target=None, address=0x00095F29, size=6)
    insn.operands = [_Mem()]
    lines = lifter._lift_jmp(insn, insn.operands)
    text = "\n".join(lines)
    assert "recomp_guest_longjmp(edx); return;" in text, text
    assert "RECOMP_ITAIL" not in text, text
    print("ok  longjmp_tail_uses_host_longjmp")


def test_ordinary_indirect_tail_unchanged():
    lifter = Lifter(func_db={})
    lifter.func_start = 0x00012345
    insn = _Insn(target=None, address=0x00012380, size=2)
    insn.operands = [_Mem()]
    lines = lifter._lift_jmp(insn, insn.operands)
    text = "\n".join(lines)
    assert "RECOMP_ITAIL" in text, text
    assert "recomp_guest_longjmp" not in text, text
    print("ok  ordinary_indirect_tail_unchanged")


if __name__ == "__main__":
    test_setjmp_call_is_host_wrapped()
    test_longjmp_tail_uses_host_longjmp()
    test_ordinary_indirect_tail_unchanged()
    print("\nall passed")
