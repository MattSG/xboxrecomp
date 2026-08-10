"""Self-check: data-reachable function starts are recovered via seeds.

MM3 sub_001BC979 (the DICE 'zip' handler) has zero direct call sites - it is
reachable only through the registry value dword (readTblA -> mapRead calls
[node+0x28]). The CFG never discovers it, so no generated function/dispatch
entry exists and RECOMP_ICALL_SAFE silently returns eax=0 (run 391 proved the
'zip' node IS in the registry tree with val=0x1BC979; sub_001BC8D3's span ends
right before it and no function covers 0x1BC979-0x1BC9A3). Seeding the
address must recover it as a function with the right extent.

    py -3 tools/disasm/test_seed_data_reachable.py
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from tools.disasm.engine import DisasmEngine  # noqa: E402
from tools.disasm.functions import FunctionDetector  # noqa: E402
from tools.disasm.labels import LabelManager  # noqa: E402
from tools.disasm.loader import BinaryImage, SectionInfo  # noqa: E402
from tools.disasm.xrefs import XRefTracker  # noqa: E402

BASE = 0x00400000


def assemble():
    """A0 (entry) + A1 (data-reachable island) + A2 (call target).

    A1 mirrors sub_001BC979: push esi; push imm; mov esi, ecx;
    mov eax, esi; pop esi; ret - no prologue signature, no direct caller.
    """
    a0 = BASE
    code = bytearray()
    code += bytes([0x55, 0x8B, 0xEC])                   # push ebp; mov ebp,esp
    a0_call = len(code)
    code += bytes([0xE8, 0, 0, 0, 0])                    # call A2 (patched)
    code += bytes([0xB8, 0, 0, 0, 0, 0x5D, 0xC3])        # mov eax,0; pop ebp; ret
    a1 = a0 + len(code)

    code += bytes([0x56, 0x68, 0x38, 0x01, 0x00, 0x00])  # push esi; push 0x138
    code += bytes([0x8B, 0xF1])                          # mov esi, ecx
    code += bytes([0x8B, 0xC6, 0x5E, 0xC3])              # mov eax,esi; pop esi; ret
    a2 = a0 + len(code)

    code += bytes([0x55, 0x8B, 0xEC, 0xB8, 0x01, 0x00, 0x00, 0x00,
                   0x5D, 0xC3])                          # A2: push ebp; ...; ret

    code += struct.pack("<I", a1)        # data dword: the only ref to A1
    data = bytearray(code)
    struct.pack_into("<i", data, a0_call + 1, a2 - (a0 + a0_call + 5))
    return bytes(data), a0, a1, a2


def make_engine(data, a0):
    sec = SectionInfo(name=".text", virtual_addr=BASE, virtual_size=len(data),
                      raw_addr=0, raw_size=len(data), writable=False,
                      executable=True, flags="")
    img = BinaryImage(filepath="", raw_data=data, base_address=BASE,
                      image_size=len(data), entry_point=a0,
                      kernel_thunk_addr=0)
    img.sections = [sec]
    eng = DisasmEngine(img)
    eng.linear_sweep(sec)
    return eng, img, sec


def run(seeded, a0, a1):
    data, _, _, _ = assemble()
    eng, img, sec = make_engine(data, a0)
    det = FunctionDetector(eng, img, XRefTracker(), LabelManager())
    if seeded:
        det._add_candidate(a1, 0.95, "seed_vtable_thunk")
    det.detect_all([sec])
    return det.functions


def test_data_reachable_needs_seed():
    data, a0, a1, a2 = assemble()
    funcs = run(False, a0, a1)
    assert a1 not in funcs, (
        "data-reachable fn 0x%X must not be discovered without a seed"
        % a1)
    funcs2 = run(True, a0, a1)
    assert a1 in funcs2, (
        "seed must recover the data-reachable function at 0x%X" % a1)
    assert funcs2[a1].end == a2, (
        "seeded extent must cover the whole body up to 0x%X, got 0x%X"
        % (a2, funcs2[a1].end))
    assert a0 in funcs2 and a2 in funcs2, "callers must stay detected"
    print("ok  seed_data_reachable (0x%X..0x%X)" % (a1, funcs2[a1].end))


if __name__ == "__main__":
    test_data_reachable_needs_seed()
