#include "win32_compat.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "wait-all line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

typedef struct wait_args { HANDLE handles[2]; DWORD result; } wait_args;
typedef struct mutex_probe { HANDLE handle; DWORD result; } mutex_probe;
typedef struct timer_wait_args {
    HANDLE timer;
    DWORD result;
    int entered;
    int done;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} timer_wait_args;

static void *wait_reversed(void *arg)
{
    wait_args *a = (wait_args *)arg;
    a->result = WaitForMultipleObjects(2, a->handles, TRUE, 2000);
    return NULL;
}

static void *probe_mutex(void *arg)
{
    mutex_probe *p = (mutex_probe *)arg;
    p->result = WaitForSingleObject(p->handle, 0);
    if (p->result == WAIT_OBJECT_0) ReleaseMutex(p->handle);
    return NULL;
}

static DWORD wait_on_timer(LPVOID arg)
{
    timer_wait_args *a = (timer_wait_args *)arg;
    pthread_mutex_lock(&a->lock);
    a->entered = 1;
    pthread_cond_broadcast(&a->cond);
    pthread_mutex_unlock(&a->lock);
    a->result = WaitForSingleObject(a->timer, 5000);
    pthread_mutex_lock(&a->lock);
    a->done = 1;
    pthread_cond_broadcast(&a->cond);
    pthread_mutex_unlock(&a->lock);
    return 0;
}

static void *wait_on_timer_thread(void *arg)
{
    (void)wait_on_timer(arg);
    return NULL;
}

static void *set_later(void *arg)
{
    HANDLE *handles = (HANDLE *)arg;
    struct timespec delay = { .tv_sec = 0, .tv_nsec = 1000000 };
    nanosleep(&delay, NULL);
    SetEvent(handles[0]);
    SetEvent(handles[1]);
    return NULL;
}

static void *service_timers_without_alerting_owner(void *arg)
{
    (void)arg;
    for (int i = 0; i < 220; i++) Sleep(1);
    return NULL;
}

static volatile LONG timer_apc_calls;
static ULONG_PTR timer_apc_argument;
static DWORD timer_apc_low;
static DWORD timer_apc_high;
static ULONGLONG timer_apc_time;

static void timer_apc(PVOID arg, DWORD low, DWORD high)
{
    timer_apc_argument = (ULONG_PTR)arg;
    timer_apc_low = low;
    timer_apc_high = high;
    timer_apc_time = ((ULONGLONG)high << 32) | low;
    InterlockedIncrement(&timer_apc_calls);
}

static volatile LONG user_apc_calls;
static void user_apc(ULONG_PTR data)
{
    timer_apc_argument = data;
    InterlockedIncrement(&user_apc_calls);
}

static DWORD arm_timer_then_exit(LPVOID arg)
{
    LARGE_INTEGER due = { .QuadPart = -200000 };
    CHECK(SetWaitableTimer((HANDLE)arg, &due, 0, timer_apc, NULL, FALSE));
    Sleep(30); /* allow expiry and APC queueing before owner rundown */
    return 0;
}

static void test_waitable_timers(void)
{
    HANDLE timer = CreateWaitableTimerW(NULL, TRUE, NULL);
    CHECK(timer != NULL);
    LARGE_INTEGER due = { .QuadPart = -10000 };
    /* A waiter must observe a rearm that moves the deadline earlier. */
    due.QuadPart = -600000000LL;
    CHECK(SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE));
    timer_wait_args timer_wait = { .timer = timer, .result = WAIT_FAILED };
    CHECK(pthread_mutex_init(&timer_wait.lock, NULL) == 0);
    CHECK(pthread_cond_init(&timer_wait.cond, NULL) == 0);
    pthread_t timer_waiter;
    CHECK(pthread_create(&timer_waiter, NULL, wait_on_timer_thread,
                         &timer_wait) == 0);
    pthread_mutex_lock(&timer_wait.lock);
    while (!timer_wait.entered)
        pthread_cond_wait(&timer_wait.cond, &timer_wait.lock);
    pthread_mutex_unlock(&timer_wait.lock);
    struct timespec settle = { .tv_sec = 0, .tv_nsec = 25000000L };
    nanosleep(&settle, NULL);
    due.QuadPart = -10000;
    CHECK(SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE));
    struct timespec rearm_timeout;
    clock_gettime(CLOCK_REALTIME, &rearm_timeout);
    rearm_timeout.tv_sec++;
    pthread_mutex_lock(&timer_wait.lock);
    int rearm_wait_result = 0;
    while (!timer_wait.done && rearm_wait_result == 0)
        rearm_wait_result = pthread_cond_timedwait(&timer_wait.cond,
                                                   &timer_wait.lock,
                                                   &rearm_timeout);
    pthread_mutex_unlock(&timer_wait.lock);
    CHECK(rearm_wait_result == 0);
    CHECK(pthread_join(timer_waiter, NULL) == 0);
    CHECK(timer_wait.result == WAIT_OBJECT_0);
    CHECK(pthread_cond_destroy(&timer_wait.cond) == 0);
    CHECK(pthread_mutex_destroy(&timer_wait.lock) == 0);
    CHECK(SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE));
    Sleep(10);
    CHECK(CancelWaitableTimer(timer));
    CHECK(WaitForSingleObject(timer, 0) == WAIT_OBJECT_0);

    due.QuadPart = -50000;
    CHECK(SetWaitableTimer(timer, &due, 10, NULL, NULL, FALSE));
    CHECK(WaitForSingleObject(timer, 500) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(timer, 0) == WAIT_OBJECT_0);

    HANDLE auto_timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    due.QuadPart = -50000;
    CHECK(auto_timer && SetWaitableTimer(auto_timer, &due, 15, NULL, NULL, FALSE));
    CHECK(WaitForSingleObject(auto_timer, 500) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(auto_timer, 0) == WAIT_TIMEOUT);
    CHECK(WaitForSingleObject(auto_timer, 500) == WAIT_OBJECT_0);
    CHECK(CancelWaitableTimer(auto_timer));
    CloseHandle(auto_timer);

    /* Re-arming clears a manual timer's old signal. Cancel preserves a due signal. */
    due.QuadPart = -20000;
    CHECK(SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE));
    CHECK(WaitForSingleObject(timer, 500) == WAIT_OBJECT_0);
    CHECK(CancelWaitableTimer(timer));
    CHECK(WaitForSingleObject(timer, 0) == WAIT_OBJECT_0);

    due.QuadPart = -30000;
    CHECK(SetWaitableTimer(timer, &due, 0, timer_apc, (PVOID)(ULONG_PTR)0x1234, FALSE));
    CHECK(SleepEx(1000, TRUE) == WAIT_IO_COMPLETION);
    CHECK(timer_apc_calls == 1);
    CHECK(timer_apc_argument == (ULONG_PTR)0x1234);
    CHECK(timer_apc_time > 100000000000000000ULL);
    CHECK(WaitForSingleObject(timer, 0) == WAIT_OBJECT_0);

    LONG before_close_callback = timer_apc_calls;
    HANDLE closed_callback_timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    due.QuadPart = -10000;
    CHECK(closed_callback_timer && SetWaitableTimer(closed_callback_timer, &due, 0,
                                                     timer_apc, NULL, FALSE));
    CHECK(CloseHandle(closed_callback_timer));
    CHECK(SleepEx(500, TRUE) == WAIT_IO_COMPLETION);
    CHECK(timer_apc_calls == before_close_callback + 1);

    before_close_callback = timer_apc_calls;
    closed_callback_timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    CHECK(closed_callback_timer && SetWaitableTimer(closed_callback_timer, &due, 0,
                                                     timer_apc, NULL, FALSE));
    Sleep(10); /* expiration queues the APC before the last handle closes */
    CHECK(CloseHandle(closed_callback_timer));
    CHECK(SleepEx(0, TRUE) == WAIT_IO_COMPLETION);
    CHECK(timer_apc_calls == before_close_callback + 1);

    /* Periodic completion APCs are delivered once per alertable wait interval. */
    due.QuadPart = -10000;
    CHECK(SetWaitableTimer(timer, &due, 10, timer_apc, NULL, FALSE));
    CHECK(SleepEx(500, TRUE) == WAIT_IO_COMPLETION);
    ULONGLONG first_periodic_time = timer_apc_time;
    CHECK(SleepEx(500, TRUE) == WAIT_IO_COMPLETION);
    CHECK(timer_apc_time >= first_periodic_time);
    CHECK(CancelWaitableTimer(timer));

    /* Expiries while the first periodic completion APC is queued coalesce. */
    due.QuadPart = -10000;
    LONG before_coalesced = timer_apc_calls;
    CHECK(SetWaitableTimer(timer, &due, 100, timer_apc, NULL, FALSE));
    pthread_t timer_service;
    CHECK(pthread_create(&timer_service, NULL, service_timers_without_alerting_owner, NULL) == 0);
    CHECK(pthread_join(timer_service, NULL) == 0);
    CHECK(SleepEx(0, TRUE) == WAIT_IO_COMPLETION);
    CHECK(timer_apc_calls == before_coalesced + 1);
    CHECK(SleepEx(0, TRUE) == 0);
    CHECK(CancelWaitableTimer(timer));

    /* Resume is unsupported, but the timer is still armed successfully. */
    due.QuadPart = -10000;
    SetLastError(0);
    CHECK(SetWaitableTimer(timer, &due, 0, NULL, NULL, TRUE));
    CHECK(GetLastError() == ERROR_NOT_SUPPORTED);
    CHECK(WaitForSingleObject(timer, 500) == WAIT_OBJECT_0);

    /* Alertable waits report APC before consuming an already-signaled event. */
    HANDLE ready = CreateEventA(NULL, FALSE, TRUE, NULL);
    HANDLE not_ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    CHECK(ready != NULL);
    CHECK(not_ready != NULL);
    CHECK(WaitForSingleObjectEx(ready, 0, TRUE) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObjectEx(not_ready, 0, TRUE) == WAIT_TIMEOUT);
    CHECK(SetEvent(ready));
    CHECK(QueueUserAPC(user_apc, GetCurrentThread(), 9) != 0);
    CHECK(WaitForSingleObjectEx(ready, 0, TRUE) == WAIT_IO_COMPLETION);
    CHECK(WaitForSingleObject(ready, 0) == WAIT_OBJECT_0);
    CHECK(user_apc_calls == 1 && timer_apc_argument == 9);
    CloseHandle(ready); CloseHandle(not_ready);

    /* Timer APCs interrupt an alertable wait-all without consuming its event. */
    HANDLE wait_all_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE wait_all_handles[1] = { wait_all_event };
    due.QuadPart = -10000;
    CHECK(SetWaitableTimer(timer, &due, 0, timer_apc, NULL, FALSE));
    CHECK(WaitForMultipleObjectsEx(1, wait_all_handles, TRUE, 500, TRUE) == WAIT_IO_COMPLETION);
    CHECK(WaitForSingleObject(wait_all_event, 0) == WAIT_TIMEOUT);
    CHECK(CancelWaitableTimer(timer));
    CloseHandle(wait_all_event);

    HANDLE exiting_owner_timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    HANDLE owner_thread = CreateThread(NULL, 0, arm_timer_then_exit, exiting_owner_timer, 0, NULL);
    CHECK(exiting_owner_timer && owner_thread);
    CHECK(WaitForSingleObject(owner_thread, 1000) == WAIT_OBJECT_0);
    CHECK(SleepEx(0, TRUE) == 0);
    CHECK(WaitForSingleObject(exiting_owner_timer, 0) == WAIT_OBJECT_0);
    CloseHandle(owner_thread);
    CloseHandle(exiting_owner_timer);

    /* Cancel removes a queued timer APC without clearing the signaled state. */
    due.QuadPart = -10000;
    CHECK(SetWaitableTimer(timer, &due, 0, timer_apc, NULL, FALSE));
    Sleep(10);
    CHECK(CancelWaitableTimer(timer));
    CHECK(SleepEx(0, TRUE) == 0);
    CHECK(timer_apc_calls == before_coalesced + 2);

    /* Timer completions remain pending when the bounded APC queue fills. */
    HANDLE many[18];
    due.QuadPart = -1;
    LONG before_many = timer_apc_calls;
    for (int i = 0; i < 18; i++) {
        many[i] = CreateWaitableTimerW(NULL, FALSE, NULL);
        CHECK(many[i] && SetWaitableTimer(many[i], &due, 0, timer_apc, NULL, FALSE));
    }
    for (int i = 0; i < 4 && timer_apc_calls < before_many + 18; i++)
        CHECK(SleepEx(0, TRUE) == WAIT_IO_COMPLETION);
    CHECK(timer_apc_calls == before_many + 18);
    for (int i = 0; i < 18; i++) CloseHandle(many[i]);

    /* Extreme relative value is accepted without signed overflow. */
    due.QuadPart = (-9223372036854775807LL - 1LL);
    CHECK(SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE));
    CHECK(CancelWaitableTimer(timer));

    CloseHandle(timer);

    HANDLE duplicate_source = CreateWaitableTimerW(NULL, FALSE, NULL);
    HANDLE duplicate_target = NULL;
    due.QuadPart = -10000000;
    CHECK(duplicate_source && SetWaitableTimer(duplicate_source, &due, 0, NULL, NULL, FALSE));
    CHECK(DuplicateHandle(GetCurrentProcess(), duplicate_source, GetCurrentProcess(),
                          &duplicate_target, 0, FALSE,
                          DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE));
    CHECK(CloseHandle(duplicate_target));

    /* Closing the last handle to an armed timer drops registry ownership. */
    for (int i = 0; i < 1000; i++) {
        HANDLE armed = CreateWaitableTimerW(NULL, FALSE, NULL);
        due.QuadPart = -600000000LL;
        CHECK(armed && SetWaitableTimer(armed, &due, 1000, NULL, NULL, FALSE));
        CHECK(CloseHandle(armed));
    }
    puts("PASS waitable timer periodic, signal, APC, cancel, and overflow semantics");
}

static void test_negative_timer_period(void)
{
    HANDLE timer = CreateWaitableTimerW(NULL, TRUE, NULL);
    LARGE_INTEGER due = { .QuadPart = -10000 };
    CHECK(timer != NULL);
    CHECK(!SetWaitableTimer(timer, &due, -1, NULL, NULL, FALSE));
    CHECK(GetLastError() == ERROR_INVALID_PARAMETER);
    CloseHandle(timer);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "timer") == 0) {
        test_waitable_timers();
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "timer-invalid") == 0) {
        test_negative_timer_period();
        return 0;
    }
    HANDLE event = CreateEventA(NULL, FALSE, TRUE, NULL);
    HANDLE missing = CreateEventA(NULL, FALSE, FALSE, NULL);
    HANDLE sem = CreateSemaphoreA(NULL, 1, 1, NULL);
    HANDLE mutex = CreateMutexA(NULL, FALSE, NULL);
    HANDLE pair[2] = { event, missing };
    CHECK(event && missing && sem && mutex);

    CHECK(WaitForMultipleObjects(2, pair, TRUE, 5) == WAIT_TIMEOUT);
    CHECK(WaitForSingleObject(event, 0) == WAIT_OBJECT_0);

    pair[0] = sem;
    CHECK(WaitForMultipleObjects(2, pair, TRUE, 5) == WAIT_TIMEOUT);
    CHECK(WaitForSingleObject(sem, 0) == WAIT_OBJECT_0);

    pair[0] = mutex;
    CHECK(WaitForMultipleObjects(2, pair, TRUE, 5) == WAIT_TIMEOUT);
    mutex_probe probe = { mutex, WAIT_FAILED };
    pthread_t probe_thread;
    CHECK(pthread_create(&probe_thread, NULL, probe_mutex, &probe) == 0);
    CHECK(pthread_join(probe_thread, NULL) == 0);
    CHECK(probe.result == WAIT_OBJECT_0);

    HANDLE timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    LARGE_INTEGER due = { .QuadPart = -10000 };
    CHECK(timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE));
    pair[0] = timer;
    CHECK(WaitForMultipleObjects(2, pair, TRUE, 25) == WAIT_TIMEOUT);
    CHECK(WaitForSingleObject(timer, 0) == WAIT_OBJECT_0);
    CloseHandle(timer);

    CHECK(SetEvent(event));
    CHECK(SetEvent(missing));
    CHECK(ReleaseSemaphore(sem, 1, NULL));
    HANDLE success[3] = { event, sem, mutex };
    CHECK(WaitForMultipleObjects(3, success, TRUE, 0) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(event, 0) == WAIT_TIMEOUT);
    CHECK(WaitForSingleObject(sem, 0) == WAIT_TIMEOUT);
    CHECK(ReleaseMutex(mutex));

    CHECK(ResetEvent(missing));
    HANDLE delayed[2] = { event, missing };
    pthread_t setter;
    CHECK(pthread_create(&setter, NULL, set_later, delayed) == 0);
    CHECK(WaitForMultipleObjects(2, delayed, TRUE, 2000) == WAIT_OBJECT_0);
    CHECK(pthread_join(setter, NULL) == 0);
    CHECK(WaitForSingleObject(event, 0) == WAIT_TIMEOUT);
    CHECK(WaitForSingleObject(missing, 0) == WAIT_TIMEOUT);

    CHECK(WaitForMultipleObjects(0, pair, TRUE, 0) == WAIT_FAILED);
    CHECK(GetLastError() == ERROR_INVALID_PARAMETER);
    CHECK(WaitForMultipleObjects(2, NULL, TRUE, 0) == WAIT_FAILED);
    CHECK(GetLastError() == ERROR_INVALID_PARAMETER);
    HANDLE invalid[1] = { NULL };
    CHECK(WaitForMultipleObjects(1, invalid, TRUE, 0) == WAIT_FAILED);
    CHECK(GetLastError() == ERROR_INVALID_HANDLE);
    HANDLE duplicate[2] = { event, event };
    CHECK(WaitForMultipleObjects(2, duplicate, TRUE, 0) == WAIT_FAILED);
    CHECK(GetLastError() == ERROR_INVALID_PARAMETER);
    HANDLE too_many[MAXIMUM_WAIT_OBJECTS + 1];
    for (DWORD i = 0; i < MAXIMUM_WAIT_OBJECTS + 1; i++) too_many[i] = event;
    CHECK(WaitForMultipleObjects(MAXIMUM_WAIT_OBJECTS + 1, too_many, TRUE, 0) == WAIT_FAILED);
    CHECK(GetLastError() == ERROR_INVALID_PARAMETER);

    HANDLE a = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE b = CreateEventA(NULL, TRUE, FALSE, NULL);
    CHECK(a && b);
    for (int i = 0; i < 100; i++) {
        wait_args args[2] = { { { a, b }, WAIT_FAILED }, { { b, a }, WAIT_FAILED } };
        pthread_t threads[2];
        CHECK(pthread_create(&threads[0], NULL, wait_reversed, &args[0]) == 0);
        CHECK(pthread_create(&threads[1], NULL, wait_reversed, &args[1]) == 0);
        CHECK(SetEvent(a) && SetEvent(b));
        CHECK(pthread_join(threads[0], NULL) == 0);
        CHECK(pthread_join(threads[1], NULL) == 0);
        CHECK(args[0].result == WAIT_OBJECT_0 && args[1].result == WAIT_OBJECT_0);
        CHECK(WaitForMultipleObjects(2, args[0].handles, FALSE, 0) == WAIT_OBJECT_0);
        CHECK(ResetEvent(a) && ResetEvent(b));
    }

    CloseHandle(a); CloseHandle(b); CloseHandle(event); CloseHandle(missing);
    CloseHandle(sem); CloseHandle(mutex);
    puts("PASS wait-all preserves signals on timeout and acquires atomically");
    return 0;
}
