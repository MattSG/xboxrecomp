"""Run the generic title overlay/upload contract (MSVC environment required).

Title-specific HUD and minimap placement belong to each title's own regression
suite. This harness verifies the actual renderer callback boundary.
"""
from pathlib import Path
import argparse
import subprocess


def main():
    runtime = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=runtime / "build/tests/nv2a_bindings")
    args = parser.parse_args()
    subprocess.run(["cmake", "-S", str(runtime / "tests/nv2a_bindings"), "-B", str(args.build_dir),
                    "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Debug"], check=True)
    subprocess.run(["cmake", "--build", str(args.build_dir)], check=True)
    subprocess.run(["ctest", "--test-dir", str(args.build_dir), "--output-on-failure",
                    "-R", "overlay_upload"], check=True)


if __name__ == "__main__":
    main()
