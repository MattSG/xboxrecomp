"""
Self-check for repe cmpsb / repne scasb string-comparison lifting.

Run: py -3 tools/recomp/test_cmpsb.py

Regression guard for the bug where `repe cmpsb` was lifted as a no-op comment
and the following conditional jump was hard-coded to "strings matched" (always
true). Any loop that searched for a string mismatch would never terminate
(observed as the boot stalling in sub_00015C28's string-compare path). The
fix emits a real byte/word/dword comparison that stores ZF in `_cmps_zf` and
CF in `_cf`, and makes the following je/jne/jb use them.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.disasm import Disassembler  # noqa: E402
from tools.recomp.lifter import Lifter  # noqa: E402


def _lift(code_bytes):
    ds = Disassembler()
    insns = ds.disassemble_function(code_bytes, 0x400000, 0x400000 + len(code_bytes))
    lifter = Lifter()
    return " ".join(s for block in [lifter.lift_instruction(i) for i in insns] for s in block)


def test_repe_cmpsb_je_uses_match_result():
    # repe cmpsb; je +0x20, padded so the target is in range
    out = _lift(bytes.fromhex("f3 a6 74 20" + "90" * 40))
    assert "for (_i = 0; _i < ecx; _i++)" in out, out
    assert "MEM8(esi+_i) != MEM8(edi+_i)" in out, out
    assert "strings matched" not in out, out
    assert "!_cmps_zf" not in out or "_cmps_zf" in out, out
    print("ok  repe_cmpsb_je_uses_match_result:", out.strip()[:110])


def test_repe_cmpsb_jne_uses_not_match():
    # repe cmpsb; jne +0x20
    out = _lift(bytes.fromhex("f3 a6 75 20" + "90" * 40))
    assert "MEM8(esi+_i) != MEM8(edi+_i)" in out, out
    assert "strings differed" not in out, out
    print("ok  repe_cmpsb_jne_uses_not_match:", out.strip()[:110])


def test_repe_cmpsd_matches_dword():
    # repe cmpsd; je +0x20
    out = _lift(bytes.fromhex("f3 a7 74 20" + "90" * 40))
    assert "MEM32(esi+_i*4) != MEM32(edi+_i*4)" in out, out
    print("ok  repe_cmpsd_matches_dword:", out.strip()[:110])


if __name__ == "__main__":
    test_repe_cmpsb_je_uses_match_result()
    test_repe_cmpsb_jne_uses_not_match()
    test_repe_cmpsd_matches_dword()
    print("\nall cmpsb checks passed")
