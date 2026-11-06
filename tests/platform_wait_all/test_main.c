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
typedef struct condition_wait_args {
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv;
    pthread_cond_t cv_storage;
    HANDLE ready;
    HANDLE returned;
    HANDLE allow_leave;
    int notified;
    int wait_result;
} condition_wait_args;

static DWORD condition_waiter(LPVOID arg)
{
    condition_wait_args *a = (condition_wait_args *)arg;
    EnterCriticalSection(&a->cs);
    SetEvent(a->ready);
    while (!a->notified)
        a->wait_result = SleepConditionVariableCS(&a->cv, &a->cs, 2000);
    SetEvent(a->returned);
    WaitForSingleObject(a->allow_leave, INFINITE);
    LeaveCriticalSection(&a->cs);
    return 0;
}

static DWORD condition_cancel_waiter(LPVOID arg)
{
    condition_wait_args *a = (condition_wait_args *)arg;
    EnterCriticalSection(&a->cs);
    SetEvent(a->ready);
    SleepConditionVariableCS(&a->cv, &a->cs, INFINITE);
    LeaveCriticalSection(&a->cs);
    return 0;
}

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

typedef struct thread_lifecycle_args {
    HANDLE timer;
    HANDLE ready;
    DWORD wait_result;
    BOOL timer_set;
} thread_lifecycle_args;

static volatile LONG lifecycle_worker_ran;

static DWORD return_thread_code(LPVOID arg)
{
    return (DWORD)(ULONG_PTR)arg;
}

static DWORD exit_thread_code(LPVOID arg)
{
    ExitThread((DWORD)(ULONG_PTR)arg);
    return 0;
}

static DWORD wait_for_timer_until_cancel(LPVOID arg)
{
    thread_lifecycle_args *a = (thread_lifecycle_args *)arg;
    SetEvent(a->ready);
    a->wait_result = WaitForSingleObject(a->timer, INFINITE);
    return 0x11111111u;
}

static DWORD alertable_timer_owner(LPVOID arg)
{
    thread_lifecycle_args *a = (thread_lifecycle_args *)arg;
    LARGE_INTEGER due = { .QuadPart = -300000000LL };
    a->timer_set = SetWaitableTimer(a->timer, &due, 0, timer_apc, NULL, FALSE);
    SetEvent(a->ready);
    if (a->timer_set) a->wait_result = SleepEx(INFINITE, TRUE);
    return 0x22222222u;
}

static DWORD queued_apc_owner(LPVOID arg)
{
    thread_lifecycle_args *a = (thread_lifecycle_args *)arg;
    LARGE_INTEGER due = { .QuadPart = -10000 };
    a->timer_set = SetWaitableTimer(a->timer, &due, 0, timer_apc, NULL, FALSE);
    SetEvent(a->ready);
    Sleep(5000); /* nonalertable: let another thread queue its timer APC */
    return 0x33333333u;
}

static DWORD suspended_worker(LPVOID arg)
{
    (void)arg;
    InterlockedIncrement(&lifecycle_worker_ran);
    return 0x44444444u;
}

static DWORD cancelable_sleep_worker(LPVOID arg)
{
    (void)arg;
    Sleep(INFINITE);
    return 0x55555555u;
}

static void check_thread_exit(HANDLE thread, DWORD expected)
{
    DWORD code = 0;
    CHECK(WaitForSingleObject(thread, 2000) == WAIT_OBJECT_0);
    CHECK(GetExitCodeThread(thread, &code));
    CHECK(code == expected);
}

static void test_thread_lifecycle(void)
{
    HANDLE thread = CreateThread(NULL, 0, return_thread_code,
                                 (LPVOID)(ULONG_PTR)0x12345678u, 0, NULL);
    CHECK(thread != NULL);
    check_thread_exit(thread, 0x12345678u);
    CHECK(CloseHandle(thread));

    thread = CreateThread(NULL, 0, exit_thread_code,
                          (LPVOID)(ULONG_PTR)0x87654321u, 0, NULL);
    CHECK(thread != NULL);
    check_thread_exit(thread, 0x87654321u);
    CHECK(CloseHandle(thread));

    /* SleepConditionVariableCS returns with the critical section held. */
    condition_wait_args condition = { 0 };
    InitializeCriticalSection(&condition.cs);
    condition.cv.Ptr = &condition.cv_storage;
    CHECK(pthread_cond_init(&condition.cv_storage, NULL) == 0);
    condition.ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    condition.returned = CreateEventA(NULL, TRUE, FALSE, NULL);
    condition.allow_leave = CreateEventA(NULL, TRUE, FALSE, NULL);
    thread = CreateThread(NULL, 0, condition_waiter, &condition, 0, NULL);
    CHECK(thread && condition.ready && condition.returned && condition.allow_leave);
    CHECK(WaitForSingleObject(condition.ready, 1000) == WAIT_OBJECT_0);
    EnterCriticalSection(&condition.cs);
    condition.notified = 1;
    WakeConditionVariable(&condition.cv);
    LeaveCriticalSection(&condition.cs);
    CHECK(WaitForSingleObject(condition.returned, 1000) == WAIT_OBJECT_0);
    CHECK(condition.wait_result);
    CHECK(!TryEnterCriticalSection(&condition.cs));
    SetEvent(condition.allow_leave);
    check_thread_exit(thread, 0);
    CHECK(TryEnterCriticalSection(&condition.cs));
    LeaveCriticalSection(&condition.cs);
    CHECK(CloseHandle(thread));
    CHECK(CloseHandle(condition.ready));
    CHECK(CloseHandle(condition.returned));
    CHECK(CloseHandle(condition.allow_leave));
    CHECK(pthread_cond_destroy(&condition.cv_storage) == 0);
    DeleteCriticalSection(&condition.cs);

    /* Cancellation releases the condition wait's reacquired critical section. */
    condition_wait_args canceled_condition = { 0 };
    InitializeCriticalSection(&canceled_condition.cs);
    canceled_condition.cv.Ptr = &canceled_condition.cv_storage;
    CHECK(pthread_cond_init(&canceled_condition.cv_storage, NULL) == 0);
    canceled_condition.ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    thread = CreateThread(NULL, 0, condition_cancel_waiter,
                          &canceled_condition, 0, NULL);
    CHECK(thread && canceled_condition.ready);
    CHECK(WaitForSingleObject(canceled_condition.ready, 1000) == WAIT_OBJECT_0);
    CHECK(TerminateThread(thread, 0xA0A0A0A0u));
    check_thread_exit(thread, 0xA0A0A0A0u);
    EnterCriticalSection(&canceled_condition.cs);
    LeaveCriticalSection(&canceled_condition.cs);
    CHECK(CloseHandle(thread));
    CHECK(CloseHandle(canceled_condition.ready));
    CHECK(pthread_cond_destroy(&canceled_condition.cv_storage) == 0);
    DeleteCriticalSection(&canceled_condition.cs);

    /* Cancellation at a timer condition wait must release the reacquired lock. */
    thread_lifecycle_args a = {0};
    a.timer = CreateWaitableTimerW(NULL, TRUE, NULL);
    a.ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    LARGE_INTEGER due = { .QuadPart = -300000000LL };
    CHECK(a.timer && a.ready && SetWaitableTimer(a.timer, &due, 0, NULL, NULL, FALSE));
    thread = CreateThread(NULL, 0, wait_for_timer_until_cancel, &a, 0, NULL);
    CHECK(thread && WaitForSingleObject(a.ready, 1000) == WAIT_OBJECT_0);
    CHECK(TerminateThread(thread, 0xA1A1A1A1u));
    check_thread_exit(thread, 0xA1A1A1A1u);
    CHECK(CancelWaitableTimer(a.timer));
    CHECK(CloseHandle(thread));
    CHECK(CloseHandle(a.ready));
    CHECK(CloseHandle(a.timer));

    /* Alertable timer owners are disarmed and their completion APC is removed. */
    memset(&a, 0, sizeof(a));
    a.timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    a.ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    LONG before = timer_apc_calls;
    thread = CreateThread(NULL, 0, alertable_timer_owner, &a, 0, NULL);
    CHECK(thread && a.timer && a.ready &&
          WaitForSingleObject(a.ready, 1000) == WAIT_OBJECT_0);
    CHECK(a.timer_set);
    CHECK(TerminateThread(thread, 0xA2A2A2A2u));
    check_thread_exit(thread, 0xA2A2A2A2u);
    CHECK(timer_apc_calls == before);
    CHECK(WaitForSingleObject(a.timer, 0) == WAIT_TIMEOUT);
    CHECK(CloseHandle(thread));
    CHECK(CloseHandle(a.ready));
    CHECK(CloseHandle(a.timer));

    /* A queued timer APC is dropped on owner cancellation; duplicate handle
     * keeps the thread object alive after the original closes immediately. */
    memset(&a, 0, sizeof(a));
    a.timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    a.ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    before = timer_apc_calls;
    thread = CreateThread(NULL, 0, queued_apc_owner, &a, 0, NULL);
    CHECK(thread && a.timer && a.ready &&
          WaitForSingleObject(a.ready, 1000) == WAIT_OBJECT_0);
    CHECK(a.timer_set);
    Sleep(20); /* services the expired APC while its owner remains nonalertable */
    HANDLE duplicate = NULL;
    CHECK(DuplicateHandle(GetCurrentProcess(), thread, GetCurrentProcess(),
                          &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS));
    CHECK(TerminateThread(thread, 0xA3A3A3A3u));
    CHECK(CloseHandle(thread));
    check_thread_exit(duplicate, 0xA3A3A3A3u);
    CHECK(timer_apc_calls == before);
    CHECK(WaitForSingleObject(a.timer, 0) == WAIT_OBJECT_0);
    CHECK(CloseHandle(duplicate));
    CHECK(CloseHandle(a.ready));
    CHECK(CloseHandle(a.timer));

    /* Cancellation while suspended runs the same finalizer without starting. */
    InterlockedExchange(&lifecycle_worker_ran, 0);
    thread = CreateThread(NULL, 0, suspended_worker, NULL, CREATE_SUSPENDED, NULL);
    CHECK(thread && TerminateThread(thread, 0xA4A4A4A4u));
    check_thread_exit(thread, 0xA4A4A4A4u);
    CHECK(lifecycle_worker_ran == 0);
    CHECK(CloseHandle(thread));

    /* Repeated cancel/close exercises worker-vs-handle reference teardown. */
    for (int i = 0; i < 24; i++) {
        thread = CreateThread(NULL, 0, cancelable_sleep_worker, NULL, 0, NULL);
        CHECK(thread != NULL);
        CHECK(TerminateThread(thread, (DWORD)i));
        check_thread_exit(thread, (DWORD)i);
        CHECK(CloseHandle(thread));
    }
    puts("PASS thread return, ExitThread, cancellation, timer/APC rundown, and refs");
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "timer") == 0) {
        test_waitable_timers();
        test_thread_lifecycle();
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
