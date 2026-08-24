import argparse
import hashlib
import json
from pathlib import Path

def build(xbe, functions):
    digest = hashlib.sha256(Path(xbe).read_bytes()).hexdigest()
    entries = json.loads(Path(functions).read_text(encoding="utf-8"))
    result = []
    for item in entries:
        start = int(item["start"], 0)
        end = int(item.get("end", start + item.get("size", 0)), 0) if isinstance(item.get("end", start), str) else item.get("end", start + item.get("size", 0))
        if end <= start:
            continue
        result.append({"guest_start": start, "guest_end": end,
                       "symbol": item.get("name", f"sub_{start:08X}"),
                       "xbe_section": item.get("section", "unknown"),
                       "xbe_hash": digest,
                       "secondary_entries": item.get("secondary_entries", []),
                       "known_direct_callers": item.get("called_by", []),
                       "classification": item.get("classification", "unknown")})
    return {"format": 1, "xbe_sha256": digest, "functions": result}

def main():
    parser = argparse.ArgumentParser(description="Build guest-address resolver metadata")
    parser.add_argument("--xbe", required=True)
    parser.add_argument("--functions", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    Path(args.out).write_text(json.dumps(build(args.xbe, args.functions), indent=2) + "\n", encoding="utf-8")

if __name__ == "__main__":
    main()
