/* Direct fence waits keep the original unsigned distance predicate. */
#include "kernel/nv2a_notify_wait.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    XBOX_NV2A_WAKE wake;
    volatile uint32_t completed;
    volatile LONG stop, wait_calls;
    HANDLE started, waiting;
    int inject;
} FIXTURE;
static FIXTURE *active;
static BOOL (WINAPI *native_wait)(volatile VOID *, PVOID, SIZE_T, DWORD);
static volatile LONG bad_timeout, missed_wake;

static BOOL WINAPI measured_wait(volatile VOID *address, PVOID compare,
                                 SIZE_T size, DWORD timeout)
{
    FIXTURE *f = active;
    if (size != sizeof(LONG) || timeout != 1) InterlockedExchange(&bad_timeout, 1);
    InterlockedIncrement(&f->wait_calls);
    SetEvent(f->waiting);
    if (f->inject) {
        f->inject = 0;
        f->completed = 1;
        xbox_nv2a_wake_publish(&f->wake);
        if (!native_wait(address, compare, size, timeout))
            InterlockedExchange(&missed_wake, 1);
        return TRUE;
    }
    return native_wait(address, compare, size, timeout);
}

static int initialize(FIXTURE *f)
{
    memset(f, 0, sizeof *f);
    f->completed = 0xFFFFFFFEu;
    xbox_nv2a_wake_init(&f->wake);
    if (!f->wake.wait_address || !f->wake.wake_all) return 0;
    native_wait = f->wake.wait_address;
    f->wake.wait_address = measured_wait;
    f->started = CreateEventW(NULL, TRUE, FALSE, NULL);
    f->waiting = CreateEventW(NULL, TRUE, FALSE, NULL);
    active = f;
    return f->started && f->waiting;
}

static void cleanup(FIXTURE *f)
{
    CloseHandle(f->started);
    CloseHandle(f->waiting);
}

static DWORD WINAPI worker(LPVOID parameter)
{
    FIXTURE *f = parameter;
    SetEvent(f->started);
    xbox_nv2a_fence_wait(&f->wake, &f->completed, 3, 2, &f->stop);
    return 0;
}

static int concurrent_completion(unsigned kind)
{
    FIXTURE f;
    HANDLE thread;
    if (!initialize(&f)) return 0;
    if (kind == 3) {
        f.wake.wait_address = NULL;
        f.wake.wake_all = NULL;
    }
    thread = CreateThread(NULL, 0, worker, &f, 0, NULL);
    if (!thread) ExitProcess(20);
    if (WaitForSingleObject(kind == 3 ? f.started : f.waiting, 5000) != WAIT_OBJECT_0)
        ExitProcess(21);
    xbox_nv2a_wake_publish(&f.wake); /* Wake cannot change completion rules. */
    if (WaitForSingleObject(thread, 0) != WAIT_TIMEOUT) ExitProcess(22);
    if (kind == 1) InterlockedExchange(&f.stop, 1);
    else f.completed = 1;
    if (kind != 2) xbox_nv2a_wake_publish(&f.wake);
    if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0) ExitProcess(23);
    CloseHandle(thread);
    if (kind == 1 && f.completed != 0xFFFFFFFEu) ExitProcess(24);
    cleanup(&f);
    return !bad_timeout && !missed_wake;
}

int main(void)
{
    static const struct { uint32_t submitted, completed, limit; int pending; } cases[] = {
        { 0x1234, 0x1200, 0x20, 1 }, { 0x1234, 0x1214, 0x20, 0 },
        { 0x1234, 0x1215, 0x20, 0 }, { 3, 0xFFFFFFFEu, 2, 1 },
        { 3, 1, 2, 0 }, { 3, 2, 2, 0 }, { 3, 3, 2, 0 },
        { 0xFFFFFFFEu, 1, 2, 1 }, { 0xFFFFFFFEu, 0xFFFFFFFCu, 2, 0 },
        { 3, 2, 0, 1 }, { 3, 3, 0, 0 }, { 3, 4, 0, 1 },
        { 3, 4, 0xFFFFFFFFu, 0 }, { 3, 0xFFFFFFFEu, 0xFFFFFFFFu, 0 }
    };
    FIXTURE f;
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        volatile uint32_t completed = cases[i].completed;
        if (xbox_nv2a_fence_pending(&completed, cases[i].submitted, cases[i].limit)
                != cases[i].pending || completed != cases[i].completed)
            return 1;
    }
    if (!initialize(&f)) return 2;
    f.completed = 1;
    xbox_nv2a_fence_wait(&f.wake, &f.completed, 3, 2, &f.stop);
    if (f.wait_calls || f.completed != 1) return 3;
    cleanup(&f);
    if (!initialize(&f)) return 4;
    f.inject = 1;
    xbox_nv2a_fence_wait(&f.wake, &f.completed, 3, 2, &f.stop);
    if (f.wait_calls != 1 || f.completed != 1 || missed_wake || bad_timeout) return 5;
    cleanup(&f);
    for (unsigned kind = 0; kind < 4; kind++)
        if (!concurrent_completion(kind)) return 6 + kind;
    puts("PASS: exact unsigned fence distances, wrap boundaries, early completion, "
         "check-to-wait race, spurious/stop wakes and bounded fallback");
    return 0;
}
