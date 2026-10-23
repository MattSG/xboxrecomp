"""Synthetic flat-image placement and headless import arguments."""
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from tools.conformance import mkxbe

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("ghidra_extract", HERE / "extract_for_ghidra.py")
extract = importlib.util.module_from_spec(spec)
spec.loader.exec_module(extract)

class ExtractTest(unittest.TestCase):
    def test_image_placement_follows_header_base(self):
        for base in (0x10000, 0x20000):
            with self.subTest(base=base), tempfile.TemporaryDirectory() as folder:
                work = Path(folder)
                data = bytearray(mkxbe.build())
                for offset in (0x104, 0x118, 0x120, 0x128, 0x13C, 0x404, 0x414):
                    value, = struct.unpack_from("<I", data, offset)
                    struct.pack_into("<I", data, offset, value + base - mkxbe.BASE)
                image = work / "input.xbe"
                image.write_bytes(data)
                meta = extract.build(str(image), str(work / "out"))
                self.assertEqual(int(meta["base_address"], 16), base)
                section = meta["sections"][0]
                self.assertEqual(int(section["image_offset"], 16), mkxbe.TEXT_OFF)
                flat = Path(meta["flat_image"]).read_bytes()
                self.assertEqual(flat[:mkxbe.TEXT_OFF], bytes(mkxbe.TEXT_OFF))
                self.assertEqual(flat[mkxbe.TEXT_OFF:], data[mkxbe.TEXT_OFF:])
                self.assertEqual(json.loads((work / "out/sections.json").read_text()), meta)

    @unittest.skipIf(os.name == "nt", "POSIX shell fixture; no Ghidra required")
    def test_import_forwards_metadata_base(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            here = root / "tool"
            here.mkdir()
            (here / "run_ghidra.sh").write_text((HERE / "run_ghidra.sh").read_text())
            (here / "extract_for_ghidra.py").write_text(
                "import sys,json,os\nout=sys.argv[sys.argv.index('--out-dir')+1]\n"
                "os.makedirs(out,exist_ok=True)\n"
                "json.dump({'base_address':'0x20000'},open(os.path.join(out,'sections.json'),'w'))\n")
            install = root / "ghidra/support"
            install.mkdir(parents=True)
            headless = install / "analyzeHeadless.bat"
            headless.write_text('#!/bin/sh\nfor arg do echo "$arg"; done > "$ARG_LOG"\n')
            headless.chmod(0o755)
            bindir = root / "bin"
            bindir.mkdir()
            py = bindir / "py"
            py.write_text('#!/bin/sh\n[ "$1" != "-3" ] || shift\nexec python3 "$@"\n')
            py.chmod(0o755)
            image = root / "input.xbe"
            image.write_bytes(b"synthetic")
            env = {**os.environ, "GHIDRA_HOME": str(install.parent), "XBE": str(image),
                   "IMPORT": "1", "ANALYZE": "0", "ARG_LOG": str(root / "args"),
                   "PATH": str(bindir) + os.pathsep + os.environ["PATH"]}
            run = subprocess.run(["bash", str(here / "run_ghidra.sh")], env=env,
                                 capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            args = (root / "args").read_text().splitlines()
            self.assertEqual(args[args.index("-loader-baseAddr") + 1], "0x20000")

if __name__ == "__main__":
    unittest.main()
