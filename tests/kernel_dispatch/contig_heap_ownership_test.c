/* Exercise production heap/contiguous ownership with a private host arena.
 * Heap high-water marks include addresses skipped over contiguous blocks;
 * only blocks actually issued by the heap reserve physical addresses. */
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

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expr); exit(1); \
} } while (0)

static uint8_t *guest_bytes(uint32_t address)
{
    return (uint8_t *)((uintptr_t)address + g_memory_offset);
}

static void expect_bytes(uint32_t address, uint32_t size, uint8_t value)
{
    const uint8_t *bytes = guest_bytes(address);
    for (uint32_t i = 0; i < size; ++i) CHECK(bytes[i] == value);
}

static void check_ownership(void)
{
    /* Freed blocks still own their physical addresses in both tables. */
    for (LONG i = 0; i < g_contig_alloc_count; ++i) {
        uint32_t begin = g_contig_allocs[i].offset;
        uint64_t end = (uint64_t)begin + g_contig_allocs[i].size;
        CHECK(begin >= 0x1000u && !(begin & 0xFFFu));
        CHECK(!(g_contig_allocs[i].size & 0xFFFu));
        CHECK(end <= XBOX_CONTIG_SIZE - XBOX_GPU_INSTANCE_DEFAULT);
        if (i) CHECK((uint64_t)g_contig_allocs[i - 1].offset +
                     g_contig_allocs[i - 1].size <= begin);
        CHECK(xbox_ContiguousRangeAllocated(begin, g_contig_allocs[i].size));
        for (int j = 0; j < g_heap_block_count; ++j) {
            uint64_t heap_end = (uint64_t)g_heap_blocks[j].addr + g_heap_blocks[j].size;
            CHECK(end <= g_heap_blocks[j].addr || heap_end <= begin);
        }
    }
    for (int i = 0; i < g_heap_block_count; ++i)
        CHECK(!xbox_ContiguousRangeAllocated(g_heap_blocks[i].addr, 1));
}

int main(void)
{
    const size_t reservation = (size_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE;
    uint8_t *arena = VirtualAlloc(NULL, reservation, MEM_RESERVE, PAGE_READWRITE);
    uint32_t heap, heap_cross, heap_later, first, aligned, fill, after_heap, reused, gap;
    uint32_t alignment_gap, second_alignment_gap, large_alignment_gap, heap_skip_gap;
    CHECK(arena);
    CHECK(VirtualAlloc(arena + XBOX_CONTIG_BASE, XBOX_CONTIG_SIZE,
                       MEM_COMMIT, PAGE_READWRITE));
    CHECK(VirtualAlloc(arena + XBOX_HEAP_BASE, 0x20000,
                       MEM_COMMIT, PAGE_READWRITE));
    g_memory_offset = (ptrdiff_t)arena;
    InitializeCriticalSection(&g_allocator_lock);
    g_allocator_lock_initialized = 1;
    SetEnvironmentVariableA("RECOMP_CONTIG_TRACE", NULL);

    heap = xbox_HeapAlloc(0x2000, 4096);
    CHECK(heap == XBOX_HEAP_BASE);
    memset(guest_bytes(heap), 0xA5, 0x2000);
    first = xbox_ContiguousAlloc(0x1000, 4096);
    aligned = xbox_ContiguousAlloc(0x1000, 0x10000);
    CHECK(first == XBOX_CONTIG_BASE + 0x1000);
    CHECK(aligned == XBOX_CONTIG_BASE + 0x10000);
    CHECK(!xbox_ContiguousRangeAllocated(0x2000, 1));
    fill = xbox_ContiguousAlloc(XBOX_HEAP_BASE - 0x11000, 4096);
    CHECK(fill == aligned + 0x1000);
    after_heap = xbox_ContiguousAlloc(0x4000, 4096);
    CHECK(after_heap == XBOX_CONTIG_BASE + heap + 0x2000);
    check_ownership();
    expect_bytes(heap, 0x2000, 0xA5);

    /* Ordinary heap growth skips an existing contiguous block. Alignment
     * then leaves another unissued gap below its high-water mark. */
    heap_cross = xbox_HeapAlloc(0x3000, 4096);
    CHECK(heap_cross == heap + 0x6000);
    CHECK(g_heap_next > after_heap - XBOX_CONTIG_BASE + 0x4000);
    memset(guest_bytes(heap_cross), 0x3C, 0x3000);
    heap_later = xbox_HeapAlloc(0x3000, 0x10000);
    CHECK(heap_later == heap + 0x10000);
    memset(guest_bytes(heap_later), 0x5A, 0x3000);
    memset(guest_bytes(after_heap), 0xC3, 0x4000);
    CHECK(xbox_ContiguousRangeAllocated(after_heap - XBOX_CONTIG_BASE, 0x4000));
    CHECK(xbox_ContiguousFree(after_heap));
    CHECK(!xbox_ContiguousBlockSize(after_heap));
    CHECK(xbox_ContiguousRangeAllocated(after_heap - XBOX_CONTIG_BASE, 0x4000));
    reused = xbox_ContiguousAlloc(0x4000, 4096);
    CHECK(reused == after_heap);
    expect_bytes(reused, 0x4000, 0);
    gap = xbox_ContiguousAlloc(0x3000, 4096);
    CHECK(gap == XBOX_CONTIG_BASE + heap + 0x9000);
    CHECK(!xbox_ContiguousRangeAllocated(heap + 0xC000, 1));
    check_ownership();
    expect_bytes(heap, 0x2000, 0xA5);
    expect_bytes(heap_cross, 0x3000, 0x3C);
    expect_bytes(heap_later, 0x3000, 0x5A);

    xbox_HeapFree(heap);
    CHECK(!xbox_HeapBlockSize(heap));
    CHECK(!xbox_ContiguousRangeAllocated(heap, 0x2000));
    expect_bytes(heap, 0x2000, 0xA5);

    /* Simulate an exhausted bump without issuing every remaining page. Only
     * the bump changes: production tables still describe the real holes.
     * The fallback must insert these new lower offsets in sorted order. */
    g_contig_next = XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE - XBOX_GPU_INSTANCE_DEFAULT;
    alignment_gap = xbox_ContiguousAlloc(0x1000, 0x4000);
    CHECK(alignment_gap == XBOX_CONTIG_BASE + 0x4000);
    CHECK(!(alignment_gap & 0x3FFF));
    second_alignment_gap = xbox_ContiguousAlloc(0x2000, 0x4000);
    CHECK(second_alignment_gap == XBOX_CONTIG_BASE + 0x8000);
    CHECK(!(second_alignment_gap & 0x3FFF));
    large_alignment_gap = xbox_ContiguousAlloc(0x6000, 4096);
    CHECK(large_alignment_gap == XBOX_CONTIG_BASE + 0xA000);
    heap_skip_gap = xbox_ContiguousAlloc(0x4000, 4096);
    CHECK(heap_skip_gap == XBOX_CONTIG_BASE + heap + 0xC000);
    CHECK(xbox_ContiguousRangeAllocated(heap + 0x2000, 0x4000));
    CHECK(!xbox_ContiguousRangeAllocated(heap + 0x6000, 1));
    CHECK(xbox_ContiguousRangeAllocated(heap + 0x9000, 0x7000));
    CHECK(!xbox_ContiguousRangeAllocated(heap + 0x10000, 1));
    CHECK(!xbox_ContiguousRangeAllocated(0x3000, 2));
    check_ownership();
    expect_bytes(heap, 0x2000, 0xA5);
    expect_bytes(heap_cross, 0x3000, 0x3C);
    expect_bytes(heap_later, 0x3000, 0x5A);

    /* Heap free/reuse cannot transfer its address to contiguous storage. */
    CHECK(xbox_HeapAlloc(0x2000, 4096) == heap);
    expect_bytes(heap, 0x2000, 0);
    expect_bytes(heap_cross, 0x3000, 0x3C);
    expect_bytes(heap_later, 0x3000, 0x5A);
    check_ownership();
    CHECK(xbox_ContiguousFree(alignment_gap));
    CHECK(xbox_ContiguousRangeAllocated(0x4000, 0x1000));
    CHECK(xbox_ContiguousAlloc(0x1000, 0x4000) == alignment_gap);
    check_ownership();

    puts("contiguous exact heap ownership and exhausted-arena gap reuse passed");
    DeleteCriticalSection(&g_allocator_lock);
    CHECK(VirtualFree(arena, 0, MEM_RELEASE));
    return 0;
}
