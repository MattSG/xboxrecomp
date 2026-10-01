"""POPF restores flags; its branches must read the image, not an old compare."""
import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import (Lifter, lift_basic_block,
                     _EFLAGS_PRESERVE, _FLAGS_UNDEFINED, FLAG_SETTERS)


def _lift(middle):
    cmp_ = Instruction(0, 2, "cmp", "eax, ebx", "39d8")
    cmp_.operands = [Operand(type="reg", reg="eax"),
                     Operand(type="reg", reg="ebx")]
    insns = [cmp_]
    off = 2
    for m in middle:
        i = Instruction(off, 1, m, "", "9d")
        i.operands = []
        insns.append(i)
        off += 1
    je = Instruction(off, 2, "je", "0x40", "7440")
    je.operands = [Operand(type="imm", imm=0x40)]
    je.jump_target = 0x40
    insns.append(je)
    lifted, _ = lift_basic_block(
        Lifter(), BasicBlock(start=0, instructions=insns))
    return "\n".join(lifted)


class PopfdFlagTrackingTest(unittest.TestCase):
    def test_popfd_is_not_treated_as_flag_preserving(self):
        self.assertNotIn("popfd", _EFLAGS_PRESERVE)
        self.assertNotIn("popfd", _FLAGS_UNDEFINED)
        self.assertIn("popfd", FLAG_SETTERS)

    def test_pushfd_still_is(self):
        # pushfd reads the flags without changing them; it must stay.
        self.assertIn("pushfd", _EFLAGS_PRESERVE)

    def test_a_branch_after_popfd_does_not_use_the_old_compare(self):
        lifted = _lift(["popfd"])
        self.assertNotIn("CMP_EQ", lifted)
        self.assertIn("g_eflags", lifted)
        self.assertIn("sub_00000040", lifted)

    def test_the_same_branch_without_popfd_still_resolves(self):
        # The positive control. Without it, a lifter that resolved NOTHING
        # would pass the test above.
        self.assertIn("CMP_EQ", _lift([]))

    def test_a_genuinely_preserving_instruction_still_preserves(self):
        # nop is in _EFLAGS_PRESERVE and must stay there: this pins that the
        # change was to popfd and not to the mechanism.
        self.assertIn("CMP_EQ", _lift(["nop"]))

    def test_popfd_anywhere_in_the_run_breaks_the_chain(self):
        self.assertNotIn("CMP_EQ", _lift(["nop", "popfd", "nop"]))


if __name__ == "__main__":
    unittest.main()
