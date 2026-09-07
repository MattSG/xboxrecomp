"""Regression checks for Xbox-frequency RDTSC lifting."""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.disasm import Disassembler  # noqa: E402
from tools.recomp.lifter import Lifter  # noqa: E402


def test_rdtsc_uses_one_scaled_guest_sample():
    insn = Disassembler().disassemble_function(bytes.fromhex("0f31"), 0x400000, 0x400002)[0]
    out = " ".join(Lifter().lift_instruction(insn))
    assert out.count("recomp_guest_rdtsc()") == 1, out
    assert "__rdtsc()" not in out, out

