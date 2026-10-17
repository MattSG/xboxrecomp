/* Exercise the production allocator and its positive DMA coverage hints.
 * A private arena replaces only memory-layout initialization, not allocation. */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include "../kernel_test_host.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static RECOMP_TLS unsigned lock_entries;
static void counted_enter(CRITICAL_SECTION *lock)
{
    ++lock_entries;
    EnterCriticalSection(lock);
}
#define EnterCriticalSection counted_enter
#include "../../src/kernel/xbox_memory_layout.c"
#undef EnterCriticalSection

void *recomp_lookup(ULONG address) { (void)address; abort(); }
void *recomp_lookup_manual(ULONG address) { (void)address; abort(); }

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expr); exit(1); \
} } while (0)

static int uncached_range(uint32_t offset, uint32_t size)
{
    uint64_t end = (uint64_t)offset + (size ? size : 1u);
    uint32_t begin, covered;
    if (offset >= XBOX_CONTIG_SIZE || end > XBOX_CONTIG_SIZE)
        return 0;
    return contig_range_lookup(offset, end, &begin, &covered);
}

static void expect_range(uint32_t offset, uint32_t size, int result)
{
    CHECK(uncached_range(offset, size) == result);
    CHECK(xbox_ContiguousRangeAllocated(offset, size) == result);
}

typedef struct {
    HANDLE ready, proceed;
    uint32_t positive, future;
    unsigned first_locks, repeat_locks, miss_locks, new_locks;
    int positive_before, positive_after, missed, found;
} WORKER;

static DWORD WINAPI check_worker(LPVOID argument)
{
    WORKER *w = argument;
    lock_entries = 0;
    w->positive_before = xbox_ContiguousRangeAllocated(w->positive, 0x3000);
    w->first_locks = lock_entries;
    lock_entries = 0;
    for (unsigned i = 0; i < 1000; ++i)
        if (!xbox_ContiguousRangeAllocated(w->positive + (i & 0xFFF), 1))
            w->positive_before = 0;
    w->repeat_locks = lock_entries;
    lock_entries = 0;
    w->missed = !xbox_ContiguousRangeAllocated(w->future, 1) &&
                !xbox_ContiguousRangeAllocated(w->future, 1);
    w->miss_locks = lock_entries;
    SetEvent(w->ready);
    WaitForSingleObject(w->proceed, INFINITE);
    lock_entries = 0;
    w->positive_after = xbox_ContiguousRangeAllocated(w->positive, 0x3000);
    CHECK(lock_entries == 0);
    w->found = xbox_ContiguousRangeAllocated(w->future, 0x1000);
    w->new_locks = lock_entries;
    lock_entries = 0;
    CHECK(xbox_ContiguousRangeAllocated(w->future + 0xFFF, 0));
    CHECK(lock_entries == 0);
    return 0;
}

static double bench(int (*volatile query)(uint32_t, uint32_t), uint32_t offset)
{
    LARGE_INTEGER start, end, frequency;
    volatile unsigned total = 0;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    for (unsigned i = 0; i < 200000; ++i)
        total += query(offset + (i & 0xFFF), (i & 63) + 1);
    QueryPerformanceCounter(&end);
    CHECK(total == 200000);
    return 1e9 * (double)(end.QuadPart - start.QuadPart) /
           (double)frequency.QuadPart / 200000;
}

static int compare_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(void)
{
    const size_t reservation = (size_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE;
    uint8_t *arena = VirtualAlloc(NULL, reservation, MEM_RESERVE, PAGE_READWRITE);
    uint32_t a, b, c, heap, fill, after_heap, split, fresh;
    uint32_t isolated[40];
    WORKER worker = {0};
    HANDLE thread;
    double cached[7], locked[7];
    CHECK(arena);
    CHECK(VirtualAlloc(arena + XBOX_CONTIG_BASE, XBOX_CONTIG_SIZE,
                       MEM_COMMIT, PAGE_READWRITE));
    CHECK(VirtualAlloc(arena + XBOX_HEAP_BASE, 0x10000,
                       MEM_COMMIT, PAGE_READWRITE));
    g_memory_offset = (ptrdiff_t)arena;
    InitializeCriticalSection(&g_allocator_lock);
    g_allocator_lock_initialized = 1;
    SetEnvironmentVariableA("RECOMP_CONTIG_TRACE", NULL);

    expect_range(0x1000, 1, 0);
    expect_range(0, 0, 0);
    heap = xbox_HeapAlloc(0x2000, 4096);
    CHECK(heap == XBOX_HEAP_BASE);
    a = xbox_ContiguousAlloc(1, 4096);
    b = xbox_ContiguousAlloc(0x2000, 4096);
    c = xbox_ContiguousAlloc(0x1000, 0x10000);
    CHECK(a == XBOX_CONTIG_BASE + 0x1000);
    CHECK(b == a + 0x1000);
    CHECK(c == XBOX_CONTIG_BASE + 0x10000);
    expect_range(0x1000, 0x3000, 1); /* Adjacent allocations are continuous. */
    expect_range(0x1FFF, 0, 1);      /* Zero means one byte, including page tail. */
    expect_range(0x3FFF, 1, 1);
    expect_range(0x3FFF, 2, 0);
    expect_range(0x4000, 1, 0);      /* Alignment holes do not become coverage. */
    expect_range(0x10000, 0x1000, 1);
    expect_range(0xFFF0, 32, 0);

    /* Real heap ownership forces the next contiguous allocation to skip it. */
    fill = xbox_ContiguousAlloc(XBOX_HEAP_BASE - (g_contig_next - XBOX_CONTIG_BASE), 4096);
    CHECK(fill == c + 0x1000);
    after_heap = xbox_ContiguousAlloc(0x1000, 4096);
    CHECK(after_heap == XBOX_CONTIG_BASE + heap + 0x2000);
    expect_range(heap - 1, 1, 1);
    expect_range(heap, 1, 0);
    expect_range(heap - 1, 2, 0);
    expect_range(heap, 0x2001, 0);
    expect_range(after_heap - XBOX_CONTIG_BASE, 0x1000, 1);
    xbox_HeapFree(heap);
    CHECK(xbox_HeapBlockSize(heap) == 0);
    expect_range(heap, 0x2000, 0); /* Ordinary heap free does not change ownership. */

    /* A cached union remains covered after entries coalesce and split. */
    CHECK(xbox_ContiguousFree(a));
    CHECK(xbox_ContiguousFree(b | 0x20000000u));
    expect_range(0x1000, 0x3000, 1);
    split = xbox_ContiguousAlloc(0x1000, 0x2000);
    CHECK(split == b);
    CHECK(xbox_ContiguousBlockSize(split) == 0x1000);
    expect_range(0x1000, 0x3000, 1);
    expect_range(0x3000, 0x1000, 1); /* Freed tail still backs DMA. */

    /* Eviction and hash collisions must never fill the intervening gaps. */
    for (unsigned i = 0; i < 40; ++i) {
        isolated[i] = xbox_ContiguousAlloc(0x1000, 0x10000) - XBOX_CONTIG_BASE;
        CHECK(isolated[i] && (isolated[i] & 0xFFFF) == 0);
    }
    for (unsigned round = 0; round < 3; ++round)
        for (unsigned i = 0; i < 40; ++i) {
            expect_range(isolated[i], 0x1000, 1);
            expect_range(isolated[i] + 0x1000, 1, 0);
        }
    expect_range(a, 1, 0); /* This API accepts physical offsets, not aliases. */
    expect_range(a | 0x20000000u, 1, 0);
    expect_range(UINT32_MAX - 0x1000, 0x2000, 0);
    expect_range(0x1000, UINT32_MAX, 0);
    expect_range(XBOX_CONTIG_SIZE - 1, 2, 0);
    expect_range(XBOX_CONTIG_SIZE, 0, 0);

    worker.positive = 0x1000;
    worker.future = ((g_contig_next + 0xFFFF) & ~0xFFFFu) - XBOX_CONTIG_BASE;
    worker.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    worker.proceed = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK(worker.ready && worker.proceed);
    thread = CreateThread(NULL, 0, check_worker, &worker, 0, NULL);
    CHECK(thread && WaitForSingleObject(worker.ready, 5000) == WAIT_OBJECT_0);
    CHECK(worker.positive_before && worker.first_locks == 1);
    CHECK(worker.repeat_locks == 0 && worker.missed && worker.miss_locks == 2);
    CHECK(xbox_ContiguousFree(split));
    CHECK(xbox_ContiguousAlloc(0x1000, 0x2000) == split);
    fresh = xbox_ContiguousAlloc(0x1000, 0x10000);
    CHECK(fresh - XBOX_CONTIG_BASE == worker.future);
    SetEvent(worker.proceed);
    CHECK(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
    CHECK(worker.positive_after && worker.found && worker.new_locks == 1);
    CloseHandle(thread);
    CloseHandle(worker.ready);
    CloseHandle(worker.proceed);

    /* Informational medians exercise the actual exported query against the
     * same exact locked lookup. No timing threshold is a regression gate. */
    CHECK(xbox_ContiguousRangeAllocated(0x1000, 0x3000));
    for (unsigned i = 0; i < 7; ++i) {
        cached[i] = bench(xbox_ContiguousRangeAllocated, 0x1000);
        locked[i] = bench(uncached_range, 0x1000);
    }
    qsort(cached, 7, sizeof cached[0], compare_double);
    qsort(locked, 7, sizeof locked[0], compare_double);
    printf("contiguous coverage cache passed; median cached=%.2f ns locked=%.2f ns\n",
           cached[3], locked[3]);
    DeleteCriticalSection(&g_allocator_lock);
    CHECK(VirtualFree(arena, 0, MEM_RELEASE));
    return 0;
}
