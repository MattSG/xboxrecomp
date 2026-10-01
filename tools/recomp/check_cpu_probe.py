"""Compile the original MM3 CPU probe: python -m tools.recomp.check_cpu_probe."""
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    toolkit = Path(__file__).parents[2]
    project = toolkit.parent.parent
    body = subprocess.run(
        [sys.executable, "-m", "tools.recomp", str(project / "game_files/default.xbe"),
         "--function", "0x00093770", "--skip-binary-check"],
        cwd=toolkit, capture_output=True, text=True, check=True).stdout
    assert "RECOMP_UNIMPL" not in body, body
    source = """#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
#include <assert.h>
ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
RECOMP_TLS uint32_t g_ebx, g_esi, g_edi, g_ebp, g_seh_ebp, g_eflags;
RECOMP_TLS int g_df;
static unsigned char memory[0x500000];
""" + body + """
int main(void) {
    uint32_t a,b,c,d;
    recomp_cpuid(0, 99, &a,&b,&c,&d);
    assert(a==2 && b==0x756E6547 && c==0x6C65746E && d==0x49656E69);
    recomp_cpuid(1, 0, &a,&b,&c,&d);
    assert(a==0x68A && b==0 && c==0 && d==0x0383F9FD);
    assert((d & 0x03800000)==0x03800000 && !(d & 0x04000000));
    recomp_cpuid(0x80000000, 99, &a,&b,&c,&d);
    assert(a==0x03020101 && b==0 && c==0 && d==0x0C040841);
    for (uint32_t leaf=0x40000000; leaf<=0x40000001; ++leaf) {
        recomp_cpuid(leaf, 0, &a,&b,&c,&d);
        assert(a==0 && b==0 && c==0 && d==0);
    }
    recomp_cpuid(0x40000002, 0, &a,&b,&c,&d);
    assert(a==0x03020101 && b==0 && c==0 && d==0x0C040841);
    assert(recomp_flags_szp(0xFFFFFFFF,32)==0x84);
    assert(recomp_flags_cmp(0x80,1,8)==0x810);
    assert(recomp_flags_cmp(0,1,16)==0x95);
    assert((recomp_push_flags(0x30002,0,1) & 0x30400)==0x400);
    assert(recomp_materialize_flags(0x30002,0,1)==0x30402);
    assert(recomp_push_flags(0xFFFFFFFF,0x8D5,1)==0x003C7FD7);
    assert(recomp_materialize_flags(0xFFFFFFFF,0x8D5,1)==0x003F7FD7);
    assert(recomp_pop_flags(0x202,0x200286)==0x200286);
    g_xbox_mem_offset=(ptrdiff_t)memory;
    for (unsigned mode=0; mode<3; ++mode) {
        memset(memory,0,sizeof(memory));
        MEM32(0x393004)=mode==1 ? 0 : 0xFFFFFFFF;
        MEM32(0x10000)=0x2FCB3B;
        MEM32(0x10004)=mode==2 ? 0x8004000D : 0x8000000D;
        g_eax=0; g_ecx=0; g_edx=0; g_esp=0x10000;
        g_ebx=0x486440; g_ebp=0x10100; g_seh_ebp=g_ebp;
        g_esi=0x1E0; g_edi=0x280; g_eflags=0x202; g_df=0;
        sub_00093770();
        assert(g_esp==0x10004 && g_ebx==0x486440);
        assert(g_esi==0x1E0 && g_edi==0x280 && g_df==0);
        assert(MEM32(0x393004)==(mode==1 ? 0 : 1));
        assert(MEM32(0x46E580)==(mode==0 ? 1 : 0));
        if (mode!=1) {
            assert(g_edx==0x0383F9FD && g_ecx==0);
            assert(MEM32(0xFFF0)==0x486440);
            assert(MEM32(0xFFEC)==0x286); /* original restored flags image */
            assert(!(g_eflags & 0x200000));
        }
    }
}
"""
    cc = shutil.which("clang") or shutil.which("gcc") or shutil.which("cl")
    assert cc, "Run from a compiler developer shell"
    with tempfile.TemporaryDirectory(prefix="cpu-probe-") as tmp:
        path, exe = Path(tmp) / "check.c", Path(tmp) / "check.exe"
        path.write_text(source)
        include = project / "src/recomp/gen_trace"
        shared = toolkit / "include"
        args = ([cc, "/nologo", "/O2", "/I" + str(include), "/I" + str(shared),
                 str(path), "/Fe:" + str(exe)] if Path(cc).stem.lower()=="cl"
                else [cc, "-O2", "-fsanitize=undefined", "-I"+str(include),
                      "-I"+str(shared), str(path), "-o", str(exe)])
        subprocess.run(args, cwd=tmp, check=True)
        subprocess.run([str(exe)], check=True)
    print("Original CPU probe, CPUID leaves, flags image and stack checks passed")


if __name__ == "__main__":
    main()
