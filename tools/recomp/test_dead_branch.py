"""Overlapping direct branch paths must execute the original x86 instructions.

Run with MSVC activated and Unicorn installed for the native differential check.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))
from tools.recomp.test_fallthrough import _setup, _func_db, _translate, BASE

# Taken: mov al,1; ret, inside the immediate of the other stream's mov eax.
IMAGE = bytes.fromhex("83 F9 00 74 02 B8 90 B0 01 C3 C3")
JUMP_IMAGE = bytes.fromhex("83 F9 00 74 02 B8 90 EB 01 90 C3")


def translated(image=IMAGE):
    _setup(image)
    return _translate(image, _func_db([(BASE, len(image))]), BASE)


def test_both_overlapping_streams_are_preserved():
    code = translated()
    assert "RECOMP_DEAD_BRANCH" not in code, code
    assert "goto loc_00010007;" in code, code
    assert "loc_00010007:" in code, code
    assert "eax = 0xC301B090" in code, code
    assert "SET_LO8(eax, 1)" in code, code


def test_compiled_paths_match_x86():
    # Unicorn handles an internal Windows exception during initialization.
    # Keep that native setup outside pytest's process-wide faulthandler.
    subprocess.run([sys.executable, __file__, "--native"], check=True)


def _compiled_paths_match_x86():
    if not shutil.which("cl"):
        raise RuntimeError("Activate vcvars64.bat before running the native regression")
    import unicorn
    from unicorn import x86_const as x86
    header = Path(__file__).resolve().parents[2] / "templates/runtime/recomp_types.h"
    cpu_header = Path(__file__).resolve().parents[2] / "include/recomp_cpu.h"
    with tempfile.TemporaryDirectory(prefix="recomp-overlap-") as temporary:
        scratch = Path(temporary)
        shutil.copyfile(header, scratch / "recomp_types.h")
        if cpu_header.exists():  # Older headers keep the CPU helpers inline.
            shutil.copyfile(cpu_header, scratch / "recomp_cpu.h")
        for image in (IMAGE, JUMP_IMAGE):
            _check_image(scratch, image, unicorn, x86)


def _check_image(scratch, image, unicorn, x86):
    source = scratch / "overlap.c"
    source.write_text('''#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
#include <stdio.h>
#include <stdlib.h>
ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS uint32_t g_eax,g_ecx,g_esp;
''' + translated(image) + '''
int main(int argc,char **argv){
 if(argc!=2)return 99;
 g_eax=0;g_ecx=(uint32_t)atoi(argv[1]);g_esp=0x18000;
 sub_00010000();printf("%u %u\\n",g_eax,g_esp);return 0;
}
''')
    executable = scratch / "overlap.exe"
    subprocess.run(["cl", "/nologo", "/W3", "/Od", "/I" + str(scratch),
                    "/Fo" + str(scratch / "overlap.obj"),
                    "/Fe" + str(executable), str(source)], check=True)
    for ecx in (0, 1):
        machine = unicorn.Uc(unicorn.UC_ARCH_X86, unicorn.UC_MODE_32)
        machine.mem_map(BASE, 0x10000)
        machine.mem_write(BASE, image)
        machine.mem_write(0x18000, (0x19000).to_bytes(4, "little"))
        machine.reg_write(x86.UC_X86_REG_EAX, 0)
        machine.reg_write(x86.UC_X86_REG_ECX, ecx)
        machine.reg_write(x86.UC_X86_REG_ESP, 0x18000)
        machine.emu_start(BASE, 0x19000, count=20)
        assert machine.reg_read(x86.UC_X86_REG_EIP) == 0x19000
        expected = [machine.reg_read(reg) for reg in
                    (x86.UC_X86_REG_EAX, x86.UC_X86_REG_ESP)]
        actual = list(map(int, subprocess.check_output(
            [str(executable), str(ecx)], text=True).split()))
        assert actual == expected, (ecx, actual, expected)


if __name__ == "__main__":
    if "--native" in sys.argv:
        _compiled_paths_match_x86()
    else:
        test_both_overlapping_streams_are_preserved()
        test_compiled_paths_match_x86()
    print("PASS: taken and untaken overlapping branch streams match x86 EAX/ESP")
