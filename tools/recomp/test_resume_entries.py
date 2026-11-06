"""A function with resume entries can be entered at a registered address.

Callers that need to start in the middle of a function set g_recomp_resume;
the translated body then dispatches to the label for that address after its
prologue. With no resume entry, the body is unchanged from the ordinary
translation."""
import pytest

from . import config
from .translator import FunctionTranslator

BASE = 0x10000
# mov eax, 1 / mov eax, 2 (the resume entry) / ret
BODY = bytes.fromhex("b8010000 00 b802000000 c3".replace(" ", ""))
ENTRY = BASE + 5
END = BASE + len(BODY)


@pytest.fixture(autouse=True)
def layout(monkeypatch):
    monkeypatch.setattr(config, "_SECTIONS", [
        config.Section(".text", BASE, 0x400, 0, 0x400, True),
    ])


def function(start, end):
    return {"_addr": start, "start": hex(start), "end": end,
            "size": end - start, "section": ".text"}


def translator():
    return FunctionTranslator(BODY.ljust(0x400, b"\xcc"), {
        BASE: function(BASE, END),
    })


def test_resume_entry_gets_a_label_and_a_dispatch():
    subject = translator()
    subject.lifter.resume_entries.add(ENTRY)
    code = subject.translate_function(BASE, subject.func_db[BASE])
    assert "if (g_recomp_resume)" in code
    assert f"if (_resume == 0x{ENTRY:08X}u) goto loc_{ENTRY:08X};" in code
    assert f"loc_{ENTRY:08X}: ;" in code


def test_without_resume_entries_there_is_no_dispatch():
    subject = translator()
    code = subject.translate_function(BASE, subject.func_db[BASE])
    assert "g_recomp_resume" not in code
