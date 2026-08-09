"""Self-check: switch-dispatch jump tables seed their case leaves.

The linear sweep decodes the dispatch (movzx; sub; jmp [edx*4+table]) and
whatever falls after it, but the table's other case leaves are reachable
only through the table dwords, so they never become function-start
candidates. The recomp then emits RECOMP_ITAIL for a VA with no generated
function -> unresolved switch tail that drops the caller's frame (MM3
sub_00346C80: the runtime fell to safe_stub and leaked 4 esp bytes into
sub_00341E50). Reading the table dwords must recover every leaf.

    py -3 tools/disasm/test_jump_table_seed.py
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


def leaf(va):
    return struct.pack("<I", va)


def assemble():
    # Dispatch: movzx edx, byte [ecx+0xD]; sub edx, 0x2A;
    #           jmp dword [edx*4 + TABLE]  (mirrors MM3 0x346C80)
    table_va = BASE + 0x2C
    dispatch = (bytes([0x0F, 0xB6, 0x51, 0x0D])
                + bytes([0x83, 0xEA, 0x2A])
                + bytes([0xFF, 0x24, 0x95])
                + struct.pack("<I", table_va))
    d = BASE + len(dispatch)
    # Four 7-byte leaves: mov dword [eax], imm32; ret
    leaves = []
    code = bytearray(dispatch)
    for imm in (0x477FFF00, 0x4B7FFFFF, 0x43FFF800, 0x7149F2CA):
        leaves.append(d)
        code += bytes([0xC7, 0x00]) + struct.pack("<I", imm) + bytes([0xC3])
        d += 7
    assert d == table_va - 2, hex(d)
    code += bytes(2)  # padding before the 4-aligned table
    table = (leaf(BASE + 0x0E) + leaf(BASE + 0x15) + leaf(BASE + 0x1C)
             + leaf(BASE + 0x23) + leaf(BASE + 0x0E))
    code += table
    return bytes(code), leaves


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


def run():
    data, leaves = assemble()
    eng, img, sec = make_engine(data)
    det = FunctionDetector(eng, img, XRefTracker(), LabelManager())
    det._candidates[BASE] = (0.95, "prologue")
    det._pass_indirect_jump_tables([sec])
    det._build_functions([sec])
    return det.functions, leaves


def test_seeds_all_switch_leaves():
    funcs, leaves = run()
    for leaf_va in leaves:
        assert leaf_va in funcs, (
            "table leaf 0x%X must become a function" % leaf_va)
        assert funcs[leaf_va].end == leaf_va + 7, (
            "leaf 0x%X extent must cover mov+ret, got end 0x%X"
            % (leaf_va, funcs[leaf_va].end))
    assert BASE in funcs, "dispatch keeps its own function"
    print("ok  seeds_all_switch_leaves")


if __name__ == "__main__":
    test_seeds_all_switch_leaves()
