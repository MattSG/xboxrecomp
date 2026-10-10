/* Where a watched thread spends a spike (RECOMP_SPIKE_CAPTURE).
 *
 * The stage split says when the renderer is not to blame; then the time is in
 * the title's own thread, which no timer covers. A sampler thread suspends the
 * watched thread about once a millisecond and records its instruction
 * pointer; each spike prints the functions those samples fall in, named from
 * the executable's PDB -- recompiled code is sub_<guest address>, so the
 * answer is a guest function. No ETW, so no elevation needed.
 * ponytail: one watched thread; extend to a list when a second one matters. */
#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RING 16384
static struct { LONGLONG t; DWORD64 ip; } s_ring[RING];
static volatile LONG s_head;
static HANDLE s_thread;
static int s_sym_ok;

static DWORD WINAPI sampler(LPVOID unused)
{
    CONTEXT c;
    LARGE_INTEGER t;
    (void)unused;
    timeBeginPeriod(1);
    for (;;) {
        Sleep(1);
        if (SuspendThread(s_thread) == (DWORD)-1)
            continue;
        memset(&c, 0, sizeof c);
        c.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(s_thread, &c)) {
            LONG h = s_head;
            QueryPerformanceCounter(&t);
            s_ring[h % RING].t = t.QuadPart;
            s_ring[h % RING].ip = c.Rip;
            InterlockedExchange(&s_head, h + 1);
        }
        ResumeThread(s_thread);
    }
    return 0;
}

void recomp_sampler_watch_current_thread(void)
{
    static LONG once;
    HANDLE t;
    if (!getenv("RECOMP_SPIKE_CAPTURE") || InterlockedExchange(&once, 1))
        return;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_thread,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
        return;
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    s_sym_ok = SymInitialize(GetCurrentProcess(), NULL, TRUE);
    t = CreateThread(NULL, 0, sampler, NULL, 0, NULL);
    if (t) { SetThreadPriority(t, THREAD_PRIORITY_HIGHEST); CloseHandle(t); }
    fprintf(stderr, "[SPIKE] sampling the game thread for spike reports\n");
}

/* The functions the watched thread was in over the last `ms` milliseconds. */
void recomp_sampler_report(double ms)
{
    static struct { char name[64]; int n; } top[64];
    LARGE_INTEGER now, f;
    LONG h = s_head, i;
    int nt = 0, total = 0, k, best;
    char buf[sizeof(SYMBOL_INFO) + 128];
    SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
    if (!s_thread || !s_sym_ok)
        return;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&f);
    for (i = h - 1; i >= 0 && i > h - RING; i--) {
        const char *name = "?";
        DWORD64 disp = 0;
        if ((now.QuadPart - s_ring[i % RING].t) * 1000.0 / f.QuadPart > ms)
            break;
        memset(buf, 0, sizeof buf);
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 127;
        if (SymFromAddr(GetCurrentProcess(), s_ring[i % RING].ip, &disp, si))
            name = si->Name;
        total++;
        for (k = 0; k < nt; k++)
            if (!strcmp(top[k].name, name)) { top[k].n++; break; }
        if (k == nt && nt < 64) {
            snprintf(top[nt].name, sizeof top[nt].name, "%s", name);
            top[nt++].n = 1;
        }
    }
    if (!total)
        return;
    fprintf(stderr, "[SPIKE]   game thread (%d samples):", total);
    for (k = 0; k < 6; k++) {
        int j;
        best = -1;
        for (j = 0; j < nt; j++)
            if (top[j].n && (best < 0 || top[j].n > top[best].n)) best = j;
        if (best < 0) break;
        fprintf(stderr, " %s %d%%", top[best].name, top[best].n * 100 / total);
        top[best].n = 0;
    }
    fprintf(stderr, "\n");
}
#else
void recomp_sampler_watch_current_thread(void) {}
void recomp_sampler_report(double ms) { (void)ms; }
#endif
