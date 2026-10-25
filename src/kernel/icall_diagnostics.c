/* Diagnostics for indirect calls the dispatcher cannot resolve.
 *
 * The runtime declares recomp_icall_fail_log and recomp_icall_not_code_log and
 * leaves their bodies to the title. This file supplies the generic versions:
 * the first 50 reports of each kind print in full, later ones are counted and
 * summarized at exit and every 1024 events, so a hot loop stays bounded. Titles
 * observe the non-code reports through xbox_icall_set_report_hook. */
#include "icall_diagnostics.h"
#include "xbox_memory_layout.h"
#include "platform/xbox_winnt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_ebx, g_esp, g_esi, g_edi, g_ebp, g_seh_ebp;
extern RECOMP_TLS int g_df;

static volatile LONG s_trace_all;
static void (*s_report_hook)(const XboxIcallReport *report);

/* Past the 50-line cap the loggers only count. 0: failed resolve, 1: non-code. */
static volatile LONG icall_suppressed[2];
static volatile LONG icall_summary_armed;

static void icall_summary(void)
{
    fprintf(stderr, "[ICALL] suppressed after the 50-line cap: failed-resolve=%ld non-code=%ld\n",
            (long)icall_suppressed[0], (long)icall_suppressed[1]);
    fflush(stderr);
}

static void icall_count_suppressed(int kind)
{
    LONG count = InterlockedIncrement(&icall_suppressed[kind]);
    if (InterlockedCompareExchange(&icall_summary_armed, 1, 0) == 0)
        atexit(icall_summary);
    if (count % 1024 == 0)
        icall_summary();
}

void xbox_icall_set_trace_all(int enabled)
{
    InterlockedExchange(&s_trace_all, enabled != 0);
}

void xbox_icall_set_report_hook(void (*hook)(const XboxIcallReport *report))
{
    s_report_hook = hook;
}

int xbox_icall_read(uint32_t va, void *out, size_t size)
{
    uintptr_t address, end, cursor;
    SIZE_T copied = 0;
    if (!size || size > 128 || va < 0x10000u ||
        va >= 0x08000000u || size > 0x08000000u - va) return 0;
    address = (uintptr_t)va + (uintptr_t)xbox_GetMemoryOffset();
    end = address + size;
    if (end < address) return 0;
    for (cursor = address; cursor < end;) {
        MEMORY_BASIC_INFORMATION info;
        uintptr_t next;
        if (!VirtualQuery((void *)cursor, &info, sizeof info) ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return 0;
        next = (uintptr_t)info.BaseAddress + info.RegionSize;
        if (next <= cursor) return 0;
        cursor = next;
    }
    /* A concurrent guest free can invalidate a page after VirtualQuery.
     * ReadProcessMemory reports that race instead of faulting in diagnostics. */
    return ReadProcessMemory(GetCurrentProcess(), (const void *)address, out,
                             size, &copied) && copied == size;
}

void recomp_icall_fail_log(uint32_t xbox_va)
{
    static volatile LONG printed;
    uint32_t caller = 0;
    /* A broken guest callback can recur in a polling loop. Keep the first
     * failures useful without turning a fault into unbounded synchronous I/O.
     * Dispatch and fallback behavior are unchanged. */
    if (!InterlockedCompareExchange(&s_trace_all, 0, 0) &&
        (printed >= 50 || InterlockedIncrement(&printed) > 50)) {
        icall_count_suppressed(0);
        return;
    }
    if (g_esp >= 0x00010000u && g_esp < 0x04000000u)
        caller = *(const uint32_t *)((uintptr_t)g_esp + (uintptr_t)xbox_GetMemoryOffset());
    fprintf(stderr, "[ICALL] Failed resolve VA 0x%08X caller=0x%08X\n", xbox_va, caller);
}

/* Decode the original near indirect CALL ending at its return address. A
 * target-value scan cannot identify a null vtable slot: every zero in an
 * unrelated callback table matches it. The common FF /2 addressing forms
 * below include register, absolute, displacement and SIB operands.
 * ESP at operand evaluation precedes the pushed return address by four bytes. */
static unsigned icall_decode(uint32_t caller, uint32_t target,
                             uint32_t *slot, unsigned *base)
{
    uint32_t regs[8] = {g_eax, g_ecx, g_edx, g_ebx, g_esp + 4u,
                        g_ebp, g_esi, g_edi};
    unsigned length;
    *slot = 0; *base = 9;
    if (caller < g_xbox_code_lo + 2u || caller > g_xbox_code_hi) return 0;
    for (length = 7; length >= 2; --length) {
        unsigned char bytes[7];
        unsigned mod, rm, used = 2, reg;
        uint32_t address = 0, observed;
        int32_t displacement = 0;
        if (caller - g_xbox_code_lo < length ||
            !xbox_icall_read(caller - length, bytes, length) || bytes[0] != 0xFF ||
            ((bytes[1] >> 3) & 7u) != 2u) continue;
        mod = bytes[1] >> 6; rm = bytes[1] & 7u;
        if (mod == 3) {
            if (length == 2 && regs[rm] == target) { *base = rm; return 2; }
            continue;
        }
        reg = rm;
        if (rm == 4) {
            unsigned sib, index;
            if (used == length) continue;
            sib = bytes[used++]; reg = sib & 7u; index = (sib >> 3) & 7u;
            if (index != 4) address = regs[index] << (sib >> 6);
        }
        if (mod == 0 && reg == 5) {
            reg = 8; /* Absolute addressing, including a SIB without a base. */
        } else {
            address += regs[reg];
        }
        if (mod == 1) {
            if (used + 1 != length) continue;
            displacement = (int8_t)bytes[used++];
        } else if (mod == 2 || reg == 8) {
            if (used + 4 != length) continue;
            memcpy(&displacement, bytes + used, 4); used += 4;
        }
        if (used != length) continue;
        address += (uint32_t)displacement;
        if (xbox_icall_read(address, &observed, 4) && observed != target) continue;
        *slot = address; *base = reg;
        return 1;
    }
    return 0;
}

void recomp_icall_not_code_log(uint32_t xbox_va)
{
    static volatile LONG printed;
    static volatile LONG sequence;
    XboxIcallReport report;
    uint32_t slot = 0;
    uint32_t slot_value = 0, object_words[8] = {0};
    uint32_t frame_words[16] = {0};
    uint32_t frame = g_ebp >= 0x10020u ? g_ebp - 0x20u : 0;
    unsigned base = 9, kind, slot_read, object_read, frame_read;
    unsigned long long tick;
    static const char *const names[] = {
        "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "absolute", "unknown"};
    /* Broken callbacks can recur every frame. Keep a bounded diagnostic,
     * with no decoding, pointer reads or synchronous I/O after the limit. */
    if (!InterlockedCompareExchange(&s_trace_all, 0, 0) &&
        (printed >= 50 || InterlockedIncrement(&printed) > 50)) {
        icall_count_suppressed(1);
        return;
    }
    memset(&report, 0, sizeof report);
    report.sample = (unsigned)InterlockedIncrement(&sequence);
    report.target = xbox_va;
    report.esp = g_esp;
    tick = (unsigned long long)GetTickCount64();
    xbox_icall_read(g_esp, &report.caller, 4);
    report.stack_read = xbox_icall_read(g_esp, report.stack_words, sizeof report.stack_words);
    kind = icall_decode(report.caller, xbox_va, &slot, &base);
    slot_read = kind == 1 && xbox_icall_read(slot, &slot_value, 4);
    object_read = xbox_icall_read(g_ecx, object_words, sizeof object_words);
    frame_read = xbox_icall_read(frame, frame_words, sizeof frame_words);
    fprintf(stderr, "[ICALL] non-code target 0x%08X slot=0x%08X caller=0x%08X esp=0x%08X"
            " kind=%s base=%s slot_read=%u slot_value=%08X"
            " eax=%08X ecx=%08X edx=%08X esi=%08X edi=%08X object_read=%u"
            " object_words=%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X"
            " sample=%u tick_ms=%llu\n",
            xbox_va, slot, report.caller, g_esp,
            kind == 1 ? "memory" : kind == 2 ? "register" : "unknown", names[base],
            slot_read, slot_value, g_eax, g_ecx, g_edx, g_esi, g_edi, object_read,
            object_words[0], object_words[1], object_words[2], object_words[3],
            object_words[4], object_words[5], object_words[6], object_words[7],
            report.sample, tick);
    fprintf(stderr, "[ICALL_STATE] sample=%u tick_ms=%llu ebx=%08X ebp=%08X seh_ebp=%08X df=%d"
            " stack_base=%08X stack_read=%u stack_words=", report.sample, tick,
            g_ebx, g_ebp, g_seh_ebp, g_df, g_esp, report.stack_read);
    {
        unsigned i;
        for (i = 0; i < 16; ++i)
            fprintf(stderr, "%s%08X", i ? "," : "", report.stack_words[i]);
    }
    fprintf(stderr, " frame_base=%08X frame_read=%u frame_words=", frame, frame_read);
    {
        unsigned i;
        for (i = 0; i < 16; ++i)
            fprintf(stderr, "%s%08X", i ? "," : "", frame_words[i]);
    }
    fprintf(stderr, "\n");
    fflush(stderr);
    if (s_report_hook)
        s_report_hook(&report);
}
