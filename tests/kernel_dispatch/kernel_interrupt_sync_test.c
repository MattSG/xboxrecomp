/* Synthetic guest callbacks exercise the production interrupt bridge. */
#ifndef KERNEL_BRIDGE_SOURCE
#define KERNEL_BRIDGE_SOURCE "../../src/kernel/kernel_bridge.c"
#endif
#include KERNEL_BRIDGE_SOURCE

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "Interrupt synchronization check failed at line %d: %s\n", \
            __LINE__, #c); \
    ExitProcess(1); \
} } while (0)

enum { INTERRUPT = 0x20000, SYNC_ROUTINE = 0x22000, ISR_ROUTINE = 0x24000,
       SYNC_CONTEXT = 0x26000, VECTOR = 3 };
static uint8_t *memory;
static HANDLE worker_started, callback_entered, callback_release;
static volatile LONG sync_callbacks, isr_callbacks;
static int exclusion_mode, hold_callback;

static void guest_sync(void)
{
    CHECK(BRIDGE_MEM32(g_esp) == 0);
    CHECK(BRIDGE_MEM32(g_esp + 4) == SYNC_CONTEXT);
    if (!exclusion_mode) CHECK(BRIDGE_MEM8(g_fs_base + 0x24) == 16);
    InterlockedIncrement(&sync_callbacks);
    if (hold_callback) {
        CHECK(SetEvent(callback_entered));
        CHECK(WaitForSingleObject(callback_release, 3000) == WAIT_OBJECT_0);
    }
    g_eax = 0xA5;
    g_esp += 8; /* Guest BOOLEAN callback stdcall ret 4. */
}

static void guest_isr(void)
{
    CHECK(BRIDGE_MEM8(g_fs_base + 0x24) == 16);
    CHECK(BRIDGE_MEM32(g_esp) == 0);
    CHECK(BRIDGE_MEM32(g_esp + 4) == INTERRUPT);
    CHECK(BRIDGE_MEM32(g_esp + 8) == SYNC_CONTEXT);
    InterlockedIncrement(&isr_callbacks);
    g_eax = 1;
    g_esp += 12; /* Guest ServiceRoutine stdcall ret 8. */
}

recomp_func_t recomp_lookup(uint32_t address)
{
    if (address == SYNC_ROUTINE) return guest_sync;
    if (address == ISR_ROUTINE) return guest_isr;
    return NULL;
}
recomp_func_t recomp_lookup_manual(uint32_t address)
{
    (void)address;
    return NULL;
}

static void call_sync(void)
{
    BRIDGE_MEM32(g_esp) = INTERRUPT;
    BRIDGE_MEM32(g_esp + 4) = SYNC_ROUTINE;
    BRIDGE_MEM32(g_esp + 8) = SYNC_CONTEXT;
    uint32_t stack_before = g_esp;
    bridge_KeSynchronizeExecution();
    CHECK(g_esp == stack_before && g_eax == 0xA5);
}

static DWORD WINAPI worker(LPVOID run_isr)
{
    g_esp = run_isr ? 0x90000 : 0x80000;
    g_fs_base = run_isr ? 0x18000 : 0x14000;
    uint32_t stack_before = g_esp;
    CHECK(SetEvent(worker_started));
    if (run_isr) CHECK(kernel_raise_interrupt(VECTOR) == 1);
    else call_sync();
    CHECK(g_esp == stack_before);
    CHECK(BRIDGE_MEM8(g_fs_base + 0x24) == 0);
    return 0;
}

static HANDLE start_worker(int run_isr)
{
    CHECK(ResetEvent(worker_started));
    HANDLE thread = CreateThread(NULL, 0, worker,
                                run_isr ? (LPVOID)1 : NULL, 0, NULL);
    CHECK(thread);
    CHECK(WaitForSingleObject(worker_started, 3000) == WAIT_OBJECT_0);
    return thread;
}

static void join_worker(HANDLE thread)
{
    CHECK(WaitForSingleObject(thread, 3000) == WAIT_OBJECT_0);
    CHECK(CloseHandle(thread));
}

int main(int argc, char **argv)
{
    exclusion_mode = argc > 1 && !strcmp(argv[1], "exclusion");
    memory = VirtualAlloc(NULL, 16 * 1024 * 1024,
                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(memory);
    g_xbox_mem_offset = (ptrdiff_t)memory;
    g_fs_base = 0x10000;
    g_esp = 0x30000;
    worker_started = CreateEventW(NULL, TRUE, FALSE, NULL);
    callback_entered = CreateEventW(NULL, TRUE, FALSE, NULL);
    callback_release = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK(worker_started && callback_entered && callback_release);
    BRIDGE_MEM32(INTERRUPT) = ISR_ROUTINE;
    BRIDGE_MEM32(INTERRUPT + 4) = SYNC_CONTEXT;
    BRIDGE_MEM32(INTERRUPT + 8) = VECTOR;
    g_connected_isr[VECTOR] = INTERRUPT;

    if (argc > 1 && !strcmp(argv[1], "preemption")) {
        /* The separate ISR gate must never recreate the audio timeout: a
         * device interrupt completes while another thread owns DPC level. */
        int saved_irql = xbox_IrqlEnterInterrupt(2);
        HANDLE isr_thread = start_worker(1);
        join_worker(isr_thread);
        CHECK(isr_callbacks == 1 && sync_callbacks == 0);
        xbox_IrqlLeaveInterrupt(saved_irql);
    } else if (exclusion_mode) {
        /* The ISR owns the processor. A passive synchronization request must
         * wait before entering its callback or touching the shared SYNC_CONTEXT. */
        int saved_irql = xbox_IrqlEnterInterrupt(16);
        HANDLE sync_thread = start_worker(0);
        CHECK(WaitForSingleObject(sync_thread, 100) == WAIT_TIMEOUT);
        CHECK(sync_callbacks == 0);
        xbox_IrqlLeaveInterrupt(saved_irql);
        join_worker(sync_thread);
        CHECK(sync_callbacks == 1);

        /* Conversely, a synchronization callback excludes the real ISR
         * dispatcher until it returns. No interrupt is discarded. */
        hold_callback = 1;
        sync_thread = start_worker(0);
        CHECK(WaitForSingleObject(callback_entered, 3000) == WAIT_OBJECT_0);
        HANDLE isr_thread = start_worker(1);
        CHECK(WaitForSingleObject(isr_thread, 100) == WAIT_TIMEOUT);
        CHECK(isr_callbacks == 0);
        CHECK(SetEvent(callback_release));
        join_worker(sync_thread);
        join_worker(isr_thread);
        CHECK(sync_callbacks == 2 && isr_callbacks == 1);
    } else {
        /* A passive caller and a nested DPC caller both observe ISR IRQL,
         * keep the callback's return value and restore their prior IRQL. */
        call_sync();
        CHECK(BRIDGE_MEM8(g_fs_base + 0x24) == 0);
        int saved_irql = xbox_IrqlEnterInterrupt(2);
        call_sync();
        CHECK(BRIDGE_MEM8(g_fs_base + 0x24) == 2);
        xbox_IrqlLeaveInterrupt(saved_irql);
        CHECK(sync_callbacks == 2);

        BRIDGE_MEM32(g_esp + 4) = 0; /* Missing routine remains a false result. */
        bridge_KeSynchronizeExecution();
        CHECK(g_esp == 0x30000 && g_eax == 0 && sync_callbacks == 2);
    }

    CHECK(BRIDGE_MEM8(g_fs_base + 0x24) == 0);
    CHECK(CloseHandle(worker_started));
    CHECK(CloseHandle(callback_entered));
    CHECK(CloseHandle(callback_release));
    CHECK(VirtualFree(memory, 0, MEM_RELEASE));
    printf("PASS: interrupt synchronization %s; callback arguments, result "
           "and stack preserved\n", exclusion_mode ? "exclusion" : "IRQL");
    return 0;
}
