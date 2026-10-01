"""Run with python -m tools.recomp.check_ac97_registers in a compiler shell."""
import shutil
import subprocess
import tempfile
from pathlib import Path


def main():
    source = (Path(__file__).parents[2] / "src/kernel/xbox_memory_layout.c").read_text()
    constants = source[source.index("#define AC97_RR"):source.index("static void *g_ac97_page")]
    helper = source[source.index("static void ac97_apply_register_side_effects"):
                    source.index("static LONG CALLBACK ac97_write_veh")]
    harness = """#include <stdint.h>
#include <assert.h>
static uint32_t page[1024];
static void *g_ac97_page = page;
""" + constants + helper + """
int main(void) {
    uint8_t *bytes = (uint8_t *)page;
    for (unsigned value = 0; value < 256; ++value) {
        for (unsigned off = 0x10B; off < 0x180; off += 0x10)
            bytes[off] = (uint8_t)value;
        page[AC97_CODEC_STATUS / 4] = value;
        bytes[0x180] = 0xA5;
        ac97_apply_register_side_effects();
        for (unsigned off = 0x10B; off < 0x180; off += 0x10)
            assert(bytes[off] == (value & ~AC97_RR));
        assert(page[AC97_CODEC_STATUS / 4] == (value | AC97_CODEC_READY));
        assert(bytes[0x180] == 0xA5);
    }
}
"""
    cc = shutil.which("clang") or shutil.which("gcc") or shutil.which("cl")
    assert cc, "Run from a compiler developer shell"
    with tempfile.TemporaryDirectory(prefix="ac97-registers-") as tmp:
        path = Path(tmp) / "check.c"
        path.write_text(harness)
        exe = Path(tmp) / "check.exe"
        args = ([cc, "/nologo", "/O2", str(path), "/Fe:" + str(exe)]
                if Path(cc).stem.lower() == "cl"
                else [cc, "-O2", "-fsanitize=undefined", str(path), "-o", str(exe)])
        subprocess.run(args, cwd=tmp, check=True)
        subprocess.run([str(exe)], check=True)
    print("AC97 reset, codec-ready and unrelated-bit checks passed")


if __name__ == "__main__":
    main()
