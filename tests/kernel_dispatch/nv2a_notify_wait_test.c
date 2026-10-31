/* Exercise the exact host wait helper without GPU devices or game assets. */
#include "kernel/nv2a_notify_wait.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    XBOX_NV2A_WAKE wake;
    volatile uint32_t get, put;
    volatile LONG notifications, stop, ready_count, wait_calls;
    HANDLE started, waiting;
    int inject_completion;
} FIXTURE;
static FIXTURE *active_fixture;
static BOOL (WINAPI *real_wait)(volatile VOID *, PVOID, SIZE_T, DWORD);
static XBOX_THREAD_LOCAL int t_wait_reported;
static volatile LONG bad_timeout, missed_wake;

static BOOL WINAPI observed_wait(volatile VOID *address, PVOID observed,
                                 SIZE_T size, DWORD timeout)
{
    FIXTURE *f = active_fixture;
    if (size != sizeof(LONG) || timeout != 1) InterlockedExchange(&bad_timeout, 1);
    InterlockedIncrement(&f->wait_calls);
    if (!t_wait_reported) {
        t_wait_reported = 1;
        InterlockedIncrement(&f->ready_count);
        SetEvent(f->waiting);
    }
    /* This hook is entered AFTER the helper's second predicate check but
     * BEFORE the native atomic wait. Its wake happens before any waiter can
     * be registered, so an event-only implementation would lose it. */
    if (f->inject_completion) {
        f->inject_completion = 0;
        f->get = f->put;
        xbox_nv2a_wake_publish(&f->wake);
        if (!real_wait(address, observed, size, timeout))
            InterlockedExchange(&missed_wake, 1);
        return TRUE;
    }
    return real_wait(address, observed, size, timeout);
}

static int init_fixture(FIXTURE *f)
{
    memset(f, 0, sizeof *f);
    xbox_nv2a_wake_init(&f->wake);
    if (!f->wake.wait_address || !f->wake.wake_all) return 0;
    real_wait = f->wake.wait_address;
    f->wake.wait_address = observed_wait;
    f->put = 1;
    f->started = CreateEventW(NULL, TRUE, FALSE, NULL);
    f->waiting = CreateEventW(NULL, TRUE, FALSE, NULL);
    active_fixture = f;
    t_wait_reported = 0;
    return f->started && f->waiting;
}

static void close_fixture(FIXTURE *f)
{
    CloseHandle(f->started);
    CloseHandle(f->waiting);
}

static DWORD WINAPI waiter(LPVOID parameter)
{
    FIXTURE *f = parameter;
    SetEvent(f->started);
    xbox_nv2a_notify_wait(&f->wake, &f->get, &f->put,
                         &f->notifications, 0, &f->stop);
    return 0;
}

static int finish(HANDLE thread)
{
    if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0) {
        fprintf(stderr, "FAIL: guest wait did not complete\n");
        /* Fixtures live on the calling stack; never return with a live user. */
        ExitProcess(20);
    }
    CloseHandle(thread);
    return !bad_timeout && !missed_wake;
}

static int completion_case(unsigned kind)
{
    FIXTURE f;
    HANDLE thread;
    if (!init_fixture(&f)) return 0;
    if (kind == 4) { /* The host API is unavailable: retain yield fallback. */
        f.wake.wait_address = NULL;
        f.wake.wake_all = NULL;
        if (!xbox_nv2a_wake_wait(&f.wake, 0)) return 0;
    }
    thread = CreateThread(NULL, 0, waiter, &f, 0, NULL);
    if (!thread) return 0;
    if (WaitForSingleObject(kind == 4 ? f.started : f.waiting, 5000) != WAIT_OBJECT_0)
        ExitProcess(21);
    /* A host wake alone must not pretend that the GPU completed work. */
    xbox_nv2a_wake_publish(&f.wake);
    if (WaitForSingleObject(thread, 0) != WAIT_TIMEOUT) ExitProcess(22);
    if (kind == 1) InterlockedIncrement(&f.notifications);
    else if (kind == 2) InterlockedExchange(&f.stop, 1);
    else f.get = f.put;
    /* Guest-origin GET changes are found through the bounded timeout even
     * when there is no corresponding executor notification. */
    if (kind != 3) xbox_nv2a_wake_publish(&f.wake);
    int okay = finish(thread);
    close_fixture(&f);
    return okay;
}

static int multiple_waiters(void)
{
    FIXTURE f;
    HANDLE threads[4];
    DWORD started = GetTickCount();
    if (!init_fixture(&f)) return 0;
    for (unsigned i = 0; i < 4; i++) {
        threads[i] = CreateThread(NULL, 0, waiter, &f, 0, NULL);
        if (!threads[i]) ExitProcess(23);
    }
    while (InterlockedCompareExchange(&f.ready_count, 0, 0) != 4) {
        if (GetTickCount() - started > 5000) ExitProcess(24);
        Sleep(1);
    }
    f.get = f.put;
    xbox_nv2a_wake_publish(&f.wake);
    if (WaitForMultipleObjects(4, threads, TRUE, 5000) != WAIT_OBJECT_0) ExitProcess(25);
    for (unsigned i = 0; i < 4; i++) CloseHandle(threads[i]);
    close_fixture(&f);
    return !bad_timeout;
}

int main(void)
{
    FIXTURE f;
    if (!init_fixture(&f)) return 10;
    f.get = f.put; /* Completed before entering the helper: no host wait. */
    xbox_nv2a_notify_wait(&f.wake, &f.get, &f.put, &f.notifications, 0, &f.stop);
    if (f.wait_calls != 0) return 1;
    close_fixture(&f);

    if (!init_fixture(&f)) return 11;
    f.inject_completion = 1;
    xbox_nv2a_notify_wait(&f.wake, &f.get, &f.put, &f.notifications, 0, &f.stop);
    if (f.wait_calls != 1 || missed_wake || bad_timeout) return 2;
    close_fixture(&f);

    for (unsigned kind = 0; kind < 5; kind++)
        if (!completion_case(kind)) return 3 + kind;
    if (!multiple_waiters()) return 8;
    puts("PASS: early completion, check-to-wait race, GET/NOP/stop wakes, "
         "spurious wake, bounded fallback and multiple guest waiters");
    return 0;
}
