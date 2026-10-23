"""Vtable ranges come from metadata or known functions, not section names."""
import struct
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


if __name__ == "__main__":
    test_ranges()
    print("Vtable metadata, known-function fallback and section tail passed")
