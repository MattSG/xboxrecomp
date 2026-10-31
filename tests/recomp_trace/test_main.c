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
static volatile LONG start_workers, budget_admitted;
static int testing_budget;
static DWORD WINAPI count_worker(void *unused)
{
    (void)unused;
    while (!InterlockedCompareExchange(&start_workers, 0, 0)) Sleep(0);
    for (int i = 0; i < 50000; i++) {
        if (testing_budget) {
            if (trace_should_log(NULL, 0, 0)) InterlockedIncrement(&budget_admitted);
        } else recomp_trace_enter("worker", 0x20000);
    }
    return 0;
}

static int check_profile(int budget)
{
    HANDLE workers[4];
    char name[] = "snapshot";
    unsigned slot = (0x30000u * 2654435761u) & (PROF_SLOTS - 1);
    testing_budget = budget;
    if (!budget) recomp_trace_enter(name, 0x30000);
    name[0] = 'X';
    for (int i = 0; i < 4; i++) {
        workers[i] = CreateThread(NULL, 0, count_worker, NULL, 0, NULL);
        CHECK(workers[i]);
    }
    InterlockedExchange(&start_workers, 1);
    for (int i = 0; i < 4; i++) {
        CHECK(WaitForSingleObject(workers[i], INFINITE) == WAIT_OBJECT_0);
        CloseHandle(workers[i]);
    }
    if (budget) {
        CHECK(budget_admitted == 100000);
        fprintf(stderr, "budget concurrent counts passed\n");
        return 0;
    }
    CHECK(g_prof_calls == 200001);
    slot = (0x20000u * 2654435761u) & (PROF_SLOTS - 1);
    while (g_prof[slot].va != 0x20000) slot = (slot + 1) & (PROF_SLOTS - 1);
    CHECK(g_prof[slot].hits == 200000);
    slot = (0x30000u * 2654435761u) & (PROF_SLOTS - 1);
    CHECK(strcmp(g_prof_name[slot], "snapshot") == 0);
    fprintf(stderr, "profile concurrent counts passed\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "profile") == 0) return check_profile(0);
    if (argc > 1 && strcmp(argv[1], "budget") == 0) return check_profile(1);
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
