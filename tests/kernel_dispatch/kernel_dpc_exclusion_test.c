/* Synthetic guest DPCs exercise the real queue, timer and IRQL dispatcher. */
#ifndef KERNEL_BRIDGE_SOURCE
#define KERNEL_BRIDGE_SOURCE "../../src/kernel/kernel_bridge.c"
#endif
#include KERNEL_BRIDGE_SOURCE
/* A before-source comparison can supply the unmodified timer-loop body as
 * this callable adapter; normal tests call the production helper directly. */
#ifdef KERNEL_TIMER_CLAIM_ADAPTER
#include KERNEL_TIMER_CLAIM_ADAPTER
#endif

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "DPC exclusion check failed at line %d: %s\n", __LINE__, #c); \
    ExitProcess(1); \
} } while (0)

enum { DPC = 0x20000, TIMER = 0x22000, DPC_CONTEXT = 0x24000, ROUTINE = 0x26000 };
static uint8_t *memory;
static HANDLE worker_started;
static volatile LONG callbacks;
static int timer_mode;

static void guest_dpc(void)
{
    CHECK(BRIDGE_MEM8(g_fs_base + 0x24) == 2);
    CHECK(BRIDGE_MEM32(g_esp) == 0);
    CHECK(BRIDGE_MEM32(g_esp + 4) == DPC);
    CHECK(BRIDGE_MEM32(g_esp + 8) == DPC_CONTEXT);
    CHECK(BRIDGE_MEM32(g_esp + 12) == (timer_mode ? 0 : 0x1234));
    CHECK(BRIDGE_MEM32(g_esp + 16) == (timer_mode ? 0 : 0x5678));
    InterlockedIncrement(&callbacks);
    g_esp += 20; /* Original guest DeferredRoutine stdcall ret 16. */
}

recomp_func_t recomp_lookup(uint32_t address)
{
    return address == ROUTINE ? guest_dpc : NULL;
}
recomp_func_t recomp_lookup_manual(uint32_t address)
{
    (void)address;
    return NULL;
}

static void prepare(void)
{
    memset(memory + DPC, 0, 32);
    BRIDGE_MEM16(DPC) = 0x13;
    BRIDGE_MEM32(DPC + 12) = ROUTINE;
    BRIDGE_MEM32(DPC + 16) = DPC_CONTEXT;
    if (timer_mode) {
        g_timers[0].timer_va = TIMER;
        g_timers[0].dpc_va = DPC;
        g_timers[0].due_ms = 0;
        g_timers[0].period_ms = 0;
    } else {
        g_esp = 0x30000;
        BRIDGE_MEM32(g_esp) = DPC;
        BRIDGE_MEM32(g_esp + 4) = 0x1234;
        BRIDGE_MEM32(g_esp + 8) = 0x5678;
        bridge_KeInsertQueueDpc();
        CHECK(g_eax == 1 && BRIDGE_MEM8(DPC + 2) == 1);
    }
}

static DWORD WINAPI drain(LPVOID unused)
{
    (void)unused;
    g_esp = 0x80000;
    g_fs_base = 0x18000;
    CHECK(SetEvent(worker_started));
    if (timer_mode) kernel_fire_due_timer(0, 1000);
    else kernel_drain_dpcs();
    CHECK(g_esp == 0x80000);
    CHECK(BRIDGE_MEM8(g_fs_base + 0x24) == 0);
    return 0;
}

static HANDLE start_worker(void)
{
    CHECK(ResetEvent(worker_started));
    HANDLE worker = CreateThread(NULL, 0, drain, NULL, 0, NULL);
    CHECK(worker);
    CHECK(WaitForSingleObject(worker_started, 3000) == WAIT_OBJECT_0);
    return worker;
}

int main(int argc, char **argv)
{
    timer_mode = argc > 1 && !strcmp(argv[1], "timer");
    memory = VirtualAlloc(NULL, 16 * 1024 * 1024,
                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(memory);
    g_xbox_mem_offset = (ptrdiff_t)memory;
    g_fs_base = 0x10000;
    worker_started = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK(worker_started);
    InitializeCriticalSection(&g_timer_lock);
    g_timer_started = 1; /* Use an explicitly driven worker, not a poll thread. */

    int saved_irql = xbox_IrqlEnterInterrupt(2);
    prepare();
    HANDLE worker = start_worker();
    /* The processor belongs to this guest thread. A host worker must neither
     * claim nor run deferred work until this thread restores its old IRQL. */
    CHECK(WaitForSingleObject(worker, 100) == WAIT_TIMEOUT);
    CHECK(callbacks == 0);
    if (timer_mode) {
        EnterCriticalSection(&g_timer_lock);
        int still_armed = g_timers[0].timer_va == TIMER;
        LeaveCriticalSection(&g_timer_lock);
        CHECK(still_armed);
        CHECK(xbox_kernel_cancel_timer(TIMER) == 1);
    } else {
        CHECK(BRIDGE_MEM8(DPC + 2) == 1);
        g_esp = 0x30000;
        BRIDGE_MEM32(g_esp) = DPC;
        bridge_KeRemoveQueueDpc();
        CHECK(g_eax == 1 && BRIDGE_MEM8(DPC + 2) == 0);
    }
    xbox_IrqlLeaveInterrupt(saved_irql);
    CHECK(WaitForSingleObject(worker, 3000) == WAIT_OBJECT_0);
    CHECK(CloseHandle(worker));
    CHECK(callbacks == 0 && BRIDGE_MEM8(g_fs_base + 0x24) == 0);

    /* Uncancelled work still executes once, at the correct IRQL, with the
     * original four guest arguments and stack cleanup. */
    prepare();
    worker = start_worker();
    CHECK(WaitForSingleObject(worker, 3000) == WAIT_OBJECT_0);
    CHECK(CloseHandle(worker));
    CHECK(callbacks == 1);
    CHECK(CloseHandle(worker_started));
    CHECK(VirtualFree(memory, 0, MEM_RELEASE));
    printf("PASS: %s claim obeys guest DISPATCH exclusion and cancellation; "
           "uncancelled callback ABI preserved\n", timer_mode ? "timer" : "DPC");
    return 0;
}
