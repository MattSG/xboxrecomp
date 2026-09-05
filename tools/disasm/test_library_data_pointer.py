"""XDK *DATA sections can contain function-pointer tables."""
import struct

from tools.disasm.engine import DisasmEngine
from tools.disasm.functions import FunctionDetector
from tools.disasm.labels import LabelManager
from tools.disasm.loader import BinaryImage, SectionInfo
from tools.disasm.xrefs import XRefTracker


def test_library_data_pointer_seeds_function():
    base = 0x400000
    target = base + 8
    code = b"\x55\x8b\xec\x5d\xc3\x90\x90\x90\x56\x5e\xc3"
    table = struct.pack("<8I", *([target] * 8))
    text = SectionInfo("BINK", base, len(code), 0, len(code), False, True, "")
    data = SectionInfo("BINKDATA", base + 0x100, len(table), len(code),
                       len(table), False, True, "")
    image = BinaryImage("", code + table, base, 0x200, base, 0)
    image.sections = [text, data]
    engine = DisasmEngine(image)
    engine.linear_sweep(text)
    detector = FunctionDetector(engine, image, XRefTracker(), LabelManager())
    detector.detect_all([text])
    assert target in detector.functions
