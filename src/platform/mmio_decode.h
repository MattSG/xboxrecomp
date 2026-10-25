/*
 * mmio_decode.h -- one x86-64 instruction decoder for trapped MMIO.
 *
 * A device whose registers need semantics cannot be plain memory: the reads
 * have to be answered and the writes have to be seen. The way that works here
 * is to leave the page PAGE_NOACCESS, catch the access in a vectored handler,
 * decode the faulting instruction, service it against the device model, and
 * step over it.
 *
 * Accessors are passed by the device-specific fault handler.
 *
 * Header-only and static inline: one function, one caller per device, and a
 * library for it would be more build wiring than code.
 *
 * The instructions covered are what the XDK device code actually emits against
 * registers -- moves both ways, the immediate forms, the zero-extending loads,
 * and the ALU, read-modify-write and flag-setting forms a poll loop is built
 * from (DirectSound polls with `test dword [mem], imm32`, and a C compiler
 * folds `x += REG` into an ALU op with a memory operand).
 * Anything outside that set returns 0 rather than guessing: stepping over an
 * instruction that was not understood corrupts the guest silently, which is
 * far worse than a fault naming the opcode.
 */
#ifndef MMIO_DECODE_H
#define MMIO_DECODE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32) || defined(__x86_64__)
#include "xbox_winnt.h"
#if !defined(_WIN32)
#include "win32_compat.h"
#endif

/* Service one register access. dev is passed straight back to the callbacks. */
typedef uint64_t (*mmio_read_fn)(void *dev, uint32_t off, int size);
typedef void     (*mmio_write_fn)(void *dev, uint32_t off, uint64_t val, int size);

static inline uint64_t *mmio_ctx_reg(PCONTEXT c, int reg)
{
    switch (reg & 0xF) {
    case 0:  return (uint64_t *)&c->Rax;   case 1:  return (uint64_t *)&c->Rcx;
    case 2:  return (uint64_t *)&c->Rdx;   case 3:  return (uint64_t *)&c->Rbx;
    case 4:  return (uint64_t *)&c->Rsp;   case 5:  return (uint64_t *)&c->Rbp;
    case 6:  return (uint64_t *)&c->Rsi;   case 7:  return (uint64_t *)&c->Rdi;
    case 8:  return (uint64_t *)&c->R8;    case 9:  return (uint64_t *)&c->R9;
    case 10: return (uint64_t *)&c->R10;   case 11: return (uint64_t *)&c->R11;
    case 12: return (uint64_t *)&c->R12;   case 13: return (uint64_t *)&c->R13;
    case 14: return (uint64_t *)&c->R14;   case 15: return (uint64_t *)&c->R15;
    default: return NULL;
    }
}

static inline uint64_t mmio_width_mask(int size)
{
    return size == 8 ? UINT64_MAX : (1ULL << (size * 8)) - 1;
}

static inline uint64_t mmio_read_reg(PCONTEXT ctx, int reg, int size, int rex)
{
    int high_byte = size == 1 && !rex && reg >= 4 && reg < 8;
    return (*mmio_ctx_reg(ctx, high_byte ? reg - 4 : reg) >> (high_byte ? 8 : 0)) &
           mmio_width_mask(size);
}

static inline void mmio_write_reg(PCONTEXT ctx, int reg, int size, int rex, uint64_t value)
{
    int high_byte = size == 1 && !rex && reg >= 4 && reg < 8;
    int shift = high_byte ? 8 : 0;
    uint64_t mask = mmio_width_mask(size), *dst = mmio_ctx_reg(ctx, high_byte ? reg - 4 : reg);
    if (size >= 4) *dst = value & mask; /* DWORD writes clear the upper half. */
    else *dst = (*dst & ~(mask << shift)) | ((value & mask) << shift);
}

static inline int mmio_modrm_len(const uint8_t *ip, int rex_b)
{
    uint8_t modrm = *ip;
    int mod = (modrm >> 6) & 3;
    int rm  = (modrm & 7) | (rex_b ? 8 : 0);
    int len = 1;

    if (mod == 3) return 1;
    if ((rm & 7) == 4) {
        len += 1;                                  /* SIB */
        if (mod == 0 && (ip[1] & 7) == 5) len += 4; /* no base: disp32 */
    }
    if (mod == 0 && (rm & 7) == 5) len += 4;        /* disp32 */
    else if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
    return len;
}

/* Defined status flags; auxiliary carry is set separately for subtraction. */
static inline void mmio_set_flags(PCONTEXT ctx, uint64_t result, int size,
                                  int carry, int overflow)
{
    unsigned parity;
    ctx->EFlags &= ~(0x0001u | 0x0004u | 0x0040u | 0x0080u | 0x0800u);
    result &= mmio_width_mask(size);
    if (result == 0) ctx->EFlags |= 0x0040u;
    if (result & (1ULL << (size * 8 - 1))) ctx->EFlags |= 0x0080u;
    if (carry) ctx->EFlags |= 0x0001u;
    if (overflow) ctx->EFlags |= 0x0800u;
    parity = (unsigned)result & 0xFFu;
    parity ^= parity >> 4; parity ^= parity >> 2; parity ^= parity >> 1;
    if (!(parity & 1)) ctx->EFlags |= 0x0004u;
}

/* Decode a caller-owned snapshot; no callback/context mutation before validation. */
static inline int mmio_emulate_bytes(PCONTEXT ctx, uint32_t off, void *dev,
                                     mmio_read_fn rd, mmio_write_fn wr,
                                     const uint8_t *ip, size_t length)
{
    int prefix = 0, has66 = 0, rex = 0, has_rex = 0;
    const uint8_t *op;
    int size, rex_w, rex_r, rex_b, mlen, reg, ilen, alu;
    uint64_t m, v, imm;

    if (!ctx || !ip || !rd || !wr) return 0;
    if (length > 15) length = 15;
    for (;;) {
        if ((size_t)prefix >= length) return 0;
        uint8_t b = ip[prefix];
        if (b == 0x66) { has66 = 1; has_rex = 0; rex = 0; prefix++; }
        else if (b == 0xF2 || b == 0xF3) { has_rex = 0; rex = 0; prefix++; }
        else if (b >= 0x40 && b <= 0x4F) { rex = b; has_rex = 1; prefix++; }
        else break;
    }
    rex_w = has_rex && (rex & 0x08);
    rex_r = has_rex && (rex & 0x04);
    rex_b = has_rex && (rex & 0x01);

    op   = ip + prefix;
    size = rex_w ? 8 : (has66 ? 2 : 4);

    /* Validate the complete supported memory encoding before touching a device. */
    {
        size_t opcode_bytes = 1, modrm, total, immediate = 0;
        switch (op[0]) {
        case 0x0F:
            if ((size_t)prefix + 1 >= length || (op[1] != 0xB6 && op[1] != 0xB7)) return 0;
            opcode_bytes = 2;
            break;
        case 0xC7: immediate = size == 2 ? 2 : 4; break;
        case 0xC6: immediate = 1; break;
        case 0x88: case 0x89: case 0x8A: case 0x8B:
        case 0x84: case 0x85: case 0x38: case 0x39: case 0x3A: case 0x3B:
        case 0x08: case 0x09: case 0x20: case 0x21: break;
        default: return 0;
        }
        modrm = (size_t)prefix + opcode_bytes;
        if (modrm >= length || (ip[modrm] & 0xC0) == 0xC0) return 0;
        if ((op[0] == 0xC6 || op[0] == 0xC7) && (ip[modrm] & 0x38)) return 0;
        if ((ip[modrm] & 7) == 4 && modrm + 1 >= length) return 0;
        total = modrm + mmio_modrm_len(ip + modrm, rex_b) + immediate;
        if (total > length) return 0;
    }

    /* Two-operand ALU: 00-03 ADD, 08-0B OR, 20-23 AND, 28-2B SUB, 30-33 XOR,
     * 38-3B CMP. Bit 0 clear is the byte form; bit 1 set makes the register
     * the destination. 10-1B (ADC/SBB) are not decoded. */
    if (op[0] < 0x40 && (op[0] & 7) < 4) {
        alu = op[0] >> 3;
        if (alu == 2 || alu == 3)
            return 0;
        if (!(op[0] & 1)) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        wr(dev, off, mmio_read_reg(ctx, reg, size, has_rex), size);
        ctx->Rip += prefix + 1 + mlen;
        return 1;
    }

    case 0xC7: {                                     /* MOV r/m, imm16|32 */
        uint64_t value;
        int immediate_size = size == 2 ? 2 : 4;
        mlen = mmio_modrm_len(op + 1, rex_b);
        if (size == 2) {
            uint16_t immediate;
            memcpy(&immediate, op + 1 + mlen, sizeof(immediate));
            value = immediate;
        } else {
            uint32_t immediate;
            memcpy(&immediate, op + 1 + mlen, sizeof(immediate));
            value = size == 8 ? (uint64_t)(int64_t)(int32_t)immediate : immediate;
        }
        wr(dev, off, value, size);
        ctx->Rip += prefix + 1 + mlen + immediate_size;
        return 1;
    }

    case 0xC6: case 0xC7:                            /* MOV r/m, imm         */
        if (op[0] == 0xC6) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        imm  = mmio_imm(op + 1 + mlen, size, &ilen);
        wr(dev, off, imm & mmio_size_mask(size), size);
        ctx->Rip += prefix + 1 + mlen + ilen;
        return 1;

    case 0x8A: case 0x8B: {                          /* MOV r, r/m   (read)  */
        uint64_t v;
        if (op[0] == 0x8A) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        v    = rd(dev, off, size);
        mmio_write_reg(ctx, reg, size, has_rex, v);
        ctx->Rip += prefix + 1 + mlen;
        return 1;

    case 0x84: case 0x85:                            /* TEST r/m, r          */
        if (op[0] == 0x84) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        m    = rd(dev, off, size);
        r    = mmio_read_reg(ctx, reg, size, has_rex);
        mmio_set_flags(ctx, m & r, size, 0, 0);
        ctx->Rip += prefix + 1 + mlen;
        return 1;

    case 0x38: case 0x39:                            /* CMP r/m, r */
    case 0x3A: case 0x3B: {                          /* CMP r, r/m */
        uint64_t m, r, mask;
        if (!(op[0] & 1)) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        m    = rd(dev, off, size);
        r    = mmio_read_reg(ctx, reg, size, has_rex);
        mask = (size < 8) ? ((1ULL << (size * 8)) - 1) : ~0ULL;
        m &= mask; r &= mask;
        if (op[0] & 2) { uint64_t swap = m; m = r; r = swap; }
        uint64_t result = m - r;
        int overflow = ((m ^ r) & (m ^ result) & (1ULL << (size * 8 - 1))) != 0;
        mmio_set_flags(ctx, result, size, m < r, overflow);
        ctx->EFlags = (ctx->EFlags & ~0x10u) | (unsigned)((m ^ r ^ result) & 0x10u);
        ctx->Rip += prefix + 1 + mlen;
        return 1;
    }

    case 0x08: case 0x09:                            /* OR r/m, r */
    case 0x20: case 0x21: {                          /* AND r/m, r */
        uint64_t value, operand;
        if (!(op[0] & 1)) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        value = rd(dev, off, size);
        operand = mmio_read_reg(ctx, reg, size, has_rex);
        value = (op[0] < 0x20 ? value | operand : value & operand) & mmio_width_mask(size);
        wr(dev, off, value, size);
        mmio_set_flags(ctx, value, size, 0, 0);
        ctx->Rip += prefix + 1 + mlen;
        return 1;
    }

    case 0x0F:
        if (op[1] == 0xB6 || op[1] == 0xB7) {        /* MOVZX r, r/m8|16     */
            int s = (op[1] == 0xB6) ? 1 : 2;
            mlen  = mmio_modrm_len(op + 2, rex_b);
            reg   = ((op[2] >> 3) & 7) | (rex_r ? 8 : 0);
            mmio_write_reg(ctx, reg, size, has_rex, rd(dev, off, s) & mmio_width_mask(s));
            ctx->Rip += prefix + 2 + mlen;
            return 1;
        }
        return 0;

    default:
        return 0;
    }
}

/* Snapshot at most one architectural instruction without faulting on its page
 * boundary. Usually one host read; byte reads recover a readable short prefix
 * when the host refuses a cross-page bulk read. Preserve caller LastError. */
static inline int mmio_emulate(PCONTEXT ctx, uint32_t off, void *dev,
                               mmio_read_fn rd, mmio_write_fn wr)
{
    uint8_t code[15];
    SIZE_T copied = 0;
    DWORD error;
    uintptr_t address;
    if (!ctx || !rd || !wr) return 0;
    error = GetLastError();
    address = (uintptr_t)ctx->Rip;
    ReadProcessMemory(GetCurrentProcess(), (const void *)address, code, sizeof(code), &copied);
    if (!copied) {
        while (copied < sizeof(code) && address <= UINTPTR_MAX - copied) {
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), (const void *)(address + copied),
                                   code + copied, 1, &got) || got != 1) break;
            copied++;
        }
    }
    SetLastError(error);
    return mmio_emulate_bytes(ctx, off, dev, rd, wr, code, copied);
}

#endif /* _WIN32 || __x86_64__ */
#endif /* MMIO_DECODE_H */
