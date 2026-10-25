"""cpuid is lowered to the Xbox CPU profile, not left as an unimplemented site.

Titles used to patch each CPUID site by address; the lifter now emits the
call itself, and the emitted C must not carry the RECOMP_UNIMPL marker.
"""
import unittest

from .disasm import Instruction
from .lifter import Lifter


class CpuidLowering(unittest.TestCase):
    def test_cpuid_calls_the_profile_and_is_not_unimplemented(self):
        lifter = Lifter()
        insn = Instruction(0x00012340, 2, "cpuid", "", "")
        code = " ".join(lifter.lift_instruction(insn))
        self.assertIn("recomp_cpuid(eax, ecx, &eax, &ebx, &ecx, &edx);", code)
        self.assertNotIn("RECOMP_UNIMPL", code)
        self.assertNotIn("cpuid", lifter.unimplemented)


if __name__ == "__main__":
    unittest.main()
