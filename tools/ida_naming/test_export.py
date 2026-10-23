"""Caller-selected image base and export shape with mocked IDA APIs."""
import importlib.util
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
from unittest.mock import patch


def check_export():
    mocks = {name: SimpleNamespace() for name in
             ("ida_auto", "ida_bytes", "ida_funcs", "ida_nalt", "ida_name",
              "ida_typeinf", "idautils", "idc")}
    mocks["ida_auto"].auto_wait = lambda: None
    mocks["ida_nalt"].get_imagebase = lambda: 0x20000
    mocks["idautils"].Functions = lambda: []
    mocks["idautils"].Names = lambda: []
    with patch.dict("sys.modules", mocks):
        spec = importlib.util.spec_from_file_location(
            "ida_export", Path(__file__).with_name("export_xbe_names.py"))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            for label, options, expected in (
                ("trusted", [], 0),
                ("matched", ["--image-base", "0x20000"], 0),
                ("mismatched", ["--image-base", "0x10000"], 1),
            ):
                out = root / label
                mocks["idc"].ARGV = ["export_xbe_names.py", str(out)] + options
                assert module.main() == expected
                if expected:
                    assert not out.exists()
                else:
                    for name in ("functions.json", "symbols.json"):
                        assert json.loads((out / name).read_text()) == []


if __name__ == "__main__":
    check_export()
    print("IDA export: caller base, mismatch rejection and JSON shape passed")
