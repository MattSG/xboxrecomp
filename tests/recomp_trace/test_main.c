#if !defined(_WIN32)
#include "platform/win32_compat.h"
#include <unistd.h>
#endif
#include "../../src/kernel/recomp_trace.c"
#include <string.h>
RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi;
ptrdiff_t g_xbox_mem_offset;
ptrdiff_t xbox_GetMemoryOffset(void) { return g_xbox_mem_offset; }
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s\n", #x); return 1; } } while (0)
int main(void)
{
    size_t page;
    uint8_t *memory;
    uint32_t header[2];
#if defined(_WIN32)
    SYSTEM_INFO si;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    GetSystemInfo(&si);
    page = si.dwPageSize;
#else
    page = (size_t)sysconf(_SC_PAGESIZE);
#endif
    memory = VirtualAlloc(NULL, page * 2, MEM_RESERVE, PAGE_NOACCESS);
    CHECK(memory);
    CHECK(VirtualAlloc(memory, page, MEM_COMMIT, PAGE_READWRITE));
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory - 0x10000;
    g_esp = 0x10000 + page - 2; /* Return address crosses a guard. */
    g_eax = g_esp;
    recomp_trace_enter("guard", 0x20000);
    recomp_debug_service(1, g_esp); /* Header crosses a guard. */
    header[0] = 16; header[1] = g_esp;
    memcpy(memory, header, sizeof header);
    recomp_debug_service(1, 0x10000); /* String crosses a guard. */
    memcpy(memory + 32, "valid\n", 6);
    header[0] = 6; header[1] = 0x10020;
    memcpy(memory, header, sizeof header);
    recomp_debug_service(1, 0x10000);
    g_esp = 0x10000;
    header[0] = 0x12345678; header[1] = 0x10000 + page - 2;
    memcpy(memory, header, sizeof header);
    recomp_trace_enter("pointer-guard", 0x20000);
    SetLastError(123);
    CHECK(!trace_read(header[1], header, sizeof header));
    CHECK(GetLastError() == 123);
    CHECK(!trace_read(0xFFFFFFFFull, header, sizeof header));
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory - 0x50000000;
    CHECK(trace_read(0x50000000, header, sizeof header));
    CHECK(header[0] == 0x12345678);
    trace_dump("high-address", 0x50000000, 12);
    g_esp = 0xFFFFFFFFu;
    recomp_trace_enter("overflow", 0x20000);
    CHECK(VirtualFree(memory, 0, MEM_RELEASE));
    return 0;
}
