/* The indirect-call decoder and its bounded guest reads, with synthetic x86
 * call bytes. Windows only: the read checks use VirtualAlloc page protection.
 * The title's own call sites are checked by that title's tests. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <emmintrin.h>
#include "recomp_types.h"

ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp, g_seh_ebp, g_fs_base;
RECOMP_TLS int g_df;
static unsigned char memory[0x1000000];
uint32_t g_xbox_code_lo = 0x12000, g_xbox_code_hi = 0x362000;
ptrdiff_t xbox_GetMemoryOffset(void) { return g_xbox_mem_offset; }
#include "icall_diagnostics.c"

static void check(uint32_t caller, uint32_t target, unsigned kind, uint32_t expected, unsigned reg)
{
    uint32_t slot = 0xAAAAAAAA;
    unsigned base = 99;
    assert(icall_decode(caller, target, &slot, &base) == kind);
    assert(slot == expected && base == reg);
}

static void code(const unsigned char *b, size_t n) { memcpy(memory + 0x50000, b, n); }

int main(void)
{
    uint32_t word = 0, buffer[8] = {0};
    g_xbox_mem_offset = (ptrdiff_t)memory;
    g_esp = 0x700000; g_eax = 0x720000; g_ecx = 0x730000; g_edx = 2;
    g_ebx = 0x740000; g_ebp = 0x710000; g_seh_ebp = 0x710020;
    g_esi = 0x23456789; g_edi = 0x3456789A;
    MEM32(g_ebp) = 0x456789AB; MEM32(g_ebp + 4) = 0x12345678;
    MEM32(g_esp) = 0x1F635D; MEM32(g_ecx) = g_eax;

    /* call [abs]; call eax; call [eax+edx*4+0x20]; jmp [ebx-4];
     * call [esp+4]; call [edx*4+0x740000]; a non-call. */
    { unsigned char b[] = {0xFF, 0x15, 0x34, 0x28, 0x39, 0x00}; code(b, sizeof b);
      check(0x50006, 0, 1, 0x392834, 8); }
    { unsigned char b[] = {0xFF, 0xD0}; code(b, sizeof b);
      check(0x50002, g_eax, 2, 0, 0); }
    { unsigned char b[] = {0xFF, 0x54, 0x90, 0x20}; code(b, sizeof b);
      check(0x50004, 0, 1, g_eax + g_edx * 4 + 0x20, 0); }
    { unsigned char b[] = {0xFF, 0x93, 0xFC, 0xFF, 0xFF, 0xFF}; code(b, sizeof b);
      check(0x50006, 0, 1, g_ebx - 4, 3); }
    { unsigned char b[] = {0xFF, 0x54, 0x24, 0x04}; code(b, sizeof b);
      check(0x50004, 0, 1, g_esp + 8, 4); }
    { unsigned char b[] = {0xFF, 0x14, 0x95, 0x00, 0x00, 0x74, 0x00}; code(b, sizeof b);
      check(0x50007, 0, 1, 0x740000 + g_edx * 4, 8); }
    { unsigned char b[] = {0x90, 0x90}; code(b, sizeof b);
      check(0x50002, 0, 0, 0, 9); }

    /* Reads: null, high, out of the 32-bit range, and over the memory end. */
    assert(!xbox_icall_read(0, &word, 4));
    assert(!xbox_icall_read(0xFFFFFFFE, &word, 4));
    assert(!xbox_icall_read(0x07FFFFFE, &word, 4));
    assert(!xbox_icall_read(0x700000, &word, 129));

    /* Committed, reserved, uncommitted, no-access and guard pages. */
    {
        unsigned char *pages = (unsigned char *)VirtualAlloc(0, 0x30000, MEM_RESERVE, PAGE_NOACCESS);
        DWORD prior;
        ptrdiff_t original_offset = g_xbox_mem_offset;
        assert(pages);
        assert(VirtualAlloc(pages, 0x10000, MEM_COMMIT, PAGE_READWRITE) == pages);
        g_xbox_mem_offset = (ptrdiff_t)pages - 0x10000;
        *(uint32_t *)pages = 0xCAFEBABE;
        assert(xbox_icall_read(0x10000, &word, 4) && word == 0xCAFEBABE);
        assert(!xbox_icall_read(0x20000, &word, 4));          /* reserved, uncommitted */
        assert(!xbox_icall_read(0x1FFFF, buffer, sizeof buffer)); /* crosses into uncommitted */
        assert(VirtualProtect(pages, 0x10000, PAGE_NOACCESS, &prior));
        assert(!xbox_icall_read(0x10000, &word, 4));
        assert(VirtualProtect(pages, 0x10000, PAGE_READWRITE | PAGE_GUARD, &prior));
        assert(!xbox_icall_read(0x10000, &word, 4));
        assert(VirtualProtect(pages, 0x10000, PAGE_READWRITE, &prior));
        assert(VirtualFree(pages, 0, MEM_RELEASE));
        g_xbox_mem_offset = original_offset;
    }
    printf("PASS: indirect-call operands decode to exact slots; null, high, uncommitted, "
           "no-access and guard reads fail safely\n");
    return 0;
}
