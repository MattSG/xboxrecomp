/* Exercise real executor dispatch and the production completion publisher.
 * Only guest arena setup and GPU readback readiness are supplied by the test. */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int readback_busy;
static unsigned readback_polls;
static int fail_next_malloc;
static void *test_malloc(size_t bytes)
{
    if (fail_next_malloc) { fail_next_malloc = 0; return NULL; }
    return malloc(bytes);
}
static int test_writeback_busy(void)
{
    ++readback_polls;
    return readback_busy;
}
#define nv2a_d3d_writeback_busy test_writeback_busy
#define malloc test_malloc
#include "../../src/kernel/xbox_memory_layout.c"
#undef malloc
#undef nv2a_d3d_writeback_busy

void *recomp_lookup(ULONG address) { (void)address; abort(); }
void *recomp_lookup_manual(ULONG address) { (void)address; abort(); }
extern void nv2a_pb_exec_method(uint32_t, uint32_t, uint32_t);

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expr); exit(1); \
} } while (0)

static volatile uint32_t *guest(uint32_t va)
{
    return (volatile uint32_t *)((uintptr_t)va + g_memory_offset);
}
static void method(uint32_t address, uint32_t value)
{
    nv2a_pb_exec_method(0, address, value);
}

int main(void)
{
    uint8_t *arena = VirtualAlloc(NULL, 0x100000, MEM_RESERVE | MEM_COMMIT,
                                  PAGE_READWRITE);
    uint8_t *registers = calloc(1, 0x1000000);
    volatile uint32_t *regs = (volatile uint32_t *)registers;
    volatile uint32_t *get = (volatile uint32_t *)(registers + NV2A_USER_DMA_GET);
    volatile uint32_t *put = (volatile uint32_t *)(registers + NV2A_USER_DMA_PUT);
    LONG generation;
    unsigned polls;
    XBOX_NV2A_CAPTURE_STATE snapshot;
    CHECK(arena && registers);
    g_memory_base = arena;
    g_memory_size = 0x100000;
    g_memory_offset = (ptrdiff_t)arena;
    xbox_nv2a_wake_init(&g_nv2a_wake);
    *guest(0x10000) = 0x20000;
    *guest(0x20030) = 0x30000;
    *guest(0x2002C) = 99; /* The next submitted counter is not completed work. */
    *guest(0x30000) = 23;
    *get = 111; *put = 999;
    CHECK(xbox_Nv2aCommandFence(0x10000, 0xC, 0) == -1);
    CHECK(xbox_Nv2aMirrorFence(0x10000, 0x2C, 0x30) == 0);
    CHECK(xbox_Nv2aCommandFence(0x10000, 0xC, 0) == 0);

    /* Startup/idle cannot invent a first release, even before DMA_PUT moves. */
    generation = xbox_nv2a_wake_snapshot(&g_nv2a_wake);
    fence_mirrors_tick(0); fence_mirrors_tick(1);
    nv2a_publish_readback_complete(regs);
    CHECK(*guest(0x30000) == 23 && *get == 111 && !readback_polls);
    CHECK(xbox_nv2a_wake_snapshot(&g_nv2a_wake) == generation);

    /* Actual dispatch filters the configured DMA, offset and 3D subchannel. */
    method(0x01A8, 0xD); method(0x1D6C, 0); method(0x1D70, 100);
    nv2a_pb_exec_method(1, 0x01A8, 0xC);
    method(0x1D70, 101);
    method(0x01A8, 0xC); method(0x1D6C, 4); method(0x1D70, 102);
    nv2a_pb_exec_method(1, 0x1D6C, 0);
    method(0x1D70, 103);
    CHECK(!command_fences_pending());
    method(0x1D6C, 0);
    nv2a_pb_exec_method(1, 0x1D70, 104);
    CHECK(!command_fences_pending());

    /* Zero is a real release; a busy prior readback defers its publication. */
    readback_busy = 1;
    method(0x1D70, 0);
    CHECK(command_fences_pending());
    nv2a_publish_readback_complete(regs);
    *guest(0x2002C) = 12345;
    fence_mirrors_tick(0); fence_mirrors_tick(1);
    CHECK(*guest(0x30000) == 23 && *get == 111);
    CHECK(xbox_nv2a_wake_snapshot(&g_nv2a_wake) == generation);
    readback_busy = 0;
    nv2a_publish_readback_complete(regs);
    CHECK(*guest(0x30000) == 0 && *guest(0x2002C) == 12345 && *get == 111);
    CHECK(!command_fences_pending());
    CHECK(xbox_nv2a_wake_snapshot(&g_nv2a_wake) == generation + 1);
    polls = readback_polls;
    nv2a_publish_readback_complete(regs);
    CHECK(readback_polls == polls); /* No work adds no driver polls. */

    /* Latest execution order wins across wrap, never a numeric MAX. */
    *guest(0x30000) = 1;
    method(0x1D70, UINT32_MAX); method(0x1D70, 0);
    nv2a_publish_readback_complete(regs);
    CHECK(*guest(0x30000) == 0);
    method(0x1D70, UINT32_MAX - 1); method(0x1D70, 1);
    nv2a_publish_readback_complete(regs);
    CHECK(*guest(0x30000) == 1);
    method(0x1D70, 3); method(0x1D70, 5);
    nv2a_publish_readback_complete(regs);
    CHECK(*guest(0x30000) == 5);

    /* Retain the target at execution, not a later device/get-pointer chain. */
    *guest(0x22030) = 0x31000;
    *guest(0x31000) = 0xDEADBEEF;
    readback_busy = 1;
    method(0x1D70, 7);
    *guest(0x10000) = 0x22000;
    *guest(0x20030) = 0x31000;
    nv2a_publish_readback_complete(regs);
    CHECK(*guest(0x30000) == 5 && *guest(0x31000) == 0xDEADBEEF);
    readback_busy = 0;
    nv2a_publish_readback_complete(regs);
    CHECK(*guest(0x30000) == 7 && *guest(0x31000) == 0xDEADBEEF);
    method(0x1D70, 9);
    nv2a_publish_readback_complete(regs);
    CHECK(*guest(0x31000) == 9);

    /* Multiple targets before one readback completion must all be retained.
     * Returning to the first target is a new ordered write, not a reorder. */
    readback_busy = 1;
    method(0x1D70, 10);
    CHECK(xbox_Nv2aCommandFence(0x10000, 0xC, 0) == -1);
    *guest(0x22030) = 0x30000;
    method(0x1D70, 11);
    *guest(0x22030) = 0x31000;
    method(0x1D70, 12);
    method(0x1D70, 14); /* Consecutive writes coalesce only at the tail. */
    CHECK(g_fence_mirrors[0].command_head && g_fence_mirrors[0].command_tail);
    CHECK(g_fence_mirrors[0].command_head->target == 0x30000);
    CHECK(g_fence_mirrors[0].command_tail->target == 0x31000);
    CHECK(g_fence_mirrors[0].command_tail->value == 14);
    xbox_Nv2aCaptureSnapshot(&snapshot);
    CHECK(snapshot.version == 1 && snapshot.bytes == sizeof snapshot);
    CHECK(snapshot.mirrors[0].command_queue_count == 3);
    CHECK(snapshot.mirrors[0].device_bytes == XBOX_NV2A_CAPTURE_DEVICE_BYTES);
    nv2a_publish_readback_complete(regs);
    CHECK(*guest(0x30000) == 7 && *guest(0x31000) == 9);
    readback_busy = 0;
    nv2a_publish_readback_complete(regs);
    CHECK(*guest(0x30000) == 11 && *guest(0x31000) == 14);
    CHECK(!g_fence_mirrors[0].command_head && !g_fence_mirrors[0].command_tail);

    /* A target-change CRT allocation failure uses the native heap without
     * publishing any fence/GET/notification or polling prior GPU work. */
    method(0x1D70, 18);
    *guest(0x22030) = 0x30000;
    fail_next_malloc = 1;
    polls = readback_polls;
    generation = xbox_nv2a_wake_snapshot(&g_nv2a_wake);
    method(0x1D70, 20);
    CHECK(!fail_next_malloc && g_fence_mirrors[0].command_tail->native_heap);
    CHECK(*guest(0x31000) == 14 && *guest(0x30000) == 11 && *get == 111);
    CHECK(readback_polls == polls && !g_notify_count &&
          xbox_nv2a_wake_snapshot(&g_nv2a_wake) == generation);
    nv2a_publish_readback_complete(regs);
    CHECK(*guest(0x31000) == 18 && *guest(0x30000) == 20 && *get == 111);
    *guest(0x22030) = 0x31000;
    *guest(0x31000) = 9;

    /* Unreadable chains cannot change guest bytes or invent pending work. */
    *guest(0x10000) = 0;
    method(0x1D70, 11);
    *guest(0x10000) = 0xFFFFFFF0;
    method(0x1D70, 11);
    *guest(0x10000) = 0x22000;
    *guest(0x22030) = 0;
    method(0x1D70, 11);
    CHECK(!command_fences_pending() && *guest(0x31000) == 9);
    *guest(0x22030) = 0x31000;

    /* A cap retains even an executed fence. Publishing it alongside an old
     * GET could let guest reserve logic reuse the unread part of that lap. */
    s_scan_incomplete = 1;
    readback_busy = 1;
    method(0x1D70, 13);
    nv2a_publish_readback_complete(regs);
    CHECK(*guest(0x31000) == 9 && *get == 111);
    readback_busy = 0;
    nv2a_publish_readback_complete(regs);
    CHECK(*guest(0x31000) == 9 && *get == 111 && s_scan_incomplete);
    s_get_held = 1; s_get_held_value = 222;
    readback_busy = 1;
    method(0x1D70, 15);
    nv2a_publish_readback_complete(regs);
    CHECK(s_get_held && *get == 111 && *guest(0x31000) == 9);
    readback_busy = 0;
    nv2a_publish_readback_complete(regs);
    CHECK(!s_get_held && *get == 222 && *put == 999 && s_scan_incomplete);
    CHECK(*guest(0x31000) == 9 && command_fences_pending());
    s_scan_incomplete = 0;
    nv2a_publish_readback_complete(regs);
    CHECK(*guest(0x31000) == 15 && !command_fences_pending());

    /* Publication failure retains the exact target/value for a later retry. */
    method(0x1D70, 17);
    g_memory_size = 0x29000;
    nv2a_publish_readback_complete(regs);
    CHECK(command_fences_pending() && *guest(0x31000) == 15);
    g_memory_size = 0x100000;
    nv2a_publish_readback_complete(regs);
    CHECK(!command_fences_pending() && *guest(0x31000) == 17);

    /* Other titles retain the original generic counter-snapshot behavior. */
    *guest(0x12000) = 0x24000;
    *guest(0x24030) = 0x34000;
    *guest(0x2402C) = 76;
    *guest(0x34000) = 55;
    CHECK(xbox_Nv2aMirrorFence(0x12000, 0x2C, 0x30) == 0);
    fence_mirrors_tick(0);
    *guest(0x2402C) = 77;
    fence_mirrors_tick(1);
    CHECK(*guest(0x34000) == 76 && *guest(0x31000) == 17);

    /* Diagnostics distinguish the real aperture from a guest software
     * channel, retain raw device fields, and never query/read back the GPU. */
    g_nv2a_memory = registers;
    *guest(0x22008) = 0x2000;
    *guest(0x237C0) = 0x14000;
    *guest(0x14040) = 0x4444; *guest(0x14044) = 0x5555;
    *guest(0x224F8) = XBOX_NV2A_BASE;
    *(uint32_t *)(registers + NV2A_PFIFO_DMA_SUBROUTINE) = 0x7777;
    CHECK(xbox_Nv2aNotifyEvent(0x10000, 0x196C, 5) == 0);
    g_notify_count = 7;
    s_scan_incomplete = 1;
    polls = readback_polls;
    SetLastError(0x12345678);
    xbox_Nv2aCaptureSnapshot(&snapshot);
    CHECK(GetLastError() == 0x12345678 && readback_polls == polls);
    CHECK(snapshot.mmio_valid && snapshot.user_put == 999 && snapshot.user_get == 222);
    CHECK(snapshot.scan_incomplete == 1 && !snapshot.get_held);
    CHECK(snapshot.notify_count == 7 && snapshot.notify_param == 5);
    CHECK(snapshot.mirror_count == 2 && !snapshot.counter_mirrors && !snapshot.frame_counters);
    CHECK(snapshot.mirrors[0].device_ptr_va == 0x10000 && snapshot.mirrors[0].device_va == 0x22000);
    CHECK(snapshot.mirrors[0].channel_valid && snapshot.mirrors[0].channel_va == 0x14000);
    CHECK(snapshot.mirrors[0].channel_put == 0x4444 && snapshot.mirrors[0].channel_get == 0x5555);
    CHECK(snapshot.mirrors[0].pfifo_valid && snapshot.mirrors[0].pfifo_subroutine == 0x7777);
    CHECK(*(uint32_t *)(snapshot.mirrors[0].device + 8) == 0x2000);
    CHECK(snapshot.mirrors[0].fence_valid && snapshot.mirrors[0].fence_value == 17);
    CHECK(snapshot.mirrors[0].command_mode && !snapshot.mirrors[0].command_queue_count);
    *guest(0x237C0) = XBOX_NV2A_BASE + 0x800000;
    xbox_Nv2aCaptureSnapshot(&snapshot);
    CHECK(snapshot.mirrors[0].channel_valid && snapshot.mirrors[0].channel_put == 999 &&
          snapshot.mirrors[0].channel_get == 222);
    g_nv2a_memory = NULL;
    *guest(0x10000) = UINT32_MAX - 8;
    xbox_Nv2aCaptureSnapshot(&snapshot);
    CHECK(!snapshot.mmio_valid && !snapshot.mirrors[0].device_valid &&
          !snapshot.mirrors[0].device_bytes);
    xbox_Nv2aCaptureSnapshot(NULL);

    puts("PASS: real semaphore dispatch, no idle early fence, readback/cap/GET "
         "ordering, uint32 wrap, captured targets, generic fallback and read-only state capture");
    g_memory_base = NULL;
    free(registers);
    CHECK(VirtualFree(arena, 0, MEM_RELEASE));
    return 0;
}
