/* Production allocator regression: above-RAM reservations and the mapped
 * ordinary heap must never grant the same bytes, in either allocation order. */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include "../kernel_test_host.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/kernel/xbox_memory_layout.c"

void *recomp_lookup(ULONG address) { (void)address; abort(); }
void *recomp_lookup_manual(ULONG address) { (void)address; abort(); }

/* The bridge seam: the guest entry in the thunk table at entry_va is looked up
 * on each call, because recomp_lookup_kernel records the slot as it returns. */
typedef void (*guest_fn)(void);
extern guest_fn recomp_lookup_kernel(uint32_t xbox_va);
extern RECOMP_TLS uint32_t g_eax, g_esp;
static uint32_t guest_call(uint32_t entry_va, uint32_t sp_va, uint32_t nargs,
                           const uint32_t *args)
{
    uint32_t *sp = (uint32_t *)((uintptr_t)sp_va + g_memory_offset);
    uint32_t i;
    sp[0] = 0xBEEF0001u;
    for (i = 0; i < nargs; i++)
        sp[1 + i] = args[i];
    g_esp = sp_va;
    g_eax = 0xDEADBEEFu;
    recomp_lookup_kernel(*(uint32_t *)((uintptr_t)entry_va + g_memory_offset))();
    return g_eax;
}

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expr); exit(1); \
} } while (0)
#define RAM_END 0x04000000u
#define MAP_END 0x08000000u

static uint8_t *bytes(uint32_t address)
{
    return (uint8_t *)((uintptr_t)address + g_memory_offset);
}

static void expect_bytes(uint32_t address, uint32_t size, uint8_t value)
{
    for (uint32_t i = 0; i < size; ++i) CHECK(bytes(address)[i] == value);
}

static void reset_allocators(void)
{
    g_heap_next = XBOX_HEAP_BASE;
    g_heap_block_count = 0;
    g_heap_alloc_count = 0;
    g_reserve_next = 0;
    g_reserve_alloc_count = 0;
    t_reserve_begin = t_reserve_end = 0;
    g_contig_alloc_count = 0;
    g_contig_next = XBOX_CONTIG_BASE + 0x1000;
}

static void check_ownership(void)
{
    for (int i = 0; i < g_reserve_alloc_count; ++i) {
        uint64_t begin = g_reserve_allocs[i].addr;
        uint64_t end = begin + g_reserve_allocs[i].size;
        CHECK(begin >= RAM_END && end <= MAP_END);
        if (i) CHECK((uint64_t)g_reserve_allocs[i - 1].addr +
                     g_reserve_allocs[i - 1].size <= begin);
        for (int j = 0; j < g_heap_block_count; ++j)
            CHECK(end <= g_heap_blocks[j].addr ||
                  (uint64_t)g_heap_blocks[j].addr + g_heap_blocks[j].size <= begin);
    }
}

enum { THREADS = 8, ALLOCS_PER_THREAD = 128 };
static HANDLE start_event;
static DWORD WINAPI allocation_worker(void *context)
{
    uintptr_t kind = (uintptr_t)context;
    CHECK(WaitForSingleObject(start_event, 10000) == WAIT_OBJECT_0);
    for (unsigned i = 0; i < ALLOCS_PER_THREAD; ++i) {
        uint32_t address = kind & 1
            ? xbox_ReserveAlloc(4096, 4096) : xbox_HeapAlloc(4096, 4096);
        CHECK(address);
        memset(bytes(address), (int)(kind + 1), 4096);
    }
    return 0;
}

int main(void)
{
    /* This fixture owns fixed raw scratch addresses below the export page. */
    if (!xbox_ConfigureKernelData(0x00740000u)) return 90;

    uint8_t *arena = VirtualAlloc(NULL, MAP_END, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    uint32_t reservation, heap, second, next;
    HANDLE workers[THREADS];
    CHECK(arena);
    g_memory_offset = (ptrdiff_t)arena;
    g_memory_size = MAP_END;
    g_xbox_map_size = MAP_END;
    g_xbox_total_ram = RAM_END;
    InitializeCriticalSection(&g_allocator_lock);
    g_allocator_lock_initialized = 1;
    SetEnvironmentVariableA("RECOMP_HEAP_WATCH", NULL);

    /* This is the menu-stall layout: CRT reserves at 64 MB before the
     * ordinary heap grows across that address. No allocator may zero it. */
    reservation = xbox_ReserveAlloc(0x100000, 4096);
    CHECK(reservation == RAM_END);
    memset(bytes(reservation), 0xA5, 0x100000);
    g_heap_next = RAM_END - 4096;
    heap = xbox_HeapAlloc(8192, 4096);
    CHECK(heap == RAM_END + 0x100000);
    expect_bytes(reservation, 0x100000, 0xA5);
    check_ownership();

    /* A reservation made later must skip live and freed heap blocks. */
    reset_allocators();
    g_heap_next = RAM_END;
    heap = xbox_HeapAlloc(8192, 4096);
    memset(bytes(heap), 0x3C, 8192);
    xbox_HeapFree(heap);
    reservation = xbox_ReserveAlloc(0x100000, 4096);
    CHECK(reservation == RAM_END + 8192);
    CHECK(!xbox_ReserveRangeAllocated(heap));
    memset(bytes(reservation), 0xA5, 0x100000);
    expect_bytes(heap, 8192, 0x3C);
    CHECK(xbox_HeapAlloc(8192, 4096) == heap);
    expect_bytes(reservation, 0x100000, 0xA5);
    check_ownership();

    /* Alignment and skipped heap bytes do not become reservation coverage. */
    reset_allocators();
    g_heap_next = RAM_END + 0x10000;
    heap = xbox_HeapAlloc(8192, 4096);
    reservation = xbox_ReserveAlloc(8192, 4096);
    second = xbox_ReserveAlloc(8192, 0x10000);
    CHECK(reservation == RAM_END && second == RAM_END + 0x20000);
    CHECK(xbox_ReserveRangeAllocated(reservation + 8191));
    CHECK(!xbox_ReserveRangeAllocated(reservation + 8192));
    CHECK(!xbox_ReserveRangeAllocated(heap));
    CHECK(!xbox_ReserveRangeAllocated(second - 1));
    CHECK(xbox_ReserveRangeAllocated(second));
    CHECK(!xbox_ReserveRangeAllocated(second + 8192));
    next = xbox_HeapAlloc(8192, 4096);
    CHECK(next == RAM_END + 0x12000);
    check_ownership();

    /* All failing arithmetic leaves ownership/high-water metadata intact. */
    next = g_reserve_next;
    CHECK(!xbox_ReserveAlloc(0xFFFFFFFFu, 4096));
    CHECK(!xbox_ReserveAlloc(4096, 0x80000000u));
    CHECK(!xbox_ReserveAlloc(4096, 12288));
    CHECK(g_reserve_next == next);
    next = g_heap_next;
    CHECK(!xbox_HeapAlloc(0xFFFFFFFFu, 4096));
    CHECK(!xbox_HeapAlloc(4096, 0x80000000u));
    CHECK(!xbox_HeapAlloc(4096, 12288));
    CHECK(g_heap_next == next);
    check_ownership();

    /* Simultaneous reservations cannot race each other or heap allocation. */
    reset_allocators();
    g_heap_next = RAM_END;
    start_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    CHECK(start_event);
    for (uintptr_t i = 0; i < THREADS; ++i) {
        workers[i] = CreateThread(NULL, 0, allocation_worker, (void *)i, 0, NULL);
        CHECK(workers[i]);
    }
    CHECK(SetEvent(start_event));
    CHECK(WaitForMultipleObjects(THREADS, workers, TRUE, 10000) == WAIT_OBJECT_0);
    for (unsigned i = 0; i < THREADS; ++i) {
        DWORD code;
        CHECK(GetExitCodeThread(workers[i], &code) && code == 0);
        CloseHandle(workers[i]);
    }
    CloseHandle(start_event);
    CHECK(g_reserve_alloc_count == THREADS / 2 * ALLOCS_PER_THREAD);
    CHECK(g_heap_block_count == THREADS / 2 * ALLOCS_PER_THREAD);
    check_ownership();

    /* NtAllocateVirtualMemory through the bridge, on this allocator. A reserve
     * that neither the arena above RAM nor the heap can back is refused with
     * STATUS_NO_MEMORY, not clamped to a smaller block that reports success. A
     * commit above the mapping is refused: nothing there is backed. A reserve
     * above it is granted inside the arena, and the write-back says where. */
    {
        enum { ALLOC_VA = 0x10000, BASE_PTR = 0x20000, SIZE_PTR = 0x20004, SP = 0x30000 };
        uint32_t a[5], status, base, size;

        reset_allocators();
        g_heap_next = XBOX_HEAP_BASE;
        g_xbox_mem_offset = g_memory_offset;
        *(uint32_t *)bytes(ALLOC_VA) = 0x80000000u | 184u;
        xbox_kernel_set_thunk_address(ALLOC_VA, 1);
        xbox_kernel_bridge_init();

        /* 16 MB pure reserve: the arena above RAM takes it. */
        *(uint32_t *)bytes(BASE_PTR) = 0;
        *(uint32_t *)bytes(SIZE_PTR) = 0x01000000u;
        a[0] = BASE_PTR; a[1] = 0; a[2] = SIZE_PTR; a[3] = 0x2000; a[4] = 0x04;
        status = guest_call(ALLOC_VA, SP, 5, a);
        base = *(uint32_t *)bytes(BASE_PTR);
        size = *(uint32_t *)bytes(SIZE_PTR);
        CHECK(status == 0);
        CHECK(base >= RAM_END && (uint64_t)base + size <= MAP_END && size == 0x01000000u);
        check_ownership();

        /* 256 MB pure reserve: the arena has 48 MB left and the heap about 127 MB.
         * Refused, and neither out-parameter is written. */
        *(uint32_t *)bytes(BASE_PTR) = 0;
        *(uint32_t *)bytes(SIZE_PTR) = 0x10000000u;
        status = guest_call(ALLOC_VA, SP, 5, a);
        CHECK(status == 0xC0000017u);
        CHECK(*(uint32_t *)bytes(SIZE_PTR) == 0x10000000u);
        CHECK(*(uint32_t *)bytes(BASE_PTR) == 0);
        check_ownership();

        /* Commit above the mapping with no reservation behind it would claim
         * pages nobody reserved, and their RAM alias is live. Refused. */
        *(uint32_t *)bytes(BASE_PTR) = MAP_END + 0x100000u;
        *(uint32_t *)bytes(SIZE_PTR) = 4096;
        a[0] = BASE_PTR; a[1] = 0; a[2] = SIZE_PTR; a[3] = 0x1000; a[4] = 0x04;
        status = guest_call(ALLOC_VA, SP, 5, a);
        CHECK(status == 0xC0000018u);
        CHECK(*(uint32_t *)bytes(BASE_PTR) == MAP_END + 0x100000u);

        /* Reserve above the mapping at an address that would have aliased RAM
         * under the old mirror layout. There is no alias now, so the allocator
         * grants it from backed memory inside the mapping (here the heap, the
         * reserve being under the arena threshold), and the write-back reports
         * that address, not the requested one. */
        *(uint32_t *)bytes(BASE_PTR) = MAP_END + 0x100000u;
        *(uint32_t *)bytes(SIZE_PTR) = 0x10000;
        a[3] = 0x2000;
        status = guest_call(ALLOC_VA, SP, 5, a);
        base = *(uint32_t *)bytes(BASE_PTR);
        CHECK(status == 0);
        CHECK(base != MAP_END + 0x100000u);
        CHECK((uint64_t)base + 0x10000u <= MAP_END);
        CHECK(*(uint32_t *)bytes(SIZE_PTR) == 0x10000u);
        check_ownership();

        /* Reserve above the mapping at an address past RAM: granted, and the
         * write-back reports wherever the allocator put it. */
        *(uint32_t *)bytes(BASE_PTR) = MAP_END + RAM_END + 0x10000u;
        *(uint32_t *)bytes(SIZE_PTR) = 0x10000;
        status = guest_call(ALLOC_VA, SP, 5, a);
        CHECK(status == 0);
        CHECK(*(uint32_t *)bytes(SIZE_PTR) == 0x10000u);
        check_ownership();
    }

    /* A retail-sized mapping still has no above-RAM reserve arena. */
    g_memory_size = RAM_END;
    CHECK(!xbox_ReserveAlloc(4096, 4096));
    DeleteCriticalSection(&g_allocator_lock);
    CHECK(VirtualFree(arena, 0, MEM_RELEASE));
    puts("reserve/heap exact ownership, arithmetic and concurrency passed");
    return 0;
}
