"""Run with python -m tools.recomp.check_neg_flags (no pytest required)."""
import shutil
import subprocess
import tempfile
from pathlib import Path

from .disasm import Operand, Instruction
from .lifter import Lifter, _make_condition


def main():
    ops = [Operand(type="reg", reg="edi")]
    lifter = Lifter()
    lifter.needs_cf = True
    statements = lifter.lift_instruction(Instruction(0, 2, "neg", "edi", "", ops))
    condition = _make_condition("je", "neg", ops)[0]
    source = """#include <stdint.h>
#include <assert.h>
int main(void) {
    const uint32_t inputs[] = {0, 1, 0xFFFFFFFFu, 0x80000000u};
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t edi = inputs[i], _fa = 0x5C;
        int32_t _fas = 0;
        int _cf = 0;
    """ + "\n".join(statements) + """
        assert(edi == 0u - inputs[i]);
        assert(_cf == (inputs[i] != 0));
        edi = 0x99; /* flags must survive a later register write */
        assert((""" + condition + """ ) == (inputs[i] == 0));
        assert((_fas < 0) == ((int32_t)(0u - inputs[i]) < 0));
    }
}
"""
    cc = shutil.which("clang") or shutil.which("gcc") or shutil.which("cl")
    assert cc, "Run from a compiler developer shell"
    with tempfile.TemporaryDirectory(prefix="neg-flags-") as tmp:
        path = Path(tmp) / "check.c"
        path.write_text(source)
        exe = Path(tmp) / "check.exe"
        args = ([cc, "/nologo", "/O2", str(path), "/Fe:" + str(exe)]
                if Path(cc).stem.lower() == "cl"
                else [cc, "-O2", "-fsanitize=undefined", str(path), "-o", str(exe)])
        subprocess.run(args, cwd=tmp, check=True)
        subprocess.run([str(exe)], check=True)
    print("NEG result, carry, sign and zero-branch checks passed")


if __name__ == "__main__":
    main()
