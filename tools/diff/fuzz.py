"""Deterministic bounded x86 semantic case generator."""
import argparse
import json
import random
from pathlib import Path
from .case import Case
from .runner import run_unicorn, RunnerUnavailable

OPS = ("01d8", "11d8", "29d8", "19d8", "39d8", "85d8", "40", "48", "d1e0", "d1e8", "d1f8")
REGS = ("eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp", "eflags")

def make_case(seed, count):
    rng = random.Random(seed)
    code = "".join(rng.choice(OPS) for _ in range(count))
    return Case({"name": f"x86-semantics-{seed}", "code": code, "entry_eip": 0x1000,
        "stop": {"instructions": count}, "state": {name: rng.getrandbits(32) for name in REGS},
        "memory": [{"address": 0x8000, "data": "00" * 4096}], "calls": [], "seed": seed}).validate()

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--cases", type=int, default=10)
    parser.add_argument("--instructions", type=int, default=16)
    parser.add_argument("--out", default="cases/fuzz")
    args = parser.parse_args()
    try:
        for offset in range(args.cases):
            case = make_case(args.seed + offset, args.instructions)
            path = Path(args.out) / case.data["name"]
            path.mkdir(parents=True, exist_ok=True)
            case.save(path / "case.json")
            (path / "oracle.json").write_text(json.dumps(run_unicorn(case), indent=2) + "\n", encoding="utf-8")
        print(f"generated {args.cases} deterministic cases from seed {args.seed}")
    except RunnerUnavailable as error:
        print(f"cases generated; oracle skipped: {error}")

if __name__ == "__main__":
    main()
