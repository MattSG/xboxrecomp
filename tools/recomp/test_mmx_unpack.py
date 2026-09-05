#!/usr/bin/env python3
"""Self-check: MMX byte unpack used by Bink pixel converters."""
from tools.recomp.disasm import Disassembler
from tools.recomp.lifter import Lifter


def lift(code):
    insns = Disassembler().disassemble_function(bytes.fromhex(code), 0x400000,
                                                 0x400000 + len(bytes.fromhex(code)))
    return " ".join(s for insn in insns for s in Lifter().lift_instruction(insn))


low = lift("0F60C1")       # punpcklbw mm0, mm1
high = lift("0F68C1")      # punpckhbw mm0, mm1
memory = lift("0F6000")    # punpcklbw mm0, [eax]

assert "_mm_a >> (0 + _mm_i * 8)" in low and "mm0 = _mm_r" in low, low
assert "_mm_a >> (32 + _mm_i * 8)" in high and "mm0 = _mm_r" in high, high
assert "_mm_b = (uint64_t)(MEM64(eax))" in memory, memory
assert "_mm_r |= (uint64_t)" in low + high, low + high
assert "SSE:" not in low + high + memory
