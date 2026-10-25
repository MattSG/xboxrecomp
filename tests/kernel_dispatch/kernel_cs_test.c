/* Permanent shadow caches must preserve native exclusion and recursion. */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*guest_fn)(void);
extern guest_fn recomp_lookup_kernel(uint32_t);
extern RECOMP_TLS uint32_t g_eax, g_esp;
extern ptrdiff_t g_xbox_mem_offset;
void *recomp_lookup(ULONG address) { (void)address; abort(); }
void *recomp_lookup_manual(ULONG address) { (void)address; abort(); }

#define THREADS 4
#define COLLISIONS 40
#define SHARED_ITERATIONS 5000
#define COLLISION_ITERATIONS 100
#define BENCH_ITERATIONS 100000
static uint8_t *memory;
static HANDLE start_event;
static volatile LONG failed, active;
static uint32_t shared_count, collision_count[COLLISIONS];
static uint32_t imports[2];

static PRTL_CRITICAL_SECTION guest_lock(unsigned i)
{
    /* Every address shares the table bucket and thread-cache bucket. */
    return (PRTL_CRITICAL_SECTION)(memory + 0x10000 + i * 0x10000);
}

typedef struct { unsigned id, phase; } WORKER;
static DWORD WINAPI worker(LPVOID p)
{
    WORKER *w = p;
    WaitForSingleObject(start_event, INFINITE);
    if (w->phase == 0) {
        for (unsigned i = 0; i < SHARED_ITERATIONS; i++) {
            PRTL_CRITICAL_SECTION lock = guest_lock(0);
            /* The first call races lazy initialization across all workers. */
            xbox_RtlEnterCriticalSection(lock);
            xbox_RtlEnterCriticalSection(lock);
            if (active) InterlockedExchange(&failed, 1);
            active = 1;
            shared_count++;
            if ((i & 255) == 0) SwitchToThread();
            active = 0;
            xbox_RtlLeaveCriticalSection(lock);
            xbox_RtlLeaveCriticalSection(lock);
        }
    } else if (w->phase == 1) {
        for (unsigned i = 0; i < COLLISION_ITERATIONS; i++)
            for (unsigned j = 0; j < COLLISIONS; j++) {
                PRTL_CRITICAL_SECTION lock = guest_lock(j);
                xbox_RtlEnterCriticalSection(lock);
                collision_count[j]++;
                xbox_RtlLeaveCriticalSection(lock);
            }
    } else {
        /* Independent repeated locks expose table contention without a
         * guest-lock bottleneck. Timing is informational, never a pass gate. */
        PRTL_CRITICAL_SECTION lock = guest_lock(w->id);
        for (unsigned i = 0; i < BENCH_ITERATIONS; i++) {
            xbox_RtlEnterCriticalSection(lock);
            xbox_RtlLeaveCriticalSection(lock);
        }
    }
    return 0;
}

static int run_workers(unsigned phase)
{
    HANDLE threads[THREADS];
    WORKER workers[THREADS];
    ResetEvent(start_event);
    for (unsigned i = 0; i < THREADS; i++) {
        workers[i].id = i;
        workers[i].phase = phase;
        threads[i] = CreateThread(NULL, 0, worker, &workers[i], 0, NULL);
        if (!threads[i]) abort();
    }
    SetEvent(start_event);
    DWORD wait = WaitForMultipleObjects(THREADS, threads, TRUE, 30000);
    if (wait != WAIT_OBJECT_0) {
        fprintf(stderr, "FAIL: lock phase %u timed out\n", phase);
        /* Worker parameters are on this stack; do not return with live users. */
        ExitProcess(2);
    }
    for (unsigned i = 0; i < THREADS; i++) CloseHandle(threads[i]);
    return !failed;
}

static uint32_t invoke(unsigned slot)
{
    guest_fn fn = recomp_lookup_kernel(imports[slot]);
    uint32_t *sp;
    if (!fn) abort();
    g_esp = 0x700000;
    sp = (uint32_t *)(memory + g_esp);
    sp[0] = 0xBEEF0001;
    sp[1] = 0x10000;
    fn();
    if (g_esp != 0x700008) InterlockedExchange(&failed, 1);
    return g_eax;
}

static DWORD WINAPI probe_try(LPVOID p)
{
    int expected = *(int *)p;
    uint32_t got = invoke(0);
    if (got != (uint32_t)expected) InterlockedExchange(&failed, 1);
    if (got) invoke(1);
    return 0;
}

static int try_from_other_thread(int expected)
{
    HANDLE thread = CreateThread(NULL, 0, probe_try, &expected, 0, NULL);
    if (!thread) return 0;
    if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0) ExitProcess(3);
    CloseHandle(thread);
    return !failed;
}

int main(int argc, char **argv)
{
    /* This private arena keeps its export/heap floor above raw scratch. */
    if (!xbox_ConfigureKernelData(0x00740000u)) return 90;

    LARGE_INTEGER freq, begin, end;
    PRTL_CRITICAL_SECTION lock;
    _putenv_s("RECOMP_CS_MODE", "");
    _putenv_s("RECOMP_CS_TRACE_CRT", "");
    _putenv_s("RECOMP_CS_WATCH", "");
    _putenv_s("RECOMP_CS_COUNTERS", argc > 1 && !strcmp(argv[1], "counters") ? "1" : "0");
    _putenv_s("RECOMP_KERNEL_LOG_BUDGET", "0");
    memory = VirtualAlloc(NULL, 16 * 1024 * 1024,
                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    start_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!memory || !start_event) return 10;
    g_xbox_mem_offset = (ptrdiff_t)memory;
    for (unsigned i = 0; i < COLLISIONS; i++)
        memset(guest_lock(i), 0xA5, 32);
    imports[0] = 0x80000000u | 306; /* TryEnter */
    imports[1] = 0x80000000u | 294; /* Leave */
    memcpy(memory + 0x600000, imports, sizeof imports);
    xbox_kernel_set_thunk_address(0x600000, 2);
    xbox_kernel_bridge_init();
    memcpy(imports, memory + 0x600000, sizeof imports);

    if (argc > 1 && !strcmp(argv[1], "diagnostics")) {
        DWORD old;
        extern uint32_t g_xbox_code_lo, g_xbox_code_hi;
        _putenv_s("RECOMP_CS_TRACE_CRT", "all");
        _putenv_s("RECOMP_CS_WATCH", "0x10000");
        g_xbox_code_lo = 0x10000; g_xbox_code_hi = 0x20000;
        if (!VirtualProtect(memory + 0x800000, 4096, PAGE_NOACCESS, &old)) return 12;
        xbox_SetCrtLockTable(0x800000, 2);
        g_esp = 0x800000;
        SetLastError(1234);
        xbox_RtlEnterCriticalSection(guest_lock(0));
        xbox_RtlLeaveCriticalSection(guest_lock(0));
        if (GetLastError() != 1234) return 13;
        xbox_SetCrtLockTable(0xfffffffc, 2);
        g_esp = 0xfffffffe;
        xbox_RtlEnterCriticalSection(guest_lock(0));
        xbox_RtlLeaveCriticalSection(guest_lock(0));
        puts("PASS: unreadable and overflowing guest lock diagnostics");
        return 0;
    }

    if (!run_workers(0) || shared_count != THREADS * SHARED_ITERATIONS ||
        !run_workers(1)) return 1;
    for (unsigned i = 0; i < COLLISIONS; i++)
        if (collision_count[i] != THREADS * COLLISION_ITERATIONS) return 2;

    lock = guest_lock(0);
    xbox_RtlEnterCriticalSection(lock);
    xbox_RtlEnterCriticalSection(lock);
    if (!try_from_other_thread(0)) return 3;
    xbox_RtlLeaveCriticalSection(lock);
    if (!try_from_other_thread(0)) return 4;
    xbox_RtlLeaveCriticalSection(lock);
    /* Try+bridge Leave must free the same native shadow, on every thread. */
    if (!try_from_other_thread(1) || !try_from_other_thread(1)) return 5;
    if (invoke(0) != 1 || invoke(0) != 1) return 6;
    xbox_RtlLeaveCriticalSection(lock);
    if (!try_from_other_thread(0)) return 7;
    xbox_RtlLeaveCriticalSection(lock);
    if (!try_from_other_thread(1)) return 8;

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&begin);
    if (!run_workers(2)) return 9;
    QueryPerformanceCounter(&end);
    for (unsigned i = 0; i < COLLISIONS; i++)
        for (unsigned j = 0; j < 32; j++)
            if (((uint8_t *)guest_lock(i))[j] != 0xA5) return 11;
    printf("PASS: lazy initialization, recursive exclusion, 40 colliding locks, "
           "bridge Try/Leave and unchanged guest fields; %u independent lock pairs in %.2f ms\n",
           THREADS * BENCH_ITERATIONS,
           (end.QuadPart - begin.QuadPart) * 1000.0 / freq.QuadPart);
    CloseHandle(start_event);
    VirtualFree(memory, 0, MEM_RELEASE);
    return 0;
}
