"""
Regression guard for sub-32-bit signed flag math.

Proven on MM3 sub_0034E420 at 0x0034E85B:

    test bl, bl
    js  loc_0034E88B

Hardware SF = bit 7 of bl. The lifter emitted
`TEST_S(LO8(ebx), LO8(ebx))` where LO8 is uint8_t: the value zero-extends
to 32 bits, so the sign bit is never set and the batch-builder pump never
takes the slot-advance path -- infinite spin in sub_0034E4A0/sub_0034E4D0
(run 408 freeze at d8=00004011).

The fix casts flag-setter operands to their signed width (int8_t/int16_t)
before computing signed conditions, so `js`/`jns`/`jl`/`jge`/`jle`/`jg`
evaluate at the operand width like real x86 flag math.

Run: py -3 tools/recomp/test_sign_flag_width.py
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.disasm import Disassembler, BasicBlock, Operand  # noqa: E402
from tools.recomp.lifter import Lifter, lift_basic_block, _make_condition  # noqa: E402


def _lift(code_bytes):
    ds = Disassembler()
    insns = ds.disassemble_function(code_bytes, 0x400000, 0x400000 + len(code_bytes))
    blk = BasicBlock(start=0x400000, instructions=insns, successors=[])
    lifter = Lifter()
    stmts, _ = lift_basic_block(lifter, blk, flag_state=None, snap_counter=None)
    return "\n".join(stmts)


def test_test_bl_js_uses_int8():
    # test bl, bl; js +3; nop; ret
    out = _lift(bytes.fromhex("84db 7803 90 c3"))
    assert "(int8_t)(LO8(ebx))" in out, (
        "test bl,bl; js must evaluate SF at 8-bit width, got:\n" + out)


def test_test_si_js_uses_int16():
    # 66 85 f6: test si, si; js +3; nop; ret
    out = _lift(bytes.fromhex("6685f6 7803 90 c3"))
    assert "(int16_t)(LO16(esi))" in out, (
        "test si,si; js must evaluate SF at 16-bit width, got:\n" + out)


def test_test_eax_js_stays_int32():
    # test eax, eax; js +3; nop; ret
    out = _lift(bytes.fromhex("85c0 7803 90 c3"))
    assert "(int32_t)(eax)" in out, (
        "32-bit test must stay int32-width, got:\n" + out)


def test_snapshot_temp_keeps_width():
    # Split-block jcc consumers read snapshot temps; the 8-bit width must
    # survive the snapshot or SF is evaluated at 32 bits again.
    cond, _ = _make_condition("js", "test", [
        Operand(type="reg", reg="_fcmp_0_a", size=1),
        Operand(type="reg", reg="_fcmp_1_b", size=1)])
    assert "TEST_S((int8_t)(_fcmp_0_a), (int8_t)(_fcmp_1_b))" == cond, cond


if __name__ == "__main__":
    failed = []
    for name, fn in [
        ("test_bl_js_int8", test_test_bl_js_uses_int8),
        ("test_si_js_int16", test_test_si_js_uses_int16),
        ("test_eax_js_int32", test_test_eax_js_stays_int32),
        ("snapshot_temp_keeps_width", test_snapshot_temp_keeps_width),
    ]:
        try:
            fn()
            print("ok  ", name)
        except AssertionError as exc:
            print("FAIL:", name, "-", exc)
            failed.append(name)
    if failed:
        print("\n%d sign-flag-width check(s) FAILED: %s" % (len(failed), ", ".join(failed)))
        sys.exit(1)
    print("\nall sign-flag-width checks passed")
