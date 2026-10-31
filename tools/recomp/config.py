"""
Recompiler configuration - section mappings and constants.

These describe the target XBE's memory layout. Call configure_from_xbe() before
translating anything; it reads the real section table out of the binary being
recompiled. Before configuration the layout is empty; synthetic callers must
install their own layout instead of inheriting a reference image.

Historically this file held a hand-edited section table for whichever game was
being worked on last, which silently truncated any larger binary -- every VA past
the stale table's end looked like "not code" and its functions failed to lift.
"""

from dataclasses import dataclass
from typing import List, Optional

# Section flag bit from the XBE format (see tools/xbe_parser).
SECTION_EXECUTABLE = 0x00000004

# Sections that hold code despite not being marked executable in the XBE, or
# that we always want treated as code. Matched by exact section name.
FORCE_CODE_SECTIONS = {"XIPS"}

# Sections that are resources/data even if flagged executable.
FORCE_DATA_SECTIONS = {
    ".data", ".data1", ".rdata",
}


@dataclass
class Section:
    name: str
    va: int
    va_size: int
    raw_addr: int
    raw_size: int
    is_code: bool


# No image is configured until the caller supplies its layout.
_SECTIONS: List[Section] = []
SECTIONS = []
TEXT_VA_START = TEXT_VA_END = 0
RDATA_VA_START = RDATA_VA_END = 0
DATA_VA_START = DATA_VA_END = 0
KERNEL_THUNK_ADDR = ENTRY_POINT = 0

_configured_from: Optional[str] = None

# Display name used in generated-code banners. Title-agnostic default; set it
# to match the game being recompiled via --game-name (see tools.recomp __main__)
# or set_game_name().
GAME_NAME = "Xbox Game"
_game_name_explicit = False


def set_game_name(name: str) -> None:
    """Set the display name stamped into generated code banners."""
    global GAME_NAME, _game_name_explicit
    if name:
        GAME_NAME = name
        _game_name_explicit = True


def banner_name(title: Optional[str] = None) -> str:
    """The name to stamp into a generated-code banner.

    Two mechanisms arrived at this from different directions and both are
    worth keeping: --game-name (an explicit override, from #17) and the title
    read out of the XBE header (automatic, and cannot go stale). The header
    wins over the default because it is right without anyone remembering a
    flag; the explicit override wins over the header because someone asked
    for it by name.
    """
    if _game_name_explicit:
        return GAME_NAME
    return title or GAME_NAME


def _classify(name: str, flags: int) -> bool:
    """Decide whether a section holds code."""
    if name in FORCE_DATA_SECTIONS:
        return False
    if name in FORCE_CODE_SECTIONS:
        return True
    return bool(flags & SECTION_EXECUTABLE)


def configure_from_xbe(xbe_path: str) -> None:
    """Load the real section layout from the XBE being recompiled.

    This replaces the complete prior image layout, including absent ranges.
    """
    from tools.xbe_parser.xbe_parser import XBEParser

    xbe = XBEParser(xbe_path).parse()
    sections = [
        Section(
            name=s.name,
            va=s.virtual_addr,
            va_size=s.virtual_size,
            raw_addr=s.raw_addr,
            raw_size=s.raw_size,
            is_code=_classify(s.name, s.flags),
        )
        for s in xbe.sections
    ]
    if not sections:
        raise ValueError(f"XBE has no sections: {xbe_path}")

    _install(sections, xbe.header.entry_point, xbe.header.kernel_thunk_addr, xbe_path)


def _install(sections, entry_point, kernel_thunk_addr, origin):
    global _SECTIONS, SECTIONS, _configured_from
    global TEXT_VA_START, TEXT_VA_END, RDATA_VA_START, RDATA_VA_END
    global DATA_VA_START, DATA_VA_END, KERNEL_THUNK_ADDR, ENTRY_POINT

    if not sections:
        raise ValueError("cannot install an empty XBE section layout")
    _SECTIONS = sorted(sections, key=lambda s: s.va)
    RDATA_VA_START = RDATA_VA_END = DATA_VA_START = DATA_VA_END = 0
    SECTIONS = [(s.name, s.va, s.va_size, s.raw_addr) for s in _SECTIONS]
    _configured_from = origin

    by_name = {s.name: s for s in _SECTIONS}

    text = by_name.get(".text") or next(
        (s for s in _SECTIONS if s.is_code), _SECTIONS[0]
    )
    TEXT_VA_START = text.va
    TEXT_VA_END = text.va + text.va_size

    rdata = by_name.get(".rdata")
    data = by_name.get(".data")
    # Some titles (the Dashboard among them) ship no .rdata; fall back to .data
    # so the read-only-data range is never left pointing at another game.
    rdata = rdata or data
    data = data or rdata
    if rdata:
        RDATA_VA_START = rdata.va
        RDATA_VA_END = rdata.va + rdata.va_size
    if data:
        DATA_VA_START = data.va
        DATA_VA_END = data.va + data.va_size

    ENTRY_POINT = entry_point
    KERNEL_THUNK_ADDR = kernel_thunk_addr


def configured_from() -> Optional[str]:
    """Path of the XBE this layout came from, or None if unconfigured."""
    return _configured_from


def va_to_file_offset(va):
    """Convert virtual address to XBE file offset."""
    for s in _SECTIONS:
        if s.va <= va < s.va + s.va_size:
            offset = va - s.va + s.raw_addr
            # Sections are zero-padded when virtual_size exceeds raw_size (BSS
            # tails); those addresses exist at runtime but have no file bytes.
            if va - s.va >= s.raw_size:
                return None
            return offset
    return None


# The longest x86 instruction. The loader's zero fill past a code section's
# raw_size is readable for this many bytes less one -- enough to finish an
# instruction that starts in the file bytes. Must equal
# tools/disasm/loader.py's MAX_INSN_LEN, which bounds the disassembler's fill.
MAX_INSN_LEN = 15


def section_at(va) -> Optional[Section]:
    """The section whose virtual range holds VA, or None."""
    for s in _SECTIONS:
        if s.va <= va < s.va + s.va_size:
            return s
    return None


def readable_end(section: Section) -> int:
    """Exclusive end of the bytes a code section's functions may read.

    The XBE loader zero-fills from raw_size to virtual_size, and Spartan's
    D3DX section relies on it: its raw size stops one byte short of the
    `ret 0x10` that ends D3DXMatrixLookAtLH. The disassembler reads that
    fill for at most one instruction, so the lifter does too; a start in it
    is still not code (va_to_file_offset returns None there).
    """
    raw_end = section.va + section.raw_size
    if not section.is_code or section.va_size <= section.raw_size:
        return raw_end
    return raw_end + min(section.va_size - section.raw_size, MAX_INSN_LEN - 1)


def is_code_address(va):
    """Check if VA is in an executable section (.text or XDK library sections)."""
    for s in _SECTIONS:
        if s.va <= va < s.va + s.va_size:
            return s.is_code
    return False


def is_data_address(va):
    """Check if VA is in a data section."""
    for s in _SECTIONS:
        if s.va <= va < s.va + s.va_size:
            return not s.is_code
    return False
