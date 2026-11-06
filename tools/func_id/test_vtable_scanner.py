"""Vtable ranges come from metadata or known functions, not section names."""
import struct
import tempfile
import json
from pathlib import Path
from .output import write_results
from unittest.mock import patch
from . import config
from .vtable_scanner import scan_vtables


def test_ranges():
    entries = [0x2010, 0x2030, 0x2070]
    data = struct.pack("<III", *entries)
    sections = [dict(name="CodeBlock", va=0x2000, size=0x100,
                     raw=12, raw_size=0, executable=True),
                dict(name="EnglishXlate", va=0x3000, size=12,
                     raw=0, raw_size=12, executable=False)]
    results, tables = scan_vtables(data, [], {}, sections=sections)
    assert len(tables) == 1 and tables[0]["entries"] == entries, tables
    assert set(results) == set(entries)
    functions = [{"start": hex(a), "size": 1} for a in entries]
    with patch.object(config, "SECTIONS", [("EnglishXlate", 0x3000, 12, 0)]):
        results, tables = scan_vtables(data, functions, {})
        assert len(tables) == 1 and set(results) == set(entries)
        assert scan_vtables(data, [], {}) == ({}, [])
    sections[0]["executable"] = False
    assert scan_vtables(data, [], {}, sections=sections) == ({}, [])


def test_constructor_output():
    data = struct.pack("<III", 0x2010, 0x2030, 0x2070)
    data += b"\xb8" + struct.pack("<I", 0x3000) + b"\x90\x90\xc3"
    sections = [dict(name="code", va=0x2000, size=0x100, raw=12,
                     raw_size=8, executable=True),
                dict(name="tables", va=0x3000, size=12, raw=0,
                     raw_size=12, executable=False)]
    functions = [dict(start=hex(a), end=hex(a+n), size=n, name="candidate",
                      section="code") for a, n in
                 ((0x2000, 8), (0x2010, 1), (0x2030, 1), (0x2070, 1))]
    # Only the first vtable target is known; the other two are discovered.
    with patch.object(config, "SECTIONS", [("code", 0x2000, 0x100, 12)]):
        results, tables = scan_vtables(data, functions[:2], {}, sections=sections)
    with tempfile.TemporaryDirectory() as td:
        summary = write_results(functions, {}, {}, results, {}, td)
        saved = json.loads((Path(td) / "identified_functions.json").read_text())
        actual = (results[0x2000]["subcategory"],
                  summary["classification"]["vtable_methods"], saved[0]["subcategory"])
        assert actual == (tables[0]["class_id"], 4, tables[0]["class_id"]), actual


if __name__ == "__main__":
    test_ranges()
    test_constructor_output()
    print("Vtable metadata, known-function fallback and section tail passed")
