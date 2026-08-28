"""
Self-check for rcl/rcr (rotate through carry).

Run: py -3 tools/recomp/test_rotate_carry.py

Regression guard for the bug where rcl/rcr were listed as flag setters but had
no emitter, so every site lifted to a bare comment and the destination kept its
previous value. MM3 has 1,945 such sites in .text alone; each silently produced
a wrong result and left CF stale for any following adc/sbb/jc.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.disasm import Disassembler  # noqa: E402
from tools.recomp.lifter import Lifter  # noqa: E402


def _lift(code_bytes):
    ds = Disassembler()
    insns = ds.disassemble_function(code_bytes, 0x400000,
                                    0x400000 + len(code_bytes))
    lifter = Lifter()
    return " ".join(s for block in [lifter.lift_instruction(i) for i in insns]
                    for s in block)


def main():
    failures = []

    out = _lift(bytes.fromhex("D1 D8"))          # rcr eax, 1
    if "RCR32" not in out:
        failures.append("rcr eax,1 did not emit RCR32: %r" % out)
    if "TODO" in out:
        failures.append("rcr eax,1 still unhandled: %r" % out)
    if "_cf = _co" not in out:
        failures.append("rcr did not publish outgoing CF: %r" % out)

    out = _lift(bytes.fromhex("D1 D0"))          # rcl eax, 1
    if "RCL32" not in out:
        failures.append("rcl eax,1 did not emit RCL32: %r" % out)

    out = _lift(bytes.fromhex("D3 D9"))          # rcr ecx, cl
    if "RCR32" not in out:
        failures.append("rcr ecx,cl did not emit RCR32: %r" % out)

    out = _lift(bytes.fromhex("C1 D8 03"))       # rcr eax, 3
    if "RCR32" not in out:
        failures.append("rcr eax,3 did not emit RCR32: %r" % out)

    # rol/ror must keep working and must NOT route through the carry form.
    out = _lift(bytes.fromhex("D1 C0"))          # rol eax, 1
    if "ROL32" not in out or "RCL32" in out:
        failures.append("rol eax,1 regressed: %r" % out)

    if failures:
        for f in failures:
            print("FAIL:", f)
        return 1
    print("rotate-carry self-check OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
