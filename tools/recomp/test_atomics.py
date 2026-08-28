#!/usr/bin/env python3
"""Self-check: LOCK-prefixed instructions, cmpxchg/xadd, and the bare
cmps/scas forms.

Every one of these used to lift to a bare TODO comment - the instruction
vanished and execution carried on with stale values. LOCK was the widest
case: the prefix is folded into the mnemonic, so "lock inc" matched no
dispatch entry and ordinary increments were dropped.

These checks assert on the emitted C, not just on the absence of TODO,
because a wrong emitter is worse than a missing one.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from tools.recomp.disasm import Disassembler          # noqa: E402
from tools.recomp.lifter import Lifter, lift_basic_block  # noqa: E402

BASE = 0x400000
_fail = []


class _Block:
    def __init__(self, instructions):
        self.instructions = instructions


def _lift(hexbytes):
    code = bytes.fromhex(hexbytes)
    insns = Disassembler().disassemble_function(code, BASE, BASE + len(code))
    result = lift_basic_block(Lifter(), _Block(insns))
    stmts = result[0] if isinstance(result, tuple) else result
    return "\n".join(stmts)


def check(name, hexbytes, must_contain=(), must_not_contain=("TODO",)):
    out = _lift(hexbytes)
    for needle in must_contain:
        if needle not in out:
            _fail.append(f"{name}: expected {needle!r} in:\n{out}")
    for needle in must_not_contain:
        if needle in out:
            _fail.append(f"{name}: unexpected {needle!r} in:\n{out}")


# LOCK must be stripped so the underlying operation still lifts.
check("lock inc [ecx]", "F0FF01", ["MEM32(ecx) = MEM32(ecx) + 1"])
check("lock add [ecx],eax", "F00101", ["MEM32(ecx) = MEM32(ecx) + eax", "_cf ="])
check("lock or [ecx],eax", "F00901", ["MEM32(ecx) = MEM32(ecx) | eax"])

# cmpxchg: on equal store src, otherwise load dst into the accumulator.
check("cmpxchg [ecx],edx", "0FB111",
      ["_cmpx_zf = (_a == _t)", "if (_cmpx_zf) { MEM32(ecx) = edx; }",
       "else { eax = _t; }"])
# 8-bit form must compare AL, not EAX.
check("cmpxchg cl,dl", "0FB0D1", ["_a = LO8(eax)", "SET_LO8(eax, _t)"])

# xadd: src takes the old dst, dst takes the sum.
check("xadd [ecx],edx", "0FC111",
      ["_o = MEM32(ecx)", "edx = _o;", "MEM32(ecx) = _r", "_cf = (_r < _s)"])

# The published ZF must reach the following branch. Deriving it from the
# operands is not possible: cmpxchg's failure path makes them equal.
check("lock cmpxchg;jne", "F00FB1117505",
      ["if (!_cmpx_zf)"], must_not_contain=("TODO", "_flags"))
check("lock xadd;je", "F00FC1117405",
      ["if (_cmpx_zf)"], must_not_contain=("TODO", "_flags"))

# Bare (unprefixed) string compare/scan. Only the rep forms were handled,
# so these fell through to the unknown-instruction arm and the branch that
# consumed them read a stale flag variable.
check("cmpsb;je", "A67405",
      ["_cmps_zf = (_a == _b)", "esi++", "edi++", "if (_cmps_zf)"],
      must_not_contain=("TODO", "_flags"))
check("scasb;jne", "AE7505",
      ["_a = LO8(eax)", "_b = MEM8(edi)", "if (!_cmps_zf)"],
      must_not_contain=("TODO", "_flags"))
check("scasd", "AF", ["_b = MEM32(edi)", "edi += 4"])
check("cmpsw", "66A7", ["MEM16(esi)", "esi += 2"])

if _fail:
    print("FAIL (%d)" % len(_fail))
    for f in _fail:
        print(" -", f)
    sys.exit(1)
print("test_atomics: all checks passed")
