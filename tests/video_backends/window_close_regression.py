"""Compile and run the real framebuffer close-message handling on Windows."""
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import cc

ROOT = Path(__file__).resolve().parents[2]


def main():
    cc.skip_unless_windows("links user32")
    cc.require_compiler()
    with tempfile.TemporaryDirectory(prefix="xboxrecomp-window-close-") as temporary:
        folder = Path(temporary)
        executable = folder / f"contract{cc.exe_suffix()}"
        subprocess.run(cc.compile_command(
            [ROOT / "tests/video_backends/window_close_contract.c"], executable,
            includes=[ROOT / "src"], defines=["_CRT_SECURE_NO_WARNINGS"],
            std="c11", optimize="speed", warnings="normal", werror=True, libs=["user32"],
            obj=folder / "contract.obj"), cwd=folder, check=True)
        subprocess.run([str(executable)], cwd=folder, check=True)


if __name__ == "__main__":
    main()
