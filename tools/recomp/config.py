"""Target XBE section mappings loaded from xbe_parser analysis JSON."""

import json


SECTIONS = []
CODE_SECTIONS = []
TEXT_VA_START = TEXT_VA_END = 0
RDATA_VA_START = RDATA_VA_END = 0
DATA_VA_START = DATA_VA_END = 0
KERNEL_THUNK_ADDR = ENTRY_POINT = 0

_DATA_SECTIONS = {".data", ".data1", ".rdata", "$$XTIMAGE", "$$XSIMAGE"}


def _number(value):
    return int(value, 0) if isinstance(value, str) else value


def configure(analysis_path):
    """Load all target-specific addresses before importing the translator."""
    global SECTIONS, CODE_SECTIONS, TEXT_VA_START, TEXT_VA_END
    global RDATA_VA_START, RDATA_VA_END, DATA_VA_START, DATA_VA_END
    global KERNEL_THUNK_ADDR, ENTRY_POINT

    with open(analysis_path, encoding="utf-8") as stream:
        analysis = json.load(stream)

    sections = analysis["sections"]
    SECTIONS = [
        (section["name"], _number(section["virtual_addr"]),
         _number(section["virtual_size"]), _number(section["raw_addr"]))
        for section in sections
    ]
    CODE_SECTIONS = [section for section in SECTIONS
                     if section[0] not in _DATA_SECTIONS]

    by_name = {section[0]: section for section in SECTIONS}
    text = by_name[".text"]
    TEXT_VA_START, TEXT_VA_END = text[1], text[1] + text[2]

    rdata = by_name.get(".rdata")
    RDATA_VA_START = rdata[1] if rdata else 0
    RDATA_VA_END = rdata[1] + rdata[2] if rdata else 0
    data_sections = [section for section in SECTIONS if section[0] in _DATA_SECTIONS]
    DATA_VA_START = min(section[1] for section in data_sections)
    DATA_VA_END = max(section[1] + section[2] for section in data_sections)
    KERNEL_THUNK_ADDR = _number(analysis["kernel_thunk_addr"])
    ENTRY_POINT = _number(analysis["entry_point"])


def va_to_file_offset(va):
    """Convert virtual address to XBE file offset."""
    for _, sec_va, sec_size, sec_raw in SECTIONS:
        if sec_va <= va < sec_va + sec_size:
            return va - sec_va + sec_raw
    return None


def is_code_address(va):
    """Check whether VA belongs to game or XDK code, excluding data sections."""
    return any(sec_va <= va < sec_va + sec_size
               for _, sec_va, sec_size, _ in CODE_SECTIONS)


def is_data_address(va):
    """Check whether VA belongs to a known data section."""
    return any(name in _DATA_SECTIONS and sec_va <= va < sec_va + sec_size
               for name, sec_va, sec_size, _ in SECTIONS)
