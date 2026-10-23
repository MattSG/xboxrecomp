/**
 * Function tracing for recompiled code.
 *
 * Generic function-entry/exit, stack-pointer and guest debug diagnostics.
 *
 * Output goes to stderr, unbuffered, because the question tracing answers is
 * usually "what was the last thing that happened before it died".
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#if !defined(_WIN32)
#include "platform/win32_compat.h"
#endif

#include "xbox_memory_layout.h"

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;

/* ponytail: one diagnostic-state lock; shard only if trace contention matters. */
static SRWLOCK g_trace_lock = SRWLOCK_INIT;

/* A run that recurses produces trace lines without limit, and the useful
 * window is rarely the first few thousand. The budget stops a diagnostic from
 * filling a disk, and is deliberately generous: a budget that runs out before
 * the interesting part turns "no trace here" into a false negative, which is
 * worse than a large file. Override with RECOMP_TRACE_BUDGET. */
static long trace_budget(void)
{
    static long budget = -1;
    if (budget < 0) {
        const char *env = getenv("RECOMP_TRACE_BUDGET");
        budget = env ? strtol(env, NULL, 0) : 400000;
        if (budget < 0) budget = 0;
    }
    return budget > 0 ? budget-- : 0;
}

/* Where a title spends its calls.
 *
 * A recompiled title that is CPU-bound gives no clue which guest code is
 * responsible: the native profile is a wall of sub_XXXX, and the guest has no
 * program counter to sample. Counting entries does answer it, and the trace
 * hook is already on every function the run was generated to trace -- so
 * tracing everything and tallying instead of printing turns the existing
 * mechanism into a profile for the cost of an array increment.
 *
 * Counts, not time: a function called once that loops for a minute does not
 * appear here, and one called ten million times cheaply does. It says where
 * the calls go, which is the first question, not the last.
 *
 * Enable with RECOMP_TRACE_PROFILE=1. The report goes to stderr at exit,
 * hottest first.
 */
#define PROF_SLOTS 8192                 /* open addressing, power of two */

static struct { uint32_t va; unsigned long long hits; } g_prof[PROF_SLOTS];
static const char *g_prof_name[PROF_SLOTS];
static int g_prof_used, g_prof_full;
static unsigned long long g_prof_calls;

static void prof_report_locked(void)
{
    int taken[40], ntaken = 0;
    int i, j, shown;

    if (!g_prof_used)
        return;
    fprintf(stderr, "\n[PROFILE] %d functions entered%s, hottest first:\n",
            g_prof_used, g_prof_full ? " (table full, some dropped)" : "");
    for (shown = 0; shown < 40; shown++) {
        int best = -1;
        for (i = 0; i < PROF_SLOTS; i++) {
            if (!g_prof[i].hits)
                continue;
            for (j = 0; j < ntaken; j++)
                if (taken[j] == i)
                    break;
            if (j < ntaken)
                continue;
            if (best < 0 || g_prof[i].hits > g_prof[best].hits)
                best = i;
        }
        if (best < 0)
            break;
        taken[ntaken++] = best;
        fprintf(stderr, "  %14llu  %s (0x%08X)\n",
                g_prof[best].hits, g_prof_name[best] ? g_prof_name[best] : "?",
                g_prof[best].va);
    }
    fflush(stderr);
}

static void prof_report(void)
{
    AcquireSRWLockExclusive(&g_trace_lock);
    prof_report_locked();
    ReleaseSRWLockExclusive(&g_trace_lock);
}

/* RECOMP_TRACE_PROFILE=1 profiles; a larger number is also how often to
 * report, in calls. The default suits a title burning a core in a spin loop;
 * a title that no longer has one may never reach it, and then the only report
 * is the one at exit -- which a killed run never gets. */
static unsigned long long prof_interval(void)
{
    static unsigned long long every;
    if (!every) {
        const char *v = getenv("RECOMP_TRACE_PROFILE");
        unsigned long long n = v ? strtoull(v, NULL, 0) : 0;
        every = n > 1 ? n : 20000000ull;
    }
    return every;
}

static int prof_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("RECOMP_TRACE_PROFILE") ? 1 : 0;
        if (on)
            atexit(prof_report);
    }
    return on;
}

static void prof_count(const char *name, uint32_t va)
{
    unsigned i = (va * 2654435761u) & (PROF_SLOTS - 1);
    unsigned n;

    /* A title being profiled for a hang or a slowdown is a title that gets
     * killed rather than exited, and a kill does not reach atexit. Report as
     * it goes, so there is always a recent one. */
    if (++g_prof_calls % prof_interval() == 0)
        prof_report_locked();

    for (n = 0; n < PROF_SLOTS; n++) {
        unsigned k = (i + n) & (PROF_SLOTS - 1);
        if (g_prof[k].va == va && g_prof[k].hits) { g_prof[k].hits++; return; }
        if (!g_prof[k].hits) {
            g_prof[k].va = va;
            g_prof[k].hits = 1;
            /* Own the name until process exit, like the profile table itself. */
            if (name) {
                size_t bytes = strlen(name) + 1;
                char *copy = malloc(bytes);
                if (copy) memcpy(copy, name, bytes);
                g_prof_name[k] = copy;
            }
            g_prof_used++;
            return;
        }
    }
    g_prof_full = 1;
}

static int trace_should_log(const char *name, uint32_t va, int entry)
{
    int log = 0;
    AcquireSRWLockExclusive(&g_trace_lock);
    if (prof_enabled()) {
        if (entry) prof_count(name, va);
    } else {
        log = trace_budget() != 0;
    }
    ReleaseSRWLockExclusive(&g_trace_lock);
    return log;
}

/* Diagnostics must not fault on the guest memory they are diagnosing. */
static int trace_read(uint64_t va, void *out, size_t bytes)
{
    SIZE_T copied = 0;
    DWORD error = GetLastError();
    BOOL ok;
    if (!va || va > UINT32_MAX || bytes > 0x100000000ull - va) return 0;
    ok = ReadProcessMemory(GetCurrentProcess(),
        (const void *)((uintptr_t)xbox_GetMemoryOffset() + (uintptr_t)va),
        out, bytes, &copied);
    SetLastError(error);
    return ok && copied == bytes;
}

static void trace_dump(const char *label, uint32_t va, int count)
{
    uint32_t words[24];
    int i;
    fprintf(stderr, "         %s -> [%08X]:", label, va);
    if (!trace_read(va, words, (size_t)count * sizeof(words[0]))) {
        fprintf(stderr, " <unreadable>\n");
        return;
    }
    for (i = 0; i < count; i++) fprintf(stderr, " %08X", words[i]);
    fprintf(stderr, "\n");
}

void recomp_trace_enter(const char *name, uint32_t va)
{
    if (!trace_should_log(name, va, 1)) return;
    uint32_t from = 0, args[32];
    int readable = trace_read(g_esp, &from, sizeof(from));
    fprintf(stderr, "[TRACE] -> %s (0x%08X)  from=%08X%s esp=%08X eax=%08X "
            "ecx=%08X esi=%08X edi=%08X ebx=%08X\n",
            name, va, from, readable ? "" : "<unreadable>",
            g_esp, g_eax, g_ecx, g_esi, g_edi, g_ebx);
    if (getenv("RECOMP_TRACE_ARGS")) {
        int n = atoi(getenv("RECOMP_TRACE_ARGS")), i;
        if (n <= 0 || n > 32) n = 8;
        readable = trace_read((uint64_t)g_esp + 4, args, (size_t)n * sizeof(args[0]));
        fprintf(stderr, "         args:");
        if (!readable) fprintf(stderr, " <unreadable>");
        else for (i = 0; i < n; i++) fprintf(stderr, " %d=%08X", i + 1, args[i]);
        fprintf(stderr, "\n");
        if (readable && getenv("RECOMP_TRACE_DEREF")) {
            for (i = 0; i < n; i++) {
                char label[16];
                if (!args[i]) continue;
                snprintf(label, sizeof(label), "arg%d", i + 1);
                trace_dump(label, args[i], 12);
            }
        }
        if (g_eax) trace_dump("eax", g_eax, 24);
    }
    fflush(stderr);
}

/* Entry values answer "what was it called with"; only exit values answer "what
 * did the caller get back", which is the question when a callee-saved register
 * comes back wrong. */
void recomp_trace_exit(const char *name, uint32_t va)
{
    if (!trace_should_log(name, va, 0)) return;
    fprintf(stderr, "[TRACE] <- %s (0x%08X)  esp=%08X eax=%08X ecx=%08X "
            "esi=%08X edi=%08X ebx=%08X\n",
            name, va, g_esp, g_eax, g_ecx, g_esi, g_edi, g_ebx);
    fflush(stderr);
}

/* esp at a specific point inside a traced function. The epilogue's
 * `mov esp, ebp` hides drift from any return-time sample, so a leak of a few
 * bytes per call is only visible from a sample taken before it. */
void recomp_trace_esp(const char *name, const char *tag)
{
    if (!trace_should_log(name, 0, 0)) return;
    fprintf(stderr, "[ESP] %s @%s  esp=%08X esi=%08X edi=%08X\n",
            name, tag, g_esp, g_esi, g_edi);
    fflush(stderr);
}

/* ---------------------------------------------------------------------------
 * Guest debug output (INT 2D / DebugService).
 *
 * The Xbox kernel debug trap. eax selects the service and ecx carries its
 * argument; service 1 is "print this ANSI_STRING", which is what
 * OutputDebugStringA and the XDK's DbgPrint compile down to. On hardware the
 * kernel consumes the trap and resumes at the int3 that follows, skipping it.
 *
 * Printing it is the whole point: this is the title telling us what it thinks
 * is happening, and during bring-up that is the most valuable output there is.
 * ------------------------------------------------------------------------- */

void recomp_debug_service(uint32_t service, uint32_t arg_va)
{
    uint8_t header[8];
    uint16_t length;
    uint32_t buffer_va;
    char *text;
    if (service != 1) {
        fprintf(stderr, "[GUEST] DebugService %u (arg 0x%08X), ignored\n",
                (unsigned)service, arg_va);
        fflush(stderr);
        return;
    }
    if (!trace_read(arg_va, header, sizeof(header))) return;
    memcpy(&length, header, sizeof(length));
    memcpy(&buffer_va, header + 4, sizeof(buffer_va));
    if (!buffer_va || !length) return;
    text = malloc(length);
    if (!text) return;
    if (trace_read(buffer_va, text, length)) {
        fprintf(stderr, "[GUEST] %.*s", (int)length, text);
        if (text[length - 1] != '\n') fputc('\n', stderr);
        fflush(stderr);
    }
    free(text);
}
