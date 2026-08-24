import argparse
import subprocess
import time

def timed(command):
    start = time.perf_counter()
    result = subprocess.run(command, shell=True, check=False)
    return time.perf_counter() - start, result.returncode

def main():
    parser = argparse.ArgumentParser(description="Record reproducible XboxRecomp build timings")
    parser.add_argument("--clean", required=True)
    parser.add_argument("--incremental", required=True)
    args = parser.parse_args()
    for name, command in (("clean", args.clean), ("incremental", args.incremental)):
        seconds, code = timed(command)
        print(f"{name}\t{seconds:.3f}s\texit={code}")
        if code:
            return code
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
