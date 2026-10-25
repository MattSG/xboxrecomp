/*
 * mmio_decode - does the trapped-MMIO instruction decoder service what the
 * XDK's device code emits, and refuse what it does not understand.
 *
 * This is testable without a device or a fault because the decoder only ever
 * reads instruction bytes at ctx->Rip and edits a CONTEXT. Point Rip at a
 * buffer of hand-encoded instructions and the whole thing is a pure function.
 *
 * The cases that matter are the ones where being wrong is silent: an
 * instruction length that leaves Rip mid-instruction, a 32-bit read that does
 * not clear the high half of the destination, flags that send a poll loop the
 * wrong way, and an unrecognised opcode reported as handled -- which steps
 * over an instruction nobody decoded and corrupts the guest with no message.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif
#include "platform/xbox_winnt.h"

#include "mmio_decode.h"

static int failures;
#define CHECK(name, cond) \
    do { if (cond) {} else { printf("FAIL: %s\n", name); failures++; } } while (0)
#define CHECK_U64(name, got, want) \
    do { uint64_t g_=(uint64_t)(got), w_=(uint64_t)(want); \
         if (g_ == w_) {} else { \
            printf("FAIL: %s (got 0x%llX, want 0x%llX)\n", name, \
                   (unsigned long long)g_, (unsigned long long)w_); \
            failures++; } } while (0)

/* A device that just records what it was asked for. */
static struct {
    uint32_t off; uint64_t val; int size; int reads, writes;
    uint64_t next_read;
} dev;

static uint64_t t_read(void *d, uint32_t off, int size)
{
    (void)d; dev.off = off; dev.size = size; dev.reads++;
    return dev.next_read;
}
static void t_write(void *d, uint32_t off, uint64_t v, int size)
{
    (void)d; dev.off = off; dev.val = v; dev.size = size; dev.writes++;
}

/* Run one instruction. Returns what mmio_emulate returned; `code` is the
 * encoding, and Rip is expected to land exactly past it. */
static int run(const uint8_t *code, size_t len, CONTEXT *ctx)
{
    int r;
    memset(&dev, 0, sizeof dev);
    dev.next_read = 0;
    ctx->Rip = (uint64_t)(uintptr_t)code;
    r = mmio_emulate(ctx, 0x54, NULL, t_read, t_write);
    if (r) {
        uint64_t advanced = ctx->Rip - (uint64_t)(uintptr_t)code;
        if (advanced != len) {
            printf("FAIL: Rip advanced %llu, instruction is %llu bytes\n",
                   (unsigned long long)advanced, (unsigned long long)len);
            failures++;
        }
    }
    return r;
}

static void check_snapshot_bounds(void)
{
    const uint8_t code[][8] = {{0x89,0x0C,0x25,0,0x10,0,0},
        {0x66,0xC7,0,0x34,0x12}, {0x48,0xC7,0,0,0,0,0x80}, {0x0F,0xB6,0}};
    const unsigned lengths[] = {7, 5, 7, 3};
    for (unsigned i = 0; i < 4; i++) {
        for (unsigned available = 0; available < lengths[i]; available++) {
            CONTEXT ctx, before;
            memset(&ctx, 0, sizeof ctx); ctx.Rip = (uintptr_t)code[i];
            memcpy(&before, &ctx, sizeof ctx); memset(&dev, 0, sizeof dev);
            CHECK("truncated snapshot refused", !mmio_emulate_bytes(&ctx, 0x54, NULL,
                  t_read, t_write, code[i], available));
            CHECK("truncation preserves context", !memcmp(&ctx, &before, sizeof ctx));
            CHECK("truncation has no callback", dev.reads == 0 && dev.writes == 0);
        }
    }
    CONTEXT ctx = {0};
    const uint8_t legacy_after_rex[] = {0x48,0x66,0xC7,0,0x34,0x12};
    CHECK("legacy after REX handled", run(legacy_after_rex, sizeof legacy_after_rex, &ctx));
    CHECK_U64("legacy after REX ignores REX.W", dev.size, 2);
    CHECK_U64("legacy after REX immediate", dev.val, 0x1234);
    uint8_t maximum[15];
    memset(maximum, 0x66, 13); maximum[13] = 0x89; maximum[14] = 0;
    CHECK("maximum instruction length", run(maximum, sizeof maximum, &ctx));

    size_t page;
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si); page = si.dwPageSize;
#else
    page = (size_t)sysconf(_SC_PAGESIZE);
#endif
    uint8_t *memory = VirtualAlloc(NULL, page * 2, MEM_RESERVE, PAGE_NOACCESS);
    CHECK("instruction page reserve", memory != NULL);
    if (!memory) return;
    CHECK("instruction page commit", VirtualAlloc(memory, page, MEM_COMMIT, PAGE_READWRITE) != NULL);
    memory[page - 2] = 0x89; memory[page - 1] = 0x08;
    ctx.Rip = (uintptr_t)(memory + page - 2); ctx.Rcx = 0x1234;
    memset(&dev, 0, sizeof dev); SetLastError(123);
    CHECK("short instruction at page end", mmio_emulate(&ctx, 0x54, NULL, t_read, t_write));
    CHECK_U64("instruction snapshot preserves LastError", GetLastError(), 123);
    CHECK_U64("page end instruction value", dev.val, 0x1234);
    CHECK_U64("page end instruction RIP", ctx.Rip, (uintptr_t)(memory + page));
    memory[page - 2] = 0xC7; memory[page - 1] = 0;
    ctx.Rip = (uintptr_t)(memory + page - 2); memset(&dev, 0, sizeof dev);
    CHECK("page end truncation refused", !mmio_emulate(&ctx, 0x54, NULL, t_read, t_write));
    CHECK("page end truncation has no callback", dev.reads == 0 && dev.writes == 0);
    ctx.Rip = (uintptr_t)(memory + page);
    CHECK("unreadable instruction refused", !mmio_emulate(&ctx, 0x54, NULL, t_read, t_write));
    CHECK("instruction pages freed", VirtualFree(memory, 0, MEM_RELEASE));
}

static void check_invalid_forms(void)
{
    const uint8_t codes[][3] = {{0x88,0xC0}, {0x89,0xC0}, {0x8A,0xC0},
        {0x8B,0xC0}, {0x84,0xC0}, {0x85,0xC0}, {0x38,0xC0}, {0x39,0xC0}, {0x3A,0xC0}, {0x3B,0xC0},
        {0x08,0xC0}, {0x09,0xC0}, {0x20,0xC0}, {0x21,0xC0}, {0x0F,0xB6,0xC0}};
    CONTEXT ctx = {0};
    for (unsigned i = 0; i < sizeof codes / sizeof codes[0]; i++) {
        memset(&dev, 0, sizeof dev); ctx.Rip = (uintptr_t)codes[i];
        CHECK("register-only instruction refused", !mmio_emulate(&ctx, 0x54, NULL, t_read, t_write));
        CHECK("register-only form has no callback", dev.reads == 0 && dev.writes == 0);
        CHECK_U64("register-only form preserves RIP", ctx.Rip, (uintptr_t)codes[i]);
    }
    uint8_t overlong[17];
    memset(overlong, 0x66, 15); overlong[15] = 0x89; overlong[16] = 0;
    ctx.Rip = (uintptr_t)overlong; memset(&dev, 0, sizeof dev);
    CHECK("overlong encoding refused", !mmio_emulate(&ctx, 0x54, NULL, t_read, t_write));
    CHECK("overlong encoding has no callback", dev.reads == 0 && dev.writes == 0);
    CHECK_U64("overlong encoding preserves RIP", ctx.Rip, (uintptr_t)overlong);
}

static void check_compare_widths(void)
{
    const uint8_t code[4][3] = {{0x38, 0x08}, {0x66, 0x39, 0x08},
                                {0x39, 0x08}, {0x48, 0x39, 0x08}};
    const int widths[4] = {1, 2, 4, 8};
    for (unsigned w = 0; w < 4; w++) {
        uint64_t sign = 1ull << (widths[w] * 8 - 1);
        uint64_t mask = widths[w] == 8 ? UINT64_MAX : (sign * 2 - 1);
        const uint64_t left[] = {sign, 0, sign - 1, 0, 0x10};
        const uint64_t right[] = {1, 1, mask, 0, 1};
        const unsigned expected[] = {w ? 0x814 : 0x810, 0x95,
                                     w ? 0x885 : 0x881, 0x44, 0x14};
        for (unsigned reverse = 0; reverse < 2; reverse++)
        for (unsigned i = 0; i < 5; i++) {
            CONTEXT ctx = {0};
            uint8_t instruction[3];
            memcpy(instruction, code[w], sizeof instruction);
            if (reverse) instruction[(w == 1 || w == 3) ? 1 : 0] += 2;
            memset(&dev, 0, sizeof dev);
            ctx.Rcx = reverse ? left[i] : right[i]; ctx.EFlags = 0x400;
            ctx.Rip = (uintptr_t)instruction; dev.next_read = reverse ? right[i] : left[i];
            CHECK("CMP width handled", mmio_emulate(&ctx, 0x54, NULL, t_read, t_write));
            CHECK_U64("CMP width status flags", ctx.EFlags & 0xCD5, expected[i] | 0x400);
            CHECK("CMP read-only", dev.reads == 1 && dev.writes == 0);
        }
    }
}

static void check_registers_and_flags(void)
{
    CONTEXT ctx = {0};
    { const uint8_t c[] = {0x88, 0x20}; /* MOV [rax], AH */
      ctx.Rax = 0x1234ABCD; ctx.Rsp = 0x55;
      CHECK("MOV AH handled", run(c, sizeof c, &ctx));
      CHECK_U64("MOV AH value", dev.val, 0xAB); }
    { const uint8_t c[] = {0x8A, 0x20}; /* MOV AH, [rax] */
      ctx.Rax = 0xFFEEDDCC; ctx.Rsp = 0xAAAA55;
      memset(&dev, 0, sizeof dev); dev.next_read = 0x12;
      ctx.Rip = (uintptr_t)c;
      CHECK("read AH handled", mmio_emulate(&ctx, 0x54, NULL, t_read, t_write));
      CHECK_U64("read AH preserves other bits", ctx.Rax, 0xFFEE12CC);
      CHECK_U64("read AH preserves RSP", ctx.Rsp, 0xAAAA55); }
    { const uint8_t c[] = {0x40, 0x8A, 0x20}; /* MOV SPL, [rax] */
      memset(&dev, 0, sizeof dev); dev.next_read = 0x12; ctx.Rip = (uintptr_t)c;
      CHECK("REX byte handled", mmio_emulate(&ctx, 0x54, NULL, t_read, t_write));
      CHECK_U64("REX selects SPL", ctx.Rsp, 0xAAAA12); }
    { const uint8_t c[] = {0x66, 0x0F, 0xB6, 0x00};
      ctx.Rax = 0x123456789ABCFFFFull;
      memset(&dev, 0, sizeof dev); dev.next_read = 0xAB; ctx.Rip = (uintptr_t)c;
      CHECK("MOVZX AX handled", mmio_emulate(&ctx, 0x54, NULL, t_read, t_write));
      CHECK_U64("MOVZX AX preserves upper", ctx.Rax, 0x123456789ABC00ABull); }
    { const uint8_t c[] = {0x09, 0x08};
      ctx.Rcx = 2; ctx.EFlags = 0xCC5;
      memset(&dev, 0, sizeof dev); dev.next_read = 1; ctx.Rip = (uintptr_t)c;
      CHECK("OR flags handled", mmio_emulate(&ctx, 0x54, NULL, t_read, t_write));
      CHECK_U64("OR result", dev.val, 3);
      CHECK_U64("OR parity and preserved DF", ctx.EFlags & 0xCC5, 0x404); }
    { const uint8_t c[] = {0x21, 0x08};
      ctx.Rcx = 0x0F; ctx.EFlags = 0xCC5;
      memset(&dev, 0, sizeof dev); dev.next_read = 0xF0; ctx.Rip = (uintptr_t)c;
      CHECK("AND flags handled", mmio_emulate(&ctx, 0x54, NULL, t_read, t_write));
      CHECK_U64("AND zero/even parity", ctx.EFlags & 0xCC5, 0x444); }
    { const uint8_t c[] = {0x84, 0x20}; /* TEST [rax], AH */
      ctx.Rax = 0x8000; ctx.Rsp = 0; ctx.EFlags = 0xCC5;
      memset(&dev, 0, sizeof dev); dev.next_read = 0x80; ctx.Rip = (uintptr_t)c;
      CHECK("TEST AH handled", mmio_emulate(&ctx, 0x54, NULL, t_read, t_write));
      CHECK_U64("TEST AH sign/odd parity", ctx.EFlags & 0xCC5, 0x480); }
    { const uint8_t c[] = {0x38, 0x08};
      ctx.Rcx = 1; ctx.EFlags = 0x400;
      memset(&dev, 0, sizeof dev); dev.next_read = 0x80; ctx.Rip = (uintptr_t)c;
      CHECK("CMP overflow handled", mmio_emulate(&ctx, 0x54, NULL, t_read, t_write));
      CHECK_U64("CMP signed overflow and auxiliary carry", ctx.EFlags & 0xCD5, 0xC10); }
}

int main(void)
{
    check_snapshot_bounds();
    check_invalid_forms();
    check_registers_and_flags();
    check_compare_widths();
    CONTEXT ctx;
    printf("mmio_decode: running\n");
    memset(&ctx, 0, sizeof ctx);

    /* mov [rax], ecx  -- 89 08 */
    { const uint8_t c[] = { 0x89, 0x08 };
      ctx.Rcx = 0xDEADBEEF;
      CHECK("mov r/m,r handled", run(c, sizeof c, &ctx));
      CHECK_U64("mov r/m,r value", dev.val, 0xDEADBEEF);
      CHECK_U64("mov r/m,r size",  dev.size, 4);
      CHECK_U64("mov r/m,r offset", dev.off, 0x54); }

    /* mov [rax], cl   -- 88 08, byte sized */
    { const uint8_t c[] = { 0x88, 0x08 };
      ctx.Rcx = 0xAA;
      CHECK("mov r/m8,r8 handled", run(c, sizeof c, &ctx));
      CHECK_U64("mov r/m8 size", dev.size, 1); }

    /* mov ecx, [rax]  -- 8B 08. A 32-bit read must clear the high half; if it
     * does not, a register holding a stale 64-bit value reads back wrong. */
    { const uint8_t c[] = { 0x8B, 0x08 };
      ctx.Rcx = 0xFFFFFFFFFFFFFFFFULL;
      memset(&dev, 0, sizeof dev);
      ctx.Rip = (uint64_t)(uintptr_t)c;
      dev.next_read = 0x12345678;
      CHECK("mov r,r/m handled", mmio_emulate(&ctx, 0x54, NULL, t_read, t_write));
      CHECK_U64("32-bit read clears high half", ctx.Rcx, 0x12345678ULL); }

    /* mov dword [rax], imm32 -- C7 00 EF BE AD DE */
    { const uint8_t c[] = { 0xC7, 0x00, 0xEF, 0xBE, 0xAD, 0xDE };
      CHECK("mov r/m,imm32 handled", run(c, sizeof c, &ctx));
      CHECK_U64("imm32 value", dev.val, 0xDEADBEEF); }

    /* movzx eax, byte [rax] -- 0F B6 00 */
    { const uint8_t c[] = { 0x0F, 0xB6, 0x00 };
      memset(&dev, 0, sizeof dev);
      ctx.Rip = (uint64_t)(uintptr_t)c;
      ctx.Rax = 0xFFFFFFFFFFFFFFFFULL;
      dev.next_read = 0x00000091;
      CHECK("movzx r32,r/m8 handled",
            mmio_emulate(&ctx, 0x54, NULL, t_read, t_write));
      CHECK_U64("movzx zero-extends", ctx.Rax, 0x91ULL); }

    /* test [rax], ecx -- 85 08. The whole point is the flags: a poll loop
     * spins on jz/jnz off this. */
    { const uint8_t c[] = { 0x85, 0x08 };
      memset(&dev, 0, sizeof dev);
      ctx.Rip = (uint64_t)(uintptr_t)c;
      ctx.Rcx = 0x00000001; ctx.EFlags = 0;
      dev.next_read = 0x00000001;
      mmio_emulate(&ctx, 0x54, NULL, t_read, t_write);
      CHECK("test bit set -> ZF clear", (ctx.EFlags & 0x40) == 0);

      memset(&dev, 0, sizeof dev);
      ctx.Rip = (uint64_t)(uintptr_t)c;
      dev.next_read = 0x00000002;
      mmio_emulate(&ctx, 0x54, NULL, t_read, t_write);
      CHECK("test bit clear -> ZF set", (ctx.EFlags & 0x40) != 0); }

    /* cmp [rax], ecx -- 39 08. Equal sets ZF; below sets CF. */
    { const uint8_t c[] = { 0x39, 0x08 };
      memset(&dev, 0, sizeof dev);
      ctx.Rip = (uint64_t)(uintptr_t)c;
      ctx.Rcx = 0x10; dev.next_read = 0x10; ctx.EFlags = 0;
      mmio_emulate(&ctx, 0x54, NULL, t_read, t_write);
      CHECK("cmp equal -> ZF", (ctx.EFlags & 0x40) != 0);
      CHECK("cmp equal -> no CF", (ctx.EFlags & 0x01) == 0);

      memset(&dev, 0, sizeof dev);
      ctx.Rip = (uint64_t)(uintptr_t)c;
      ctx.Rcx = 0x20; dev.next_read = 0x10;
      mmio_emulate(&ctx, 0x54, NULL, t_read, t_write);
      CHECK("cmp below -> CF", (ctx.EFlags & 0x01) != 0); }

    /* or [rax], ecx -- 09 08, and and [rax], ecx -- 21 08. Both are
     * read-modify-write: a device that only sees the write loses the bits it
     * already had. */
    { const uint8_t c[] = { 0x09, 0x08 };
      memset(&dev, 0, sizeof dev);
      ctx.Rip = (uint64_t)(uintptr_t)c;
      ctx.Rcx = 0x0F; dev.next_read = 0xF0;
      mmio_emulate(&ctx, 0x54, NULL, t_read, t_write);
      CHECK_U64("or merges existing bits", dev.val, 0xFF);
      CHECK("or reads before writing", dev.reads == 1 && dev.writes == 1); }

    { const uint8_t c[] = { 0x21, 0x08 };
      memset(&dev, 0, sizeof dev);
      ctx.Rip = (uint64_t)(uintptr_t)c;
      ctx.Rcx = 0x0F; dev.next_read = 0xFF;
      mmio_emulate(&ctx, 0x54, NULL, t_read, t_write);
      CHECK_U64("and masks existing bits", dev.val, 0x0F); }

    /* 66 prefix selects 16-bit, REX.W selects 64-bit. */
    { const uint8_t c[] = { 0x66, 0x89, 0x08 };
      ctx.Rcx = 0x1234;
      CHECK("66-prefixed handled", run(c, sizeof c, &ctx));
      CHECK_U64("66 prefix -> 2 bytes", dev.size, 2); }

    { const uint8_t c[] = { 0x48, 0x89, 0x08 };
      CHECK("REX.W handled", run(c, sizeof c, &ctx));
      CHECK_U64("REX.W -> 8 bytes", dev.size, 8); }

    /* disp8 and disp32 forms have to add to the length, or Rip lands inside
     * the instruction and the next decode is garbage.
     * mov [rax+0x10], ecx -- 89 48 10        (mod=01, disp8)
     * mov [rax+0x1000], ecx -- 89 88 00 10 00 00  (mod=10, disp32) */
    { const uint8_t c1[] = { 0x89, 0x48, 0x10 };
      CHECK("disp8 handled", run(c1, sizeof c1, &ctx)); }
    { const uint8_t c2[] = { 0x89, 0x88, 0x00, 0x10, 0x00, 0x00 };
      CHECK("disp32 handled", run(c2, sizeof c2, &ctx)); }
    /* SIB: mov [rax+rbx*1], ecx -- 89 0C 18 */
    { const uint8_t c3[] = { 0x89, 0x0C, 0x18 };
      CHECK("SIB handled", run(c3, sizeof c3, &ctx)); }

    /* A no-base SIB consumes disp32 even with REX.B; C7's immediate follows it. */
    { const uint8_t c[] = {0x89, 0x0C, 0x25, 0x00, 0x10, 0x00, 0x00};
      CHECK("SIB no-base disp32", run(c, sizeof c, &ctx)); }
    { const uint8_t c[] = {0x41, 0x89, 0x0C, 0x25, 0x00, 0x10, 0x00, 0x00};
      CHECK("REX.B SIB no-base disp32", run(c, sizeof c, &ctx)); }
    { const uint8_t c[] = {0x66, 0xC7, 0x00, 0x34, 0x12, 0xAA, 0xBB};
      CHECK("C7 word immediate", run(c, 5, &ctx));
      CHECK_U64("C7 word value", dev.val, 0x1234); }
    { const uint8_t c[] = {0x66, 0x48, 0xC7, 0x00, 0x00, 0x00, 0x00, 0x80};
      CHECK("C7 REX.W immediate", run(c, sizeof c, &ctx));
      CHECK_U64("REX.W takes precedence", dev.size, 8);
      CHECK_U64("C7 sign extends imm32", dev.val, 0xFFFFFFFF80000000ull); }

    { const uint8_t invalid[][3] = {{0xC7, 0x08, 0}, {0xC6, 0xC0, 0}};
      for (unsigned i = 0; i < 2; i++) {
        ctx.Rip = (uint64_t)(uintptr_t)invalid[i];
        memset(&dev, 0, sizeof dev);
        CHECK("invalid immediate form refused", !mmio_emulate(&ctx, 0x54, NULL, t_read, t_write));
        CHECK("invalid form has no device access", dev.reads == 0 && dev.writes == 0);
        CHECK_U64("invalid form preserves RIP", ctx.Rip, (uintptr_t)invalid[i]);
      } }

    /* And the one that must fail. An unknown opcode reported as handled steps
     * over an instruction nobody decoded. */
    { const uint8_t c[] = { 0xF7, 0x00, 0x01, 0x00, 0x00, 0x00 };  /* test imm32 */
      memset(&dev, 0, sizeof dev);
      ctx.Rip = (uint64_t)(uintptr_t)c;
      CHECK("unknown opcode refused",
            mmio_emulate(&ctx, 0x54, NULL, t_read, t_write) == 0);
      CHECK("refused leaves Rip alone", ctx.Rip == (uint64_t)(uintptr_t)c); }

    if (failures == 0) {
        printf("mmio_decode: ALL PASS\n");
        return 0;
    }
    printf("mmio_decode: %d FAILURE(S)\n", failures);
    return 1;
}
