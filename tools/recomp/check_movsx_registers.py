"""Run with python -m tools.recomp.check_movsx_registers."""
import shutil
import subprocess
import tempfile
from pathlib import Path

from .disasm import Instruction, Operand
from .lifter import Lifter


def main():
    functions = []
    checks = []
    for source, destination in zip(
            ("ax", "bx", "cx", "dx", "si", "di", "bp", "sp"),
            ("eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp")):
        operands = [Operand(type="reg", reg=destination), Operand(type="reg", reg=source)]
        code = Lifter().lift_instruction(Instruction(0, 3, "movsx", f"{destination}, {source}", "", operands))
        functions.append(f"uint32_t extend_{source}(uint32_t {destination}) {{\n"
                         + "\n".join(code) + f"\nreturn {destination};\n}}")
        checks.append(f"assert(extend_{source}(value) == (uint32_t)(int32_t)(int16_t)value);")
    source = """#include <stdint.h>
#include <assert.h>
#define LO16(x) ((uint16_t)(x))
#define SX16(x) ((uint32_t)(int32_t)(int16_t)(x))
""" + "\n".join(functions) + """
int main(void) {
    for (uint32_t low = 0; low <= 0xffff; ++low) {
        uint32_t value = 0xa5a50000u | low;
        """ + "\n".join(checks) + """
    }
}
"""
    cc = shutil.which("clang") or shutil.which("gcc") or shutil.which("cl")
    assert cc, "Run from a compiler developer shell"
    with tempfile.TemporaryDirectory(prefix="movsx-registers-") as tmp:
        path = Path(tmp) / "check.c"
        path.write_text(source)
        exe = Path(tmp) / "check.exe"
        args = ([cc, "/nologo", "/O2", str(path), "/Fe:" + str(exe)]
                if Path(cc).stem.lower() == "cl"
                else [cc, "-O2", str(path), "-o", str(exe)])
        subprocess.run(args, cwd=tmp, check=True)
        subprocess.run([str(exe)], check=True)
    print("MOVSX: all 65536 word values pass for all eight register sources")


if __name__ == "__main__":
    main()
