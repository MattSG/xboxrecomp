import argparse
import json
from .metadata import load, resolve

def report(metadata, calls):
    result = []
    for call in calls:
        target = call.get("target")
        if isinstance(target, str):
            target = int(target, 0)
        match = resolve(metadata, target)
        result.append({"caller_eip": call.get("caller_eip"), "requested_target": target,
                       "resolution": match, "provenance": call.get("provenance", "UNKNOWN"),
                       "classification": match.get("classification", "unknown")})
    return result

def main():
    parser = argparse.ArgumentParser(description="Report checked target resolution for indirect calls")
    parser.add_argument("metadata")
    parser.add_argument("calls")
    parser.add_argument("--resolve-all-calls", action="store_true")
    args = parser.parse_args()
    if not args.resolve_all_calls:
        parser.error("pass --resolve-all-calls to enable diagnostic reporting")
    calls = json.loads(open(args.calls, encoding="utf-8").read())
    print(json.dumps(report(load(args.metadata), calls), indent=2))

if __name__ == "__main__":
    main()
