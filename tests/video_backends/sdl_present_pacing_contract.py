"""Compile SDL present pacing control flow with deterministic GPU mocks."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import cc

ROOT = Path(__file__).resolve().parents[2]


def production_bodies(source):
    pacing = re.search(r"typedef struct FlipPacing \{.*?\}\s*FlipPacing;", source, re.S)
    if pacing is None:
        raise ValueError("Missing production FlipPacing type")
    bodies = [pacing.group()]
    for name in ("pace_after_present", "pace_flip", "present_kept_image", "nv2a_d3d_tick"):
        match = re.search(r"^(?:static )?(?:int|void) " + name + r"\([^\n]*\)\n\{.*?^\}",
                          source, re.M | re.S)
        if match is None:
            raise ValueError(f"Missing production {name} body")
        bodies.append(match.group())
    return "\n\n".join(bodies)


def main():
    cc.require_compiler()
    source = (ROOT / "src/video/nv2a_sdlgpu.c").read_text()
    with tempfile.TemporaryDirectory(prefix="xboxrecomp-sdl-present-pacing-") as folder:
        folder = Path(folder)
        (folder / "production_pacing.inc").write_text(production_bodies(source))
        exe = folder / f"contract{cc.exe_suffix()}"
        subprocess.run(cc.compile_command(
            [ROOT / "tests/video_backends/sdl_present_pacing_contract.c"], exe,
            includes=[folder], std="c11", optimize="none", warnings="high", werror=True,
            obj=folder / "contract.obj"), check=True, cwd=folder)
        for policy in ("before", "after"):
            subprocess.run([str(exe), policy], check=True)


if __name__ == "__main__":
    main()
