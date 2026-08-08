"""
Self-check for the _icall_esp save placement fixup.

Run: py -3 tools/recomp/test_icall_esp_save.py

Regression guard for sub_001BE953: six dwords are pushed for a vtable call,
an interleaved stdcall (sub_0002E735, `ret 4`) consumes two of them, and the
remaining four dwords are the vtable call's args. The old fixup stopped at
the loc_001BECC8 label and saved g_esp at the icall line - after the pending
args - so RECOMP_ICALL_SAFE restored a stack 16 bytes too low, corrupted the
epilogue unwind, and crashed with eax=0xFF037AC4.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.translator import _fixup_icall_esp_save  # noqa: E402


def _sample_interleaved_call():
    """The sub_001BE953 crash shape (condensed)."""
    return [
        "void sub_001BE953(void)",
        "{",
        "loc_001BECA1: ;",
        "    PUSH32(esp, MEM32(ebp + 0x7C));",
        "    eax = MEM32(esi);",
        "    ecx = ebp + -204;",
        "    PUSH32(esp, ecx);",
        "    ecx = esi;",
        "    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 0x30), _icall_esp); /* indirect call */",
        "",
        "loc_001BECB2: ;",
        "    PUSH32(esp, MEM32(ebp + 0x7C));",
        "    ecx = ebp + 8;",
        "    PUSH32(esp, ecx);",
        "    PUSH32(esp, eax);",
        "    PUSH32(esp, ecx);",
        "    eax = esp;",
        "    ecx = ebp + 0x28;",
        "    PUSH32(esp, ecx);",
        "    MEM32(eax) = edi;",
        "    PUSH32(esp, 0); sub_0002E735(); /* call 0x0002E735 ret 4 */",
        "",
        "loc_001BECC8: ;",
        "    ecx = MEM32(ebp + 0x2C);",
        "    eax = MEM32(ecx);",
        "    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 4), _icall_esp); /* indirect call */",
        "}",
    ]


def test_save_crosses_interleaved_call_cleanup():
    out = _fixup_icall_esp_save(_sample_interleaved_call())
    text = "\n".join(out)
    # The vtable icall's save must sit before the first of its four arg
    # pushes (the loc_001BECB2 block), crossing the interleaved
    # sub_0002E735 (ret 4) whose own arg dword is skipped.
    idx = text.index("loc_001BECB2: ;")
    after = text[idx:].split("\n")
    assert after[1].strip() == "{ uint32_t _icall_esp = g_esp;", after[1]
    assert "    }" in text
    assert text.count("{ uint32_t _icall_esp = g_esp;") == 2
    assert text.count("    }") == 2
    print("ok  save_crosses_interleaved_call_cleanup")


def test_plain_icall_save_is_unchanged():
    lines = [
        "loc_00010000: ;",
        "    PUSH32(esp, eax);",
        "    PUSH32(esp, ecx);",
        "    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx), _icall_esp); /* indirect call */",
    ]
    out = _fixup_icall_esp_save(lines)
    text = "\n".join(out)
    assert text.startswith(
        "loc_00010000: ;\n"
        "    { uint32_t _icall_esp = g_esp;\n"
        "    PUSH32(esp, eax);"), text
    print("ok  plain_icall_save_is_unchanged")


def test_unknown_cleanup_stops_at_call():
    lines = [
        "loc_00010000: ;",
        "    PUSH32(esp, eax);",
        "    PUSH32(esp, ecx);",
        "    PUSH32(esp, 0); sub_00020000(); /* call 0x00020000 */",
        "loc_00010010: ;",
        "    PUSH32(esp, edx);",
        "    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(ebx), _icall_esp); /* indirect call */",
    ]
    out = _fixup_icall_esp_save(lines)
    text = "\n".join(out)
    # Cleanup unknown: the pushes before sub_00020000 cannot be attributed,
    # so the save goes at the icall's own args only.
    idx = text.index("loc_00010010: ;")
    after = text[idx:].split("\n")
    assert after[1].strip() == "{ uint32_t _icall_esp = g_esp;", after[1]
    assert text.count("{ uint32_t _icall_esp = g_esp;") == 1
    print("ok  unknown_cleanup_stops_at_call")


def test_ret0_call_keeps_its_args_for_the_icall():
    lines = [
        "    PUSH32(esp, eax);",
        "    PUSH32(esp, ecx);",
        "    PUSH32(esp, 0); sub_00020000(); /* call 0x00020000 ret 0 */",
        "    PUSH32(esp, edx);",
        "    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(ebx), _icall_esp); /* indirect call */",
    ]
    out = _fixup_icall_esp_save(lines)
    text = "\n".join(out)
    # ret 0 leaves its args on the stack, so they are the icall's pending
    # args: the save goes before the first push.
    assert text.startswith(
        "    { uint32_t _icall_esp = g_esp;\n"
        "    PUSH32(esp, eax);"), text
    print("ok  ret0_call_keeps_its_args_for_the_icall")


def test_stops_at_jump_target_label():
    lines = [
        "    PUSH32(esp, eax);",
        "loc_00010050: ;",
        "    PUSH32(esp, edx);",
        "    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(ebx), _icall_esp); /* indirect call */",
        "    if (CMP_EQ(eax, 0)) goto loc_00010050;",
    ]
    out = _fixup_icall_esp_save(lines)
    text = "\n".join(out)
    # loc_00010050 is a jump target: the save must sit after it so every
    # path reaching the icall initializes _icall_esp.
    idx = text.index("loc_00010050: ;")
    after = text[idx:].split("\n")
    assert after[1].strip() == "{ uint32_t _icall_esp = g_esp;", after[1]
    print("ok  stops_at_jump_target_label")


def test_stops_at_esp_reassignment():
    lines = [
        "    PUSH32(esp, eax);",
        "    PUSH32(esp, ecx);",
        "    PUSH32(esp, 0); sub_00020000(); /* call 0x00020000 ret 0 */",
        "    esp = esp + 8;",
        "    PUSH32(esp, edx);",
        "    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(ebx), _icall_esp); /* indirect call */",
    ]
    out = _fixup_icall_esp_save(lines)
    text = "\n".join(out)
    # Caller-side cleanup reassigns esp: only the pushes after it belong to
    # the icall.
    idx = text.index("esp = esp + 8;")
    after = text[idx:].split("\n")
    assert after[1].strip() == "{ uint32_t _icall_esp = g_esp;", after[1]
    print("ok  stops_at_esp_reassignment")


if __name__ == "__main__":
    test_save_crosses_interleaved_call_cleanup()
    test_plain_icall_save_is_unchanged()
    test_unknown_cleanup_stops_at_call()
    test_ret0_call_keeps_its_args_for_the_icall()
    test_stops_at_jump_target_label()
    test_stops_at_esp_reassignment()
    print("\nall passed")
