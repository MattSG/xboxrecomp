/*
 * NV2A MMIO Hook - VEH instruction decoder for GPU register access
 *
 * Decodes x86-64 MOV instructions that access NV2A MMIO registers,
 * routes them through the NV2A register handlers, and advances RIP.
 */

#include "nv2a_mmio_hook.h"
#include "nv2a_state.h"
#include "../kernel/kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

/* NV2A MMIO base in Xbox VA space */
#define NV2A_MMIO_BASE  0xFD000000u
#define NV2A_MMIO_SIZE  0x01000000u  /* 16MB */
#define NV2A_VRAM_BASE  0xF0000000u
#define NV2A_VRAM_SIZE  (64 * 1024 * 1024)  /* 64MB Xbox VRAM */
#define NV2A_RAMIN_SIZE (2 * 1024 * 1024)   /* includes Xbox RAMHT at 0x1F0000 */

static ptrdiff_t g_mem_offset = 0;
static uint8_t *g_nv2a_vram = NULL;

/* Statistics */
static int g_mmio_read_count = 0;
static int g_mmio_write_count = 0;
static int g_mmio_decode_fail = 0;
static int g_pb_retire_log = 0;
static int g_dma_get_log = 0;
static uint32_t g_semaphore_offset;
static int g_semaphore_log;
static int g_release_complete_log;
static int g_semaphore_offset_log;
static int g_sema_scan_log;
/* One authentic GPU completion per consumed semaphore release.  The guest
 * arms the completion event (dev+0x1970 clear in sub_00344640) before each
 * fence wait, so deliver one pending release completion per arm instead of
 * one per advancing flush.  Fences 5/7/9/0B/0D are released in the ring but
 * only four flush batches advance GET; flip #2 re-waits on the already-
 * consumed fence 0B and needs the fifth completion. */
static volatile LONG g_pending_release_count = 0;
static volatile LONG g_completion_thread_started = 0;
static uint32_t g_completion_dev = 0;

static DWORD WINAPI nv2a_completion_signal_thread(LPVOID parameter)
{
    (void)parameter;
    uint32_t dev = g_completion_dev;
    volatile uint32_t *state = (volatile uint32_t *)(uintptr_t)
        (dev + 0x1970u + g_mem_offset);
    for (;;) {
        if (g_pending_release_count <= 0) {
            Sleep(1);
            continue;
        }
        /* The original GPU interrupt is asynchronous.  Wait for the guest's
         * sub_00344640 arm/clear write before delivering the completion. */
        Sleep(1);
        for (unsigned i = 0; i < 1000 && *state != 0; ++i)
            Sleep(1);
        if (*state != 0)
            continue;
        if (InterlockedDecrement(&g_pending_release_count) < 0)
            continue;
        uint32_t event = dev + 0x196Cu;
        fprintf(stderr, "[NV2A-SIGNAL] dev=%08X event=%08X state=%08X type=%08X offset=%p\n",
                dev, event, *(uint32_t *)(uintptr_t)(event + 4u + g_mem_offset),
                *(uint32_t *)(uintptr_t)(event + g_mem_offset), (void *)g_mem_offset);
        xbox_KeSetEvent((PVOID)(uintptr_t)event, 0, 0);
        fprintf(stderr, "[NV2A-SIGNAL-AFTER] event=%08X state=%08X\n",
                event, *(uint32_t *)(uintptr_t)(event + 4u + g_mem_offset));
    }
    return 0;
}

static void nv2a_defer_completion_signal(uint32_t dev)
{
    if (InterlockedCompareExchange(&g_completion_thread_started, 1, 0) == 0) {
        g_completion_dev = dev;
        HANDLE thread = CreateThread(NULL, 0, nv2a_completion_signal_thread,
                                     NULL, 0, NULL);
        if (thread)
            CloseHandle(thread);
    }
}

/* Decode guest push-buffer packets. Return last valid cursor; malformed or
 * incomplete packet stops retirement. */
static uint32_t nv2a_consume_pushbuffer(uint32_t get, uint32_t put)
{
    NV2AState *gpu = nv2a_get_state();
    uint32_t cursor = get;
    uint32_t packets = 0;

    if (!gpu || get >= 0x04000000u || put >= 0x04000000u || put < get)
        return get;

    if (getenv("MM3_TRACE_PB")) {
        fprintf(stderr, "[NV2A-PB] get=%08X put=%08X words:", get, put);
        for (uint32_t i = 0; i < 8 && get + i * 4u < put; ++i)
            fprintf(stderr, " %08X",
                    *(uint32_t *)(uintptr_t)(get + i * 4u + g_mem_offset));
        fprintf(stderr, "\n");

        /* Occupancy of the submitted span. The guest advances PUT by far
         * more than the number of method packets the parser finds, so
         * report how much of GET..PUT is actually non-zero and where those
         * words sit. A dense span means the decode is wrong; a sparse one
         * means the writes are not landing in the span PUT describes. */
        uint32_t total = (put - get) / 4u, nonzero = 0;
        uint32_t first_nz = 0, last_nz = 0;
        for (uint32_t i = 0; i < total; ++i) {
            uint32_t w = *(uint32_t *)(uintptr_t)(get + i * 4u + g_mem_offset);
            if (w) {
                if (!nonzero) first_nz = i;
                last_nz = i;
                nonzero++;
            }
        }
        fprintf(stderr, "[NV2A-PB-OCC] dwords=%u nonzero=%u (%u%%) "
                "first_nz=%u last_nz=%u\n",
                total, nonzero, total ? (nonzero * 100u / total) : 0u,
                first_nz, last_nz);
        fflush(stderr);
    }

    while (cursor < put && packets++ < 0x100000u) {
        uint32_t header = *(uint32_t *)(uintptr_t)(cursor + g_mem_offset);
        if (header == 0u) {
            cursor += 4u; /* push-buffer NOP/padding */
            continue;
        }
        uint32_t mode = header & 0xE0030003u;
        uint32_t count = (header >> 18) & 0x7FFu;
        uint32_t method = header & 0x1FFCu;
        uint32_t subchannel = (header >> 13) & 7u;
        uint32_t words = count + 1u;
        int non_incrementing = (mode == 0x40000000u);

        if ((mode != 0u && !non_incrementing) || count == 0u ||
            cursor + words * 4u > put)
            break;

        for (uint32_t i = 0; i < count; ++i) {
            uint32_t param = *(uint32_t *)(uintptr_t)(cursor + 4u * (i + 1u)
                                                       + g_mem_offset);
            uint32_t actual_method = non_incrementing ? method : method + i * 4u;
            if (actual_method == 0x0000u) {
                uint32_t context = 0;
                nv2a_trace_ramht_lookup(param, 0u, subchannel);
                if (nv2a_bind_ramht_object(param, 0u, subchannel, &context))
                    continue; /* PFIFO SET_OBJECT does not reach PGRAPH. */
            }
            if (actual_method == 0x1D6Cu) {
                g_semaphore_offset = param;
                /* The write-back below never fired in a whole run, so record
                 * that the guest does set an offset and what it is. */
                if (g_semaphore_offset_log++ < 8)
                    fprintf(stderr, "[NV2A-SEMA-OFF] offset=%08X\n",
                            param);
            } else if (actual_method == 0x1D70u) {
                uint32_t *g351f48 = (uint32_t *)(uintptr_t)(0x351F48u + g_mem_offset);
                uint32_t dev = *g351f48;
                /* The guest's fence completion is tied to the release packet
                 * itself: count it as consumed work when the GPU retires it,
                 * independent of the RAMIN object bookkeeping below. */
                InterlockedIncrement(&g_pending_release_count);
                if (g_release_complete_log++ < 16)
                    fprintf(stderr, "[NV2A-RELEASE-COMPLETE] fence=%04X dev=%08X\n",
                            param, dev);
                if (dev < 0x04000000u) {
                    uint32_t *devp = (uint32_t *)(uintptr_t)(dev + g_mem_offset);
                    uint32_t ring = devp[0x30 / 4];
                    /* [NV2A-SEMA] fires zero times across a full run, so the
                     * scan below never matches and the fence value is never
                     * written where the guest can poll it. Report what the
                     * scan is actually looking at. */
                    if (g_sema_scan_log++ < 6) {
                        unsigned matches = 0, nonzero = 0;
                        for (uint32_t o = 0; o < 0x400u; o += 0x10u) {
                            uint32_t *ob = (uint32_t *)(gpu->ramin_ptr + o);
                            if (ob[0] | ob[1] | ob[2]) nonzero++;
                            if ((ob[0] & NV_DMA_CLASS) == 0x3Du) matches++;
                        }
                        fprintf(stderr, "[NV2A-SEMA-SCAN] ring=%08X off=%08X "
                                "ramin_nonzero=%u class3D=%u\n",
                                ring, g_semaphore_offset, nonzero, matches);
                    }
                    for (uint32_t off = 0; off < 0x400u; off += 0x10u) {
                        uint32_t *obj = (uint32_t *)(gpu->ramin_ptr + off);
                        uint32_t address = (obj[2] & NV_DMA_ADDRESS) |
                                           (obj[0] & NV_DMA_ADJUST);
                        if ((obj[0] & NV_DMA_CLASS) == 0x3Du &&
                            address == (ring & NV_DMA_ADDRESS) &&
                            g_semaphore_offset < obj[1]) {
                            *(uint32_t *)(uintptr_t)(address + g_semaphore_offset +
                                                      g_mem_offset) = param;
                            if (g_semaphore_log++ < 4)
                                fprintf(stderr, "[NV2A-SEMA] release=%08X target=%08X offset=%08X inst=%04X\n",
                                        param, address, g_semaphore_offset, off);
                            break;
                        }
                    }
                }
            }
            pgraph_method(gpu, subchannel, actual_method, param);
        }
        cursor += words * 4u;
    }

    if (cursor != get && g_pb_retire_log++ < 8)
        fprintf(stderr, "[NV2A-PB] retired GET=%08X PUT=%08X -> %08X\n",
                get, put, cursor);
    return cursor;
}

/* Global APU state pointer is declared in apu.h and referenced from main.c
 * (regardless of whether the MMIO hook is active). Keep its definition
 * outside the Win32 guard. */

#if defined(_WIN32)

/* ============================================================
 * x86-64 register access helpers
 * ============================================================ */

/* Map ModRM reg field (+ REX.R) to CONTEXT register pointer */
static uint64_t *ctx_reg64(PCONTEXT ctx, int reg)
{
    switch (reg & 0xF) {
    case 0:  return (uint64_t*)&ctx->Rax;
    case 1:  return (uint64_t*)&ctx->Rcx;
    case 2:  return (uint64_t*)&ctx->Rdx;
    case 3:  return (uint64_t*)&ctx->Rbx;
    case 4:  return (uint64_t*)&ctx->Rsp;
    case 5:  return (uint64_t*)&ctx->Rbp;
    case 6:  return (uint64_t*)&ctx->Rsi;
    case 7:  return (uint64_t*)&ctx->Rdi;
    case 8:  return (uint64_t*)&ctx->R8;
    case 9:  return (uint64_t*)&ctx->R9;
    case 10: return (uint64_t*)&ctx->R10;
    case 11: return (uint64_t*)&ctx->R11;
    case 12: return (uint64_t*)&ctx->R12;
    case 13: return (uint64_t*)&ctx->R13;
    case 14: return (uint64_t*)&ctx->R14;
    case 15: return (uint64_t*)&ctx->R15;
    default: return NULL;
    }
}

/* ============================================================
 * x86-64 instruction decoder (focused on MOV patterns)
 *
 * We only need to handle the patterns MSVC generates for
 * volatile memory access (MEM8/MEM16/MEM32 macros):
 *
 * Writes:
 *   89 /r      MOV r/m32, r32       (32-bit reg → memory)
 *   88 /r      MOV r/m8, r8         (8-bit reg → memory)
 *   66 89 /r   MOV r/m16, r16       (16-bit reg → memory)
 *   C7 /0 id   MOV r/m32, imm32     (32-bit immediate → memory)
 *   C6 /0 ib   MOV r/m8, imm8       (8-bit immediate → memory)
 *
 * Reads:
 *   8B /r      MOV r32, r/m32       (memory → 32-bit reg)
 *   8A /r      MOV r8, r/m8         (memory → 8-bit reg)
 *   66 8B /r   MOV r16, r/m16       (memory → 16-bit reg)
 *   0F B6 /r   MOVZX r32, r/m8      (zero-extend byte → 32-bit)
 *   0F B7 /r   MOVZX r32, r/m16     (zero-extend word → 32-bit)
 *
 * With REX prefixes for 64-bit register extension.
 * ============================================================ */

/* Decode ModRM + optional SIB + displacement, return instruction length */
static int decode_modrm_len(const uint8_t *ip, int has_rex_b)
{
    uint8_t modrm = *ip;
    int mod = (modrm >> 6) & 3;
    int rm = (modrm & 7) | (has_rex_b ? 8 : 0);
    int len = 1; /* modrm byte */

    if (mod == 3) {
        /* Register-direct, no memory access - shouldn't happen for MMIO */
        return len;
    }

    /* Check for SIB byte */
    if ((rm & 7) == 4) {
        len++; /* SIB byte */
    }

    /* Displacement */
    if (mod == 0) {
        if ((rm & 7) == 5) len += 4; /* disp32 (RIP-relative or [disp32]) */
    } else if (mod == 1) {
        len += 1; /* disp8 */
    } else if (mod == 2) {
        len += 4; /* disp32 */
    }

    return len;
}

/*
 * Try to decode and handle the faulting instruction.
 * Returns true if successfully handled, false if unrecognized.
 */
static bool decode_and_handle(PCONTEXT ctx, uint32_t mmio_offset, int is_write)
{
    const uint8_t *ip = (const uint8_t *)ctx->Rip;
    NV2AState *nv2a = nv2a_get_state();
    if (!nv2a) return false;

    int prefix_len = 0;
    int has_66 = 0;     /* operand size override */
    int rex = 0;        /* REX prefix byte */
    int has_rex = 0;

    /* Parse prefixes */
    while (1) {
        uint8_t b = ip[prefix_len];
        if (b == 0x66) {
            has_66 = 1;
            prefix_len++;
        } else if (b == 0xF2 || b == 0xF3) {
            /* REP/REPNE prefix - skip */
            prefix_len++;
        } else if (b >= 0x40 && b <= 0x4F) {
            /* REX prefix */
            rex = b;
            has_rex = 1;
            prefix_len++;
        } else {
            break;
        }
    }

    int rex_w = has_rex && (rex & 0x08); /* 64-bit operand */
    int rex_r = has_rex && (rex & 0x04); /* extends ModRM reg */
    int rex_b = has_rex && (rex & 0x01); /* extends ModRM r/m */

    const uint8_t *opcode = ip + prefix_len;
    int access_size = 4; /* default 32-bit */
    if (has_66) access_size = 2;
    if (rex_w) access_size = 8;

    /* ── MOV r/m, r (write: 88/89) ── */
    if (opcode[0] == 0x89 || opcode[0] == 0x88) {
        if (opcode[0] == 0x88) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = *ctx_reg64(ctx, reg);

        /* Mask to access size */
        if (access_size == 1) val &= 0xFF;
        else if (access_size == 2) val &= 0xFFFF;
        else if (access_size == 4) val &= 0xFFFFFFFF;

        nv2a_mmio_write(nv2a, mmio_offset, val, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_mmio_write_count++;
        return true;
    }

    /* ── MOV r, r/m (read: 8A/8B) ── */
    if (opcode[0] == 0x8B || opcode[0] == 0x8A) {
        if (opcode[0] == 0x8A) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = nv2a_mmio_read(nv2a, mmio_offset, access_size);

        uint64_t *dest = ctx_reg64(ctx, reg);
        if (access_size == 1) {
            *dest = (*dest & ~0xFFULL) | (val & 0xFF);
        } else if (access_size == 2) {
            *dest = (*dest & ~0xFFFFULL) | (val & 0xFFFF);
        } else if (access_size == 4) {
            *dest = val & 0xFFFFFFFF; /* 32-bit write zero-extends */
        } else {
            *dest = val;
        }

        ctx->Rip += prefix_len + 1 + modrm_len;
        g_mmio_read_count++;
        return true;
    }

    /* ── MOV r/m32, imm32 (C7 /0) ── */
    if (opcode[0] == 0xC7) {
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        /* immediate follows modrm+sib+disp */
        const uint8_t *imm_ptr = opcode + 1 + modrm_len;
        uint32_t imm = *(const uint32_t *)imm_ptr;
        int imm_len = (rex_w ? 4 : 4); /* still 32-bit imm even with REX.W */

        nv2a_mmio_write(nv2a, mmio_offset, imm, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len + imm_len;
        g_mmio_write_count++;
        return true;
    }

    /* ── MOV r/m8, imm8 (C6 /0) ── */
    if (opcode[0] == 0xC6) {
        access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        uint8_t imm = *(opcode + 1 + modrm_len);

        nv2a_mmio_write(nv2a, mmio_offset, imm, 1);
        ctx->Rip += prefix_len + 1 + modrm_len + 1;
        g_mmio_write_count++;
        return true;
    }

    /* ── MOVZX r32, r/m8 (0F B6) ── */
    if (opcode[0] == 0x0F && opcode[1] == 0xB6) {
        int modrm_len = decode_modrm_len(opcode + 2, rex_b);
        int reg = ((opcode[2] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = nv2a_mmio_read(nv2a, mmio_offset, 1) & 0xFF;

        uint64_t *dest = ctx_reg64(ctx, reg);
        *dest = val; /* zero-extend to 64-bit */

        ctx->Rip += prefix_len + 2 + modrm_len;
        g_mmio_read_count++;
        return true;
    }

    /* ── MOVZX r32, r/m16 (0F B7) ── */
    if (opcode[0] == 0x0F && opcode[1] == 0xB7) {
        int modrm_len = decode_modrm_len(opcode + 2, rex_b);
        int reg = ((opcode[2] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = nv2a_mmio_read(nv2a, mmio_offset, 2) & 0xFFFF;

        uint64_t *dest = ctx_reg64(ctx, reg);
        *dest = val;

        ctx->Rip += prefix_len + 2 + modrm_len;
        g_mmio_read_count++;
        return true;
    }

    /* ── TEST r/m, r (84/85) - reads memory for flag comparison ── */
    if (opcode[0] == 0x85 || opcode[0] == 0x84) {
        if (opcode[0] == 0x84) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = nv2a_mmio_read(nv2a, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);

        if (access_size == 1) { mem_val &= 0xFF; reg_val &= 0xFF; }
        else if (access_size == 2) { mem_val &= 0xFFFF; reg_val &= 0xFFFF; }
        else if (access_size == 4) { mem_val &= 0xFFFFFFFF; reg_val &= 0xFFFFFFFF; }

        uint64_t result = mem_val & reg_val;

        /* Update flags: ZF, SF, PF; clear OF, CF */
        ctx->EFlags &= ~(0x0001 | 0x0040 | 0x0080 | 0x0800); /* CF, ZF, SF, OF */
        if (result == 0) ctx->EFlags |= 0x0040; /* ZF */
        if (result & (1ULL << (access_size * 8 - 1))) ctx->EFlags |= 0x0080; /* SF */

        ctx->Rip += prefix_len + 1 + modrm_len;
        g_mmio_read_count++;
        return true;
    }

    /* ── CMP r/m, r (38/39) or CMP r, r/m (3A/3B) ── */
    if (opcode[0] == 0x39 || opcode[0] == 0x38) {
        if (opcode[0] == 0x38) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = nv2a_mmio_read(nv2a, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);

        if (access_size <= 4) {
            mem_val &= (1ULL << (access_size * 8)) - 1;
            reg_val &= (1ULL << (access_size * 8)) - 1;
        }

        /* CMP r/m, r: compute r/m - r */
        uint64_t result = mem_val - reg_val;
        ctx->EFlags &= ~(0x0001 | 0x0040 | 0x0080 | 0x0800); /* CF, ZF, SF, OF */
        if (result == 0) ctx->EFlags |= 0x0040; /* ZF */
        if (mem_val < reg_val) ctx->EFlags |= 0x0001; /* CF */
        if (result & (1ULL << (access_size * 8 - 1))) ctx->EFlags |= 0x0080; /* SF */

        ctx->Rip += prefix_len + 1 + modrm_len;
        g_mmio_read_count++;
        return true;
    }

    if (opcode[0] == 0x3B || opcode[0] == 0x3A) {
        if (opcode[0] == 0x3A) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = nv2a_mmio_read(nv2a, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);

        if (access_size <= 4) {
            mem_val &= (1ULL << (access_size * 8)) - 1;
            reg_val &= (1ULL << (access_size * 8)) - 1;
        }

        /* CMP r, r/m: compute r - r/m */
        uint64_t result = reg_val - mem_val;
        ctx->EFlags &= ~(0x0001 | 0x0040 | 0x0080 | 0x0800);
        if (result == 0) ctx->EFlags |= 0x0040;
        if (reg_val < mem_val) ctx->EFlags |= 0x0001;
        if (result & (1ULL << (access_size * 8 - 1))) ctx->EFlags |= 0x0080;

        ctx->Rip += prefix_len + 1 + modrm_len;
        g_mmio_read_count++;
        return true;
    }

    /* ── OR r/m, r (08/09) - read-modify-write ── */
    if (opcode[0] == 0x09 || opcode[0] == 0x08) {
        if (opcode[0] == 0x08) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = nv2a_mmio_read(nv2a, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        uint64_t result = mem_val | reg_val;

        nv2a_mmio_write(nv2a, mmio_offset, result, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_mmio_write_count++;
        return true;
    }

    /* ── AND r/m, r (20/21) ── */
    if (opcode[0] == 0x21 || opcode[0] == 0x20) {
        if (opcode[0] == 0x20) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = nv2a_mmio_read(nv2a, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        uint64_t result = mem_val & reg_val;

        nv2a_mmio_write(nv2a, mmio_offset, result, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_mmio_write_count++;
        return true;
    }

    /* Unrecognized instruction */
    g_mmio_decode_fail++;
    if (g_mmio_decode_fail <= 20) {
        fprintf(stderr, "[NV2A] MMIO decode fail at RIP=%p: %02X %02X %02X %02X %02X %02X\n",
                (void*)ctx->Rip, ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
        fflush(stderr);
    }
    return false;
}

/* ============================================================
 * Public API
 * ============================================================ */

void nv2a_hook_init(ptrdiff_t xbox_mem_offset)
{
    g_mem_offset = xbox_mem_offset;

    /* The guest VRAM aperture (0xF0000000-0xF3FFFFFF) is pre-committed
     * PAGE_READWRITE by main.c. Point the standalone GPU's VRAM at that
     * same backing so CPU texture/geometry writes land where PGRAPH samples
     * them (real Xbox: the aperture IS VRAM). RAMIN follows at +64MB. */
    g_nv2a_vram = (uint8_t *)(uintptr_t)(NV2A_VRAM_BASE + (uintptr_t)g_mem_offset);

    uint8_t *ramin_ptr = g_nv2a_vram + NV2A_VRAM_SIZE;

    /* Initialize NV2A state machine */
    nv2a_init_standalone(g_nv2a_vram, NV2A_VRAM_SIZE,
                         ramin_ptr, NV2A_RAMIN_SIZE);

    fprintf(stderr, "[NV2A] MMIO hook initialized: VRAM=%p RAMIN=%p\n",
            (void*)g_nv2a_vram, (void*)ramin_ptr);
}

bool nv2a_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                           uint32_t fault_xbox_va, int is_write)
{
    /* Compute MMIO offset within NV2A register space */
    uint32_t mmio_offset = fault_xbox_va - NV2A_MMIO_BASE;

    /* Stand in for the GPU DMA engine: the emulated GPU is always caught up,
     * so when the guest reads NV_USER_DMA_GET (0xFD800044, USER block +0x44)
     * advance the guest GET (dev+0x24) and the register mirror to the
     * submitted PUT. The builder polls this register to learn how far the GPU
     * consumed the ring; a kick-time-only advance left dev+0x24 stuck after
     * the single early WBC flush (runs 404/406 froze in sub_0034E420). */
    if (!is_write && mmio_offset == 0x800000u + 0x44u) {
        uint32_t *g351f48 = (uint32_t *)(uintptr_t)(0x351F48u + g_mem_offset);
        uint32_t dev = *g351f48;
        if (dev < 0x04000000u) {
            uint32_t *devp = (uint32_t *)(uintptr_t)(dev + g_mem_offset);
            uint32_t put = devp[0];
            uint32_t get = devp[0x24 / 4];
            uint32_t rend = devp[0x28 / 4];
            if (g_dma_get_log++ < 12)
                fprintf(stderr, "[NV2A-DMA-GET] dev=%08X get=%08X put=%08X rend=%08X\n",
                        dev, get, put, rend);
            /* Emulated GPU is always caught up: NV_USER_DMA_GET mirrors PUT
             * even when PUT wrapped past the ring end, so completion polls
             * (sub_00345740 loc_345EE0: mirror == dev[0]) pass. The software
             * cursor dev+0x24 only advances while PUT stays in-ring; once
             * wrapped, PFIFO_CACHE1_DMA_SUBROUTINE (0x324C) serves the
             * in-ring position for the sub_003444C0 free-space math. */
            uint32_t consumed = nv2a_consume_pushbuffer(get, put);
            if (consumed != get) {
                devp[0x24 / 4] = consumed;
                /* Original sub_00344640 waits on the device completion
                 * dispatcher object at dev+0x196C after the GPU advances
                 * the ring. Signal that authentic producer transition. */
                fprintf(stderr, "[NV2A-WAKE] dev=%08X get=%08X put=%08X consumed=%08X object=%08X\n",
                        dev, get, put, consumed, dev + 0x196Cu);
                nv2a_defer_completion_signal(dev);
            }
            nv2a_mmio_write(nv2a_get_state(), 0x800000u + 0x44u, consumed, 4);
        }
    }

    /* PFIFO_CACHE1_DMA_SUBROUTINE (0xFD00324C): the guest falls back to this
     * register when NV_USER_DMA_GET has wrapped past the ring end
     * (sub_003444C0 0x003444DA). The emulated GPU is always caught up, so its
     * DMA GET equals the guest's own GET cursor (dev+0x24); serve that so the
     * free-space math in sub_003444C0/sub_00344640 sees real room. */
    if (!is_write && mmio_offset == 0x324Cu) {
        uint32_t *g351f48 = (uint32_t *)(uintptr_t)(0x351F48u + g_mem_offset);
        uint32_t dev = *g351f48;
        uint32_t value = 0;
        if (dev >= 0x00001000u && dev < 0x04000000u) {
            value = ((uint32_t *)(uintptr_t)(dev + g_mem_offset))[0x24 / 4];
        }
        nv2a_mmio_write(nv2a_get_state(), 0x324Cu, value, 4);
    }

    /* Emulated pushbuffer consumption: the game flushes the write-back cache
     * after kicking a command ring (sub_00344AB0's NV_PFB_WBC write) and then
     * waits for the GPU to consume it by polling the DMA GET mirror at
     * *(dev+0x17C0)+0x44 until it equals dev->field_0 (sub_00345740
     * loc_00345EE0) and by spinning on the ring-header GET watermark
     * (sub_00344640 loc_00344733). Real PFIFO pushbuffer processing is a stub
     * (Phase 2-3), so stand in for the GPU DMA engine here: consume the
     * submitted ring from the current GET up to PUT and publish that through
     * the two places the guest reads -- NV_USER_DMA_GET (d->puser.regs) and
     * dev+0x24. This is the legitimate producer the original GPU uses; the
     * previous fake left GET stuck, which froze the sub_0034E420 batch loop
     * (run 404). The ring-header GET at [dev+0x30] is NOT an absolute
     * watermark: the guest treats it as a slot-count index (sub_00344640
     * spins on dev+0x2C - requested vs dev+0x2C - ringGET), so writing an
     * absolute address there poisoned length math and stalled earlier in a
     * table copy (run 405). */
    if (is_write && mmio_offset == 0x100000u + NV_PFB_WBC) {
        uint32_t *g351f48 = (uint32_t *)(uintptr_t)(0x351F48u + g_mem_offset);
        uint32_t dev = *g351f48;
        if (dev < 0x04000000u) {
            uint32_t *devp = (uint32_t *)(uintptr_t)(dev + g_mem_offset);
            uint32_t sub = devp[0x17C0 / 4];
            uint32_t put = devp[0];              /* submitted PUT */
            uint32_t get = devp[0x24 / 4];       /* guest-side GET cursor */
            uint32_t rend = devp[0x28 / 4];      /* ring end */
            uint32_t ring = devp[0x30 / 4];      /* ring-header struct */
            uint32_t ring_slots = devp[0x2C / 4];/* ring slot count */
            /* Decode and retire only complete packets. A malformed packet
             * leaves GET at its last known-good cursor. */
            uint32_t consumed = nv2a_consume_pushbuffer(get, put);
            if (consumed != get) {
                devp[0x24 / 4] = consumed;
                fprintf(stderr, "[NV2A-WAKE] dev=%08X get=%08X put=%08X consumed=%08X object=%08X\n",
                        dev, get, put, consumed, dev + 0x196Cu);
                nv2a_defer_completion_signal(dev);
            }
            /* NV_USER_DMA_GET (0xFD800044) is served from d->puser.regs by
             * the VEH, so the guest poll at sub_00345740 loc_345EE0 sees it. */
            if (sub >= NV2A_MMIO_BASE && sub < NV2A_MMIO_BASE + NV2A_MMIO_SIZE) {
                nv2a_mmio_write(nv2a_get_state(),
                                (sub - NV2A_MMIO_BASE) + 0x44u, consumed, 4);
            } else if (sub >= 0x00001000u && sub < 0xA0000000u - 0x44u) {
                /* DMA GET mirrors may be guest RAM, not NV2A MMIO. */
                *(uint32_t *)(uintptr_t)(sub + 0x44u + g_mem_offset) = consumed;
            }
            fprintf(stderr, "[NV2A] WBC flush: GET 0x%08X PUT 0x%08X -> 0x%08X "
                "words=%08X/%08X/%08X/%08X ring-header 0x%08X -> 0x%08X "
                "slots (dev=0x%08X)\n", get, put, consumed,
                *(uint32_t *)(uintptr_t)(get + g_mem_offset),
                *(uint32_t *)(uintptr_t)(get + 4u + g_mem_offset),
                *(uint32_t *)(uintptr_t)(get + 8u + g_mem_offset),
                *(uint32_t *)(uintptr_t)(get + 12u + g_mem_offset), ring,
                (ring < 0x04000000u && ring_slots < 0x10000u) ? ring_slots : 0,
                dev);
            fflush(stderr);
        }
    }

    bool handled = decode_and_handle(ctx, mmio_offset, is_write);

    /* DMA PUT is the submission doorbell. Retire newly submitted complete
     * packets immediately after the register write becomes visible. */
    if (handled && is_write && mmio_offset == 0x800000u + 0x40u) {
        uint32_t *g351f48 = (uint32_t *)(uintptr_t)(0x351F48u + g_mem_offset);
        uint32_t dev = *g351f48;
        if (dev < 0x04000000u) {
            uint32_t *devp = (uint32_t *)(uintptr_t)(dev + g_mem_offset);
            uint32_t get = devp[0x24 / 4];
            uint32_t put = devp[0];
            uint32_t consumed = nv2a_consume_pushbuffer(get, put);
            if (consumed != get) {
                devp[0x24 / 4] = consumed;
                fprintf(stderr, "[NV2A-WAKE] dev=%08X get=%08X put=%08X consumed=%08X object=%08X\n",
                        dev, get, put, consumed, dev + 0x196Cu);
                nv2a_defer_completion_signal(dev);
                nv2a_mmio_write(nv2a_get_state(), 0x800000u + 0x44u,
                                consumed, 4);
                uint32_t sub = devp[0x17C0 / 4];
                if (sub >= 0x00001000u && sub < 0xA0000000u - 0x44u) {
                    *(uint32_t *)(uintptr_t)(sub + 0x44u + g_mem_offset) = consumed;
                }
            }
        }
    }

    return handled;
}

bool nv2a_hook_handle_vram(uintptr_t fault_addr, uint32_t fault_xbox_va)
{
    /* For VRAM range (0xF0000000-0xF3FFFFFF), allocate pages as before.
     * In the future, we can map these to NV2A VRAM for push buffer DMA.
     * For now, just allocate writable pages. */
    uintptr_t alloc_base = fault_addr & ~(uintptr_t)0xFFFF;
    LPVOID result = VirtualAlloc((LPVOID)alloc_base, 0x10000,
                                 MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!result) {
        result = VirtualAlloc((LPVOID)alloc_base, 0x10000,
                              MEM_COMMIT, PAGE_READWRITE);
    }
    if (result) {
        memset(result, 0, 0x10000);
        return true;
    }
    return false;
}

#else /* !_WIN32 -- SIGSEGV-based MMIO trapping deferred to main.c port */

void nv2a_hook_init(ptrdiff_t xbox_mem_offset)
{ (void)xbox_mem_offset; }

bool nv2a_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                           uint32_t fault_xbox_va, int is_write)
{ (void)ctx; (void)fault_addr; (void)fault_xbox_va; (void)is_write; return false; }

bool nv2a_hook_handle_vram(uintptr_t fault_addr, uint32_t fault_xbox_va)
{ (void)fault_addr; (void)fault_xbox_va; return false; }

#endif /* _WIN32 */
