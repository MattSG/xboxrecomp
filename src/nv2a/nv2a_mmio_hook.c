/*
 * NV2A MMIO Hook - VEH instruction decoder for GPU register access
 *
 * Decodes x86-64 MOV instructions that access NV2A MMIO registers,
 * routes them through the NV2A register handlers, and advances RIP.
 */

#include "nv2a_mmio_hook.h"
#include "nv2a_state.h"
#include "platform/mmio_decode.h"
#include <stdio.h>

/* NV2A MMIO base in Xbox VA space */
#define NV2A_MMIO_BASE  0xFD000000u
#define NV2A_MMIO_SIZE  0x01000000u  /* 16MB */
#define NV2A_VRAM_SIZE  (64 * 1024 * 1024)  /* 64MB Xbox VRAM */
#define NV2A_RAMIN_SIZE (1 * 1024 * 1024)   /* 1MB RAMIN */

/* Statistics */
static volatile LONG g_mmio_read_count = 0;
static volatile LONG g_mmio_write_count = 0;
static volatile LONG g_mmio_decode_fail = 0;

#if defined(_WIN32) || defined(__x86_64__)

static SRWLOCK g_pvideo_lock = SRWLOCK_INIT;

bool nv2a_hook_pvideo_snapshot(uint32_t regs[0x1000 / 4])
{
    NV2AState *state = nv2a_get_state();
    unsigned i;
    if (!state) return false;
    AcquireSRWLockShared(&g_pvideo_lock);
    for (i = 0; i < 0x1000 / 4; ++i)
        regs[i] = state->pvideo.regs[i * 4];
    ReleaseSRWLockShared(&g_pvideo_lock);
    return true;
}

void nv2a_hook_pvideo_consume(unsigned bank)
{
    NV2AState *state = nv2a_get_state();
    uint64_t buffer;
    if (!state || bank > 1) return;
    AcquireSRWLockExclusive(&g_pvideo_lock);
    buffer = nv2a_mmio_read(state, 0x8700, 4);
    nv2a_mmio_write(state, 0x8700,
                    buffer & ~(uint64_t)(1u << (bank * 4)), 4);
    ReleaseSRWLockExclusive(&g_pvideo_lock);
}

/* Device callbacks retain the GPU model and access statistics; instruction
 * semantics and checked byte fetching belong to the shared host decoder. */
static uint64_t hook_read(void *device, uint32_t offset, int size)
{
    uint64_t value = nv2a_mmio_read((NV2AState *)device, offset, (unsigned)size);
    InterlockedIncrement(&g_mmio_read_count);
    return value;
}

static void hook_write(void *device, uint32_t offset, uint64_t value, int size)
{
    nv2a_mmio_write((NV2AState *)device, offset, value, (unsigned)size);
    InterlockedIncrement(&g_mmio_write_count);
}

static bool decode_and_handle(PCONTEXT ctx, uint32_t mmio_offset, int is_write)
{
    NV2AState *state = nv2a_get_state();
    if (!state || !ctx) return false;
    if (mmio_emulate(ctx, mmio_offset, state, hook_read, hook_write)) return true;
    if (InterlockedIncrement(&g_mmio_decode_fail) <= 20) {
        fprintf(stderr, "[NV2A] MMIO decode fail at RIP=%p offset=%08X write=%d\n",
                (void *)(uintptr_t)ctx->Rip, mmio_offset, is_write);
        fflush(stderr);
    }
    return false;
}

/* ============================================================
 * Public API
 * ============================================================ */

void nv2a_hook_init(ptrdiff_t xbox_mem_offset)
{
    uint8_t *vram;
    (void)xbox_mem_offset; /* Retained for source compatibility; no address translation here. */
    if (nv2a_get_state()) return; /* Preserve existing caller-owned backing. */
    vram = (uint8_t *)VirtualAlloc(NULL, NV2A_VRAM_SIZE + NV2A_RAMIN_SIZE,
                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!vram) {
        fprintf(stderr, "[NV2A] Failed to allocate VRAM/RAMIN backing\n");
        return;
    }
    if (!nv2a_init_standalone(vram, NV2A_VRAM_SIZE,
                             vram + NV2A_VRAM_SIZE, NV2A_RAMIN_SIZE)) {
        VirtualFree(vram, 0, MEM_RELEASE);
        fprintf(stderr, "[NV2A] Standalone state initialization failed\n");
        return;
    }
    fprintf(stderr, "[NV2A] MMIO hook initialized: VRAM=%p RAMIN=%p\n",
            (void *)vram, (void *)(vram + NV2A_VRAM_SIZE));
}

bool nv2a_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                           uint32_t fault_xbox_va, int is_write)
{
    (void)fault_addr;
    if (!ctx || fault_xbox_va < NV2A_MMIO_BASE ||
        fault_xbox_va - NV2A_MMIO_BASE >= NV2A_MMIO_SIZE) return false;
    uint32_t mmio_offset = fault_xbox_va - NV2A_MMIO_BASE;

    bool handled;
    bool pvideo = mmio_offset >= 0x8000u && mmio_offset < 0x9000u;
    if (pvideo) AcquireSRWLockExclusive(&g_pvideo_lock);
    handled = decode_and_handle(ctx, mmio_offset, is_write);
    if (pvideo) ReleaseSRWLockExclusive(&g_pvideo_lock);
    return handled;
}

#else /* no x86-64 instruction decoder for this host */

bool nv2a_hook_pvideo_snapshot(uint32_t regs[0x1000 / 4])
{ (void)regs; return false; }
void nv2a_hook_pvideo_consume(unsigned bank) { (void)bank; }

void nv2a_hook_init(ptrdiff_t xbox_mem_offset)
{ (void)xbox_mem_offset; }

bool nv2a_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                           uint32_t fault_xbox_va, int is_write)
{ (void)ctx; (void)fault_addr; (void)fault_xbox_va; (void)is_write; return false; }

#endif /* _WIN32 || __x86_64__ */
