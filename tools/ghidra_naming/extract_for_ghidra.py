#!/usr/bin/env python3
"""Prepare an XBE for headless raw-binary import.

Read the caller's XBE section table and build one flat image, starting at its
header base_address. Place each section at virtual_addr - base_address and
zero-fill gaps/virtual tails. Outputs in --out-dir are xbe_flat.bin and
sections.json; the importer must use that metadata's base_address rather than
assuming a reference image's layout. This is a partial image preparation tool,
not a full XBE loader or malformed-input validator.

Usage: python extract_for_ghidra.py path/to/input.xbe --out-dir build/ghidra
"""
import argparse
import json
import os
import sys

# Make the sibling xbe_parser importable regardless of CWD.
_HERE = os.path.dirname(os.path.abspath(__file__))
_XBE_PARSER_DIR = os.path.normpath(os.path.join(_HERE, "..", "xbe_parser"))
sys.path.insert(0, _XBE_PARSER_DIR)

from xbe_parser import XBEParser, format_flags  # noqa: E402

# Sections we want in the flat image. We include every section that has raw
# bytes on disk; $$XTIMAGE / $$XSIMAGE etc. are tiny inserted-file blobs but
# harmless to include. We DO load all of them so RTTI/vtable/string xrefs
# from code into .rdata/.data resolve.


def build(xbe_path: str, out_dir: str, include_all: bool = True):
    os.makedirs(out_dir, exist_ok=True)
    parser = XBEParser(xbe_path)
    xbe = parser.parse()
    raw_data = parser.data  # full XBE file bytes
    base = xbe.header.base_address

    # Determine the span of the flat image: from base to the end of the
    # highest section that has raw data on disk.
    loadable = []
    for s in xbe.sections:
        if s.raw_size == 0:
            # Pure BSS-style; still reserve virtual space (zero fill).
            pass
        loadable.append(s)

    end_va = base
    for s in loadable:
        sec_end = s.virtual_addr + max(s.virtual_size, s.raw_size)
        if sec_end > end_va:
            end_va = sec_end

    image_size = end_va - base
    image = bytearray(image_size)

    placed = []
    for s in loadable:
        off = s.virtual_addr - base
        raw = raw_data[s.raw_addr:s.raw_addr + s.raw_size]
        # Clip if somehow overruns (shouldn't for a valid XBE).
        if off < 0:
            continue
        end = off + len(raw)
        if end > len(image):
            raw = raw[:len(image) - off]
            end = off + len(raw)
        image[off:end] = raw
        placed.append({
            "name": s.name,
            "virtual_addr": "0x%08X" % s.virtual_addr,
            "virtual_size": s.virtual_size,
            "raw_addr": "0x%08X" % s.raw_addr,
            "raw_size": s.raw_size,
            "flags": format_flags(s.flags),
            "executable": bool(s.flags & 0x4),
            "writable": bool(s.flags & 0x1),
            "image_offset": "0x%08X" % off,
        })

    flat_path = os.path.join(out_dir, "xbe_flat.bin")
    with open(flat_path, "wb") as f:
        f.write(image)

    meta = {
        "xbe_path": os.path.abspath(xbe_path),
        "base_address": "0x%08X" % base,
        "entry_point": "0x%08X" % xbe.header.entry_point,
        "image_size": image_size,
        "image_size_hex": "0x%08X" % image_size,
        "flat_image": os.path.abspath(flat_path),
        "language_id": "x86:LE:32:default",
        "compiler_spec": "windows",
        "sections": placed,
    }
    meta_path = os.path.join(out_dir, "sections.json")
    with open(meta_path, "w") as f:
        json.dump(meta, f, indent=2)

    print("Flat image:  %s (%d bytes, 0x%08X..0x%08X)" %
          (flat_path, image_size, base, end_va))
    print("Entry point: 0x%08X" % xbe.header.entry_point)
    print("Sections placed: %d" % len(placed))
    for p in placed:
        print("  %-12s @ %s  off=%s  size=0x%X  %s" %
              (p["name"], p["virtual_addr"], p["image_offset"],
               p["raw_size"], p["flags"]))
    print("Meta:        %s" % meta_path)
    return meta


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("xbe_path", help="Path to default.xbe")
    ap.add_argument("--out-dir", default=os.path.join(_HERE, "work"),
                    help="Output directory (default tools/ghidra_naming/work)")
    args = ap.parse_args()
    if not os.path.exists(args.xbe_path):
        print("Error: not found: %s" % args.xbe_path, file=sys.stderr)
        sys.exit(1)
    build(args.xbe_path, args.out_dir)


if __name__ == "__main__":
    main()
