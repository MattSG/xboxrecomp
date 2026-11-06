/* Real publication/completion code, with only GPU readiness supplied here. */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include <stdio.h>
#include <stdlib.h>

static int readback_busy;
static int test_writeback_busy(void) { return readback_busy; }
#define nv2a_d3d_writeback_busy test_writeback_busy
#include "../../src/kernel/xbox_memory_layout.c"
#undef nv2a_d3d_writeback_busy

void *recomp_lookup(ULONG address) { (void)address; abort(); }
void *recomp_lookup_manual(ULONG address) { (void)address; abort(); }

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #x); ExitProcess(1); \
} } while (0)

static volatile uint32_t *registers;
#define REG(offset) (*(volatile uint32_t *)((char *)registers + (offset)))
static HANDLE waiting;
static volatile LONG wait_calls, wrong_wait, missed_wake;
static int inject_completion;
static BOOL (WINAPI *native_wait)(volatile VOID *, PVOID, SIZE_T, DWORD);
static VOID (WINAPI *native_wake)(PVOID);
static uint32_t observed_sequence, observed_get;
static unsigned observed_completions;

static VOID WINAPI ordered_wake(PVOID address)
{
    if (observed_sequence && (uint32_t)g_notify_count == observed_sequence) {
        CHECK(REG(NV2A_USER_DMA_GET) == observed_get);
        CHECK(REG(NV2A_PFIFO_DMA_GET) == observed_get);
        ++observed_completions;
    }
    native_wake(address);
}

static BOOL WINAPI observed_wait(volatile VOID *address, PVOID compare,
                                 SIZE_T size, DWORD timeout)
{
    if (size != sizeof(LONG) || timeout != 1) InterlockedExchange(&wrong_wait, 1);
    InterlockedIncrement(&wait_calls);
    SetEvent(waiting);
    if (inject_completion) {
        inject_completion = 0;
        nv2a_publish_consumed(registers, REG(NV2A_PFIFO_DMA_PUT), s_notify_executed);
        /* Completion occurs after the final predicate but before registration.
         * Waiting on the old generation must return immediately. */
        if (!native_wait(address, compare, size, timeout))
            InterlockedExchange(&missed_wake, 1);
        return TRUE;
    }
    return native_wait(address, compare, size, timeout);
}

static DWORD WINAPI waiter(LPVOID ignored)
{
    (void)ignored;
    xbox_Nv2aDmaWait(XBOX_NV2A_BASE);
    return 0;
}

static HANDLE start_wait(void)
{
    ResetEvent(waiting);
    HANDLE thread = CreateThread(NULL, 0, waiter, NULL, 0, NULL);
    CHECK(thread && WaitForSingleObject(waiting, 5000) == WAIT_OBJECT_0);
    return thread;
}

static void finish_wait(HANDLE thread)
{
    CHECK(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
    CHECK(CloseHandle(thread));
}

static void test_waits(void)
{
    HANDLE thread;
    unsigned before;
    native_wait = g_nv2a_wake.wait_address;
    CHECK(native_wait && g_nv2a_wake.wake_all);
    g_nv2a_wake.wait_address = observed_wait;
    waiting = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK(waiting);
    g_nv2a_ack_thread = (HANDLE)(uintptr_t)1; /* API checks ownership, not handle use. */

    before = wait_calls;
    xbox_Nv2aDmaWait(XBOX_NV2A_BASE + 4); /* Unknown base keeps guest polling. */
    xbox_Nv2aDmaWait(XBOX_NV2A_BASE);     /* Already idle requires no host wait. */
    CHECK(wait_calls == before);
    xbox_Nv2aDmaPublish(XBOX_NV2A_BASE + NV2A_USER_DMA_PUT, 0x12000);
    inject_completion = 1;
    xbox_Nv2aDmaWait(XBOX_NV2A_BASE);
    CHECK(wait_calls == before + 1 && !missed_wake && !wrong_wait);

    /* A generation-only wake and an older completed boundary cannot report
     * idle when the producer has published a newer PUT. */
    xbox_Nv2aDmaPublish(XBOX_NV2A_BASE + NV2A_USER_DMA_PUT, 0x13000);
    thread = start_wait();
    xbox_nv2a_wake_publish(&g_nv2a_wake);
    CHECK(WaitForSingleObject(thread, 0) == WAIT_TIMEOUT);
    xbox_Nv2aDmaPublish(XBOX_NV2A_BASE + NV2A_USER_DMA_PUT, 0x14000);
    nv2a_publish_consumed(registers, 0x13000, s_notify_executed);
    CHECK(REG(NV2A_PGRAPH_STATUS) && WaitForSingleObject(thread, 0) == WAIT_TIMEOUT);
    nv2a_publish_consumed(registers, 0x14000, s_notify_executed);
    finish_wait(thread);

    /* Even equal pointers are not idle while the original status register is
     * nonzero. A raw guest status write has no notification; timeout notices it. */
    REG(NV2A_PGRAPH_STATUS) = 0x80;
    thread = start_wait();
    CHECK(WaitForSingleObject(thread, 0) == WAIT_TIMEOUT);
    REG(NV2A_PGRAPH_STATUS) = 0;
    finish_wait(thread);

    xbox_Nv2aDmaPublish(XBOX_NV2A_BASE + NV2A_USER_DMA_PUT, 0x15000);
    thread = start_wait();
    InterlockedExchange(&g_nv2a_ack_stop, 1);
    xbox_nv2a_wake_publish(&g_nv2a_wake);
    finish_wait(thread);
    CHECK(REG(NV2A_USER_DMA_GET) == 0x14000 && REG(NV2A_PGRAPH_STATUS));
    g_nv2a_ack_stop = 0;

    /* Native API unavailable retains the yield path and exact predicate. */
    g_nv2a_wake.wait_address = NULL;
    thread = CreateThread(NULL, 0, waiter, NULL, 0, NULL);
    CHECK(thread && WaitForSingleObject(thread, 0) == WAIT_TIMEOUT);
    nv2a_publish_consumed(registers, 0x15000, s_notify_executed);
    finish_wait(thread);
    g_nv2a_wake.wait_address = native_wait;
    g_nv2a_ack_thread = NULL;
    CHECK(CloseHandle(waiting));
}

static void test_notifications(void)
{
    LONG generation;
    native_wake = g_nv2a_wake.wake_all;
    g_nv2a_wake.wake_all = ordered_wake;
    CHECK(xbox_Nv2aNotifyEvent(0x10000, 0x196C, 5) == 0);
    generation = xbox_nv2a_wake_snapshot(&g_nv2a_wake);
    xbox_Nv2aNop(0); xbox_Nv2aNop(4);
    CHECK(!s_notify_executed && !g_notify_count);
    xbox_Nv2aNop(5); xbox_Nv2aNop(5);
    CHECK(s_notify_executed == 2 && !g_notify_count);
    CHECK(xbox_nv2a_wake_snapshot(&g_nv2a_wake) == generation);
    s_scan_incomplete = 1;
    nv2a_publish_readback_complete(registers);
    CHECK(!g_notify_count); /* Cap/idle cannot expose an executed NOP. */

    /* A held older checkpoint captures its sequence before a newer partial
     * submission executes another NOP. Releasing the old readback publishes
     * only the old sequence, after the old GET, even with a newer live PUT. */
    s_get_held = 1; s_get_held_value = 0x16000;
    s_get_held_notify = s_notify_executed;
    xbox_Nv2aDmaPublish(XBOX_NV2A_BASE + NV2A_USER_DMA_PUT, 0x17000);
    xbox_Nv2aNop(5);
    readback_busy = 1;
    nv2a_publish_readback_complete(registers);
    CHECK(!g_notify_count && REG(NV2A_USER_DMA_GET) == 0x15000);
    readback_busy = 0;
    observed_sequence = 2; observed_get = 0x16000;
    nv2a_publish_readback_complete(registers);
    CHECK(g_notify_count == 2 && s_notify_executed == 3 && !s_get_held);
    CHECK(REG(NV2A_USER_DMA_GET) == 0x16000 && REG(NV2A_PFIFO_DMA_GET) == 0x16000);
    CHECK(REG(NV2A_USER_DMA_PUT) == 0x17000 && REG(NV2A_PGRAPH_STATUS));
    nv2a_publish_readback_complete(registers);
    CHECK(g_notify_count == 2); /* Newer capped NOP remains pending. */
    CHECK(observed_completions);
    observed_sequence = 3; observed_get = 0x17000;
    nv2a_publish_consumed(registers, 0x17000, s_notify_executed);
    CHECK(g_notify_count == 3 && !REG(NV2A_PGRAPH_STATUS));
    s_scan_incomplete = 0;

    /* Notification count is an exact unsigned event sequence across wrap. */
    s_notify_executed = UINT32_MAX;
    g_notify_count = -1;
    xbox_Nv2aNop(5);
    CHECK(s_notify_executed == 0 && g_notify_count == -1);
    nv2a_publish_consumed(registers, 0x17000, s_notify_executed);
    CHECK(g_notify_count == 0);
    observed_sequence = 0;
    g_nv2a_wake.wake_all = native_wake;
}

static volatile LONG producer_ready, producer_done;
static DWORD WINAPI producer(LPVOID ignored)
{
    (void)ignored;
    for (uint32_t i = 1; i <= 20000; ++i) {
        xbox_Nv2aDmaPublish(XBOX_NV2A_BASE + NV2A_USER_DMA_PUT, i);
        InterlockedExchange(&producer_ready, (LONG)i);
        while (InterlockedCompareExchange(&producer_done, 0, 0) != (LONG)i)
            YieldProcessor();
    }
    return 0;
}

static void test_concurrent_stores(void)
{
    HANDLE thread = CreateThread(NULL, 0, producer, NULL, 0, NULL);
    CHECK(thread);
    for (LONG i = 1; i <= 20000; ++i) {
        while (InterlockedCompareExchange(&producer_ready, 0, 0) != i)
            YieldProcessor();
        nv2a_publish_consumed(registers, (uint32_t)i, s_notify_executed);
        InterlockedExchange(&producer_done, i);
        AcquireSRWLockExclusive(&g_nv2a_dma_lock);
        CHECK(REG(NV2A_USER_DMA_PUT) == REG(NV2A_PFIFO_DMA_PUT));
        CHECK(REG(NV2A_USER_DMA_GET) == REG(NV2A_PFIFO_DMA_GET));
        CHECK(REG(NV2A_PGRAPH_STATUS) ==
            (REG(NV2A_PFIFO_DMA_PUT) != REG(NV2A_PFIFO_DMA_GET)));
        ReleaseSRWLockExclusive(&g_nv2a_dma_lock);
    }
    finish_wait(thread);
}

int main(void)
{
    uint8_t *arena = VirtualAlloc(NULL, 0x100000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    uint8_t *mmio = calloc(1, XBOX_NV2A_SIZE);
    CHECK(arena && mmio);
    g_memory_base = arena; g_memory_size = 0x100000;
    g_memory_offset = (ptrdiff_t)arena; g_nv2a_memory = mmio;
    registers = (volatile uint32_t *)mmio;
    xbox_nv2a_wake_init(&g_nv2a_wake);

    xbox_Nv2aDmaPublish(0x14040, 0x12345678);
    CHECK(*(uint32_t *)(arena + 0x14040) == 0x12345678 && !g_nv2a_dma_sync);
    CHECK(!REG(NV2A_PFIFO_DMA_PUT) && !REG(NV2A_PFIFO_DMA_GET));
    REG(NV2A_USER_DMA_GET) = 0x11000;
    xbox_Nv2aDmaPublish(XBOX_NV2A_BASE + NV2A_USER_DMA_PUT, 0x12000);
    CHECK(g_nv2a_dma_sync && REG(NV2A_PFIFO_DMA_GET) == 0x11000);
    CHECK(REG(NV2A_USER_DMA_PUT) == 0x12000 && REG(NV2A_PFIFO_DMA_PUT) == 0x12000);
    CHECK(REG(NV2A_PGRAPH_STATUS));
    nv2a_publish_consumed(registers, 0x12000, 0);
    xbox_Nv2aDmaPublish(XBOX_NV2A_BASE + NV2A_USER_DMA_PUT, 0x12000);
    CHECK(!REG(NV2A_PGRAPH_STATUS)); /* Same completed PUT is already idle. */
    xbox_Nv2aDmaPublish(0x14040, 0x87654321);
    CHECK(*(uint32_t *)(arena + 0x14040) == 0x87654321 &&
          REG(NV2A_USER_DMA_PUT) == 0x12000 && !REG(NV2A_PGRAPH_STATUS));

    /* Neither register payload nor completion order is a numeric maximum. */
    xbox_Nv2aDmaPublish(XBOX_NV2A_BASE + NV2A_USER_DMA_PUT, UINT32_MAX - 3);
    xbox_Nv2aDmaPublish(XBOX_NV2A_BASE + NV2A_USER_DMA_PUT, 0x4000);
    nv2a_publish_consumed(registers, UINT32_MAX - 3, 0);
    CHECK(REG(NV2A_PFIFO_DMA_GET) == UINT32_MAX - 3 && REG(NV2A_PGRAPH_STATUS));
    nv2a_publish_consumed(registers, 0x4000, 0);
    CHECK(REG(NV2A_USER_DMA_GET) == 0x4000 && !REG(NV2A_PGRAPH_STATUS));

    test_waits();
    test_notifications();
    test_concurrent_stores();
    CHECK(!wrong_wait && !missed_wake);
    puts("PASS: exact DMA publication/consumption, software fallback, busy/wrap ordering, "
         "concurrent stores, native wait races and deferred NOP/readback checkpoints");
    g_nv2a_memory = NULL; g_memory_base = NULL;
    free(mmio); CHECK(VirtualFree(arena, 0, MEM_RELEASE));
    return 0;
}
