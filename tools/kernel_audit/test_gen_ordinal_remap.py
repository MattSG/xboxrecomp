"""Synthetic checks for explicit name-based ordinal-map generation.

Run: python tools/kernel_audit/test_gen_ordinal_remap.py
These fixtures test matching labels, changed source IDs and unresolved names;
they do not establish any title's actual ABI or XDK compatibility.
"""

import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.kernel_audit import gen_ordinal_remap as G  # noqa: E402


def _analysis(tmp, imports):
    p = os.path.join(tmp, "a.json")
    json.dump({"title": "T", "kernel_imports": imports}, open(p, "w"))
    return p


def _canon(monkey):
    """Force a known canonical name->ordinal for deterministic tests."""
    G.canonical_name_to_ordinal = lambda: monkey


def test_matching_labels_emit_identity():
    _canon({"DbgPrint": 8, "ExFreePool": 17, "HalReturnToFirmware": 49})
    with tempfile.TemporaryDirectory() as tmp:
        a = _analysis(tmp, [
            {"ordinal": 8, "name": "DbgPrint"},
            {"ordinal": 17, "name": "ExFreePool"},
            {"ordinal": 49, "name": "HalReturnToFirmware"},
        ])
        remap, identity, unresolved, total = G.build_remap(a)
    assert remap == {}, remap
    assert identity == 3 and total == 3 and unresolved == []


def test_changed_source_ids_remap_by_name():
    # A title where HalRequestSoftwareInterrupt sits at 49, not the canonical 48.
    _canon({"HalRequestSoftwareInterrupt": 48, "HalReturnToFirmware": 49,
            "ExFreePool": 17})
    with tempfile.TemporaryDirectory() as tmp:
        a = _analysis(tmp, [
            {"ordinal": 49, "name": "HalRequestSoftwareInterrupt"},  # -> 48
            {"ordinal": 17, "name": "ExFreePool"},                   # identity
        ])
        remap, identity, unresolved, _ = G.build_remap(a)
    assert remap == {49: 48}, remap
    assert identity == 1 and unresolved == []


def test_unknown_and_absent_names_left_identity_and_reported():
    _canon({"ExFreePool": 17})
    with tempfile.TemporaryDirectory() as tmp:
        a = _analysis(tmp, [
            {"ordinal": 17, "name": "ExFreePool"},        # identity
            {"ordinal": 8, "name": "Unknown_8"},          # parser gap
            {"ordinal": 99, "name": "SomeNewXdkFunc"},    # not in canonical
        ])
        remap, identity, unresolved, _ = G.build_remap(a)
    assert remap == {}, remap
    assert identity == 1
    assert sorted(unresolved) == [(8, "Unknown_8"), (99, "SomeNewXdkFunc")], unresolved


def test_emitted_header_is_valid_c_and_indexable():
    _canon({"HalRequestSoftwareInterrupt": 48})
    with tempfile.TemporaryDirectory() as tmp:
        a = _analysis(tmp, [{"ordinal": 49, "name": "HalRequestSoftwareInterrupt"}])
        remap, _, _, _ = G.build_remap(a)
        out = os.path.join(tmp, "remap.h")
        G.emit_header(remap, out, "T", a)
        text = open(out).read()
    # array sized to cover the highest remapped ordinal, entry 49 -> 48, rest 0
    assert "g_xbox_ordinal_remap[50]" in text, text
    assert "XBOX_ORDINAL_REMAP_COUNT 50" in text, text
    # crude parse of the initializer to confirm index 49 == 48, others 0
    import re
    body = re.search(r"\{(.*?)\}", text, re.S).group(1)
    vals = [int(x) for x in re.findall(r"\d+", body)]
    assert len(vals) == 50 and vals[49] == 48 and sum(vals) == 48, vals[:5]


def test_cli_reports_unresolved_without_claiming_abi_match():
    from contextlib import redirect_stdout
    from pathlib import Path
    import io
    _canon({"ExFreePool": 17})
    with tempfile.TemporaryDirectory() as tmp:
        source = Path(tmp) / "analysis.json"
        output = Path(tmp) / "remap.h"
        source.write_text(json.dumps({"title": "Synthetic \u03b1", "kernel_imports": [
            {"ordinal": 99, "name": "Unknown_99"}]}), encoding="utf-8")
        stdout = io.StringIO()
        with redirect_stdout(stdout):
            assert G.main([str(source), "--out", str(output)]) == 0
        assert "Synthetic \u03b1" in output.read_text(encoding="utf-8")
        assert "ordinal 99" in stdout.getvalue()
        assert "review unresolved imports and ABI evidence" in stdout.getvalue()


def _run():
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    _orig = G.canonical_name_to_ordinal
    try:
        for fn in fns:
            fn()
            print("  ok  %s" % fn.__name__)
    finally:
        G.canonical_name_to_ordinal = _orig
    print("%d checks passed" % len(fns))


if __name__ == "__main__":
    _run()
