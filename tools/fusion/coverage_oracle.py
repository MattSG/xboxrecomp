"""Compare reference symbol starts with function detection for the same donor.

The supplied module's named-library starts are reference metadata, not an
exhaustive inventory of all guest functions or evidence of semantic correctness.
Report exact-start hits and missed starts inside detected functions or gaps.
Missed-inside cases are candidates for inspecting merged function boundaries.

    python -m tools.fusion.coverage_oracle <module.dll> <our_functions.json>

Generate functions.json from the guest XBE corresponding to that module build.
A different retail/updated build can produce address differences unrelated to
the detector. All module, guest and detector files are caller inputs.
"""
import bisect
import json
import sys

from tools.fusion.module import FusionModule


def load_our_functions(path):
    d = json.load(open(path, encoding="utf-8"))
    fs = d["functions"] if isinstance(d, dict) and "functions" in d else d
    it = fs.values() if isinstance(fs, dict) else fs
    spans = []
    for f in it:
        st = f["start"]; va = int(st, 16) if isinstance(st, str) else st
        end = None
        if "end" in f:
            end = f["end"]; end = int(end, 16) if isinstance(end, str) else end
        elif "size" in f and f["size"]:
            sz = f["size"] if isinstance(f["size"], int) else int(str(f["size"]), 0)
            end = va + sz
        spans.append((va, end if end else va + 1))
    spans.sort()
    return spans


def main(module_path, funcs_path):
    m = FusionModule(module_path)
    ours = load_our_functions(funcs_path)
    starts = [s for s, _ in ours]
    our_start_set = set(starts)

    # unique MS function starts (dedupe the repeated small funcs)
    ms_starts = sorted({s.guest_start for s in m.symbols})

    hit = missed_inside = missed_gap = 0
    inside_examples = []
    ms_name = {}
    for s in m.symbols:
        ms_name.setdefault(s.guest_start, s.name)

    for a in ms_starts:
        if a in our_start_set:
            hit += 1
            continue
        # which of our functions contains a, if any?
        i = bisect.bisect_right(starts, a) - 1
        if i >= 0 and ours[i][0] <= a < ours[i][1]:
            missed_inside += 1
            if len(inside_examples) < 15:
                inside_examples.append((a, ms_name[a], ours[i][0]))
        else:
            missed_gap += 1

    total = len(ms_starts)
    print(f"module: {m.source} [{m.build_tree}]  MS-named function starts: {total:,}")
    print(f"  hit (we detect a start there):      {hit:,}  ({100.0*hit/total:.1f}%)")
    print(f"  missed, inside a detected function: {missed_inside:,}  <- under-segmentation")
    print(f"  missed, in no detected function:    {missed_gap:,}  <- uncovered")
    print("\n  sample missed-inside (MS start : our containing function start):")
    for a, nm, host in inside_examples:
        print(f"    0x{a:08X} {nm:40} inside 0x{host:08X}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
