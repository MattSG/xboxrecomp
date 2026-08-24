from .analyze import Value, analyze, join, UNKNOWN

def test_conservative_provenance():
    state, events = analyze([
        {"address": 1, "mnemonic": "mov", "operands": ["esi", {"kind": "global", "address": 0x431680}]},
        {"address": 2, "mnemonic": "lea", "operands": ["eax", {"kind": "mem", "base": "esi", "disp": 0x14}]},
        {"address": 3, "mnemonic": "call", "operands": ["eax"]},
    ])
    assert str(state["eax"]).startswith("DERIVED")
    assert events[0]["kind"] == "indirect-control"
    assert join(Value("CONST", (1,)), Value("CONST", (2,))).kind == UNKNOWN
