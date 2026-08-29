#!/usr/bin/env python3
"""Self-check: the x87 forms that had no emitter.

Each of these reached the generic "/* FPU: ... */" fallback, so the
instruction did nothing and the caller read whatever was already on the
stack. fsin and fcos alone are 294 sites in .text - no angle maths can work
while they are absent.

The reverse forms are asserted explicitly. x86 puts the destination at ST(i)
rather than ST(0) for the DC and DE encodings and swaps the operand order
relative to the D8 forms; getting that backwards is silent, and a wrong
subtraction is worse than a missing one.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from tools.recomp.disasm import Disassembler   # noqa: E402
from tools.recomp.lifter import Lifter         # noqa: E402

BASE = 0x400000
_fail = []


def lift(hexbytes):
    code = bytes.fromhex(hexbytes)
    insns = Disassembler().disassemble_function(code, BASE, BASE + len(code))
    lifter = Lifter()
    return " ".join(s for i in insns for s in lifter.lift_instruction(i))


def check(name, hexbytes, *expected):
    out = lift(hexbytes)
    stripped = out.strip()
    if stripped.startswith("/*") and stripped.endswith("*/"):
        _fail.append(f"{name}: still a comment, not code: {out}")
        return
    for e in expected:
        if e not in out:
            _fail.append(f"{name}: expected {e!r} in {out!r}")


# Transcendentals.
check("fsin", "D9FE", "fp_top() = sin(fp_top());")
check("fcos", "D9FF", "fp_top() = cos(fp_top());")
# fptan pushes the 1.0 that makes the result a ratio.
check("fptan", "D9F2", "tan(fp_top())", "fp_push(1.0)")
check("fsincos", "D9FB", "sin(_a)", "fp_push(cos(_a))")
# FPATAN is atan2(ST1, ST0) with a pop, so the result lands in ST0.
check("fpatan", "D9F3", "fp_st1() = atan2(fp_st1(), fp_top());", "fp_pop();")
# FYL2X is ST1 * log2(ST0), then pop.
check("fyl2x", "D9F1", "fp_st1() * log2(fp_top())", "fp_pop();")
check("fyl2xp1", "D9F9", "log2((fp_top() + 1.0))")
check("f2xm1", "D9F0", "pow(2.0, fp_top()) - 1.0")
check("frndint", "D9FC", "rint(fp_top())")

# Integer memory operands.
check("fiadd dword", "DA00", "fp_top() + (double)(int32_t)MEM32(eax)")
check("fimul dword", "DA08", "fp_top() * (double)(int32_t)MEM32(eax)")
check("fidiv dword", "DA30", "fp_top() / (double)(int32_t)MEM32(eax)")
# The reverse forms put the memory operand on the left.
check("fidivr dword", "DA38", "(double)(int32_t)MEM32(eax) / fp_top()")
check("fisubr dword", "DA28", "(double)(int32_t)MEM32(eax) - fp_top()")
# 16-bit operands must sign-extend through int16_t, not int32_t.
check("fiadd word", "DE00", "(double)(int16_t)MEM16(eax)")

# fisttp truncates toward zero and pops.
check("fisttp qword", "DD08", "MEM64(eax) = (int64_t)trunc(fp_top());", "fp_pop();")

# Reverse subtract/divide against a stack register: ST(i) = ST(0) - ST(i).
check("fsubr st(1)", "DCE1", "fp_st1() = fp_top() - fp_st1();")
check("fdivr st(1)", "DCF1", "fp_st1() = fp_top() / fp_st1();")
# The popping forms do the same and then pop.
check("fsubrp st(1)", "DEE1", "fp_st1() = fp_top() - fp_st1();", "fp_pop();")
check("fdivrp st(1)", "DEF1", "fp_st1() = fp_top() / fp_st1();", "fp_pop();")
# A non-ST(1) destination indexes the stack directly.
check("fsubr st(2)", "DCE2", "g_fp_stack[(g_fp_top + 2) & 7]")

# The forward forms must not have changed.
check("fsubp st(1)", "DEE9", "fp_st1()-= fp_top();", "fp_pop();")
check("fsub [eax]", "D820", "fp_push(MEMF(eax));")
if "fp_top() - fp_st1()" not in lift("D828"):
    _fail.append("fsubr [eax] lost its reverse memory form")

if _fail:
    print("FAIL (%d)" % len(_fail))
    for f in _fail:
        print(" -", f)
    sys.exit(1)
print("test_fpu_ops: all checks passed")
