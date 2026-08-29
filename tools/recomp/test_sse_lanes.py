#!/usr/bin/env python3
"""Self-check: the four-lane SSE model.

An xmm register used to be modelled as a single float. The packed forms had
nowhere to put three of their four results, so addps, subps, mulps, divps,
shufps, unpck*, min/maxps and the packed compares were emitted as comments -
they did nothing at all. sub_000BAAB0, which the run reaches, contains 129
such statements: a four-wide butterfly whose every arithmetic operation was
dropped, so the function returned garbage.

Lane selections below are checked against the instruction definitions, since
getting a shuffle backwards is silent and would be worse than the comment.
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
    if "TODO" in out:
        _fail.append(f"{name}: unhandled: {out}")
        return
    stripped = out.strip()
    if stripped.startswith("/*") and stripped.endswith("*/"):
        _fail.append(f"{name}: emitted a comment, not code: {out}")
        return
    for e in expected:
        if e not in out:
            _fail.append(f"{name}: expected {e!r} in {out!r}")


# Packed arithmetic must touch all four lanes.
check("mulps xmm0,xmm1", "0F59C1",
      "xmm0[0] = xmm0[0] * xmm1[0]", "xmm0[3] = xmm0[3] * xmm1[3]")
# A packed memory operand is 16 bytes: four consecutive floats.
check("addps xmm0,[eax]", "0F5800",
      "MEMF(eax)", "MEMF(eax + 4)", "MEMF(eax + 8)", "MEMF(eax + 0xC)")
check("movaps xmm0,[eax]", "0F2800", "xmm0[3] = MEMF(eax + 0xC)")
check("movaps [eax],xmm0", "0F2900", "MEMF(eax + 0xC) = xmm0[3]")

# SHUFPS: low two lanes from src1, high two from src2, per the immediate.
# 0x1B = 00 01 10 11 -> a[3], a[2], b[1], b[0].
check("shufps xmm0,xmm1,0x1B", "0FC6C11B",
      "_s0 = xmm0[3]", "_s1 = xmm0[2]", "_s2 = xmm1[1]", "_s3 = xmm1[0]")
# UNPCKLPS: a0, b0, a1, b1.  UNPCKHPS: a2, b2, a3, b3.
check("unpcklps xmm0,xmm1", "0F14C1",
      "_s0 = xmm0[0]", "_s1 = xmm1[0]", "_s2 = xmm0[1]", "_s3 = xmm1[1]")
check("unpckhps xmm0,xmm1", "0F15C1",
      "_s0 = xmm0[2]", "_s1 = xmm1[2]", "_s2 = xmm0[3]", "_s3 = xmm1[3]")
# MOVLHPS puts src's low half into dst's high half; MOVHLPS the reverse.
check("movlhps xmm0,xmm1", "0F16C1", "_s2 = xmm1[0]", "_s3 = xmm1[1]")
check("movhlps xmm0,xmm1", "0F12C1", "_s0 = xmm1[2]", "_s1 = xmm1[3]")

# Bitwise ops are bit patterns, not arithmetic.
check("andps xmm0,xmm1", "0F54C1", "RECOMP_FAND(xmm0[0], xmm1[0])")
check("xorps xmm0,xmm1", "0F57C1", "RECOMP_FXOR(xmm0[0], xmm1[0])")
# xor with self clears all four lanes, not just the low one.
check("xorps xmm0,xmm0", "0F57C0",
      "xmm0[0] = 0.0f", "xmm0[1] = 0.0f", "xmm0[2] = 0.0f", "xmm0[3] = 0.0f")

# A packed compare writes a lane mask of all ones or all zeros.
check("cmpeqps xmm0,xmm1", "0FC2C100", "RECOMP_FMASK(xmm0[0] == xmm1[0])")
# movmskps was hardcoded to 0, so every branch on it went the same way.
check("movmskps eax,xmm1", "0F50C1", "RECOMP_MOVMSKPS(xmm1)")
# All four lanes of a normalise, not just the low one.
check("rsqrtps xmm0,xmm1", "0F52C1",
      "xmm0[0] = 1.0f / sqrtf(xmm1[0])", "xmm0[3] = 1.0f / sqrtf(xmm1[3])")

# Scalar forms stay on lane 0.
check("addss xmm0,xmm1", "F30F58C1", "xmm0[0] = xmm0[0] + xmm1[0]")
check("movss xmm0,xmm1", "F30F10C1", "xmm0[0] = xmm1[0]")
# Loading a scalar from memory zeroes the upper lanes; reg-to-reg does not.
check("movss xmm0,[eax]", "F30F1000",
      "xmm0[0] = MEMF(eax)", "xmm0[1] = 0.0f")
if "0.0f" in lift("F30F10C1"):
    _fail.append("movss reg,reg must not zero the upper lanes")

# movsd and cmpsd name two unrelated instructions. Only the SSE forms carry
# an xmm operand. Lifting the SSE form as a string move clobbered esi, edi
# and guest memory.
if "esi" in lift("F20F1000"):
    _fail.append("movsd xmm0,[eax] lifted as a string move")
check("movsd xmm0,[eax]", "F20F1000", "xmm0[0] = MEMD(eax)")
check("movsd (string)", "A5", "MEM32(edi) = MEM32(esi)")
check("cmpsd (string)", "A7", "_cmps_zf = (_a == _b)", "esi += 4")

if _fail:
    print("FAIL (%d)" % len(_fail))
    for f in _fail:
        print(" -", f)
    sys.exit(1)
print("test_sse_lanes: all checks passed")
