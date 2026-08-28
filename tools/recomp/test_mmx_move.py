#!/usr/bin/env python3
"""Self-check: MMX qword moves.

movq and movntq reached the comment-only fallback at the end of the SSE
handler, so an MMX block copy lifted to its loop counter and pointer
arithmetic with every load and store missing - the loop ran the right number
of iterations and moved nothing. sub_000120B6 is one such copy and has 77
callers; sub_001F02AD has 39.

_mem_accessor also mapped an 8-byte operand to MEM32 through its default,
which would have truncated every qword access to its low half.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from tools.recomp.disasm import Disassembler   # noqa: E402
from tools.recomp.lifter import Lifter, _mem_accessor  # noqa: E402

BASE = 0x400000
_fail = []


def lift(hexbytes):
    code = bytes.fromhex(hexbytes)
    insns = Disassembler().disassemble_function(code, BASE, BASE + len(code))
    lifter = Lifter()
    return " ".join(s for i in insns for s in lifter.lift_instruction(i))


def check(name, hexbytes, expected):
    out = lift(hexbytes)
    if expected not in out:
        _fail.append(f"{name}: expected {expected!r} in {out!r}")
    if "TODO" in out:
        _fail.append(f"{name}: still unhandled: {out!r}")


# A qword operand must use a 64-bit accessor, not MEM32 by default.
if _mem_accessor(8) != "MEM64":
    _fail.append("_mem_accessor(8) = %r, expected 'MEM64'" % _mem_accessor(8))

check("movq mm1, [esi]", "0F6F0E", "mm1 = MEM64(esi);")
check("movq [edi], mm1", "0F7F0F", "MEM64(edi) = mm1;")
check("movntq [edi], mm1", "0FE70F", "MEM64(edi) = mm1;")
check("movq mm3, mm2", "0F6FDA", "mm3 = mm2;")
# movd is 32-bit; assigning into the uint64_t local zero-extends, as the
# instruction requires.
check("movd mm1, [esi]", "0F6E0E", "mm1 = MEM32(esi);")

# The 64-byte-per-iteration copy body: eight loads then eight stores. Every
# one must produce a statement.
body = ("0F6F0E0F6F56080F6F5E10"      # movq mm1..mm3
        "0FE70F0FE747080FE74F10")     # movntq [edi], [edi+8], [edi+0x10]
out = lift(body)
if out.count("MEM64") != 6:
    _fail.append("copy body: expected 6 MEM64 accesses, got %d in %r"
                 % (out.count("MEM64"), out))

if _fail:
    print("FAIL (%d)" % len(_fail))
    for f in _fail:
        print(" -", f)
    sys.exit(1)
print("test_mmx_move: all checks passed")
