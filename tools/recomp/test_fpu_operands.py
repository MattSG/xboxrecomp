"""
Regression guard for x87 FPU memory-operand lifting.

Proven on MM3 sub_0034AA70 (D3D.asm 0x0034AAB8/0x0034AABE/0x0034AAC4/0x0034AB50):

    fld  dword ptr [esp + 8]
    fmul dword ptr [esi + 0x520]   # before: fp_st1() *= fp_top();  operand dropped
    fcom dword ptr [esp + 4]       # before: compared vs fp_st1();  operand dropped
    ...
    test ah, 0x05; jp              # before: unconditional `if (1)`
    ...
    fdivr dword ptr [esp]          # before: comment-only no-op

The register pattern fp_st1() op= fp_top(); fp_pop() is only valid when the
operand already sits below the top (fmul st(1) after an fld). Memory operands
must be loaded or the result reads a stale/uninitialized stack slot.

Run: py -3 tools/recomp/test_fpu_operands.py
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


def test_fmul_mem_operand_is_loaded():
    # fld [esp+8]; fmul [esi+0x520]
    out = _lift(bytes.fromhex("d9442408 d88e20050000"))
    assert "fp_push(MEMF(esi + 0x520))" in out, (
        "fmul [mem] must push the memory operand, not use fp_st1()")


def test_fadd_mem_operand_is_loaded():
    # fld [esp+8]; fadd [esp+4]
    out = _lift(bytes.fromhex("d9442408 d8442404"))
    assert "fp_push(MEMF(esp + 4))" in out, (
        "fadd [mem] must push the memory operand, not use fp_st1()")


def test_fcom_mem_operand_is_compared():
    # fld [esp+8]; fcom [esp+4]
    out = _lift(bytes.fromhex("d9442408 d8542404"))
    assert "fp_top() < MEMF(esp + 4)" in out, (
        "fcom [mem] must compare against the memory operand, not fp_st1()")


def test_fdivr_mem_operand_computes():
    # fdivr [esp]  (ST0 = [esp] / ST0)
    out = _lift(bytes.fromhex("d83c24"))
    assert "fp_top() / fp_st1()" in out, "fdivr [mem] must emit the division"


def test_test_ah_5_jp_is_conditional():
    # fld [esp+8]; fcom [esp+4]; fnstsw ax; test ah, 5; jp +3; nop; ret
    out = _lift(bytes.fromhex("d9442408 d8542404 dfe0 f6c405 7a03 90 c3"))
    assert "_fpu_cmp >= 0" in out, "jp after test ah,0x05 must be conditional"
    assert "if (1 " not in out, "no unconditional parity branches allowed"


def test_fst_mem_does_not_pop():
    out = _lift(bytes.fromhex("d913"))
    assert "= (float)fp_top(); /* fst" in out, "fst [mem] stores without popping"
    assert "fp_pop" not in out, "fst [mem] must not pop the FPU stack"


def test_fstp_mem_pops():
    out = _lift(bytes.fromhex("d91b"))
    assert "fp_popp();" in out, "fstp [mem] must pop the FPU stack"


def test_fcomp_mem_pops():
    out = _lift(bytes.fromhex("d81b"))
    assert "_fpu_cmp = (fp_top() < MEMF(ebx))" in out
    assert "fp_popp();" in out, "fcomp [mem] must pop ST0 after comparing"


def test_fcom_mem_does_not_pop():
    out = _lift(bytes.fromhex("d813"))
    assert "fp_popp();" not in out, "fcom [mem] must compare without popping"


def test_fld_st0_duplicates_top():
    out = _lift(bytes.fromhex("d9c0"))
    assert "double _fld_tmp = g_fp_stack[g_fp_top & 7]; fp_push(_fld_tmp);" in out, (
        "fld st(0) must duplicate ST0 through a sequenced temporary")


def test_fstp_st0_pops():
    out = _lift(bytes.fromhex("ddd8"))
    assert "fp_popp();" in out, "fstp st(0) must pop ST0"
if __name__ == "__main__":
    failed = []
    for name, fn in [
        ("fmul_mem_operand_loaded", test_fmul_mem_operand_is_loaded),
        ("fadd_mem_operand_loaded", test_fadd_mem_operand_is_loaded),
        ("fcom_mem_operand_compared", test_fcom_mem_operand_is_compared),
        ("fdivr_mem_operand_computes", test_fdivr_mem_operand_computes),
        ("test_ah_5_jp_conditional", test_test_ah_5_jp_is_conditional),
        ("fst_mem_no_pop", test_fst_mem_does_not_pop),
        ("fstp_mem_pops", test_fstp_mem_pops),
        ("fcomp_mem_pops", test_fcomp_mem_pops),
        ("fcom_mem_no_pop", test_fcom_mem_does_not_pop),
        ("fld_st0_duplicates_top", test_fld_st0_duplicates_top),
        ("fstp_st0_pops", test_fstp_st0_pops),
    ]:
        try:
            fn()
            print("ok  ", name)
        except AssertionError as exc:
            print("FAIL:", name, "-", exc)
            failed.append(name)
    if failed:
        print("\n%d fpu-operands check(s) FAILED: %s" % (len(failed), ", ".join(failed)))
        sys.exit(1)
    print("\nall fpu-operands checks passed")
