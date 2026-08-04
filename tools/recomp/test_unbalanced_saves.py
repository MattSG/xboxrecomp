"""
Self-check for shared-epilogue register-balance fixup.

Run: py -3 tools/recomp/test_unbalanced_saves.py

Regression guard for the bug where an intra-function jump target that falls
through to a shared epilogue (e.g. sub_00084506 inside sub_000844B9) was
lifted as a standalone function that pops edi/esi/ebx/ebp but pushes only
esi. The over-pop corrupted g_ebx / g_edi / g_ebp and leaked g_esp, making
the pool allocator's free-list walk spin forever.
"""

import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.translator import _fixup_unbalanced_saves  # noqa: E402


def _sample_unbalanced():
    return [
        "void sub_00084506(void)",
        "{",
        "loc_00084506: ;",
        "    PUSH32(esp, esi);",
        "    PUSH32(esp, 0); sub_00084378();",
        "loc_00084510: ;",
        "    MEM32(eax + 4) = ecx;",
        "loc_0008452A: ;",
        "    POP32(esp, edi);",
        "    POP32(esp, esi);",
        "    POP32(esp, ebx);",
        "    POP32(esp, ebp);",
        "    esp += 16; return;",
        "}",
    ]


def _sample_balanced():
    """A normal function that already saves all four registers."""
    return [
        "void sub_00084531(void)",
        "{",
        "loc_00084531: ;",
        "    PUSH32(esp, edi);",
        "    PUSH32(esp, esi);",
        "    PUSH32(esp, ebx);",
        "loc_00084534: ;",
        "    MEM32(eax + 4) = ecx;",
        "loc_0008453F: ;",
        "    POP32(esp, ebx);",
        "    POP32(esp, esi);",
        "    POP32(esp, edi);",
        "    esp += 12; return;",
        "}",
    ]


def _pushes(lines):
    return [l for l in lines if re.match(r'^\s*PUSH32\(esp, (edi|esi|ebx|ebp)\);', l)]


def _pops(lines):
    return [l for l in lines if re.match(r'^\s*POP32\(esp, (edi|esi|ebx|ebp)\);', l)]


def test_unbalanced_is_balanced_after_fixup():
    out = _fixup_unbalanced_saves(_sample_unbalanced())
    pushes = _pushes(out)
    pops = _pops(out)
    assert len(pushes) == len(pops), (pushes, pops)
    # Entry pushes must mirror the epilogue pops in reverse order.
    pop_regs = [re.match(r'\s*POP32\(esp, (\w+)\);', l).group(1) for l in pops]
    push_regs = [re.match(r'\s*PUSH32\(esp, (\w+)\);', l).group(1) for l in pushes]
    assert push_regs == list(reversed(pop_regs)), (push_regs, pop_regs)
    print("ok  unbalanced_is_balanced_after_fixup:", push_regs)


def test_balanced_function_is_untouched():
    sample = _sample_balanced()
    out = _fixup_unbalanced_saves(list(sample))
    assert out == sample, "balanced function should be unchanged"
    print("ok  balanced_function_is_untouched")


def test_no_pop_means_no_change():
    lines = [
        "loc_0009504E: ;",
        "    PUSH32(esp, ebx);",
        "    PUSH32(esp, esi);",
        "    PUSH32(esp, edi);",
        "    esp += 4; return;",
    ]
    out = _fixup_unbalanced_saves(list(lines))
    assert out == lines
    print("ok  no_pop_means_no_change")


def test_preserves_function_entry_label():
    out = _fixup_unbalanced_saves(_sample_unbalanced())
    text = "\n".join(out)
    assert "loc_00084506: ;" in text
    # The very first statement after the label must be the first balanced push.
    idx = text.index("loc_00084506: ;")
    rest = text[idx:].split("\n")
    assert rest[1].strip() == "PUSH32(esp, ebp);", rest[1]
    print("ok  preserves_function_entry_label")


def _sample_seh_epilog():
    """The MSVC __SEH_epilog: pops edi/esi/ebx that the __SEH_prolog (a
    different function) pushed. Within this one function the pops outnumber
    the pushes, but rebalancing would inject self-pushes that make the epilog
    pop its own current registers (a rotation) instead of the prolog's saved
    frame slots — leaking callee-saved registers to the caller."""
    return [
        "void sub_00094FFB(void)",
        "{",
        "loc_00094FFB: ;",
        "    ecx = MEM32(ebp + -16);",
        "    MEM32(0) = ecx;",
        "    POP32(esp, ecx);",
        "    POP32(esp, edi);",
        "    POP32(esp, esi);",
        "    POP32(esp, ebx);",
        "    esp = ebp;",
        "    POP32(esp, ebp); /* leave */",
        "    PUSH32(esp, ecx);",
        "    g_seh_ebp = ebp; esp += 4; return;",
        "}",
    ]


def test_seh_epilog_is_not_rebalanced():
    sample = _sample_seh_epilog()
    # Without the exclusion the fixup injects 4 pushes (a rotation).
    changed = _fixup_unbalanced_saves(list(sample))
    assert len(_pushes(changed)) == 4, _pushes(changed)
    # With func_addr == seh_epilog the fixup must leave it untouched.
    out = _fixup_unbalanced_saves(list(sample),
                                  func_addr=0x00094FFB, seh_epilog=0x00094FFB)
    assert out == sample, "SEH epilog must not be rebalanced"
    print("ok  seh_epilog_is_not_rebalanced")


def _sample_arg_cleanup_epilog():
    """sub_0008726E-style shared epilogue helper: pushes the call args
    ([ebp-4], ecx, eax), calls a function, then pops the args back into
    edi/esi/ebx (stdcall arg cleanup that loads the args into the registers).
    The pops outnumber the pushes, but rebalancing would inject self-pushes
    that rotate the pop targets (edi/esi/ebx receive the arg slots instead of
    the saved frame slots), leaking callee-saved registers to the caller."""
    return [
        "void sub_0008726E(void)",
        "{",
        "loc_0008726E: ;",
        "    PUSH32(esp, MEM32(ebp + -4));",
        "    PUSH32(esp, ecx);",
        "    PUSH32(esp, eax);",
        "    PUSH32(esp, 0); sub_00086ED2(); /* call 0x00086ED2 */",
        "loc_00087278: ;",
        "    POP32(esp, edi);",
        "    POP32(esp, esi);",
        "    POP32(esp, ebx);",
        "    esp = ebp;",
        "    POP32(esp, ebp); /* leave */",
        "    esp += 4; return;",
        "}",
    ]


def test_arg_cleanup_epilog_is_not_rebalanced():
    sample = _sample_arg_cleanup_epilog()
    out = _fixup_unbalanced_saves(list(sample))
    assert out == sample, "arg-cleanup epilog must not be rebalanced"
    print("ok  arg_cleanup_epilog_is_not_rebalanced")


if __name__ == "__main__":
    test_unbalanced_is_balanced_after_fixup()
    test_balanced_function_is_untouched()
    test_no_pop_means_no_change()
    test_preserves_function_entry_label()
    test_seh_epilog_is_not_rebalanced()
    test_arg_cleanup_epilog_is_not_rebalanced()
    print("\nall passed")
