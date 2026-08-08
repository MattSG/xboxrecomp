"""Self-check: seh_prologue false positives merge into the parent function.

The seh_prologue heuristic (push imm8; push imm32; call rel32) is byte-for-
byte identical to a normal two-arg call sequence, so a continuation chunk of
the previous function is routinely mis-detected as a function start (MM3
sub_001EC520 -> 0x001EC5D0). The chunk shares the parent's frame, pops the
registers the parent pushed, and returns with the parent's ret 0xc; splitting
it leaked the caller's esi and left the fall-through path unbalanced. A
candidate referenced only by jumps (or plain fall-through) is a pure
continuation and must not bound the parent: the parent's CFG walk falls
through / jumps into it and merges it. A candidate with any CALL, data_imm
(SEH handler table), or other reference is a real function start and still
bounds the parent.

    py -3 tools/disasm/test_continuation_merge.py
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
from tools.disasm.xrefs import XRef, XRefTracker, XRefType  # noqa: E402

BASE = 0x00400000
REL32 = lambda target, nxt: struct.pack("<i", target - nxt)


def assemble():
    # Build exact bytes with computed rel32s.
    pre = bytes([0x55, 0x8B, 0xEC, 0x56, 0x57, 0x8B, 0xF8, 0x83, 0xF8, 0x00])
    jne_off = len(pre)
    nxt = BASE + jne_off + 6
    body = bytes([0x90, 0x90])
    call_off = len(pre) + 6 + len(body)
    sub = BASE + 0x200
    parent_len = call_off + 5
    cont = BASE + parent_len
    jne = b"\x0f\x85" + REL32(cont, nxt)
    call = b"\xe8" + REL32(sub, BASE + call_off + 5)
    chunk = (bytes([0x6A, 0x01]) + b"\x68\xdc\x07\x3b\x00"
             + b"\xe8" + REL32(BASE + 0x100, cont + 0x07)
             + bytes([0x5F, 0x5E, 0xC9, 0xC2, 0x0C, 0x00]))
    data = pre + jne + body + call + chunk
    cont_end = BASE + len(data)
    return data, cont, cont_end


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


def run(ref_type=None):
    data, cont, cont_end = assemble()
    eng, img, sec = make_engine(data)
    xrefs = XRefTracker()
    if ref_type is not None:
        xrefs.add(XRef(from_addr=BASE + 0x300, to_addr=cont,
                       xref_type=ref_type))
    det = FunctionDetector(eng, img, xrefs, LabelManager())
    det._candidates[BASE] = (0.95, "prologue")
    det._candidates[cont] = (0.855, "seh_prologue")
    det._build_functions([sec])
    return det.functions, cont, cont_end


def test_no_ref_merges_continuation():
    funcs, cont, cont_end = run(ref_type=None)
    parent = funcs[BASE]
    assert parent.end == cont_end, (
        "parent must swallow the continuation, got end 0x%X want 0x%X"
        % (parent.end, cont_end))
    assert cont in funcs, "continuation keeps its own entry point"
    print("ok  no_ref_merges_continuation")


def test_jump_ref_merges_continuation():
    funcs, cont, cont_end = run(ref_type=XRefType.COND_JUMP)
    parent = funcs[BASE]
    assert parent.end == cont_end, (
        "a jump-only candidate must merge, got end 0x%X want 0x%X"
        % (parent.end, cont_end))
    print("ok  jump_ref_merges_continuation")


def test_called_candidate_still_bounds():
    funcs, cont, cont_end = run(ref_type=XRefType.CALL)
    parent = funcs[BASE]
    assert parent.end == cont, (
        "a called candidate must bound the parent, got end 0x%X want 0x%X"
        % (parent.end, cont))
    print("ok  called_candidate_still_bounds")


def test_data_imm_candidate_still_bounds():
    funcs, cont, cont_end = run(ref_type=XRefType.DATA_IMM)
    parent = funcs[BASE]
    assert parent.end == cont, (
        "a data_imm-referenced candidate (handler table) must bound the "
        "parent, got end 0x%X want 0x%X" % (parent.end, cont))
    print("ok  data_imm_candidate_still_bounds")


if __name__ == "__main__":
    test_no_ref_merges_continuation()
    test_jump_ref_merges_continuation()
    test_called_candidate_still_bounds()
    test_data_imm_candidate_still_bounds()
    print("all ok")
