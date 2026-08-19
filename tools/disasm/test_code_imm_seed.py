"""Self-check: code-immediate function pointers seed function starts.

MM3's D3DX vtable constructor stores function pointers as immediates
(`mov [eax+0x28], 0x2680ED`). The pointed-to entry has no prologue and no
direct caller, so the prologue/call-target passes miss it; the runtime then
falls through to the D3DX safe stub and the guest state diverges. A pass over
`imm_ref` values that point at decoded instruction boundaries in code sections
must recover the function.

    py -3 tools/disasm/test_code_imm_seed.py
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


def imm32(v):
    return struct.pack("<I", v)


def assemble():
    a0 = BASE
    body = bytearray()
    body += bytes([0x55, 0x8B, 0xEC])             # push ebp; mov ebp,esp
    # mov dword ptr [eax + 0x28], a1  (a1 patched below)
    body += bytes([0xC7, 0x40, 0x28, 0, 0, 0, 0])
    body += bytes([0xB8, 0x01, 0x00, 0x00, 0x00]) # mov eax,1
    body += bytes([0x5D, 0xC3])                   # pop ebp; ret
    a1 = BASE + len(body)
    body += bytes([0x56, 0x8B, 0x74, 0x24, 0x08, 0x5E, 0xC3])
    struct.pack_into("<I", body, 3 + 3, a1)
    return bytes(body), a1


def make_engine(data):
    sec = SectionInfo(name=".text", virtual_addr=BASE, virtual_size=len(data),
                      raw_addr=0, raw_size=len(data), writable=False,
                      executable=True, flags="")
    img = BinaryImage(filepath="", raw_data=data, base_address=BASE,
                      image_size=len(data), entry_point=BASE,
                      kernel_thunk_addr=0)
    img.sections = [sec]
    eng = DisasmEngine(img)
    eng.linear_sweep(sec)
    return eng, img, sec


def test_code_imm_seed_recovers_entry():
    data, a1 = assemble()
    eng, img, sec = make_engine(data)
    det = FunctionDetector(eng, img, XRefTracker(), LabelManager())
    det.detect_all([sec])
    assert a1 in det.functions, (
        "code-immediate target 0x%X must become a function" % a1)
    assert det.functions[a1].start == a1
    print("ok  code_imm_seed (0x%X..0x%X)" %
          (a1, det.functions[a1].end))


if __name__ == "__main__":
    test_code_imm_seed_recovers_entry()
