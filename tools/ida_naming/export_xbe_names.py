"""Export an Xbox XBE's recovered names from IDA, in the shape merge_names reads.

Export database functions and symbols for the shared name-merging tool.

Writes two files into an output directory:

  functions.json   [{address, name, signature, calling_convention,
                     param_count, is_thunk, namespace}, ...]
  symbols.json     [{address, name, type, namespace, source, primary}, ...]

then:

  py -3 tools/ghidra_naming/merge_names.py \
      --export-dir tools/ida_naming/export \
      --out tools/ida_naming/ida_names.json

Addresses are absolute database VAs. Import the flat image at the base_address
from its sections.json metadata. Optionally pass --image-base <address> to
verify the database base against that expected value before exporting.

Run it against an open database, from the GUI (File > Script file...) or
headless:

    ida -A -S"tools/ida_naming/export_xbe_names.py <outdir> --exit" xbe_flat.idb
"""
import argparse
import json
import os

import ida_auto
import ida_bytes
import ida_funcs
import ida_nalt
import ida_name
import ida_typeinf
import idautils
import idc

_HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_OUT = os.path.join(_HERE, "export")


def _hexaddr(ea):
    return "0x%08X" % ea


def _signature(ea):
    """IDA's prototype for the function, or '' when it has none."""
    try:
        tif = ida_typeinf.tinfo_t()
        if ida_nalt.get_tinfo(tif, ea):
            return str(tif)
    except Exception:
        pass
    return idc.get_type(ea) or ""


def export_functions(out_dir):
    out = []
    for ea in idautils.Functions():
        f = ida_funcs.get_func(ea)
        if not f or f.start_ea != ea:
            continue
        # FUNC_LIB is FLIRT: the CRT and SDK routines IDA recognised by
        # signature. That is the same claim Ghidra's FidDb makes, and
        # merge_names ranks it above every other bucket, so say so.
        source = "IMPORTED" if (f.flags & ida_funcs.FUNC_LIB) else "ANALYSIS"
        out.append({
            "address": _hexaddr(ea),
            "name": ida_name.get_name(ea) or "",
            "signature": _signature(ea),
            "calling_convention": "",
            "param_count": -1,
            "is_thunk": bool(f.flags & ida_funcs.FUNC_THUNK),
            "namespace": "",
            "source": source,
        })
    path = os.path.join(out_dir, "functions.json")
    with open(path, "w") as fh:
        json.dump(out, fh, indent=1)
    print("Exported %d functions -> %s" % (len(out), path))
    return len(out)


def export_symbols(out_dir):
    out = []
    for ea, name in idautils.Names():
        f = ida_funcs.get_func(ea)
        is_func = bool(f) and f.start_ea == ea
        flags = ida_bytes.get_flags(ea)
        out.append({
            "address": _hexaddr(ea),
            "name": name,
            "type": "Function" if is_func else "Label",
            "namespace": "",
            # A name the analyst typed beats one IDA invented, and merge_names
            # only reads this to bucket the result for the report.
            "source": ("USER_DEFINED" if ida_bytes.has_user_name(flags)
                       else "ANALYSIS"),
            "primary": True,
        })
    path = os.path.join(out_dir, "symbols.json")
    with open(path, "w") as fh:
        json.dump(out, fh, indent=1)
    print("Exported %d symbols -> %s" % (len(out), path))
    return len(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("out_dir", nargs="?", default=DEFAULT_OUT)
    parser.add_argument("--image-base", type=lambda value: int(value, 0),
                        help="Expected database image base, from sections.json")
    parser.add_argument("--exit", action="store_true")
    args = parser.parse_args(idc.ARGV[1:])
    out_dir = args.out_dir

    ida_auto.auto_wait()
    base = ida_nalt.get_imagebase()
    if args.image_base is not None and base != args.image_base:
        print("ERROR: image base is 0x%08X, expected 0x%08X." %
              (base, args.image_base))
        return 1

    if not os.path.isdir(out_dir):
        os.makedirs(out_dir)
    export_functions(out_dir)
    export_symbols(out_dir)
    print("Now run: py -3 tools/ghidra_naming/merge_names.py --export-dir %s"
          % out_dir)
    return 0


if __name__ == "__main__":
    rc = main()
    # Headless runs have to close IDA themselves; a GUI run must not. Asking
    # is one word and beats guessing from the command line IDA was started
    # with, which differs between versions.
    if "--exit" in idc.ARGV:
        idc.qexit(rc)
