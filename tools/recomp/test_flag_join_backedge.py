"""
Self-check for flags reaching a loop head across a back edge.

Blocks are lifted in address order, so the predecessor on a back edge sits
after the block it jumps to and has no computed out-state on a first pass.
The join that decides what flag state a block inherits then sees an unknown
predecessor and gives up, which is safe -- and expensive, because the jcc at
the top of a counted loop is exactly that shape. It lifts to the `_flags`
fallback, a variable nothing ever assigns, so the branch is compiled as never
taken and the loop has no exit.

Shin Megami Tensei: Nine has one in its XMV decoder's row padding:

    mov eax, 0Bh
    sub eax, [ebp+20h]      ; sets ZF
  L:
    jz  done                ; <-- both predecessors set ZF from eax
    movq [edi], mm0
    add edi, edx
    dec eax                 ; sets ZF
    jmp L

Lifted with the fallback it stored eight bytes and advanced edi by sixteen
until it left mapped memory, taking the process with it. Settling the flag
state to a fixed point before emitting gives the jcc its comparison back.

The second property matters as much: when the predecessors genuinely
disagree, the fallback must stay. Inheriting the wrong flags is worse than
inheriting none, because the branch then looks right and goes the wrong way.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402

BASE = 0x00010000


def _translate(image):
    config._install(
        [config.Section(".text", BASE, len(image), 0x0000, len(image), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="flag-join-test")
    db = {BASE: {"start": f"0x{BASE:08X}", "end": BASE + len(image),
                 "_addr": BASE, "size": len(image)}}
    return FunctionTranslator(image, db).translate_function(BASE, db[BASE])


def test_loop_head_inherits_flags_from_both_predecessors():
    #   +0  sub eax, ecx      ; sets ZF, falls through
    #   +2 L: jz  +9          ; back edge from +7 also sets ZF, from dec eax
    #   +4  inc edx
    #   +5  dec eax
    #   +6  jmp L
    #   +8  ret
    image = (b"\x29\xC8"          # sub eax, ecx
             b"\x74\x04"          # jz +4  -> ret at +8
             b"\x42"              # inc edx
             b"\x48"              # dec eax
             b"\xEB\xFA"          # jmp -6 -> the jz
             b"\xC3")             # ret
    code = _translate(image)
    assert "_flags" not in code.replace("int _flags = 0", ""), code
    assert "CMP_EQ" in code or "== 0" in code, code


def test_sub_countdown_snapshots_result_before_jne():
    #   L: sub ecx, 1
    #      jne L
    #      ret
    image = bytes.fromhex("83 E9 01 75 FB C3")
    code = _translate(image)
    sub = code.index("ecx = ecx - 1;")
    snapshot = code.index("_fa = (uint32_t)(ecx) & 0xFFFFFFFFu;", sub)
    branch = code.index("if ((_fa != 0)) goto loc_00010000;", snapshot)
    assert sub < snapshot < branch, code


def test_disagreeing_predecessors_keep_the_fallback():
    # Two predecessors reach the jz: one after `cmp`, one after `inc`. Their
    # ZF meanings differ (operand equality versus a result), so the join must
    # refuse.
    #   +0  cmp eax, ecx
    #   +2  jnz +1            -> the jz at +5 when ZF is clear
    #   +4  inc edx           (fallthrough reaches the jz with different flags)
    #   +5 L: jz +1
    #   +7  ret
    image = (b"\x39\xC8"          # cmp eax, ecx
             b"\x75\x01"          # jnz +1 -> +5
             b"\x42"              # inc edx
             b"\x74\x00"          # jz +0 -> +7
             b"\xC3")             # ret
    code = _translate(image)
    assert "_flags /*" in code, code


if __name__ == "__main__":
    test_loop_head_inherits_flags_from_both_predecessors()
    test_sub_countdown_snapshots_result_before_jne()
    test_disagreeing_predecessors_keep_the_fallback()
    print("ok")
