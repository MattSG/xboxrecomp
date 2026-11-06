"""EBP used as data must survive direct, indirect, and sibling calls."""
import subprocess

import pytest

from tools.recomp import config, c_fixture
from tools.recomp.translator import FunctionTranslator


def test_general_ebp_across_calls(tmp_path, monkeypatch):
    cc = c_fixture.compiler()
    if not cc:
        pytest.skip('C compiler required for generated-code execution')
    # Synthetic caller: save EBP, set a loop pointer, call a frame owner,
    # then call an EBP reader directly and indirectly, restore EBP, return.
    base = 0x10000
    caller = bytes.fromhex('55bd78563412e825000000e830000000b840000100ffd05dc3')
    frame = bytes.fromhex('558bec5dc3')
    reader = bytes.fromhex('558b04245dc3')
    image = caller.ljust(0x30, b'\x90') + frame
    image = image.ljust(0x40, b'\x90') + reader
    for key in ('_SECTIONS', 'SECTIONS', '_configured_from', 'TEXT_VA_START',
                'TEXT_VA_END', 'RDATA_VA_START', 'RDATA_VA_END', 'DATA_VA_START',
                'DATA_VA_END', 'KERNEL_THUNK_ADDR', 'ENTRY_POINT'):
        monkeypatch.setattr(config, key, getattr(config, key))
    config._install([config.Section('.text', base, len(image), 0, len(image), True)],
                    entry_point=base, kernel_thunk_addr=base, origin='ebp-call-test')
    db = {base + off: {'start': hex(base + off), 'end': base + off + len(body),
                       '_addr': base + off, 'size': len(body)}
          for off, body in ((0, caller), (0x30, frame), (0x40, reader))}
    translator = FunctionTranslator(image, db)
    bodies = '\n'.join(translator.translate_function(a, f) for a, f in db.items())
    source = r'''
#include <stdint.h>
#include <assert.h>
static uint32_t eax, ecx, edx, ebx, esi, edi, esp, g_ebp, g_seh_ebp, g_eflags;
static uint32_t ram[1024];
#define g_esp esp
#define MEM32(a) ram[(a)/4]
#define PUSH32(s,v) ((s)-=4, MEM32(s)=(v))
#define POP32(s,v) ((v)=MEM32(s), (s)+=4)
#define RECOMP_ABI_CALL(a,f) f()
#define RECOMP_ICALL_SAFE_AT(a,s,site) sub_00010040()
void sub_00010000(void); void sub_00010030(void); void sub_00010040(void);
''' + bodies + r'''
int main(void) {
    esp=4000; g_ebp=0x12345678; g_seh_ebp=0xbad;
    sub_00010040();
    assert(eax==0x12345678); assert(esp==4004);
    esp=4000; g_ebp=g_seh_ebp=0xabcdef;
    sub_00010000();
    assert(eax==0x12345678); assert(esp==4004);
    assert(g_ebp==0xabcdef); assert(g_seh_ebp==0xabcdef);
    return 0;
}
'''
    src = tmp_path / 'check.c'; exe = tmp_path / 'check.exe'
    src.write_text(source)
    subprocess.run(c_fixture.command(cc, [src], exe), cwd=tmp_path, check=True,
                   capture_output=True, text=True)
    subprocess.run([str(exe)], check=True)


@pytest.mark.parametrize("kind", ["jump", "conditional", "fallthrough"])
def test_tail_entry_reads_architectural_ebp(tmp_path, monkeypatch, kind):
    cc = c_fixture.compiler()
    if not cc:
        pytest.skip('C compiler required')
    base = 0x10000
    caller = {"jump": bytes.fromhex('558bece928000000'),
              "conditional": bytes.fromhex('558bec85c97429c9c3'),
              "fallthrough": bytes.fromhex('558bec90')}[kind]
    tail = bytes.fromhex('8bc5c9c3')
    offset = len(caller) if kind == "fallthrough" else 0x30
    image = caller.ljust(offset, b'\x90') + tail
    for key in ('_SECTIONS', 'SECTIONS', '_configured_from', 'TEXT_VA_START',
                'TEXT_VA_END', 'RDATA_VA_START', 'RDATA_VA_END', 'DATA_VA_START',
                'DATA_VA_END', 'KERNEL_THUNK_ADDR', 'ENTRY_POINT'):
        monkeypatch.setattr(config, key, getattr(config, key))
    config._install([config.Section('.text', base, len(image), 0, len(image), True)],
                    entry_point=base, kernel_thunk_addr=base, origin='ebp-tail-test')
    db = {base + off: {'start': hex(base + off), 'end': base + off + len(body),
                       '_addr': base + off, 'size': len(body)}
          for off, body in ((0, caller), (offset, tail))}
    translator = FunctionTranslator(image, db)
    caller_body = translator.translate_function(base, db[base])
    # SEH helpers can update the caller's local frame while g_ebp still
    # carries the older input. Every external tail must republish the local.
    lines = caller_body.splitlines()
    tail_line = next(i for i, line in enumerate(lines)
                     if f'sub_{base+offset:08X}(); return;' in line)
    lines.insert(tail_line, '    g_ebp = 0xbad;')
    bodies = '\n'.join(lines) + '\n' + translator.translate_function(base+offset, db[base+offset])
    source = r"""
#include <stdint.h>
#include <assert.h>
static uint32_t eax,ecx,edx,ebx,esi,edi,esp,g_ebp,g_seh_ebp,g_eflags;
static uint32_t ram[1024];
#define g_esp esp
#define MEM32(a) ram[(a)/4]
#define PUSH32(s,v) ((s)-=4, MEM32(s)=(v))
#define POP32(s,v) ((v)=MEM32(s), (s)+=4)
#define TEST_Z(a,b) (((a)&(b))==0)
#define CMP_EQ(a,b) ((a)==(b))
""" + f'void sub_{base+offset:08X}(void);\n' + bodies + r"""
int main(void) {
    esp=4000; g_ebp=0xabcdef; g_seh_ebp=0xbad; ecx=0;
    sub_00010000();
    assert(eax==3996); assert(esp==4004); assert(g_ebp==0xabcdef);
    return 0;
}
"""
    src, exe = tmp_path/'tail.c', tmp_path/'tail.exe'
    src.write_text(source)
    subprocess.run(c_fixture.command(cc, [src], exe), cwd=tmp_path,
                   check=True, capture_output=True, text=True)
    subprocess.run([str(exe)], check=True)
