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
static struct { LONGLONG t; DWORD64 ip, caller[16]; } s_ring[RING];
static DWORD64 s_img_lo, s_img_hi;

/* A sample in ntdll (a wait, a lock) names nothing useful: also keep the
 * first frames inside the executable, by unwinding the suspended stack --
 * the kernel shim first, then the guest function that called it. */
static void exe_callers(const CONTEXT *c0, DWORD64 out[16])
{
    CONTEXT c = *c0;
    int depth, n = 0;
    for (depth = 0; depth < 96 && n < 16; depth++) {
        DWORD64 base = 0;
        PRUNTIME_FUNCTION f;
        PVOID handler;
        DWORD64 frame;
        if (c.Rip >= s_img_lo && c.Rip < s_img_hi && depth)
            out[n++] = c.Rip;
        f = RtlLookupFunctionEntry(c.Rip, &base, NULL);
        if (!f) {                                 /* leaf: return address on top */
            if (!c.Rsp) break;
            c.Rip = *(DWORD64 *)c.Rsp;
            c.Rsp += 8;
        } else {
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, f, &c, &handler, &frame, NULL);
        }
        if (!c.Rip) break;
    }
}
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
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext(s_thread, &c)) {
            LONG h = s_head;
            QueryPerformanceCounter(&t);
            s_ring[h % RING].t = t.QuadPart;
            s_ring[h % RING].ip = c.Rip;
            memset(s_ring[h % RING].caller, 0, sizeof s_ring[h % RING].caller);
            exe_callers(&c, s_ring[h % RING].caller);
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
    {
        HMODULE m = GetModuleHandleW(NULL);
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)m + ((IMAGE_DOS_HEADER *)m)->e_lfanew);
        s_img_lo = (DWORD64)m;
        s_img_hi = s_img_lo + nt->OptionalHeader.SizeOfImage;
    }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    s_sym_ok = SymInitialize(GetCurrentProcess(), NULL, TRUE);
    t = CreateThread(NULL, 0, sampler, NULL, 0, NULL);
    if (t) { SetThreadPriority(t, THREAD_PRIORITY_HIGHEST); CloseHandle(t); }
    fprintf(stderr, "[SPIKE] sampling the game thread for spike reports\n");
}

/* The functions the watched thread was in over the last `ms` milliseconds. */
void recomp_sampler_report(double ms)
{
    static struct { char name[400]; int n; } top[64], chains[64];
    int nc = 0;
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
        if (s_ring[i % RING].caller[0] &&
            (s_ring[i % RING].ip < s_img_lo || s_ring[i % RING].ip >= s_img_hi)) {  /* outside the exe: say from where */
            static char both[160];
            char leafbuf[64], from[96] = "";
            int c;
            snprintf(leafbuf, sizeof leafbuf, "%s", name);
            for (c = 0; c < 8 && s_ring[i % RING].caller[c]; c++) {
                memset(buf, 0, sizeof buf);
                si->SizeOfStruct = sizeof(SYMBOL_INFO);
                si->MaxNameLen = 127;
                if (!SymFromAddr(GetCurrentProcess(), s_ring[i % RING].caller[c], &disp, si))
                    continue;
                if (!from[0] || !strncmp(si->Name, "sub_", 4))
                    snprintf(from, sizeof from, "%s", si->Name);
                if (!strncmp(si->Name, "sub_", 4))
                    break;                         /* the guest function */
            }
            if (from[0]) {
                snprintf(both, sizeof both, "%s<-%s", leafbuf, from);
                name = both;
            }
        }
        {   /* the guest call chain: leaf, then each sub_ caller */
            char chain[400];
            int c, len;
            len = snprintf(chain, sizeof chain, "%s", name);
            for (c = 0; c < 16 && s_ring[i % RING].caller[c] && len < (int)sizeof chain - 20; c++) {
                memset(buf, 0, sizeof buf);
                si->SizeOfStruct = sizeof(SYMBOL_INFO);
                si->MaxNameLen = 127;
                if (SymFromAddr(GetCurrentProcess(), s_ring[i % RING].caller[c], &disp, si) &&
                    !strncmp(si->Name, "sub_", 4) && !strstr(chain, si->Name))
                    len += snprintf(chain + len, sizeof chain - len, "<-%s", si->Name + 4);
            }
            for (k = 0; k < nc; k++)
                if (!strcmp(chains[k].name, chain)) { chains[k].n++; break; }
            if (k == nc && nc < 64) {
                snprintf(chains[nc].name, sizeof chains[nc].name, "%s", chain);
                chains[nc++].n = 1;
            }
        }
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
    for (k = 0; k < 2; k++) {
        int j;
        best = -1;
        for (j = 0; j < nc; j++)
            if (chains[j].n && (best < 0 || chains[j].n > chains[best].n)) best = j;
        if (best < 0) break;
        fprintf(stderr, "[SPIKE]   chain %d%%: %s\n", chains[best].n * 100 / total, chains[best].name);
        chains[best].n = 0;
    }
}
#else
void recomp_sampler_watch_current_thread(void) {}
void recomp_sampler_report(double ms) { (void)ms; }
#endif
