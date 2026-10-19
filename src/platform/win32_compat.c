/*
 * win32_compat.c - POSIX implementation of generic Win32 host primitives.
 *
 * See win32_compat.h. This is deliberately a *generic* OS-primitive layer
 * (threads/events/mutexes/atomics/heap/timers) -- it carries no Xbox
 * semantics. The Xbox kernel HLE in src/kernel builds on top of it.
 *
 * POSIX (Linux/MacOS) only.
 */

#if !defined(_WIN32)

/* Enable memfd_create, MAP_FIXED_NOREPLACE, timegm. Must precede all #includes. */
#define _GNU_SOURCE
/* Darwin: exposes memset_s, its explicit_bzero equivalent. */
#define __STDC_WANT_LIB_EXT1__ 1

#include "win32_compat.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <sched.h>
#include <fenv.h>
#include <sys/mman.h>
#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#endif
#ifndef MEM_MAPPED
#define MEM_MAPPED 0x00040000u
#endif
#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#else
#include <sys/sysinfo.h>
#endif

/* ===================================================================== */
/* Last-error (thread-local)                                             */
/* ===================================================================== */

static __thread DWORD t_last_error = 0;

DWORD GetLastError(void)            { return t_last_error; }
VOID  SetLastError(DWORD code)      { t_last_error = code; }

/* ===================================================================== */
/* Interlocked atomics                                                   */
/* ===================================================================== */

LONG InterlockedIncrement(volatile LONG *p)        { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
LONG InterlockedDecrement(volatile LONG *p)        { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
LONG InterlockedExchange(volatile LONG *p, LONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
LONG InterlockedExchangeAdd(volatile LONG *p, LONG v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }

LONG InterlockedCompareExchange(volatile LONG *p, LONG xchg, LONG cmp)
{
    __atomic_compare_exchange_n(p, &cmp, xchg, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}

LONGLONG InterlockedCompareExchange64(volatile LONGLONG *p, LONGLONG xchg, LONGLONG cmp)
{
    __atomic_compare_exchange_n(p, &cmp, xchg, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}

PVOID InterlockedCompareExchangePointer(PVOID volatile *p, PVOID xchg, PVOID cmp)
{
    __atomic_compare_exchange_n(p, &cmp, xchg, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}

/* ===================================================================== */
/* Critical sections (recursive pthread mutex)                           */
/* ===================================================================== */

VOID InitializeCriticalSection(LPCRITICAL_SECTION cs)
{
    memset(cs, 0, sizeof(*cs));
    pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
    cs->LockSemaphore = m;
}

VOID InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION cs, DWORD spin)
{
    InitializeCriticalSection(cs);
    cs->SpinCount = spin;
}

VOID EnterCriticalSection(LPCRITICAL_SECTION cs)
{
    if (!cs->LockSemaphore) InitializeCriticalSection(cs);
    pthread_mutex_lock((pthread_mutex_t *)cs->LockSemaphore);
    cs->RecursionCount++;
}

VOID LeaveCriticalSection(LPCRITICAL_SECTION cs)
{
    if (!cs->LockSemaphore) return;
    cs->RecursionCount--;
    pthread_mutex_unlock((pthread_mutex_t *)cs->LockSemaphore);
}

BOOL TryEnterCriticalSection(LPCRITICAL_SECTION cs)
{
    if (!cs->LockSemaphore) InitializeCriticalSection(cs);
    if (pthread_mutex_trylock((pthread_mutex_t *)cs->LockSemaphore) == 0) {
        cs->RecursionCount++;
        return TRUE;
    }
    return FALSE;
}

VOID DeleteCriticalSection(LPCRITICAL_SECTION cs)
{
    if (cs->LockSemaphore) {
        pthread_mutex_destroy((pthread_mutex_t *)cs->LockSemaphore);
        free(cs->LockSemaphore);
        cs->LockSemaphore = NULL;
    }
}

/* ===================================================================== */
/* Slim reader/writer locks                                              */
/* ===================================================================== */

/* An SRWLOCK is usable straight from SRWLOCK_INIT, so the pthread_rwlock_t
 * behind it has to appear on first use. Unlike the condition variables below
 * -- whose lazy init is covered by the caller holding the paired CRITICAL
 * SECTION -- an SRWLOCK is by definition taken from several threads at once
 * with nothing else held, so first use genuinely races. Serialise just that:
 * once Ptr is published, every acquire is a plain atomic load. */
static pthread_rwlock_t *srw_lazy_init(PSRWLOCK lock)
{
    pthread_rwlock_t *rw = __atomic_load_n((pthread_rwlock_t **)&lock->Ptr,
                                           __ATOMIC_ACQUIRE);
    if (!rw) {
        static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&init_lock);
        rw = (pthread_rwlock_t *)lock->Ptr;
        if (!rw) {
            rw = (pthread_rwlock_t *)malloc(sizeof(*rw));
            pthread_rwlock_init(rw, NULL);
            __atomic_store_n((pthread_rwlock_t **)&lock->Ptr, rw, __ATOMIC_RELEASE);
        }
        pthread_mutex_unlock(&init_lock);
    }
    return rw;
}

VOID InitializeSRWLock(PSRWLOCK lock)
{
    lock->Ptr = NULL;
    srw_lazy_init(lock);
}

VOID AcquireSRWLockShared(PSRWLOCK lock)     { pthread_rwlock_rdlock(srw_lazy_init(lock)); }
VOID ReleaseSRWLockShared(PSRWLOCK lock)     { pthread_rwlock_unlock(srw_lazy_init(lock)); }
VOID AcquireSRWLockExclusive(PSRWLOCK lock)  { pthread_rwlock_wrlock(srw_lazy_init(lock)); }
VOID ReleaseSRWLockExclusive(PSRWLOCK lock)  { pthread_rwlock_unlock(srw_lazy_init(lock)); }

/* ===================================================================== */
/* One-time initialisation                                               */
/* ===================================================================== */

/* Win32 semantics: the callback runs at most once for a given INIT_ONCE, and
 * a callback returning FALSE leaves it un-run so a later call retries. Ptr
 * doubles as the "done" flag. One global mutex covers every INIT_ONCE --
 * initialisation is rare, and the fast path never touches it. */
BOOL InitOnceExecuteOnce(PINIT_ONCE once, PINIT_ONCE_FN fn, PVOID param, PVOID *context)
{
    static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER;

    if (__atomic_load_n(&once->Ptr, __ATOMIC_ACQUIRE))
        return TRUE;

    pthread_mutex_lock(&once_lock);
    BOOL ok = TRUE;
    if (!once->Ptr) {
        ok = fn(once, param, context);
        if (ok)
            __atomic_store_n(&once->Ptr, (PVOID)1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&once_lock);
    return ok;
}

/* ===================================================================== */
/* Condition variables (paired with a CRITICAL_SECTION)                  */
/* ===================================================================== */

/* Forward decl; the definition lives further down with the wait helpers. */
static void deadline_from_ms(DWORD ms, struct timespec *ts);

static void unlock_mutex_cleanup(void *mutex)
{
    pthread_mutex_unlock((pthread_mutex_t *)mutex);
}

static int cond_wait_cancel_safe(pthread_cond_t *cond, pthread_mutex_t *mutex,
                                 int bounded, const struct timespec *until)
{
    volatile int rc;
    pthread_cleanup_push(unlock_mutex_cleanup, mutex);
    rc = bounded ? pthread_cond_timedwait(cond, mutex, until)
                 : pthread_cond_wait(cond, mutex);
    pthread_cleanup_pop(0);
    return rc;
}

static void cv_lazy_init(PCONDITION_VARIABLE cv)
{
    if (!cv->Ptr) {
        pthread_cond_t *c = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));
        pthread_cond_init(c, NULL);
        /* Race window OK for typical Win32 usage (the CS is held). */
        cv->Ptr = c;
    }
}

VOID InitializeConditionVariable(PCONDITION_VARIABLE cv)
{
    cv->Ptr = NULL;
    cv_lazy_init(cv);
}

BOOL SleepConditionVariableCS(PCONDITION_VARIABLE cv, PCRITICAL_SECTION cs, DWORD ms)
{
    cv_lazy_init(cv);
    if (!cs->LockSemaphore) InitializeCriticalSection(cs);
    pthread_cond_t  *c = (pthread_cond_t  *)cv->Ptr;
    pthread_mutex_t *m = (pthread_mutex_t *)cs->LockSemaphore;
    struct timespec ts;
    int rc;
    if (ms != INFINITE) deadline_from_ms(ms, &ts);
    pthread_cleanup_push(unlock_mutex_cleanup, m);
    rc = ms == INFINITE ? pthread_cond_wait(c, m)
                        : pthread_cond_timedwait(c, m, &ts);
    pthread_cleanup_pop(0);
    if (rc == ETIMEDOUT) { SetLastError(WAIT_TIMEOUT); return FALSE; }
    return TRUE;
}

VOID WakeConditionVariable(PCONDITION_VARIABLE cv)
{
    cv_lazy_init(cv);
    pthread_cond_signal((pthread_cond_t *)cv->Ptr);
}

VOID WakeAllConditionVariable(PCONDITION_VARIABLE cv)
{
    cv_lazy_init(cv);
    pthread_cond_broadcast((pthread_cond_t *)cv->Ptr);
}

/* ===================================================================== */
/* WaitOnAddress / WakeByAddress*                                        */
/* ===================================================================== */

/* Both implementations work on the variable's aligned 32-bit word. Linux
 * sleeps on that word with a futex. Snapshotting the word before the byte
 * compare means a store in between makes the futex return EAGAIN, so no wake
 * is lost. Elsewhere (macOS and other POSIX) a fixed table of mutex/condvar
 * buckets does the same job under the bucket lock. Spurious returns are
 * allowed, as on Windows; callers re-check their predicate. */

static int64_t w32_mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

#if defined(__linux__)

static BOOL w32_wait_futex(volatile VOID *address, PVOID compare, SIZE_T size, DWORD ms)
{
    uintptr_t addr = (uintptr_t)address;
    volatile uint32_t *word = (volatile uint32_t *)(addr & ~(uintptr_t)3);
    /* A variable crossing a word boundary has no single futex word to watch. */
    int straddles = (addr & 3) + size > 4;
    int64_t start = w32_mono_ns();
    for (;;) {
        uint32_t snapshot = __atomic_load_n(word, __ATOMIC_ACQUIRE);
        struct timespec rel, *timeout = NULL;
        int64_t left = -1;
        if (memcmp((const void *)address, compare, size) != 0) return TRUE;
        if (ms != INFINITE) {
            left = (int64_t)ms * 1000000LL - (w32_mono_ns() - start);
            if (left <= 0) { SetLastError(ERROR_TIMEOUT); return FALSE; }
            if (straddles && left > 1000000LL) left = 1000000LL; /* re-check each 1 ms */
            rel.tv_sec = left / 1000000000LL;
            rel.tv_nsec = left % 1000000000LL;
            timeout = &rel;
        }
        if (syscall(SYS_futex, word, FUTEX_WAIT_PRIVATE, (long)snapshot, timeout, NULL, 0) == 0)
            return TRUE;
        if (errno == EAGAIN || errno == EINTR) return TRUE;   /* word changed or signal */
        if (errno != ETIMEDOUT) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    }
}

static void w32_wake_futex(PVOID address, int count)
{
    uintptr_t addr = (uintptr_t)address & ~(uintptr_t)3;
    syscall(SYS_futex, (volatile uint32_t *)addr, FUTEX_WAKE_PRIVATE, count);
}

VOID WakeByAddressSingle(PVOID address) { w32_wake_futex(address, 1); }
VOID WakeByAddressAll(PVOID address)    { w32_wake_futex(address, 0x7FFFFFFF); }

#else

#define W32_PARK_BUCKETS 64
static pthread_once_t  w32_park_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t w32_park_mutex[W32_PARK_BUCKETS];
static pthread_cond_t  w32_park_cond[W32_PARK_BUCKETS];

static void w32_park_init(void)
{
    for (int i = 0; i < W32_PARK_BUCKETS; i++) {
        pthread_mutex_init(&w32_park_mutex[i], NULL);
        pthread_cond_init(&w32_park_cond[i], NULL);
    }
}

static unsigned w32_park_bucket(const volatile VOID *address)
{
    return (unsigned)(((uintptr_t)address >> 2) % W32_PARK_BUCKETS);
}

static BOOL w32_wait_park(volatile VOID *address, PVOID compare, SIZE_T size, DWORD ms)
{
    unsigned bucket;
    int64_t start = w32_mono_ns();
    pthread_once(&w32_park_once, w32_park_init);
    bucket = w32_park_bucket(address);
    pthread_mutex_lock(&w32_park_mutex[bucket]);
    while (memcmp((const void *)address, compare, size) == 0) {
        if (ms == INFINITE) {
            pthread_cond_wait(&w32_park_cond[bucket], &w32_park_mutex[bucket]);
            continue;
        }
        int64_t left = (int64_t)ms * 1000000LL - (w32_mono_ns() - start);
        if (left <= 0) {
            pthread_mutex_unlock(&w32_park_mutex[bucket]);
            SetLastError(ERROR_TIMEOUT);
            return FALSE;
        }
        struct timespec abs;
        clock_gettime(CLOCK_REALTIME, &abs);
        abs.tv_nsec += (long)(left % 1000000000LL);
        abs.tv_sec += (time_t)(left / 1000000000LL) + abs.tv_nsec / 1000000000L;
        abs.tv_nsec %= 1000000000L;
        pthread_cond_timedwait(&w32_park_cond[bucket], &w32_park_mutex[bucket], &abs);
    }
    pthread_mutex_unlock(&w32_park_mutex[bucket]);
    return TRUE;
}

static void w32_wake_park(PVOID address)
{
    unsigned bucket;
    pthread_once(&w32_park_once, w32_park_init);
    bucket = w32_park_bucket(address);
    /* Broadcast: a waiter from another address in this bucket re-checks and
     * sleeps again, so a single signal could be spent on the wrong one. */
    pthread_mutex_lock(&w32_park_mutex[bucket]);
    pthread_cond_broadcast(&w32_park_cond[bucket]);
    pthread_mutex_unlock(&w32_park_mutex[bucket]);
}

VOID WakeByAddressSingle(PVOID address) { w32_wake_park(address); }
VOID WakeByAddressAll(PVOID address)    { w32_wake_park(address); }

#endif

BOOL WaitOnAddress(volatile VOID *address, PVOID compare, SIZE_T size, DWORD ms)
{
    if (size != 1 && size != 2 && size != 4 && size != 8) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
#if defined(__linux__)
    return w32_wait_futex(address, compare, size, ms);
#else
    return w32_wait_park(address, compare, size, ms);
#endif
}

/* ===================================================================== */
/* Waitable kernel objects                                               */
/* ===================================================================== */

typedef enum { K_EVENT, K_SEM, K_MUTEX, K_THREAD, K_TIMER, K_HEAP,
               K_FILEMAP, K_FILE, K_WAITABLE_TIMER, K_PROCESS } w32_kind;

#define W32_MAX_APC 16

typedef struct w32_object w32_object;
typedef enum { W32_APC_USER, W32_APC_TIMER } w32_apc_kind;
typedef struct {
    w32_apc_kind kind;
    PAPCFUNC user_func;
    ULONG_PTR user_data;
    w32_object *timer;
    ULONGLONG timer_generation;
} w32_apc;

struct w32_object {
    w32_kind        kind;
    LONG            refcount;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    pthread_cond_t  waitable_cond;
    int             waitable_cond_monotonic;

    /* event */
    int             signaled;
    int             manual_reset;

    /* semaphore */
    long            sem_count;
    long            sem_max;

    /* mutex */
    DWORD           mtx_owner;
    int             mtx_recursion;

    /* thread */
    pthread_t       thread;
    DWORD           tid;
    int             exited;
    int             thread_termination_requested;
    DWORD           exit_code;
    int             suspend_count;
    pthread_cond_t  gate;
    LPTHREAD_START_ROUTINE start;
    LPVOID          start_param;
    int             priority;
    w32_apc         apcs[W32_MAX_APC];
    int             apc_count;

    /* timer-queue timer */
    int             timer_cancel;
    DWORD           timer_due;
    DWORD           timer_period;
    WAITORTIMERCALLBACK timer_cb;
    PVOID           timer_param;

    /* waitable timer */
    int             waitable_manual_reset;
    struct timespec waitable_due_time;
    clockid_t       waitable_clock;
    int             waitable_triggered;
    int             waitable_armed;
    LONG            waitable_period;
    PTIMERAPCROUTINE waitable_completion;
    PVOID           waitable_arg;
    LONGLONG        waitable_apc_due;
    w32_object     *waitable_apc_owner;
    ULONGLONG       waitable_generation;
    int             waitable_apc_queued;
    int             waitable_apc_due_pending;
    int             waitable_registered;
    LONG            waitable_handle_count;
    w32_object     *waitable_next;

    /* file mapping / fd-backed file handle */
    int             fd;
    SIZE_T          map_size;
    DWORD           map_protect;
    char           *file_path;
};

/* pseudo handles for "current thread"/"current process" */
#define PSEUDO_CURRENT_PROCESS ((HANDLE)(LONG_PTR)-1)
#define PSEUDO_CURRENT_THREAD  ((HANDLE)(LONG_PTR)-2)
#define STILL_ACTIVE 259u

static __thread w32_object *t_self_obj = NULL;
static __thread DWORD       t_tid      = 0;
static volatile LONG        s_next_tid = 1000;
static pthread_mutex_t s_waitable_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static w32_object *s_waitable_registry;
static pthread_key_t s_apc_context_key;
static pthread_once_t s_apc_context_once = PTHREAD_ONCE_INIT;

static void service_waitable_timers(void);
static void cancel_thread_timers(w32_object *thread);
static int timespec_before(const struct timespec *a, const struct timespec *b);

DWORD GetCurrentThreadId(void)
{
    if (t_tid == 0)
        t_tid = (DWORD)InterlockedIncrement(&s_next_tid);
    return t_tid;
}

DWORD GetCurrentProcessId(void) { return (DWORD)getpid(); }
HANDLE GetCurrentThread(void)   { return t_self_obj ? (HANDLE)t_self_obj : PSEUDO_CURRENT_THREAD; }
HANDLE GetCurrentProcess(void)  { return PSEUDO_CURRENT_PROCESS; }

/* Live objects. A HANDLE here is the object's address, so a value that is
 * not one -- a guest pointer, a stale or made-up handle -- would otherwise be
 * dereferenced. Windows answers those with ERROR_INVALID_HANDLE, and callers
 * (the kernel HLE falls back to raw guest values) rely on that.
 * Open addressing over the pointer; deleted slots keep a tombstone. */
#define W32_TOMB ((w32_object *)1)
static w32_object s_process_heap;
static w32_object **s_live;
static size_t s_live_cap, s_live_used;      /* used counts tombstones too */
static pthread_rwlock_t s_live_lock = PTHREAD_RWLOCK_INITIALIZER;

static size_t live_slot(const void *p, size_t cap)
{
    uintptr_t v = (uintptr_t)p >> 4;
    return (size_t)((v * 0x9E3779B97F4A7C15ull) >> 7) & (cap - 1);
}

static void live_insert_locked(w32_object *o)
{
    size_t i;
    if ((s_live_used + 1) * 2 > s_live_cap) {
        size_t cap = s_live_cap ? s_live_cap * 2 : 1024, j;
        w32_object **old = s_live, **grown = (w32_object **)calloc(cap, sizeof *grown);
        for (j = 0; j < s_live_cap; j++)
            if (old[j] && old[j] != W32_TOMB) {
                i = live_slot(old[j], cap);
                while (grown[i]) i = (i + 1) & (cap - 1);
                grown[i] = old[j];
            }
        s_live_used = 0;
        for (j = 0; j < cap; j++) s_live_used += grown[j] != NULL;
        free(old);
        s_live = grown;
        s_live_cap = cap;
    }
    i = live_slot(o, s_live_cap);
    while (s_live[i] && s_live[i] != W32_TOMB) i = (i + 1) & (s_live_cap - 1);
    if (!s_live[i]) s_live_used++;
    s_live[i] = o;
}

static size_t live_find_locked(const void *h)
{
    size_t i, n;
    if (!s_live_cap) return (size_t)-1;
    for (i = live_slot(h, s_live_cap), n = 0; s_live[i] && n < s_live_cap;
         i = (i + 1) & (s_live_cap - 1), n++)
        if (s_live[i] == h) return i;
    return (size_t)-1;
}

/* The object behind h, or NULL (ERROR_INVALID_HANDLE) when h is not one. */
static w32_object *obj_lookup(HANDLE h)
{
    size_t i;
    if (!h || h == INVALID_HANDLE_VALUE) return NULL;
    if ((w32_object *)h == &s_process_heap) return &s_process_heap;
    pthread_rwlock_rdlock(&s_live_lock);
    i = live_find_locked(h);
    pthread_rwlock_unlock(&s_live_lock);
    if (i == (size_t)-1) {
        SetLastError(ERROR_INVALID_HANDLE);
        return NULL;
    }
    return (w32_object *)h;
}

static w32_object *obj_alloc(w32_kind kind)
{
    w32_object *o = (w32_object *)calloc(1, sizeof(w32_object));
    if (!o) return NULL;
    o->kind     = kind;
    o->refcount = 1;
    pthread_mutex_init(&o->lock, NULL);
    pthread_cond_init(&o->cond, NULL);
#if defined(__linux__)
    pthread_condattr_t waitable_attr;
    pthread_condattr_init(&waitable_attr);
    o->waitable_cond_monotonic = pthread_condattr_setclock(&waitable_attr, CLOCK_MONOTONIC) == 0;
    pthread_cond_init(&o->waitable_cond, o->waitable_cond_monotonic ? &waitable_attr : NULL);
    pthread_condattr_destroy(&waitable_attr);
#else
    pthread_cond_init(&o->waitable_cond, NULL);
#endif
    pthread_cond_init(&o->gate, NULL);
    pthread_rwlock_wrlock(&s_live_lock);
    live_insert_locked(o);
    pthread_rwlock_unlock(&s_live_lock);
    return o;
}

static void share_forget(w32_object *o);

static void obj_release(w32_object *o)
{
    if (InterlockedDecrement(&o->refcount) > 0)
        return;
    if (o->kind == K_FILE) share_forget(o);
    {
        size_t i;
        pthread_rwlock_wrlock(&s_live_lock);
        i = live_find_locked(o);
        if (i != (size_t)-1) s_live[i] = W32_TOMB;
        pthread_rwlock_unlock(&s_live_lock);
    }
    if (o->kind == K_FILE) {
        if (o->fd >= 0) close(o->fd);
        free(o->file_path);
    } else if (o->kind == K_FILEMAP) {
        if (o->fd >= 0) close(o->fd);
    } else if (o->kind == K_WAITABLE_TIMER) {
        /* Waitable timers: no special cleanup needed */
    }
    pthread_mutex_destroy(&o->lock);
    pthread_cond_destroy(&o->cond);
    pthread_cond_destroy(&o->waitable_cond);
    pthread_cond_destroy(&o->gate);
    free(o);
}

static void apc_context_destroy(void *value)
{
    w32_object *o = (w32_object *)value;
    if (!o) return;
    cancel_thread_timers(o);
    obj_release(o);
}

static void make_apc_context_key(void)
{
    pthread_key_create(&s_apc_context_key, apc_context_destroy);
}

static w32_object *current_apc_object(void)
{
    if (t_self_obj) return t_self_obj;
    pthread_once(&s_apc_context_once, make_apc_context_key);
    w32_object *o = pthread_getspecific(s_apc_context_key);
    if (!o) {
        o = obj_alloc(K_THREAD);
        if (!o) return NULL;
        o->tid = GetCurrentThreadId();
        if (pthread_setspecific(s_apc_context_key, o) != 0) {
            obj_release(o);
            return NULL;
        }
    }
    return o;
}

static void timer_remove_apcs_locked(w32_object *timer, w32_object *owner)
{
    if (!owner) return;
    pthread_mutex_lock(&owner->lock);
    for (int i = 0; i < owner->apc_count;) {
        w32_apc *entry = &owner->apcs[i];
        if (entry->kind == W32_APC_TIMER && entry->timer == timer) {
            memmove(entry, entry + 1,
                    (size_t)(owner->apc_count - i - 1) * sizeof(*entry));
            owner->apc_count--;
            obj_release(timer); /* queued APC reference; caller still owns one */
        } else {
            i++;
        }
    }
    pthread_mutex_unlock(&owner->lock);
}

static void timer_register_locked(w32_object *timer)
{
    if (timer->waitable_registered || !timer->waitable_armed) return;
    timer->waitable_next = s_waitable_registry;
    s_waitable_registry = timer;
    timer->waitable_registered = 1;
    InterlockedIncrement(&timer->refcount); /* registry reference */
}

static int timer_unlink_locked(w32_object *timer)
{
    w32_object **link = &s_waitable_registry;
    while (*link && *link != timer) link = &(*link)->waitable_next;
    if (!*link) return 0;
    *link = timer->waitable_next;
    timer->waitable_next = NULL;
    timer->waitable_registered = 0;
    return 1;
}

static int timer_refresh_locked(w32_object *timer)
{
    if (!timer->waitable_armed || timer->waitable_triggered) return timer->waitable_triggered;
    struct timespec now;
    clock_gettime(timer->waitable_clock, &now);
    if (timespec_before(&now, &timer->waitable_due_time)) return 0;
    timer->waitable_triggered = 1;
    return 1;
}

/* Both ordinary object waits and waitable-timer waits share timer state. */
static void timer_notify_locked(w32_object *timer)
{
    pthread_cond_broadcast(&timer->cond);
    pthread_cond_broadcast(&timer->waitable_cond);
}

static void timer_add_period(struct timespec *due, LONG period, const struct timespec *now)
{
    int64_t behind_sec = (int64_t)now->tv_sec - (int64_t)due->tv_sec;
    int64_t behind_ms = behind_sec * 1000 + (now->tv_nsec - due->tv_nsec) / 1000000;
    int64_t steps = behind_ms >= 0 ? behind_ms / period + 1 : 1;
    int64_t advance_ms = steps * period;
    due->tv_sec += (time_t)(advance_ms / 1000);
    due->tv_nsec += (long)(advance_ms % 1000) * 1000000L;
    if (due->tv_nsec >= 1000000000L) { due->tv_sec++; due->tv_nsec -= 1000000000L; }
}

/* Registry is always acquired before timer/APC locks. Never call while holding an object lock. */
static void service_waitable_timers(void)
{
    pthread_mutex_lock(&s_waitable_registry_lock);
    w32_object **link = &s_waitable_registry;
    while (*link) {
        w32_object *timer = *link;
        pthread_mutex_lock(&timer->lock);
        int expired = 0;
        if (timer->waitable_armed) {
            struct timespec now;
            clock_gettime(timer->waitable_clock, &now);
            expired = !timespec_before(&now, &timer->waitable_due_time);
        }
        if (expired) {
            timer->waitable_triggered = 1;
            /* A timer has at most one completion APC outstanding. Expiries
             * while that APC is queued are coalesced, not replayed later. */
            if (timer->waitable_completion && timer->waitable_apc_owner &&
                !timer->waitable_apc_queued && !timer->waitable_apc_due_pending) {
                timer->waitable_apc_due_pending = 1;
                struct timespec realtime;
                clock_gettime(CLOCK_REALTIME, &realtime);
                timer->waitable_apc_due =
                    ((LONGLONG)realtime.tv_sec + 11644473600LL) * 10000000LL +
                    realtime.tv_nsec / 100LL;
            }
            if (timer->waitable_period > 0) {
                struct timespec now;
                clock_gettime(timer->waitable_clock, &now);
                timer_add_period(&timer->waitable_due_time, timer->waitable_period, &now);
            } else {
                timer->waitable_armed = 0;
            }
        }
        if (timer->waitable_apc_due_pending && timer->waitable_completion &&
            timer->waitable_apc_owner && !timer->waitable_apc_queued) {
            w32_object *owner = timer->waitable_apc_owner;
            pthread_mutex_lock(&owner->lock);
            if (owner->apc_count < W32_MAX_APC) {
                w32_apc *entry = &owner->apcs[owner->apc_count++];
                memset(entry, 0, sizeof(*entry));
                entry->kind = W32_APC_TIMER;
                entry->timer = timer;
                entry->timer_generation = timer->waitable_generation;
                timer->waitable_apc_queued = 1;
                timer->waitable_apc_due_pending = 0;
                InterlockedIncrement(&timer->refcount);
                pthread_cond_broadcast(&owner->cond);
            }
            pthread_mutex_unlock(&owner->lock);
        }
        timer_notify_locked(timer);
        if (!timer->waitable_armed && !timer->waitable_apc_queued &&
            !timer->waitable_apc_due_pending) {
            *link = timer->waitable_next;
            timer->waitable_next = NULL;
            timer->waitable_registered = 0;
            pthread_mutex_unlock(&timer->lock);
            obj_release(timer); /* registry reference, still under registry lock */
        } else {
            pthread_mutex_unlock(&timer->lock);
            link = &timer->waitable_next;
        }
    }
    pthread_mutex_unlock(&s_waitable_registry_lock);
}

static void cancel_thread_timers(w32_object *thread)
{
    pthread_mutex_lock(&s_waitable_registry_lock);
    w32_object **link = &s_waitable_registry;
    while (*link) {
        w32_object *timer = *link;
        pthread_mutex_lock(&timer->lock);
        if (timer->waitable_apc_owner == thread && timer->waitable_completion) {
            timer_refresh_locked(timer);
            timer->waitable_armed = 0;
            timer->waitable_completion = NULL;
            timer->waitable_apc_owner = NULL;
            timer->waitable_apc_queued = 0;
            timer->waitable_apc_due_pending = 0;
            timer->waitable_generation++;
            timer_remove_apcs_locked(timer, thread);
            timer_notify_locked(timer);
        }
        if (!timer->waitable_armed && !timer->waitable_apc_queued &&
            !timer->waitable_apc_due_pending) {
            *link = timer->waitable_next;
            timer->waitable_next = NULL;
            timer->waitable_registered = 0;
            pthread_mutex_unlock(&timer->lock);
            obj_release(timer); /* registry reference, still under registry lock */
        } else {
            pthread_mutex_unlock(&timer->lock);
            link = &timer->waitable_next;
        }
    }
    pthread_mutex_unlock(&s_waitable_registry_lock);
}

/* ---- fd-backed file handle (for the file-I/O HLE) -------------------- */
HANDLE w32_open_handle(int fd, const char *host_path)
{
    w32_object *o = obj_alloc(K_FILE);
    o->fd        = fd;
    o->file_path = host_path ? strdup(host_path) : NULL;
    return (HANDLE)o;
}

int w32_handle_fd(HANDLE h)
{
    w32_object *o = obj_lookup(h);
    return (o && o->kind == K_FILE) ? o->fd : -1;
}

const char *w32_handle_path(HANDLE h)
{
    w32_object *o = obj_lookup(h);
    return (o && o->kind == K_FILE) ? o->file_path : NULL;
}

BOOL CloseHandle(HANDLE h)
{
    if (!h || h == PSEUDO_CURRENT_THREAD || h == PSEUDO_CURRENT_PROCESS ||
        h == INVALID_HANDLE_VALUE)
        return TRUE;
    w32_object *o = obj_lookup(h);
    if (!o) return FALSE;
    if (o->kind == K_WAITABLE_TIMER) {
        int drop_registry = 0;
        pthread_mutex_lock(&s_waitable_registry_lock);
        pthread_mutex_lock(&o->lock);
        if (--o->waitable_handle_count == 0) {
            if (!o->waitable_completion) {
                timer_refresh_locked(o);
                o->waitable_armed = 0;
                o->waitable_generation++;
                o->waitable_apc_queued = 0;
                o->waitable_apc_due_pending = 0;
                drop_registry = timer_unlink_locked(o);
            }
        }
        timer_notify_locked(o);
        pthread_mutex_unlock(&o->lock);
        pthread_mutex_unlock(&s_waitable_registry_lock);
        if (drop_registry) obj_release(o);
    }
    if (o != &s_process_heap) obj_release(o);
    return TRUE;
}

BOOL DuplicateHandle(HANDLE srcProc, HANDLE src, HANDLE dstProc, PHANDLE dst,
                     DWORD access, BOOL inherit, DWORD options)
{
    (void)srcProc; (void)dstProc; (void)access; (void)inherit;
    if (!dst) return FALSE;
    if (src == PSEUDO_CURRENT_THREAD)  src = GetCurrentThread();
    /* Win32 returns a real process handle here, never the -1 pseudo-handle:
     * -1 is INVALID_HANDLE_VALUE to callers that store the result. */
    if (src == PSEUDO_CURRENT_PROCESS) { *dst = (HANDLE)obj_alloc(K_PROCESS); return TRUE; }
    if (src == PSEUDO_CURRENT_THREAD || !src) { *dst = src; return TRUE; }
    w32_object *o = obj_lookup(src);
    if (!o) return FALSE;
    if (o->kind == K_WAITABLE_TIMER) {
        pthread_mutex_lock(&s_waitable_registry_lock);
        o->waitable_handle_count++;
        InterlockedIncrement(&o->refcount);
        pthread_mutex_unlock(&s_waitable_registry_lock);
    } else {
        InterlockedIncrement(&o->refcount);
    }
    *dst = src;
    if (options & DUPLICATE_CLOSE_SOURCE)
        CloseHandle(src);
    return TRUE;
}

/* ---- deadline helper -------------------------------------------------- */
static void deadline_from_ms(DWORD ms, struct timespec *ts)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec  += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

/* Run pending user/timer APCs for the calling thread. Returns count run. */
static int drain_apcs(void)
{
    w32_object *o = current_apc_object();
    int run = 0;
    if (!o) return 0;
    pthread_mutex_lock(&o->lock);
    while (o->apc_count > 0) {
        w32_apc entry = o->apcs[0];
        memmove(o->apcs, o->apcs + 1, sizeof(w32_apc) * (o->apc_count - 1));
        o->apc_count--;
        pthread_mutex_unlock(&o->lock);
        if (entry.kind == W32_APC_USER) {
            if (entry.user_func) entry.user_func(entry.user_data);
        } else {
            w32_object *timer = entry.timer;
            PTIMERAPCROUTINE callback = NULL;
            PVOID arg = NULL;
            DWORD low = 0, high = 0;
            pthread_mutex_lock(&timer->lock);
            if (timer->waitable_generation == entry.timer_generation &&
                timer->waitable_apc_owner == o && timer->waitable_completion &&
                timer->waitable_apc_queued) {
                callback = timer->waitable_completion;
                arg = timer->waitable_arg;
                low = (DWORD)(ULONGLONG)timer->waitable_apc_due;
                high = (DWORD)((ULONGLONG)timer->waitable_apc_due >> 32);
                timer->waitable_apc_queued = 0;
                if (!timer->waitable_armed && timer->waitable_period == 0) {
                    timer->waitable_completion = NULL;
                    timer->waitable_apc_owner = NULL;
                }
            }
            pthread_mutex_unlock(&timer->lock);
            if (callback) callback(arg, low, high);
            obj_release(timer); /* queued APC reference */
            service_waitable_timers();
        }
        run++;
        pthread_mutex_lock(&o->lock);
    }
    pthread_mutex_unlock(&o->lock);
    return run;
}

static int timespec_before(const struct timespec *a, const struct timespec *b)
{
    return a->tv_sec != b->tv_sec ? a->tv_sec < b->tv_sec : a->tv_nsec < b->tv_nsec;
}

/* Signalled once the due time passes; latched, so a manual-reset timer stays
 * signalled until it is set or cancelled again. Caller holds o->lock. */
static int waitable_due(w32_object *o)
{
    return timer_refresh_locked(o);
}

static int object_ready_locked(w32_object *o)
{
    switch (o->kind) {
    case K_EVENT: return o->signaled;
    case K_THREAD: return o->exited;
    case K_SEM: return o->sem_count > 0;
    case K_MUTEX: return o->mtx_owner == 0 || o->mtx_owner == GetCurrentThreadId();
    case K_WAITABLE_TIMER: return waitable_due(o);
    case K_PROCESS: return 0;
    default: return 1;
    }
}

static void object_consume_locked(w32_object *o)
{
    switch (o->kind) {
    case K_EVENT: if (!o->manual_reset) o->signaled = 0; break;
    case K_WAITABLE_TIMER:
        if (!o->waitable_manual_reset) o->waitable_triggered = 0;
        break;
    case K_SEM: o->sem_count--; break;
    case K_MUTEX: o->mtx_owner = GetCurrentThreadId(); o->mtx_recursion++; break;
    default: break;
    }
}

/*
 * Wait on a single object. The object lock must NOT be held.
 * Returns WAIT_OBJECT_0 / WAIT_TIMEOUT.
 */
static DWORD wait_single_impl(w32_object *o, DWORD ms, int consume)
{
    struct timespec ts = {0, 0};
    int timed = (ms != INFINITE);
    int timer_wait = o->kind == K_WAITABLE_TIMER;
    clockid_t timeout_clock = timer_wait && o->waitable_cond_monotonic
                            ? CLOCK_MONOTONIC : CLOCK_REALTIME;
    if (timed) {
        clock_gettime(timeout_clock, &ts);
        ts.tv_sec += ms / 1000;
        ts.tv_nsec += (long)(ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    }

    for (;;) {
        service_waitable_timers();
        pthread_mutex_lock(&o->lock);
        int ready = object_ready_locked(o);
        if (ready) {
            if (consume) object_consume_locked(o);
            pthread_mutex_unlock(&o->lock);
            service_waitable_timers();
            return WAIT_OBJECT_0;
        }
        struct timespec now;
        clock_gettime(timeout_clock, &now);
        if (timed && !timespec_before(&now, &ts)) {
            pthread_mutex_unlock(&o->lock);
            service_waitable_timers();
            return WAIT_TIMEOUT;
        }

        /* Timer/APC state is serviced outside object locks, so cap sleeps to
         * keep the deadline visible without introducing registry lock inversions. */
        struct timespec until = ts;
        int bounded = timed;
        int use_timer_deadline = timer_wait && o->waitable_armed &&
                                 o->waitable_clock == timeout_clock;
        if (timer_wait && o->waitable_armed && !use_timer_deadline) {
            struct timespec poll = now;
            poll.tv_nsec += 5000000L;
            if (poll.tv_nsec >= 1000000000L) { poll.tv_sec++; poll.tv_nsec -= 1000000000L; }
            if (!bounded || timespec_before(&poll, &until)) until = poll;
            bounded = 1;
        } else if (use_timer_deadline &&
                   (!bounded || timespec_before(&o->waitable_due_time, &until))) {
            until = o->waitable_due_time;
            bounded = 1;
        }
        int rc = cond_wait_cancel_safe(timer_wait ? &o->waitable_cond : &o->cond,
                                       &o->lock, bounded, &until);
        pthread_mutex_unlock(&o->lock);
        (void)rc;
    }
}

static DWORD wait_single(w32_object *o, DWORD ms)
{
    return wait_single_impl(o, ms, 1);
}

DWORD WaitForSingleObject(HANDLE h, DWORD ms)
{
    if (!h) {
        SetLastError(ERROR_INVALID_HANDLE);
        return WAIT_FAILED;
    }
    if (h == PSEUDO_CURRENT_THREAD || h == PSEUDO_CURRENT_PROCESS)
        return WAIT_OBJECT_0;
    {
        w32_object *o = obj_lookup(h);
        if (!o) return WAIT_FAILED;
        service_waitable_timers();
        return wait_single(o, ms);
    }
}

DWORD WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable)
{
    if (!alertable) return WaitForSingleObject(h, ms);
    service_waitable_timers();
    if (!h) {
        SetLastError(ERROR_INVALID_HANDLE);
        return WAIT_FAILED;
    }
    if (h == PSEUDO_CURRENT_THREAD || h == PSEUDO_CURRENT_PROCESS)
        return drain_apcs() > 0 ? WAIT_IO_COMPLETION : WAIT_OBJECT_0;
    struct timespec end = {0, 0};
    int timed = ms != INFINITE;
    if (timed) deadline_from_ms(ms, &end);
    w32_object *o = obj_lookup(h);
    if (!o) return WAIT_FAILED;
    for (;;) {
        service_waitable_timers();
        if (drain_apcs() > 0) return WAIT_IO_COMPLETION;
        if (wait_single_impl(o, 0, 0) == WAIT_OBJECT_0) {
            service_waitable_timers();
            if (drain_apcs() > 0) return WAIT_IO_COMPLETION;
            if (wait_single_impl(o, 0, 1) == WAIT_OBJECT_0) return WAIT_OBJECT_0;
        }
        DWORD slice = 5;
        if (timed) {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            if (!timespec_before(&now, &end)) return WAIT_TIMEOUT;
            int64_t left_ms = ((int64_t)end.tv_sec - now.tv_sec) * 1000 +
                              (end.tv_nsec - now.tv_nsec + 999999) / 1000000;
            if (left_ms < slice) slice = (DWORD)left_ms;
        }
        DWORD result = wait_single_impl(o, slice, 0);
        service_waitable_timers();
        if (drain_apcs() > 0) return WAIT_IO_COMPLETION;
        if (result == WAIT_OBJECT_0) {
            if (wait_single_impl(o, 0, 1) == WAIT_OBJECT_0) return WAIT_OBJECT_0;
        }
        if (timed) {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            if (!timespec_before(&now, &end)) return WAIT_TIMEOUT;
        }
    }
}

/*
 * WaitForMultipleObjects: polling implementation. Adequate for the light
 * multi-object waits the Xbox kernel HLE issues; not a high-throughput path.
 */
DWORD WaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL waitAll, DWORD ms)
{
    return WaitForMultipleObjectsEx(count, handles, waitAll, ms, FALSE);
}

DWORD WaitForMultipleObjectsEx(DWORD count, const HANDLE *handles, BOOL waitAll,
                               DWORD ms, BOOL alertable)
{
    if (!handles || count == 0 || count > MAXIMUM_WAIT_OBJECTS) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return WAIT_FAILED;
    }
    for (DWORD i = 0; i < count; i++) {
        if (!handles[i] ||
            (handles[i] != PSEUDO_CURRENT_THREAD &&
             handles[i] != PSEUDO_CURRENT_PROCESS && !obj_lookup(handles[i]))) {
            SetLastError(ERROR_INVALID_HANDLE);
            return WAIT_FAILED;
        }
        for (DWORD j = 0; j < i; j++) {
            if (handles[i] == handles[j]) {
                SetLastError(ERROR_INVALID_PARAMETER);
                return WAIT_FAILED;
            }
        }
    }

    struct timespec ts = {0, 0};
    int timed = (ms != INFINITE);
    if (timed) deadline_from_ms(ms, &ts);

    w32_object *ordered[MAXIMUM_WAIT_OBJECTS];
    if (waitAll) {
        for (DWORD i = 0; i < count; i++) {
            ordered[i] = (w32_object *)handles[i];
        }
        /* Insertion sort is bounded by MAXIMUM_WAIT_OBJECTS and gives every
         * wait-all call the same lock order, including reversed arrays. */
        for (DWORD i = 1; i < count; i++) {
            w32_object *item = ordered[i];
            DWORD j = i;
            while (j > 0 && (uintptr_t)ordered[j - 1] > (uintptr_t)item) {
                ordered[j] = ordered[j - 1];
                j--;
            }
            ordered[j] = item;
        }
    }

    for (;;) {
        service_waitable_timers();
        if (alertable && drain_apcs() > 0)
            return WAIT_IO_COMPLETION;

        if (waitAll) {
            DWORD locked = 0;
            for (; locked < count; locked++) {
                if (ordered[locked] == (w32_object *)PSEUDO_CURRENT_PROCESS ||
                    ordered[locked] == (w32_object *)PSEUDO_CURRENT_THREAD) continue;
                pthread_mutex_lock(&ordered[locked]->lock);
            }
            int ready = 1;
            for (DWORD i = 0; i < count; i++) {
                if (handles[i] == PSEUDO_CURRENT_PROCESS || handles[i] == PSEUDO_CURRENT_THREAD) continue;
                if (!object_ready_locked((w32_object *)handles[i])) { ready = 0; break; }
            }
            if (ready && alertable) {
                for (DWORD i = count; i > 0; i--) {
                    if (ordered[i - 1] == (w32_object *)PSEUDO_CURRENT_PROCESS ||
                        ordered[i - 1] == (w32_object *)PSEUDO_CURRENT_THREAD) continue;
                    pthread_mutex_unlock(&ordered[i - 1]->lock);
                }
                service_waitable_timers();
                if (drain_apcs() > 0) return WAIT_IO_COMPLETION;
                for (locked = 0; locked < count; locked++) {
                    if (ordered[locked] == (w32_object *)PSEUDO_CURRENT_PROCESS ||
                        ordered[locked] == (w32_object *)PSEUDO_CURRENT_THREAD) continue;
                    pthread_mutex_lock(&ordered[locked]->lock);
                }
                for (DWORD i = 0; i < count; i++) {
                    if (handles[i] == PSEUDO_CURRENT_PROCESS || handles[i] == PSEUDO_CURRENT_THREAD) continue;
                    if (!object_ready_locked((w32_object *)handles[i])) { ready = 0; break; }
                }
            }
            if (ready) {
                for (DWORD i = 0; i < count; i++) {
                    if (handles[i] == PSEUDO_CURRENT_PROCESS || handles[i] == PSEUDO_CURRENT_THREAD) continue;
                    object_consume_locked((w32_object *)handles[i]);
                }
            }
            for (DWORD i = count; i > 0; i--) {
                if (ordered[i - 1] == (w32_object *)PSEUDO_CURRENT_PROCESS ||
                    ordered[i - 1] == (w32_object *)PSEUDO_CURRENT_THREAD) continue;
                pthread_mutex_unlock(&ordered[i - 1]->lock);
            }
            if (ready) return WAIT_OBJECT_0;
        } else {
            for (DWORD i = 0; i < count; i++) {
                HANDLE h = handles[i];
                int ready = h == PSEUDO_CURRENT_PROCESS || h == PSEUDO_CURRENT_THREAD;
                if (!ready) ready = wait_single_impl((w32_object *)h, 0, 0) == WAIT_OBJECT_0;
                if (ready) {
                    service_waitable_timers();
                    if (alertable && drain_apcs() > 0) return WAIT_IO_COMPLETION;
                    if (h != PSEUDO_CURRENT_PROCESS && h != PSEUDO_CURRENT_THREAD &&
                        wait_single_impl((w32_object *)h, 0, 1) != WAIT_OBJECT_0)
                        continue;
                    return WAIT_OBJECT_0 + i;
                }
            }
        }

        if (timed) {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            if (now.tv_sec > ts.tv_sec ||
                (now.tv_sec == ts.tv_sec && now.tv_nsec >= ts.tv_nsec))
                return WAIT_TIMEOUT;
        }
        usleep(1000);
    }
}

/* ===================================================================== */
/* Events                                                                */
/* ===================================================================== */

HANDLE CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manualReset, BOOL initialState, LPCSTR name)
{
    (void)sa; (void)name;
    w32_object *o = obj_alloc(K_EVENT);
    o->manual_reset = manualReset ? 1 : 0;
    o->signaled     = initialState ? 1 : 0;
    return (HANDLE)o;
}
HANDLE CreateEventW(LPSECURITY_ATTRIBUTES sa, BOOL manualReset, BOOL initialState, LPCWSTR name)
{
    (void)name;
    return CreateEventA(sa, manualReset, initialState, NULL);
}

BOOL SetEvent(HANDLE h)
{
    w32_object *o = obj_lookup(h);
    if (!o || o->kind != K_EVENT) return FALSE;
    pthread_mutex_lock(&o->lock);
    o->signaled = 1;
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

BOOL ResetEvent(HANDLE h)
{
    w32_object *o = obj_lookup(h);
    if (!o || o->kind != K_EVENT) return FALSE;
    pthread_mutex_lock(&o->lock);
    o->signaled = 0;
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

BOOL PulseEvent(HANDLE h)
{
    w32_object *o = obj_lookup(h);
    if (!o || o->kind != K_EVENT) return FALSE;
    pthread_mutex_lock(&o->lock);
    o->signaled = 1;
    pthread_cond_broadcast(&o->cond);
    o->signaled = 0;
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

/* ===================================================================== */
/* Semaphores                                                            */
/* ===================================================================== */

HANDLE CreateSemaphoreA(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG maximum, LPCSTR name)
{
    (void)sa; (void)name;
    w32_object *o = obj_alloc(K_SEM);
    o->sem_count = initial;
    o->sem_max   = maximum;
    return (HANDLE)o;
}
HANDLE CreateSemaphoreW(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG maximum, LPCWSTR name)
{
    (void)name;
    return CreateSemaphoreA(sa, initial, maximum, NULL);
}

BOOL ReleaseSemaphore(HANDLE h, LONG releaseCount, PLONG previousCount)
{
    w32_object *o = obj_lookup(h);
    if (!o || o->kind != K_SEM) return FALSE;
    pthread_mutex_lock(&o->lock);
    if (previousCount) *previousCount = (LONG)o->sem_count;
    o->sem_count += releaseCount;
    if (o->sem_max && o->sem_count > o->sem_max) o->sem_count = o->sem_max;
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

/* ===================================================================== */
/* Mutexes                                                               */
/* ===================================================================== */

HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL initialOwner, LPCSTR name)
{
    (void)sa; (void)name;
    w32_object *o = obj_alloc(K_MUTEX);
    if (initialOwner) { o->mtx_owner = GetCurrentThreadId(); o->mtx_recursion = 1; }
    return (HANDLE)o;
}
HANDLE CreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL initialOwner, LPCWSTR name)
{
    (void)name;
    return CreateMutexA(sa, initialOwner, NULL);
}

BOOL ReleaseMutex(HANDLE h)
{
    w32_object *o = obj_lookup(h);
    if (!o || o->kind != K_MUTEX) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    pthread_mutex_lock(&o->lock);
    if (o->mtx_owner != GetCurrentThreadId()) {
        pthread_mutex_unlock(&o->lock);
        SetLastError(ERROR_NOT_OWNER);
        return FALSE;
    }
    if (--o->mtx_recursion <= 0) {
        o->mtx_owner = 0;
        o->mtx_recursion = 0;
        pthread_cond_broadcast(&o->cond);
    }
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

/* ===================================================================== */
/* Threads                                                               */
/* ===================================================================== */

static void thread_finish(void *arg)
{
    w32_object *o = (w32_object *)arg;
    cancel_thread_timers(o);
    pthread_mutex_lock(&o->lock);
    if (!o->exited) {
        o->exited = 1;
        o->signaled = 1;
        pthread_cond_broadcast(&o->cond);
    }
    pthread_mutex_unlock(&o->lock);
    t_self_obj = NULL;
    t_tid = 0;
    obj_release(o); /* exactly one worker reference, for return or cancellation */
}

static void *thread_trampoline(void *arg)
{
    w32_object *o = (w32_object *)arg;
    pthread_cleanup_push(thread_finish, o);
    t_self_obj = o;
    t_tid      = o->tid;

    /* CREATE_SUSPENDED gate */
    pthread_mutex_lock(&o->lock);
    pthread_cleanup_push(unlock_mutex_cleanup, &o->lock);
    while (o->suspend_count > 0)
        pthread_cond_wait(&o->gate, &o->lock);
    pthread_cleanup_pop(0);
    pthread_mutex_unlock(&o->lock);

    DWORD rc = o->start ? o->start(o->start_param) : 0;
    pthread_mutex_lock(&o->lock);
    if (!o->thread_termination_requested && !o->exited)
        o->exit_code = rc;
    pthread_mutex_unlock(&o->lock);

    pthread_cleanup_pop(1);
    return NULL;
}

HANDLE CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stackSize,
                    LPTHREAD_START_ROUTINE start, LPVOID param,
                    DWORD flags, LPDWORD threadId)
{
    (void)sa;
    w32_object *o = obj_alloc(K_THREAD);
    if (!o) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    o->start         = start;
    o->start_param   = param;
    o->tid           = (DWORD)InterlockedIncrement(&s_next_tid);
    o->suspend_count = (flags & CREATE_SUSPENDED) ? 1 : 0;
    o->refcount      = 2;   /* one for caller, one for the trampoline */

    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        o->refcount = 1;
        obj_release(o);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) != 0) {
        pthread_attr_destroy(&attr);
        o->refcount = 1;
        obj_release(o);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    if (stackSize)
        pthread_attr_setstacksize(&attr, stackSize < 65536 ? 65536 : stackSize);

    if (pthread_create(&o->thread, &attr, thread_trampoline, o) != 0) {
        pthread_attr_destroy(&attr);
        o->refcount = 1;
        obj_release(o);
        SetLastError(8 /* ERROR_NOT_ENOUGH_MEMORY */);
        return NULL;
    }
    pthread_attr_destroy(&attr);

    if (threadId) *threadId = o->tid;
    return (HANDLE)o;
}

VOID ExitThread(DWORD exitCode)
{
    w32_object *o = t_self_obj;
    if (o) {
        pthread_mutex_lock(&o->lock);
        if (!o->thread_termination_requested && !o->exited)
            o->exit_code = exitCode;
        pthread_mutex_unlock(&o->lock);
    }
    pthread_exit(NULL);
}

BOOL GetExitCodeThread(HANDLE h, LPDWORD exitCode)
{
    w32_object *o = obj_lookup(h);
    if (!o || o->kind != K_THREAD || !exitCode) return FALSE;
    pthread_mutex_lock(&o->lock);
    *exitCode = o->exited ? o->exit_code : STILL_ACTIVE;
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

DWORD ResumeThread(HANDLE h)
{
    w32_object *o = obj_lookup(h);
    if (!o || o->kind != K_THREAD) return (DWORD)-1;
    pthread_mutex_lock(&o->lock);
    DWORD prev = (DWORD)o->suspend_count;
    if (o->suspend_count > 0 && --o->suspend_count == 0)
        pthread_cond_broadcast(&o->gate);
    pthread_mutex_unlock(&o->lock);
    return prev;
}

DWORD SuspendThread(HANDLE h)
{
    /* True mid-run suspension is not supported on POSIX; only the
     * CREATE_SUSPENDED start gate is. Track the count for ResumeThread. */
    w32_object *o = obj_lookup(h);
    if (!o || o->kind != K_THREAD) return (DWORD)-1;
    pthread_mutex_lock(&o->lock);
    DWORD prev = (DWORD)o->suspend_count;
    o->suspend_count++;
    pthread_mutex_unlock(&o->lock);
    return prev;
}

BOOL TerminateThread(HANDLE h, DWORD exitCode)
{
    if (h == PSEUDO_CURRENT_THREAD) {
        ExitThread(exitCode);
        return TRUE;
    }
    w32_object *o = obj_lookup(h);
    if (!o || o->kind != K_THREAD) return FALSE;
    pthread_mutex_lock(&o->lock);
    if (o->exited) {
        pthread_mutex_unlock(&o->lock);
        return TRUE;
    }
    int rc = pthread_cancel(o->thread);
    if (rc == 0) {
        o->exit_code = exitCode;
        o->thread_termination_requested = 1;
    }
    pthread_mutex_unlock(&o->lock);
    return rc == 0;
}

BOOL SetThreadPriority(HANDLE h, int priority)
{
    w32_object *o = (h == PSEUDO_CURRENT_THREAD) ? t_self_obj : obj_lookup(h);
    if (o && o->kind == K_THREAD) o->priority = priority;
    return TRUE;   /* real RT priorities need privileges; tracked only */
}

int GetThreadPriority(HANDLE h)
{
    w32_object *o = (h == PSEUDO_CURRENT_THREAD) ? t_self_obj : obj_lookup(h);
    return (o && o->kind == K_THREAD) ? o->priority : THREAD_PRIORITY_NORMAL;
}

VOID SwitchToThread(void) { sched_yield(); }
VOID YieldProcessor(void) { sched_yield(); }

DWORD QueueUserAPC(PAPCFUNC func, HANDLE thread, ULONG_PTR data)
{
    w32_object *o = (thread == PSEUDO_CURRENT_THREAD) ? current_apc_object() : obj_lookup(thread);
    if (!o || o->kind != K_THREAD || !func) return 0;
    pthread_mutex_lock(&o->lock);
    DWORD ok = 0;
    if (o->apc_count < W32_MAX_APC) {
        w32_apc *entry = &o->apcs[o->apc_count];
        memset(entry, 0, sizeof(*entry));
        entry->kind = W32_APC_USER;
        entry->user_func = func;
        entry->user_data = data;
        o->apc_count++;
        ok = 1;
    }
    pthread_mutex_unlock(&o->lock);
    return ok;
}

/* ===================================================================== */
/* Sleep                                                                 */
/* ===================================================================== */

VOID Sleep(DWORD ms)
{
    if (ms == 0) { sched_yield(); service_waitable_timers(); return; }
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { }
    service_waitable_timers();
}

DWORD SleepEx(DWORD ms, BOOL alertable)
{
    if (!alertable) { Sleep(ms); return 0; }
    if (drain_apcs() > 0) return WAIT_IO_COMPLETION;
    struct timespec end;
    int timed = ms != INFINITE;
    if (timed) deadline_from_ms(ms, &end);
    for (;;) {
        service_waitable_timers();
        if (drain_apcs() > 0) return WAIT_IO_COMPLETION;
        int64_t left_ns = 5000000LL;
        if (timed) {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            if (!timespec_before(&now, &end)) return 0;
            left_ns = ((int64_t)end.tv_sec - now.tv_sec) * 1000000000LL +
                      end.tv_nsec - now.tv_nsec;
            if (left_ns > 5000000LL) left_ns = 5000000LL;
        }
        struct timespec delay = { (time_t)(left_ns / 1000000000LL), (long)(left_ns % 1000000000LL) };
        while (nanosleep(&delay, &delay) == -1 && errno == EINTR) { }
    }
}

/* ===================================================================== */
/* Timer queues (one helper thread per timer)                            */
/* ===================================================================== */

static void *timer_thread(void *arg)
{
    w32_object *o = (w32_object *)arg;
    int once = (o->timer_period == 0);

    /* initial due time */
    if (o->timer_due) Sleep(o->timer_due);
    if (!o->timer_cancel && o->timer_cb)
        o->timer_cb(o->timer_param, TRUE);

    while (!once && !o->timer_cancel) {
        Sleep(o->timer_period);
        if (o->timer_cancel) break;
        if (o->timer_cb) o->timer_cb(o->timer_param, TRUE);
    }
    obj_release(o);
    return NULL;
}

HANDLE CreateTimerQueue(void)
{
    /* A timer queue is just a grouping token here. */
    return (HANDLE)obj_alloc(K_TIMER);
}

BOOL DeleteTimerQueue(HANDLE timerQueue)
{
    return CloseHandle(timerQueue);
}

BOOL CreateTimerQueueTimer(PHANDLE newTimer, HANDLE timerQueue,
                           WAITORTIMERCALLBACK callback, PVOID param,
                           DWORD dueTime, DWORD period, ULONG flags)
{
    (void)timerQueue;
    if (flags & WT_EXECUTEONLYONCE) period = 0;
    w32_object *o = obj_alloc(K_TIMER);
    o->timer_cb     = callback;
    o->timer_param  = param;
    o->timer_due    = dueTime;
    o->timer_period = period;
    o->refcount     = 2;   /* caller + timer thread */

    if (pthread_create(&o->thread, NULL, timer_thread, o) != 0) {
        o->refcount = 1;
        obj_release(o);
        return FALSE;
    }
    pthread_detach(o->thread);
    if (newTimer) *newTimer = (HANDLE)o;
    return TRUE;
}

BOOL ChangeTimerQueueTimer(HANDLE timerQueue, HANDLE timer, ULONG dueTime, ULONG period)
{
    (void)timerQueue;
    w32_object *o = (w32_object *)timer;
    if (!o || o->kind != K_TIMER) return FALSE;
    o->timer_due    = dueTime;
    o->timer_period = period;
    return TRUE;
}

BOOL DeleteTimerQueueTimer(HANDLE timerQueue, HANDLE timer, HANDLE completionEvent)
{
    (void)timerQueue;
    w32_object *o = (w32_object *)timer;
    if (!o || o->kind != K_TIMER) return FALSE;
    o->timer_cancel = 1;
    if (completionEvent) SetEvent(completionEvent);
    obj_release(o);
    return TRUE;
}

struct w32_tp_args { PTP_SIMPLE_CALLBACK cb; PVOID ctx; };

static void *w32_tp_trampoline(void *arg)
{
    struct w32_tp_args *a = (struct w32_tp_args *)arg;
    a->cb(NULL, a->ctx);
    free(a);
    return NULL;
}

BOOL TrySubmitThreadpoolCallback(PTP_SIMPLE_CALLBACK callback,
                                 PVOID context, PVOID env)
{
    (void)env;
    /* Run on a throwaway detached thread. */
    struct w32_tp_args *a = (struct w32_tp_args *)malloc(sizeof(*a));
    a->cb = callback; a->ctx = context;
    pthread_t th;
    if (pthread_create(&th, NULL, w32_tp_trampoline, a) != 0) { free(a); return FALSE; }
    pthread_detach(th);
    return TRUE;
}

/* ===================================================================== */
/* Waitable timers                                                       */
/* ===================================================================== */

HANDLE CreateWaitableTimerW(LPSECURITY_ATTRIBUTES sa, BOOL manualReset, LPCWSTR name)
{
    (void)sa;
    (void)name;
    w32_object *o = obj_alloc(K_WAITABLE_TIMER);
    o->waitable_manual_reset = manualReset;
    o->waitable_handle_count = 1;
    return (HANDLE)o;
}

BOOL SetWaitableTimer(HANDLE h, const LARGE_INTEGER *dueTime, LONG period,
                      PTIMERAPCROUTINE completion, PVOID arg, BOOL resume)
{
    w32_object *o = obj_lookup(h);
    if (!o || o->kind != K_WAITABLE_TIMER || !dueTime) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (period < 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    LONGLONG value = dueTime->QuadPart;
    clockid_t clock = value < 0 ? CLOCK_MONOTONIC : CLOCK_REALTIME;
    struct timespec deadline;
    if (value < 0) {
        uint64_t ticks = (uint64_t)(-(value + 1)) + 1;
        uint64_t seconds = ticks / 10000000ULL;
        uint64_t remainder = ticks % 10000000ULL;
        clock_gettime(clock, &deadline);
        deadline.tv_sec += (time_t)seconds;
        deadline.tv_nsec += (long)(remainder * 100ULL);
        if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    } else {
        /* FILETIME epoch is 1601-01-01; Unix is 1970-01-01. */
        LONGLONG unix100ns = value - 116444736000000000LL;
        LONGLONG seconds = unix100ns / 10000000LL;
        LONGLONG remainder = unix100ns % 10000000LL;
        if (remainder < 0) { seconds--; remainder += 10000000LL; }
        deadline.tv_sec = (time_t)seconds;
        deadline.tv_nsec = (long)(remainder * 100LL);
    }
    w32_object *owner = completion ? current_apc_object() : NULL;
    if (completion && !owner) {
        SetLastError(8 /* ERROR_NOT_ENOUGH_MEMORY */);
        return FALSE;
    }

    pthread_mutex_lock(&s_waitable_registry_lock);
    pthread_mutex_lock(&o->lock);
    timer_refresh_locked(o);
    timer_remove_apcs_locked(o, o->waitable_apc_owner);
    o->waitable_apc_queued = 0;
    o->waitable_apc_due_pending = 0;
    o->waitable_generation++;
    o->waitable_clock = clock;
    o->waitable_due_time = deadline;
    o->waitable_period = period;
    o->waitable_completion = completion;
    o->waitable_arg = arg;
    o->waitable_apc_due = value;
    o->waitable_apc_owner = owner;
    o->waitable_armed = 1;
    o->waitable_triggered = 0;
    timer_register_locked(o);
    timer_notify_locked(o);
    pthread_mutex_unlock(&o->lock);
    pthread_mutex_unlock(&s_waitable_registry_lock);
    if (resume) SetLastError(ERROR_NOT_SUPPORTED);
    return TRUE;
}

BOOL CancelWaitableTimer(HANDLE h)
{
    w32_object *o = obj_lookup(h);
    if (!o || o->kind != K_WAITABLE_TIMER) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    pthread_mutex_lock(&s_waitable_registry_lock);
    pthread_mutex_lock(&o->lock);
    timer_refresh_locked(o);
    o->waitable_armed = 0;
    o->waitable_generation++;
    timer_remove_apcs_locked(o, o->waitable_apc_owner);
    o->waitable_apc_queued = 0;
    o->waitable_apc_due_pending = 0;
    o->waitable_completion = NULL;
    o->waitable_apc_owner = NULL;
    int drop_registry = timer_unlink_locked(o);
    timer_notify_locked(o);
    pthread_mutex_unlock(&o->lock);
    pthread_mutex_unlock(&s_waitable_registry_lock);
    if (drop_registry) obj_release(o);
    return TRUE;
}
/* ===================================================================== */
/* Heap (thin wrapper over malloc; the single process heap)              */
/* ===================================================================== */

static w32_object s_process_heap = { .kind = K_HEAP };   /* never released */

HANDLE GetProcessHeap(void)                       { return (HANDLE)&s_process_heap; }
HANDLE HeapCreate(DWORD o, SIZE_T i, SIZE_T m)    { (void)o;(void)i;(void)m; return (HANDLE)&s_process_heap; }
BOOL   HeapDestroy(HANDLE h)                      { (void)h; return TRUE; }

LPVOID HeapAlloc(HANDLE heap, DWORD flags, SIZE_T bytes)
{
    (void)heap;
    return (flags & HEAP_ZERO_MEMORY) ? calloc(1, bytes ? bytes : 1)
                                      : malloc(bytes ? bytes : 1);
}
LPVOID HeapReAlloc(HANDLE heap, DWORD flags, LPVOID mem, SIZE_T bytes)
{
    (void)heap; (void)flags;
    return realloc(mem, bytes ? bytes : 1);
}
BOOL HeapFree(HANDLE heap, DWORD flags, LPVOID mem)
{
    (void)heap; (void)flags;
    free(mem);
    return TRUE;
}
SIZE_T HeapSize(HANDLE heap, DWORD flags, LPCVOID mem)
{
    (void)heap; (void)flags; (void)mem;
    return 0;   /* glibc malloc_usable_size could be used; not needed yet */
}

/* ===================================================================== */
/* Virtual memory                                                        */
/* ===================================================================== */

/* Win32 page protection to host PROT_*. PAGE_NOACCESS must stay PROT_NONE
 * (0): a region reserved with it has to fault on access, and the Xbox memory
 * model relies on that for the slots above the mapped range. An unknown value
 * is ERROR_INVALID_PARAMETER, as on Win32, and returns -1 rather than a guess. */
static int prot_from_page(DWORD protect)
{
    if (protect & ~0xFFu) return -1;
    switch (protect & 0xFF) {
    case PAGE_NOACCESS:          return PROT_NONE;
    case PAGE_READONLY:          return PROT_READ;
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:         return PROT_READ | PROT_WRITE;
    case PAGE_EXECUTE:           return PROT_EXEC;
    case PAGE_EXECUTE_READ:      return PROT_READ | PROT_EXEC;
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY: return PROT_READ | PROT_WRITE | PROT_EXEC;
    default:                     return -1;
    }
}

#if defined(__APPLE__)
/* Darwin has no MAP_FIXED_NOREPLACE, and the two mmap options are both wrong
 * for VirtualAlloc: MAP_FIXED silently unmaps whatever already occupies the
 * range, and a bare address hint can be relocated by the kernel for reasons
 * other than the range being taken -- so "we got a different address" is only
 * an approximation of "it was occupied", and a racy one.
 *
 * mach_vm_map with VM_FLAGS_FIXED is the exact primitive: it maps at the
 * address given, and returns KERN_NO_SPACE rather than displacing an existing
 * mapping. That is what Win32 promises, and this layer exists to keep the Xbox
 * HLE above it honest -- a VirtualAlloc that quietly replaced a live mapping
 * would corrupt whatever held it, far from the call that did it.
 *
 * Memory from mach_vm_map is released by munmap like any other, because the
 * BSD and Mach halves of Darwin share one VM map, so VirtualFree is unchanged.
 */
static void *mach_map_fixed(void *address, size_t size, int prot)
{
    mach_vm_address_t addr = (mach_vm_address_t)(uintptr_t)address;
    mach_vm_size_t len = (size + vm_page_size - 1) & ~((mach_vm_size_t)vm_page_size - 1);
    vm_prot_t vmprot = VM_PROT_NONE;

    if (prot & PROT_READ)  vmprot |= VM_PROT_READ;
    if (prot & PROT_WRITE) vmprot |= VM_PROT_WRITE;
    if (prot & PROT_EXEC)  vmprot |= VM_PROT_EXECUTE;

    kern_return_t kr = mach_vm_map(
        mach_task_self(),
        &addr,
        len,
        0,
        VM_FLAGS_FIXED,
        MEMORY_OBJECT_NULL,
        0,
        FALSE,
        vmprot,
        VM_PROT_ALL,
        VM_INHERIT_DEFAULT
    );
    if (kr != KERN_SUCCESS) {
        return MAP_FAILED;
    }
    return (void *)(uintptr_t)addr;
}
#endif

/* The same bounded registry owns both private VirtualAlloc reservations and
 * mapped views. Per-page metadata is what makes the Win32 query/protect/free
 * contracts observable without pretending arbitrary host mappings are ours. */
typedef struct {
    void *addr;
    void *allocation_base;
    size_t len;
    size_t page_count;
    DWORD allocation_protect;
    DWORD type;
    int private_allocation;
    uint64_t allocation_id;
    DWORD *pages; /* MEM_RESERVE/MEM_COMMIT plus current PAGE_* protection */
} w32_view;
static w32_view s_views[512];
static pthread_mutex_t s_views_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t s_next_view_allocation_id = 1;

static size_t vm_page_size(void)
{
    long value = sysconf(_SC_PAGESIZE);
    return value > 0 ? (size_t)value : 4096;
}

static int vm_round_size(SIZE_T size, size_t *rounded)
{
    size_t page = vm_page_size();
    if (!size || size > SIZE_MAX - (page - 1)) return 0;
    *rounded = (size + page - 1) & ~(page - 1);
    return 1;
}

static int vm_reservation_range(void *address, SIZE_T size,
                               void **base_out, size_t *size_out)
{
    const uintptr_t granularity = 64 * 1024;
    uintptr_t page = (uintptr_t)vm_page_size();
    uintptr_t start = (uintptr_t)address;
    if (!size || size > UINTPTR_MAX - start) return 0;
    uintptr_t lo = address ? start & ~(granularity - 1) : start;
    uintptr_t end = start + size;
    if (end > UINTPTR_MAX - (page - 1)) return 0;
    uintptr_t hi = (end + page - 1) & ~(page - 1);
    if (hi <= lo || hi - lo > SIZE_MAX) return 0;
    *base_out = (void *)lo;
    *size_out = (size_t)(hi - lo);
    return 1;
}

static int vm_normalize_range(void *address, SIZE_T size,
                              void **base_out, size_t *size_out)
{
    uintptr_t page = (uintptr_t)vm_page_size();
    uintptr_t start = (uintptr_t)address;
    if (!size || size > UINTPTR_MAX - start) return 0;
    uintptr_t lo = start & ~(page - 1);
    uintptr_t end = start + size;
    if (end > UINTPTR_MAX - (page - 1)) return 0;
    uintptr_t hi = (end + page - 1) & ~(page - 1);
    if (hi <= lo || hi - lo > SIZE_MAX) return 0;
    *base_out = (void *)lo;
    *size_out = (size_t)(hi - lo);
    return 1;
}

static int view_register(void *addr, size_t len, DWORD allocation_protect,
                         DWORD type, DWORD page_state, int private_allocation);
static int view_find_locked(const void *addr, size_t len, int require_private,
                            int exact_base, int *index, size_t *first_page,
                            size_t *page_count);
static int view_commit(void *address, size_t size, DWORD protect);
static int view_decommit(void *address, size_t size);
static int view_protect(void *address, size_t size, DWORD protect,
                        DWORD *old_protect);
static int view_release(void *address, SIZE_T size);

LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD allocationType, DWORD protect)
{
    int prot  = prot_from_page(protect);
    size_t rounded;
    void *map_address = address;

    if (prot < 0 || !vm_round_size(size, &rounded)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    if ((allocationType != MEM_COMMIT) && (allocationType != MEM_RESERVE) &&
        (allocationType != (MEM_RESERVE | MEM_COMMIT))) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    if (allocationType == MEM_COMMIT && address) {
        if (!view_commit(address, size, protect)) {
            SetLastError(ERROR_INVALID_ADDRESS);
            return NULL;
        }
        return (void *)((uintptr_t)address & ~(uintptr_t)(vm_page_size() - 1));
    }

    if (address) {
        if (!vm_reservation_range(address, size, &map_address, &rounded)) {
            SetLastError(ERROR_INVALID_ADDRESS);
            return NULL;
        }
    } else if (!vm_round_size(size, &rounded)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    size_t mapping_len = rounded;
    if (!map_address) {
        if (rounded > SIZE_MAX - 0x10000u) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return NULL;
        }
        mapping_len += 0x10000u;
    }

    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_FIXED_NOREPLACE)
    if (map_address) flags |= MAP_FIXED_NOREPLACE;
#endif

    int map_prot = allocationType == MEM_RESERVE ? PROT_NONE : prot;

    void *p;
#if defined(__APPLE__)
    if (map_address) {
        p = mach_map_fixed(map_address, rounded, map_prot);
    } else
#endif
    p = mmap(map_address, mapping_len, map_prot, flags, -1, 0);
    if (p == MAP_FAILED) {
        SetLastError(map_address ? ERROR_INVALID_ADDRESS : ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    if (!map_address) {
        uintptr_t aligned = ((uintptr_t)p + 0xFFFFu) & ~(uintptr_t)0xFFFFu;
        size_t prefix = (size_t)(aligned - (uintptr_t)p);
        size_t suffix = mapping_len - prefix - rounded;
        if ((prefix && munmap(p, prefix) != 0) ||
            (suffix && munmap((char *)aligned + rounded, suffix) != 0)) {
            /* munmap of aligned fragments should not fail for this fresh
             * anonymous mapping; do not publish a partially trimmed range. */
            munmap((void *)aligned, rounded);
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return NULL;
        }
        p = (void *)aligned;
    }
    DWORD state = allocationType == MEM_RESERVE ? MEM_RESERVE : MEM_COMMIT;
    if (!view_register(p, rounded, protect, MEM_PRIVATE, state, 1)) {
        munmap(p, rounded);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
#if !defined(MAP_FIXED_NOREPLACE) && !defined(__APPLE__)
    /* Older kernels without MAP_FIXED_NOREPLACE: plain MAP_FIXED would silently
     * unmap whatever already lives there, so we pass the address as a hint and
     * treat a different result as "taken". Apple goes through mach_map_fixed
     * above, which reports that properly instead of inferring it. */
    if (map_address && p != map_address) {
        view_release(p, 0);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
#endif
    return p;
}

BOOL VirtualFree(LPVOID address, SIZE_T size, DWORD freeType)
{
    if (freeType == MEM_RELEASE) return view_release(address, size);
    if (freeType == MEM_DECOMMIT) return view_decommit(address, size);
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
}

BOOL VirtualProtect(LPVOID address, SIZE_T size, DWORD newProtect, PDWORD oldProtect)
{
    int prot = prot_from_page(newProtect);

    if (prot < 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return view_protect(address, size, newProtect, oldProtect);
}

/* ===================================================================== */
/* Time                                                                  */
/* ===================================================================== */

/* 100-ns intervals between 1601-01-01 and 1970-01-01 */
#define FILETIME_EPOCH_DIFF 116444736000000000ULL

VOID GetSystemTimeAsFileTime(LPFILETIME ft)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ULONGLONG t = FILETIME_EPOCH_DIFF
                + (ULONGLONG)ts.tv_sec * 10000000ULL
                + (ULONGLONG)ts.tv_nsec / 100ULL;
    ft->dwLowDateTime  = (DWORD)(t & 0xFFFFFFFFULL);
    ft->dwHighDateTime = (DWORD)(t >> 32);
}

static void fill_systemtime(LPSYSTEMTIME st, const struct tm *tm, long nsec)
{
    st->wYear         = (WORD)(tm->tm_year + 1900);
    st->wMonth        = (WORD)(tm->tm_mon + 1);
    st->wDayOfWeek    = (WORD)tm->tm_wday;
    st->wDay          = (WORD)tm->tm_mday;
    st->wHour         = (WORD)tm->tm_hour;
    st->wMinute       = (WORD)tm->tm_min;
    st->wSecond       = (WORD)tm->tm_sec;
    st->wMilliseconds = (WORD)(nsec / 1000000L);
}

VOID GetSystemTime(LPSYSTEMTIME st)
{
    struct timespec ts; struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tm);
    fill_systemtime(st, &tm, ts.tv_nsec);
}

VOID GetLocalTime(LPSYSTEMTIME st)
{
    struct timespec ts; struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    fill_systemtime(st, &tm, ts.tv_nsec);
}

ULONGLONG GetTickCount64(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ULONGLONG)ts.tv_sec * 1000ULL + (ULONGLONG)ts.tv_nsec / 1000000ULL;
}

DWORD GetTickCount(void) { return (DWORD)GetTickCount64(); }

BOOL QueryPerformanceCounter(PLARGE_INTEGER count)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    count->QuadPart = (LONGLONG)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    return TRUE;
}

BOOL QueryPerformanceFrequency(PLARGE_INTEGER freq)
{
    freq->QuadPart = 1000000000LL;   /* QPC is in nanoseconds */
    return TRUE;
}

/* FILETIME units (100 ns) of the calling thread's CPU time, reported as user
 * time. Created/exited/kernel times are not tracked. The handle is not checked:
 * the only caller asks about the calling thread. */
BOOL GetThreadTimes(HANDLE thread, LPFILETIME created, LPFILETIME exited,
                    LPFILETIME kernel, LPFILETIME user)
{
    struct timespec ts;
    uint64_t hundred_ns;
    (void)thread;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) {
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    }
    hundred_ns = (uint64_t)ts.tv_sec * 10000000u + (uint64_t)ts.tv_nsec / 100u;
    if (created) memset(created, 0, sizeof *created);
    if (exited) memset(exited, 0, sizeof *exited);
    if (kernel) memset(kernel, 0, sizeof *kernel);
    if (user) {
        user->dwLowDateTime = (DWORD)hundred_ns;
        user->dwHighDateTime = (DWORD)(hundred_ns >> 32);
    }
    return TRUE;
}

/* ===================================================================== */
/* Misc                                                                  */
/* ===================================================================== */

VOID OutputDebugStringA(LPCSTR str)
{
    if (str) fputs(str, stderr);
}

VOID ExitProcess(UINT exitCode) { exit((int)exitCode); }

BOOL IsDebuggerPresent(void)
{
#if defined(__APPLE__)
    /* Darwin: KERN_PROC_PID reports P_TRACED when a debugger is attached. */
    struct kinfo_proc info;
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
    size_t size = sizeof(info);
    memset(&info, 0, size);
    if (sysctl(mib, sizeof(mib), &info, &size, NULL, 0) != 0) return FALSE;
    return (info.kp_proc.p_flag & P_TRACED) != 0;
#else
    /* Linux: a non-zero TracerPid in /proc/self/status means ptrace is attached. */
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return FALSE;
    char line[256];
    BOOL traced = FALSE;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "TracerPid:", 10) == 0) {
            traced = strtol(line + 10, NULL, 10) != 0;
            break;
        }
    }
    fclose(f);
    return traced;
#endif
}

VOID DebugBreak(void) {
    /* Not __debugbreak(): that is an MSVC intrinsic, and this file is the
     * half that MSVC never compiles. SIGTRAP is the POSIX equivalent --
     * continuable under a debugger, fatal without one, as on Windows. */
    raise(SIGTRAP);
}

VOID SecureZeroMemory(PVOID ptr, SIZE_T cnt)
{
#if defined(__APPLE__)
    memset_s(ptr, cnt, 0, cnt);
#else
    explicit_bzero(ptr, cnt);
#endif
}

unsigned int _clearfp(void)
{
    feclearexcept(FE_ALL_EXCEPT);
    return 0;
}

/* ===================================================================== */
/* Win32 file API on POSIX (open/read/write/fstat-backed)                */
/* ===================================================================== */

#include <fcntl.h>
#include <sys/stat.h>

/* ---- Win32 share modes for host files ------------------------------------
 * Each open file is an entry keyed by (dev, ino) holding the access its
 * handle was granted and the share mask it was opened with, both as
 * FILE_SHARE_* bits so the rule is one mask test. A new open conflicts when
 * its access is not shared by an existing open, or an existing open's access
 * is not shared by it. An entry lives as long as its file object:
 * DuplicateHandle copies share it, the last CloseHandle removes it.
 * ponytail: one list under one lock; hash by (dev, ino) if opens grow. */
typedef struct w32_share {
    struct w32_share *next;
    w32_object       *owner;
    dev_t             dev;
    ino_t             ino;
    DWORD             granted, share;
} w32_share;

static w32_share       *s_shares;
static pthread_mutex_t  s_shares_lock = PTHREAD_MUTEX_INITIALIZER;

/* The FILE_SHARE_* bit each access class needs from every other open. */
static DWORD share_needs(DWORD access)
{
    DWORD need = 0;
    if (access & (GENERIC_READ | GENERIC_ALL | FILE_READ_DATA))
        need |= FILE_SHARE_READ;
    if (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA))
        need |= FILE_SHARE_WRITE;
    if (access & (DELETE | GENERIC_ALL))
        need |= FILE_SHARE_DELETE;
    return need;
}

/* Caller holds s_shares_lock. */
static int share_conflicts(dev_t dev, ino_t ino, DWORD granted, DWORD share)
{
    for (w32_share *e = s_shares; e; e = e->next)
        if (e->dev == dev && e->ino == ino &&
            ((granted & ~e->share) || (e->granted & ~share)))
            return 1;
    return 0;
}

static void share_forget(w32_object *o)
{
    w32_share **link;
    pthread_mutex_lock(&s_shares_lock);
    for (link = &s_shares; *link; link = &(*link)->next) {
        if ((*link)->owner != o) continue;
        w32_share *dead = *link;
        *link = dead->next;
        free(dead);
        break;
    }
    pthread_mutex_unlock(&s_shares_lock);
}

HANDLE w32_open_handle_shared(int fd, const char *host_path, DWORD access,
                              DWORD share, int truncate_to_zero)
{
    struct stat st;
    w32_share *entry;
    HANDLE h;
    DWORD granted = share_needs(access);
    if (fstat(fd, &st) != 0) { SetLastError(ERROR_GEN_FAILURE); return INVALID_HANDLE_VALUE; }
    entry = calloc(1, sizeof *entry);
    if (!entry) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return INVALID_HANDLE_VALUE; }
    pthread_mutex_lock(&s_shares_lock);
    if (share_conflicts(st.st_dev, st.st_ino, granted, share)) {
        pthread_mutex_unlock(&s_shares_lock);
        free(entry);
        SetLastError(ERROR_SHARING_VIOLATION);
        return INVALID_HANDLE_VALUE;
    }
    /* Truncation waits for the check: a refused open must not empty the file. */
    if (truncate_to_zero && ftruncate(fd, 0) != 0) {
        pthread_mutex_unlock(&s_shares_lock);
        free(entry);
        SetLastError(ERROR_GEN_FAILURE);
        return INVALID_HANDLE_VALUE;
    }
    h = w32_open_handle(fd, host_path);
    entry->owner   = (w32_object *)h;
    entry->dev     = st.st_dev;
    entry->ino     = st.st_ino;
    entry->granted = granted;
    entry->share   = share;
    entry->next    = s_shares;
    s_shares       = entry;
    pthread_mutex_unlock(&s_shares_lock);
    return h;
}

HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD share,
                   LPSECURITY_ATTRIBUTES sa, DWORD disp,
                   DWORD flags, HANDLE templ)
{
    (void)sa; (void)flags; (void)templ;
    if (!name) { SetLastError(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }

    int rw = O_RDONLY;
    int wantW = (access & (GENERIC_WRITE | GENERIC_ALL)) != 0;
    int wantR = (access & (GENERIC_READ  | GENERIC_ALL)) != 0;
    if (wantW && wantR) rw = O_RDWR;
    else if (wantW)     rw = O_WRONLY;

    int extra = 0;
    switch (disp) {
    case CREATE_NEW:        extra = O_CREAT | O_EXCL;  break;
    case CREATE_ALWAYS:     extra = O_CREAT | O_TRUNC; break;
    case OPEN_EXISTING:     extra = 0;                 break;
    case OPEN_ALWAYS:       extra = O_CREAT;           break;
    case TRUNCATE_EXISTING: extra = O_TRUNC;           break;
    default:                extra = 0;                 break;
    }
    if ((extra & (O_CREAT | O_TRUNC)) && rw == O_RDONLY) rw = O_RDWR;

    /* Normalise embedded Windows-style backslashes before open(). */
    char norm[1024];
    snprintf(norm, sizeof(norm), "%s", name);
    xbox_path_normalize(norm);
    int fd = open(norm, (rw | extra) & ~O_TRUNC, 0644);
    if (fd < 0) { SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    HANDLE h = w32_open_handle_shared(fd, name, access, share, (extra & O_TRUNC) != 0);
    if (h == INVALID_HANDLE_VALUE) close(fd);
    return h;
}

HANDLE CreateFileW(LPCWSTR name, DWORD access, DWORD share,
                   LPSECURITY_ATTRIBUTES sa, DWORD disp,
                   DWORD flags, HANDLE templ)
{
    /* Basic UTF-16 -> UTF-8 (ASCII path of WideCharToMultiByte). */
    char buf[1024];
    int len = WideCharToMultiByte(CP_UTF8, 0, name, -1, buf, sizeof(buf), NULL, NULL);
    if (len <= 0) buf[0] = '\0';
    return CreateFileA(buf, access, share, sa, disp, flags, templ);
}

BOOL ReadFile(HANDLE h, LPVOID buf, DWORD len, LPDWORD nread, void *overlapped)
{
    (void)overlapped;
    int fd = w32_handle_fd(h);
    if (fd < 0) { if (nread) *nread = 0; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    ssize_t n = read(fd, buf, len);
    if (n < 0)  { if (nread) *nread = 0; SetLastError(ERROR_GEN_FAILURE);    return FALSE; }
    if (nread)  *nread = (DWORD)n;
    return TRUE;
}

BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD len, LPDWORD nwritten, void *overlapped)
{
    (void)overlapped;
    int fd = w32_handle_fd(h);
    if (fd < 0) { if (nwritten) *nwritten = 0; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    ssize_t n = write(fd, buf, len);
    if (n < 0)  { if (nwritten) *nwritten = 0; SetLastError(ERROR_GEN_FAILURE);    return FALSE; }
    if (nwritten) *nwritten = (DWORD)n;
    return TRUE;
}

DWORD GetFileSize(HANDLE h, LPDWORD high)
{
    int fd = w32_handle_fd(h);
    if (fd < 0) return INVALID_FILE_SIZE;
    struct stat st;
    if (fstat(fd, &st) != 0) return INVALID_FILE_SIZE;
    if (high) *high = (DWORD)(((uint64_t)st.st_size >> 32) & 0xFFFFFFFFu);
    return (DWORD)(st.st_size & 0xFFFFFFFFu);
}

BOOL GetFileSizeEx(HANDLE h, PLARGE_INTEGER size)
{
    int fd = w32_handle_fd(h);
    if (fd < 0 || !size) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    struct stat st;
    if (fstat(fd, &st) != 0) { SetLastError(ERROR_GEN_FAILURE); return FALSE; }
    size->QuadPart = (LONGLONG)st.st_size;
    return TRUE;
}

BOOL FlushFileBuffers(HANDLE h)
{
    int fd = w32_handle_fd(h);
    if (fd < 0) return FALSE;
    return fsync(fd) == 0;
}

/* ===================================================================== */
/* Keyboard + window helpers -- stubs. Keys reach titles through the     */
/* framebuffer window instead (xbox_FramebufferKeyDown, fb_window_sdl.c). */
/* ===================================================================== */

SHORT GetAsyncKeyState(int vKey)          { (void)vKey; return 0; }
HWND  FindWindowA(LPCSTR c, LPCSTR w)     { (void)c; (void)w; return NULL; }
HWND  GetActiveWindow(void)               { return NULL; }
BOOL  SetWindowTextA(HWND h, LPCSTR t)    { (void)h; (void)t; return TRUE; }
int   GetWindowTextA(HWND h, LPSTR t, int n) { (void)h; (void)t; (void)n; return 0; }
BOOL  EnumWindows(WNDENUMPROC p, LPARAM l) { (void)p; (void)l; return FALSE; }

int MessageBoxA(HWND h, LPCSTR text, LPCSTR caption, UINT type)
{
    (void)h; (void)type;
    fprintf(stderr, "[%s] %s\n", caption ? caption : "MessageBox",
                                   text    ? text    : "");
    return 1;   /* IDOK */
}

/* Message-loop stubs: no Win32 messages on POSIX (SDL events drive the
 * d3d8_gl backend; this layer is just for the game's Win32 message pump). */
BOOL    PeekMessageA(LPMSG m, HWND w, UINT a, UINT b, UINT f)
{ (void)m; (void)w; (void)a; (void)b; (void)f; return FALSE; }
BOOL    TranslateMessage(const MSG *m) { (void)m; return TRUE; }
LRESULT DispatchMessageA(const MSG *m) { (void)m; return 0; }

/* XInput stub: real gamepads come through src/input (SDL3 gamepads). */
DWORD XInputGetState(DWORD idx, XINPUT_STATE *state)
{ (void)idx; if (state) memset(state, 0, sizeof(*state)); return ERROR_DEVICE_NOT_CONNECTED; }

BOOL TerminateProcess(HANDLE process, UINT exitCode)
{
    (void)process;
    exit((int)exitCode);
}

VOID OutputDebugStringW(LPCWSTR str)
{
    if (!str) return;
    for (const WCHAR *p = str; *p; p++)
        fputc((*p < 128) ? (int)*p : '?', stderr);
}

/*
 * Minimal MultiByteToWideChar / WideCharToMultiByte. Handles UTF-8 and a
 * latin-1 interpretation of CP_ACP -- enough for path/name strings.
 */
int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR mb, int mbCount,
                        LPWSTR wide, int wideCount)
{
    (void)flags;
    if (!mb) return 0;
    int srcLen = (mbCount < 0) ? (int)strlen(mb) + 1 : mbCount;
    int out = 0;

    for (int i = 0; i < srcLen; ) {
        unsigned int cpval;
        unsigned char c = (unsigned char)mb[i];

        if (cp == CP_UTF8 && c >= 0x80) {
            if ((c & 0xE0) == 0xC0 && i + 1 < srcLen) {
                cpval = ((c & 0x1F) << 6) | (mb[i+1] & 0x3F); i += 2;
            } else if ((c & 0xF0) == 0xE0 && i + 2 < srcLen) {
                cpval = ((c & 0x0F) << 12) | ((mb[i+1] & 0x3F) << 6) |
                        (mb[i+2] & 0x3F); i += 3;
            } else if ((c & 0xF8) == 0xF0 && i + 3 < srcLen) {
                cpval = ((c & 0x07) << 18) | ((mb[i+1] & 0x3F) << 12) |
                        ((mb[i+2] & 0x3F) << 6) | (mb[i+3] & 0x3F); i += 4;
            } else { cpval = c; i += 1; }
        } else {
            cpval = c; i += 1;   /* ASCII / latin-1 */
        }

        if (cpval > 0xFFFF) cpval = '?';   /* no surrogate pairs */
        if (wideCount > 0) {
            if (out >= wideCount) return 0;
            wide[out] = (WCHAR)cpval;
        }
        out++;
    }
    return out;
}

int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR wide, int wideCount,
                        LPSTR mb, int mbCount, LPCSTR defChar, PBOOL usedDef)
{
    (void)flags; (void)defChar; (void)usedDef;
    if (!wide) return 0;
    int srcLen = wideCount;
    if (srcLen < 0) { srcLen = 0; while (wide[srcLen]) srcLen++; srcLen++; }
    int out = 0;

    for (int i = 0; i < srcLen; i++) {
        unsigned int cpval = wide[i];
        char buf[4]; int n;
        if (cp == CP_UTF8 && cpval >= 0x80) {
            if (cpval < 0x800) {
                buf[0] = (char)(0xC0 | (cpval >> 6));
                buf[1] = (char)(0x80 | (cpval & 0x3F)); n = 2;
            } else {
                buf[0] = (char)(0xE0 | (cpval >> 12));
                buf[1] = (char)(0x80 | ((cpval >> 6) & 0x3F));
                buf[2] = (char)(0x80 | (cpval & 0x3F)); n = 3;
            }
        } else {
            buf[0] = (char)(cpval > 0xFF ? '?' : cpval); n = 1;
        }
        if (mbCount > 0) {
            if (out + n > mbCount) return 0;
            for (int k = 0; k < n; k++) mb[out + k] = buf[k];
        }
        out += n;
    }
    return out;
}

/* ===================================================================== */
/* File mapping (memfd-backed)                                           */
/* ===================================================================== */

/* Register one complete mapping. Failure is explicit: callers unmap it before
 * publishing a pointer when all 512 ownership slots are occupied. */
static int view_register(void *addr, size_t len, DWORD allocation_protect,
                         DWORD type, DWORD page_state, int private_allocation)
{
    size_t page_count = len / vm_page_size();
    if (!page_count || page_count > SIZE_MAX / sizeof(DWORD)) return 0;
    DWORD *pages = (DWORD *)malloc(page_count * sizeof(*pages));
    if (!pages) return 0;
    DWORD value = page_state;
    if (page_state == MEM_COMMIT) value |= allocation_protect & 0xFF;
    for (size_t i = 0; i < page_count; i++) pages[i] = value;

    int registered = 0;
    pthread_mutex_lock(&s_views_lock);
    for (int i = 0; i < 512; i++)
        if (!s_views[i].addr) {
            s_views[i].addr = addr;
            s_views[i].allocation_base = addr;
            s_views[i].len = len;
            s_views[i].page_count = page_count;
            s_views[i].allocation_protect = allocation_protect;
            s_views[i].type = type;
            s_views[i].private_allocation = private_allocation;
            s_views[i].allocation_id = s_next_view_allocation_id++;
            if (!s_views[i].allocation_id)
                s_views[i].allocation_id = s_next_view_allocation_id++;
            s_views[i].pages = pages;
            registered = 1;
            break;
        }
    pthread_mutex_unlock(&s_views_lock);
    if (!registered) free(pages);
    return registered;
}

/* Find a page-aligned range wholly inside one tracked mapping. */
static int view_find_locked(const void *addr, size_t len, int require_private,
                            int exact_base, int *index_out, size_t *first_out,
                            size_t *count_out)
{
    uintptr_t p = (uintptr_t)addr;
    size_t page = vm_page_size();
    if ((p % page) || (len % page) || len > UINTPTR_MAX - p) return 0;
    for (int i = 0; i < 512; i++) {
        w32_view *v = &s_views[i];
        if (!v->addr || (require_private && !v->private_allocation)) continue;
        uintptr_t lo = (uintptr_t)v->addr, hi = lo + v->len;
        if ((exact_base && p != lo) || p < lo || p >= hi || p + len > hi)
            continue;
        size_t first = (p - lo) / page, count = len / page;
        if (index_out) *index_out = i;
        if (first_out) *first_out = first;
        if (count_out) *count_out = count;
        return 1;
    }
    return 0;
}

static int view_commit(void *address, size_t size, DWORD protect)
{
    int prot = prot_from_page(protect), index;
    size_t first, count;
    void *base;
    if (!vm_normalize_range(address, size, &base, &size)) return 0;
    if (prot < 0) return 0;
    pthread_mutex_lock(&s_views_lock);
    int found = view_find_locked(base, size, 1, 0, &index, &first, &count);
    if (!found) { pthread_mutex_unlock(&s_views_lock); return 0; }
    w32_view *v = &s_views[index];
    if (mprotect(base, size, prot) != 0) {
        pthread_mutex_unlock(&s_views_lock);
        return 0;
    }
    for (size_t i = 0; i < count; i++) v->pages[first + i] = MEM_COMMIT | (protect & 0xFF);
    pthread_mutex_unlock(&s_views_lock);
    return 1;
}

static int view_decommit(void *address, size_t size)
{
#if defined(__APPLE__)
    /* Darwin's MADV_DONTNEED does not promise that contents are discarded
     * before a recommit. Do not report Windows zero-on-recommit semantics. */
    (void)address; (void)size;
    SetLastError(ERROR_NOT_SUPPORTED);
    return 0;
#else
    size_t rounded;
    void *base;
    int index;
    size_t first, count;
    if (!address) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    pthread_mutex_lock(&s_views_lock);
    if (size == 0) {
        if (!view_find_locked(address, 0, 1, 1, &index, NULL, NULL)) {
            pthread_mutex_unlock(&s_views_lock);
            SetLastError(ERROR_INVALID_ADDRESS);
            return 0;
        }
        base = address;
        rounded = s_views[index].len;
    } else if (!vm_normalize_range(address, size, &base, &rounded)) {
        pthread_mutex_unlock(&s_views_lock);
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!view_find_locked(base, rounded, 1, 0, &index, &first, &count)) {
        pthread_mutex_unlock(&s_views_lock);
        SetLastError(ERROR_INVALID_ADDRESS);
        return 0;
    }
    if (mprotect(base, rounded, PROT_NONE) != 0) {
        pthread_mutex_unlock(&s_views_lock);
        return 0;
    }
#if defined(MADV_DONTNEED)
    if (madvise(base, rounded, MADV_DONTNEED) != 0) {
        w32_view *v = &s_views[index];
        for (size_t i = 0; i < count; i++) {
            DWORD page = v->pages[first + i];
            if ((page & MEM_COMMIT) == MEM_COMMIT)
                mprotect((char *)base + i * vm_page_size(), vm_page_size(),
                         prot_from_page(page & 0xFF));
        }
        pthread_mutex_unlock(&s_views_lock);
        return 0;
    }
#else
    /* Without a discard primitive, decommit cannot promise zero-on-recommit. */
    w32_view *v = &s_views[index];
    for (size_t i = 0; i < count; i++) {
        DWORD page = v->pages[first + i];
        if ((page & MEM_COMMIT) == MEM_COMMIT)
            mprotect((char *)base + i * vm_page_size(), vm_page_size(),
                     prot_from_page(page & 0xFF));
    }
    pthread_mutex_unlock(&s_views_lock);
    SetLastError(ERROR_NOT_SUPPORTED);
    return 0;
#endif
    for (size_t i = 0; i < count; i++) s_views[index].pages[first + i] = MEM_RESERVE;
    pthread_mutex_unlock(&s_views_lock);
    return 1;
#endif
}

static int view_protect(void *address, size_t size, DWORD protect, DWORD *old_protect)
{
    int prot = prot_from_page(protect), index;
    size_t rounded, first, count;
    void *base;
    if (!old_protect || prot < 0 || !vm_normalize_range(address, size, &base, &rounded)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    pthread_mutex_lock(&s_views_lock);
    if (!view_find_locked(base, rounded, 0, 0, &index, &first, &count)) {
        pthread_mutex_unlock(&s_views_lock);
        SetLastError(ERROR_INVALID_ADDRESS);
        return 0;
    }
    w32_view *v = &s_views[index];
    for (size_t i = 0; i < count; i++) {
        if ((v->pages[first + i] & MEM_COMMIT) != MEM_COMMIT) {
            pthread_mutex_unlock(&s_views_lock);
            SetLastError(ERROR_INVALID_ADDRESS);
            return 0;
        }
    }
    DWORD previous = v->pages[first] & 0xFF;
    if (mprotect(base, rounded, prot) != 0) {
        pthread_mutex_unlock(&s_views_lock);
        return 0;
    }
    for (size_t i = 0; i < count; i++) v->pages[first + i] = MEM_COMMIT | (protect & 0xFF);
    if (old_protect) *old_protect = previous;
    pthread_mutex_unlock(&s_views_lock);
    return 1;
}

static int view_release(void *address, SIZE_T size)
{
    int index;
    pthread_mutex_lock(&s_views_lock);
    if (size != 0 || !view_find_locked(address, 0, 1, 1, &index, NULL, NULL)) {
        pthread_mutex_unlock(&s_views_lock);
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    w32_view *v = &s_views[index];
    for (int i = 0; i < 512; i++) {
        if (i != index && s_views[i].addr && s_views[i].private_allocation &&
            s_views[i].allocation_id == v->allocation_id) {
            pthread_mutex_unlock(&s_views_lock);
            SetLastError(ERROR_INVALID_PARAMETER);
            return 0;
        }
    }
    if (v->allocation_base != address) {
        pthread_mutex_unlock(&s_views_lock);
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (munmap(v->addr, v->len) != 0) {
        pthread_mutex_unlock(&s_views_lock);
        return 0;
    }
    free(v->pages);
    memset(v, 0, sizeof(*v));
    pthread_mutex_unlock(&s_views_lock);
    return 1;
}

/* Read the page record under the registry lock; query pages with matching
 * state/protection form the Win32 region around the input page. */
static int view_query(const void *addr, MEMORY_BASIC_INFORMATION *buffer)
{
    uintptr_t p = (uintptr_t)addr;
    size_t page = vm_page_size();
    int found = 0;
    pthread_mutex_lock(&s_views_lock);
    for (int i = 0; i < 512; i++) {
        w32_view *v = &s_views[i];
        uintptr_t lo = (uintptr_t)v->addr;
        if (!v->addr || p < lo || p - lo >= v->len) continue;
        size_t at = (p - lo) / page;
        DWORD value = v->pages[at];
        size_t begin = at, end = at + 1;
        while (end < v->page_count && v->pages[end] == value) end++;
        buffer->BaseAddress = (char *)v->addr + begin * page;
        buffer->AllocationBase = v->allocation_base;
        buffer->AllocationProtect = v->allocation_protect;
        buffer->RegionSize = (end - begin) * page;
        buffer->State = value & (MEM_COMMIT | MEM_RESERVE);
        buffer->Protect = buffer->State == MEM_COMMIT ? (value & 0xFF) : 0;
        buffer->Type = v->type;
        found = 1;
        break;
    }
    pthread_mutex_unlock(&s_views_lock);
    if (!found) {
        buffer->BaseAddress = (PVOID)((uintptr_t)addr & ~(uintptr_t)(page - 1));
        buffer->State = MEM_FREE;
        buffer->Protect = PAGE_NOACCESS;
    }
    return found;
}

uint64_t w32_reservation_id(LPVOID base)
{
    uint64_t id = 0;
    pthread_mutex_lock(&s_views_lock);
    for (int i = 0; i < 512; i++) {
        if (s_views[i].addr == base && s_views[i].private_allocation) {
            id = s_views[i].allocation_id;
            break;
        }
    }
    pthread_mutex_unlock(&s_views_lock);
    return id;
}

BOOL w32_release_reserved_region(LPVOID address, SIZE_T size)
{
    int index, free_slot = -1;
    size_t first, count;
    if (!address || !size || ((uintptr_t)address % vm_page_size()) ||
        (size % vm_page_size())) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    pthread_mutex_lock(&s_views_lock);
    if (!view_find_locked(address, size, 1, 0, &index, &first, &count)) {
        pthread_mutex_unlock(&s_views_lock);
        SetLastError(ERROR_INVALID_ADDRESS);
        return FALSE;
    }
    w32_view *v = &s_views[index];
    for (size_t i = 0; i < count; i++) {
        if (v->pages[first + i] != MEM_RESERVE) {
            pthread_mutex_unlock(&s_views_lock);
            SetLastError(ERROR_INVALID_ADDRESS);
            return FALSE;
        }
    }
    size_t left_count = first;
    size_t right_count = v->page_count - first - count;
    DWORD *right_pages = NULL;
    if (left_count && right_count) {
        for (int i = 0; i < 512; i++) {
            if (!s_views[i].addr) { free_slot = i; break; }
        }
        if (free_slot < 0 || right_count > SIZE_MAX / sizeof(DWORD) ||
            !(right_pages = (DWORD *)malloc(right_count * sizeof(DWORD)))) {
            pthread_mutex_unlock(&s_views_lock);
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return FALSE;
        }
        memcpy(right_pages, v->pages + first + count,
               right_count * sizeof(DWORD));
    }
    if (munmap(address, size) != 0) {
        free(right_pages);
        pthread_mutex_unlock(&s_views_lock);
        return FALSE;
    }
    if (left_count && right_count) {
        w32_view right = *v;
        right.addr = (char *)address + size;
        right.len = right_count * vm_page_size();
        right.page_count = right_count;
        right.pages = right_pages;
        v->len = left_count * vm_page_size();
        v->page_count = left_count;
        s_views[free_slot] = right;
    } else if (left_count) {
        v->len = left_count * vm_page_size();
        v->page_count = left_count;
    } else if (right_count) {
        memmove(v->pages, v->pages + first + count, right_count * sizeof(DWORD));
        v->addr = (char *)address + size;
        v->len = right_count * vm_page_size();
        v->page_count = right_count;
    } else {
        free(v->pages);
        memset(v, 0, sizeof(*v));
    }
    pthread_mutex_unlock(&s_views_lock);
    return TRUE;
}

BOOL w32_release_reservation(uint64_t allocation_id)
{
    if (!allocation_id) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    int ok = TRUE, found = FALSE;
    pthread_mutex_lock(&s_views_lock);
    for (int i = 0; i < 512; i++) {
        w32_view *v = &s_views[i];
        if (!v->addr || !v->private_allocation || v->allocation_id != allocation_id)
            continue;
        found = TRUE;
        if (munmap(v->addr, v->len) != 0) {
            ok = FALSE;
            continue;
        }
        free(v->pages);
        memset(v, 0, sizeof(*v));
    }
    pthread_mutex_unlock(&s_views_lock);
    if (!ok || !found) return FALSE;
    return TRUE;
}

/* An unnamed file descriptor that ftruncate and mmap both accept. Linux has
 * memfd_create for this; elsewhere an immediately-unlinked temp file does. */
static int anon_map_fd(const char *name)
{
#if defined(__APPLE__)
    static volatile LONG map_counter = 0;
    char shm_name[32];
    LONG seq = InterlockedIncrement(&map_counter);
    const char *base = name ? name : "xbox_map";
    snprintf(shm_name, sizeof(shm_name), "/%s_%ld", base, seq);
    int fd = shm_open(shm_name, O_CREAT | O_RDWR | O_EXCL, 0600);
    if (fd >= 0) shm_unlink(shm_name);
    return fd;
#else
    return memfd_create(name ? name : "xbox_map", 0);
#endif
}

HANDLE CreateFileMappingA(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect,
                          DWORD maxSizeHigh, DWORD maxSizeLow, LPCSTR name)
{
    (void)sa;
    if (file != (HANDLE)(LONG_PTR)-1) {
        SetLastError(ERROR_INVALID_HANDLE);
        return NULL;
    }
    if (protect != PAGE_READONLY && protect != PAGE_READWRITE &&
        protect != PAGE_WRITECOPY) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    uint64_t requested_size = ((uint64_t)maxSizeHigh << 32) | maxSizeLow;
    if (requested_size == 0 || requested_size > SIZE_MAX ||
        requested_size > (uint64_t)INT64_MAX) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    SIZE_T size = (SIZE_T)requested_size;

    int fd = anon_map_fd(name);
    if (fd < 0) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }

    w32_object *o = obj_alloc(K_FILEMAP);
    if (!o) { close(fd); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    o->fd       = fd;
    o->map_size = size;
    o->map_protect = protect;
    return (HANDLE)o;
}

HANDLE CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect,
                          DWORD maxSizeHigh, DWORD maxSizeLow, LPCWSTR name)
{
    (void)name;
    return CreateFileMappingA(file, sa, protect, maxSizeHigh, maxSizeLow, NULL);
}

LPVOID MapViewOfFileEx(HANDLE mapping, DWORD access, DWORD offHigh, DWORD offLow,
                       SIZE_T count, LPVOID baseAddr)
{
    w32_object *o = obj_lookup(mapping);
    if (!o || o->kind != K_FILEMAP) { SetLastError(ERROR_INVALID_HANDLE); return NULL; }

    uint64_t off_value = ((uint64_t)offHigh << 32) | offLow;
    if (off_value > (uint64_t)INT64_MAX) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    off_t off = (off_t)off_value;
    if ((off_value % (64 * 1024)) != 0 ||
        (baseAddr && ((uintptr_t)baseAddr % (64 * 1024)) != 0)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (off < 0 || (SIZE_T)off >= o->map_size || (count && count > o->map_size - (SIZE_T)off)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    SIZE_T requested = count ? count : (o->map_size - (SIZE_T)off);
    size_t len;
    if (!vm_round_size(requested, &len)) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    int map_copy = access == FILE_MAP_COPY;
    if (access != FILE_MAP_READ && access != FILE_MAP_WRITE &&
        access != FILE_MAP_COPY && access != FILE_MAP_ALL_ACCESS) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if ((o->map_protect == PAGE_READONLY && access != FILE_MAP_READ) ||
        (o->map_protect == PAGE_WRITECOPY && access != FILE_MAP_READ && !map_copy)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    int prot   = PROT_READ | ((access == FILE_MAP_READ) ? 0 : PROT_WRITE);
    DWORD page_protect = access == FILE_MAP_READ ? PAGE_READONLY
                       : map_copy ? PAGE_WRITECOPY : PAGE_READWRITE;
    int flags  = map_copy ? MAP_PRIVATE : MAP_SHARED;

    /* Win32 MapViewOfFileEx *fails* when the requested address is unavailable.
     * Plain MAP_FIXED does the opposite: it silently unmaps whatever is there
     * and succeeds. The Xbox memory model maps its RAM view at an address it
     * reserved, so with a base the OS chose rather than one we picked, that
     * difference is the process quietly destroying its own libraries and heap
     * and dying somewhere unrelated a moment later. */
    if (baseAddr) {
#if defined(MAP_FIXED_NOREPLACE)
        flags |= MAP_FIXED_NOREPLACE;
#elif defined(__APPLE__)
        /* Darwin has no MAP_FIXED_NOREPLACE. Claim the range first with
         * mach_vm_map(VM_FLAGS_FIXED), which refuses rather than displaces;
         * MAP_FIXED below can then only replace the placeholder we now own. */
        if (mach_map_fixed(baseAddr, len, PROT_READ | PROT_WRITE) == MAP_FAILED) {
            SetLastError(ERROR_INVALID_ADDRESS);
            return NULL;
        }
        flags |= MAP_FIXED;
#else
        flags |= MAP_FIXED;
#endif
    }

    void *p = mmap(baseAddr, len, prot, flags, o->fd, off);
    if (p == MAP_FAILED) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    if (baseAddr && p != baseAddr) {
        /* MAP_FIXED_NOREPLACE hands back a different address instead of
         * failing on some kernels; treat that as the refusal it means. */
        munmap(p, len);
        SetLastError(ERROR_INVALID_ADDRESS);
        return NULL;
    }
    if (!view_register(p, len, page_protect, MEM_MAPPED, MEM_COMMIT, 0)) {
        munmap(p, len);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    return p;
}

LPVOID MapViewOfFile(HANDLE mapping, DWORD access, DWORD offHigh, DWORD offLow, SIZE_T count)
{
    return MapViewOfFileEx(mapping, access, offHigh, offLow, count, NULL);
}

BOOL UnmapViewOfFile(LPCVOID baseAddr)
{
    pthread_mutex_lock(&s_views_lock);
    int index;
    if (!view_find_locked(baseAddr, 0, 0, 1, &index, NULL, NULL) ||
        s_views[index].private_allocation) {
        pthread_mutex_unlock(&s_views_lock);
        SetLastError(ERROR_INVALID_ADDRESS);
        return FALSE;
    }
    w32_view *v = &s_views[index];
    if (munmap(v->addr, v->len) != 0) {
        pthread_mutex_unlock(&s_views_lock);
        return FALSE;
    }
    free(v->pages);
    memset(v, 0, sizeof(*v));
    pthread_mutex_unlock(&s_views_lock);
    return TRUE;
}

/* ===================================================================== */
/* VirtualQuery                                                           */
/* ===================================================================== */

/*
 * Answers from the view registry rather than from a constant.
 *
 * This used to report RegionSize 0x1000, MEM_COMMIT, PAGE_READWRITE and
 * AllocationBase NULL for every address it was handed, mapped or not. Four
 * kernel entry points are built on it, and one of them chooses a deallocator
 * with it: MmFreeContiguousMemory frees via VirtualFree only when
 * AllocationBase equals the pointer, so a hardcoded NULL sent every
 * contiguous buffer -- all of which come from VirtualAlloc, i.e. mmap -- to
 * _aligned_free, which is free(). The allocator aborts on the foreign
 * pointer. MmQueryAllocationSize answered 0x1000 for everything and
 * NtQueryVirtualMemory called unmapped addresses committed and readable.
 *
 * Everything the shim maps -- VirtualAlloc and MapViewOfFileEx alike -- is in
 * the registry, so it can answer for exactly the memory it owns and say
 * MEM_FREE for the rest. Saying MEM_FREE for an address it did not map is the
 * honest answer: a host heap pointer is not a Win32 reservation, and the
 * callers that branch on this want to know which allocator owns the pointer.
 */
SIZE_T VirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION buffer, SIZE_T length)
{
    if (!buffer || length < sizeof(*buffer)) return 0;
    memset(buffer, 0, sizeof(*buffer));

    if (!view_query(address, buffer)) {
        /* Not ours. Report it free rather than inventing a committed page. */
        buffer->AllocationBase = NULL;
        buffer->RegionSize     = 0;
        buffer->Type           = 0;
        return sizeof(*buffer);
    }
    return sizeof(*buffer);
}

/* Win32 GetComputerNameA. The host name stands in for the NetBIOS name; the
 * only caller hashes it into a console MAC seed, so it is not cut to 15 chars,
 * which would merge distinct hosts. Same contract as Win32: *size is the length
 * without the NUL, and a short buffer fails with ERROR_BUFFER_OVERFLOW and
 * *size set to what is needed. */
BOOL GetComputerNameA(LPSTR buffer, LPDWORD size)
{
    char name[256];
    size_t len;

    if (!buffer || !size) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (gethostname(name, sizeof name) != 0) {
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    }
    name[sizeof name - 1] = '\0';
    len = strlen(name);
    if (len + 1 > *size) {
        *size = (DWORD)(len + 1);
        SetLastError(ERROR_BUFFER_OVERFLOW);
        return FALSE;
    }
    memcpy(buffer, name, len + 1);
    *size = (DWORD)len;
    return TRUE;
}

BOOL GlobalMemoryStatusEx(LPMEMORYSTATUSEX b)
{
    if (!b) return FALSE;
#if defined(__APPLE__)
    /* Darwin has no sysinfo(2): physical memory comes from sysctl hw.memsize,
     * swap from vm.swapusage, the free page count from the Mach VM statistics. */
    uint64_t memsize = 0;
    size_t   len     = sizeof(memsize);
    int oid_memsize[] = { CTL_HW, HW_MEMSIZE };
    if (sysctl(oid_memsize, 2, &memsize, &len, NULL, 0) != 0) return FALSE;

    vm_size_t page = 0;
    if (host_page_size(mach_host_self(), &page) != KERN_SUCCESS) page = 4096;

    vm_statistics64_data_t vm;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    ULONGLONG avail = 0;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          (host_info64_t)&vm, &count) == KERN_SUCCESS)
        avail = ((ULONGLONG)vm.free_count + vm.inactive_count) * (ULONGLONG)page;

    struct xsw_usage swap;
    len = sizeof(swap);
    int oid_swapusage[] = { CTL_VM, VM_SWAPUSAGE };
    if (sysctl(oid_swapusage, 2, &swap, &len, NULL, 0) != 0)
        memset(&swap, 0, sizeof(swap));

    b->ullTotalPhys     = (ULONGLONG)memsize;
    b->ullAvailPhys     = avail;
    b->ullTotalPageFile = b->ullTotalPhys + (ULONGLONG)swap.xsu_total;
    b->ullAvailPageFile = b->ullAvailPhys + (ULONGLONG)swap.xsu_avail;
#else
    struct sysinfo si;
    if (sysinfo(&si) != 0) return FALSE;

    ULONGLONG unit = si.mem_unit ? si.mem_unit : 1;
    b->ullTotalPhys     = (ULONGLONG)si.totalram  * unit;
    b->ullAvailPhys     = (ULONGLONG)si.freeram   * unit;
    b->ullTotalPageFile = b->ullTotalPhys + (ULONGLONG)si.totalswap * unit;
    b->ullAvailPageFile = b->ullAvailPhys + (ULONGLONG)si.freeswap  * unit;
#endif
    b->ullTotalVirtual  = b->ullTotalPhys;
    b->ullAvailVirtual  = b->ullAvailPhys;
    b->ullAvailExtendedVirtual = 0;
    b->dwMemoryLoad = b->ullTotalPhys
        ? (DWORD)(100 - (b->ullAvailPhys * 100 / b->ullTotalPhys)) : 0;
    return TRUE;
}

/* ===================================================================== */
/* Aligned allocation                                                     */
/* ===================================================================== */

void *_aligned_malloc(SIZE_T size, SIZE_T alignment)
{
    if (alignment < sizeof(void *)) alignment = sizeof(void *);
    /* round alignment up to a power of two */
    SIZE_T a = sizeof(void *);
    while (a < alignment) a <<= 1;
    void *p = NULL;
    if (posix_memalign(&p, a, size ? size : 1) != 0) return NULL;
    return p;
}

void _aligned_free(void *ptr) { free(ptr); }

/* ===================================================================== */
/* Case-insensitive string compare                                        */
/* ===================================================================== */

int _stricmp(const char *a, const char *b)            { return strcasecmp(a, b); }
int _strnicmp(const char *a, const char *b, SIZE_T n) { return strncasecmp(a, b, n); }

/* ===================================================================== */
/* Wide-string helpers (16-bit Xbox WCHAR)                                */
/* ===================================================================== */

SIZE_T xbox_wcslen(const WCHAR *s)
{
    SIZE_T n = 0;
    if (s) while (s[n]) n++;
    return n;
}

int xbox_wcsncmp(const WCHAR *a, const WCHAR *b, SIZE_T n)
{
    for (SIZE_T i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)a[i] - (int)b[i];
        if (a[i] == 0)    return 0;
    }
    return 0;
}

WCHAR *xbox_wcscat(WCHAR *dst, const WCHAR *src)
{
    SIZE_T d = xbox_wcslen(dst), i = 0;
    while (src[i]) { dst[d + i] = src[i]; i++; }
    dst[d + i] = 0;
    return dst;
}

WCHAR *xbox_wcscpy(WCHAR *dst, const WCHAR *src)
{
    SIZE_T i = 0;
    while (src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
    return dst;
}

/* ===================================================================== */
/* Time conversion                                                        */
/* ===================================================================== */

BOOL SystemTimeToFileTime(const SYSTEMTIME *st, LPFILETIME ft)
{
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = st->wYear - 1900;
    tm.tm_mon  = st->wMonth - 1;
    tm.tm_mday = st->wDay;
    tm.tm_hour = st->wHour;
    tm.tm_min  = st->wMinute;
    tm.tm_sec  = st->wSecond;
    time_t t = timegm(&tm);
    ULONGLONG ticks = FILETIME_EPOCH_DIFF
                    + (ULONGLONG)t * 10000000ULL
                    + (ULONGLONG)st->wMilliseconds * 10000ULL;
    ft->dwLowDateTime  = (DWORD)(ticks & 0xFFFFFFFFULL);
    ft->dwHighDateTime = (DWORD)(ticks >> 32);
    return TRUE;
}

BOOL FileTimeToSystemTime(const FILETIME *ft, LPSYSTEMTIME st)
{
    ULONGLONG ticks = ((ULONGLONG)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    if (ticks < FILETIME_EPOCH_DIFF) { memset(st, 0, sizeof(*st)); return FALSE; }
    ULONGLONG since = ticks - FILETIME_EPOCH_DIFF;
    time_t t = (time_t)(since / 10000000ULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    fill_systemtime(st, &tm, (long)((since % 10000000ULL) * 100ULL));
    return TRUE;
}

/* ===================================================================== */
/* Exception handling (compile-shim -- SEH not yet emulated on Linux)     */
/* ===================================================================== */

VOID RtlUnwind(PVOID TargetFrame, PVOID TargetIp,
               PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue)
{
    (void)TargetFrame; (void)TargetIp; (void)ExceptionRecord; (void)ReturnValue;
    /* TODO: Windows SEH unwinding is not yet emulated on Linux. */
}

VOID RaiseException(DWORD code, DWORD flags, DWORD nargs, const ULONG_PTR *args)
{
    (void)flags; (void)nargs; (void)args;
    fprintf(stderr, "[win32_compat] RaiseException(0x%08X): SEH not emulated\n", code);
    /* TODO: on Windows this does not return; SEH dispatch unimplemented. */
}

/* ---- Vectored exception handlers over SIGSEGV/SIGBUS ------------------ */
#if defined(__x86_64__)
#if defined(__APPLE__)
#include <sys/ucontext.h>   /* <ucontext.h> is the deprecated, _XOPEN_SOURCE-only API */
#else
#include <ucontext.h>
#endif

#define VEH_MAX 16
static PVECTORED_EXCEPTION_HANDLER s_veh[VEH_MAX];
static int s_veh_count;
static pthread_mutex_t s_veh_lock = PTHREAD_MUTEX_INITIALIZER;
static struct sigaction s_prev_segv, s_prev_bus, s_prev_trap;
static LPTOP_LEVEL_EXCEPTION_FILTER s_unhandled_filter;
static void veh_install(void);

#if defined(__APPLE__)
#define VEH_GREGS(uc) (&(uc)->uc_mcontext->__ss)
#define VEH_REG(s, linux_name, mac_name) ((s)->mac_name)
#else
#define VEH_GREGS(uc) ((uc)->uc_mcontext.gregs)
#define VEH_REG(s, linux_name, mac_name) ((s)[linux_name])
#endif

/* Each register once, in both directions. */
#define VEH_REGS(X) \
    X(Rax, REG_RAX, __rax) X(Rcx, REG_RCX, __rcx) X(Rdx, REG_RDX, __rdx) \
    X(Rbx, REG_RBX, __rbx) X(Rsp, REG_RSP, __rsp) X(Rbp, REG_RBP, __rbp) \
    X(Rsi, REG_RSI, __rsi) X(Rdi, REG_RDI, __rdi) X(R8, REG_R8, __r8)   \
    X(R9, REG_R9, __r9) X(R10, REG_R10, __r10) X(R11, REG_R11, __r11)   \
    X(R12, REG_R12, __r12) X(R13, REG_R13, __r13) X(R14, REG_R14, __r14) \
    X(R15, REG_R15, __r15) X(Rip, REG_RIP, __rip)

static void veh_signal(int sig, siginfo_t *si, void *raw)
{
    ucontext_t *uc = (ucontext_t *)raw;
    CONTEXT ctx;
    EXCEPTION_RECORD rec;
    EXCEPTION_POINTERS info = { &rec, &ctx };
    struct sigaction *prev = sig == SIGBUS ? &s_prev_bus :
                             sig == SIGTRAP ? &s_prev_trap : &s_prev_segv;
    int i, count = __atomic_load_n(&s_veh_count, __ATOMIC_ACQUIRE);
#if defined(__APPLE__)
    __typeof__(VEH_GREGS(uc)) g = VEH_GREGS(uc);
    int write = (uc->uc_mcontext->__es.__err & 2) != 0;
    ctx.EFlags = (uint32_t)g->__rflags;
#else
    greg_t *g = VEH_GREGS(uc);
    int write = (g[REG_ERR] & 2) != 0;        /* page-fault error code: W */
    ctx.EFlags = (uint32_t)g[REG_EFL];
#endif
#define VEH_LOAD(field, l, m) ctx.field = (uint64_t)VEH_REG(g, l, m);
    VEH_REGS(VEH_LOAD)
    memset(&rec, 0, sizeof rec);
    rec.ExceptionAddress = (PVOID)(uintptr_t)ctx.Rip;
    if (sig == SIGTRAP) {
        /* After the trapped instruction (TF) or past the int3, as on Windows
         * for single-step; a breakpoint's address is the int3 itself. */
        rec.ExceptionCode = si->si_code == TRAP_BRKPT ? EXCEPTION_BREAKPOINT
                                                      : EXCEPTION_SINGLE_STEP;
        if (rec.ExceptionCode == EXCEPTION_BREAKPOINT)
            rec.ExceptionAddress = (PVOID)(uintptr_t)(ctx.Rip - 1);
        /* The signal's si_code, which says what trapped: TRAP_TRACE (the
         * trap flag), TRAP_HWBKPT or TRAP_PERF (a hardware watchpoint) --
         * what Dr6 tells a Win32 handler. */
        rec.NumberParameters = 1;
        rec.ExceptionInformation[0] = (ULONG_PTR)si->si_code;
    } else {
        rec.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
        rec.NumberParameters = 2;
        rec.ExceptionInformation[0] = (ULONG_PTR)write;
        rec.ExceptionInformation[1] = (ULONG_PTR)si->si_addr;
    }

    for (i = 0; i < count; i++) {
        PVECTORED_EXCEPTION_HANDLER h = s_veh[i];
        if (h && h(&info) == EXCEPTION_CONTINUE_EXECUTION) {
#define VEH_STORE(field, l, m) VEH_REG(g, l, m) = ctx.field;
            VEH_REGS(VEH_STORE)
#if defined(__APPLE__)
            g->__rflags = (g->__rflags & ~0xFFFFFFFFull) | ctx.EFlags;
#else
            g[REG_EFL] = (greg_t)(((uint64_t)g[REG_EFL] & ~0xFFFFFFFFull) | ctx.EFlags);
#endif
            return;
        }
    }
    {
        LPTOP_LEVEL_EXCEPTION_FILTER filter =
            __atomic_load_n(&s_unhandled_filter, __ATOMIC_ACQUIRE);
        if (filter && filter(&info) == EXCEPTION_CONTINUE_EXECUTION) {
            VEH_REGS(VEH_STORE)
            return;
        }
    }
    /* Unhandled: whatever was installed before, else the default (re-fault). */
    if (prev->sa_flags & SA_SIGINFO) {
        if (prev->sa_sigaction) { prev->sa_sigaction(sig, si, raw); return; }
    } else if (prev->sa_handler != SIG_DFL && prev->sa_handler != SIG_IGN) {
        prev->sa_handler(sig);
        return;
    }
    signal(sig, SIG_DFL);
    /* A fault re-executes and dies on return; a trap or a raise() would
     * just carry on, so deliver it again under the default action. */
    if (sig == SIGTRAP || si->si_code <= 0)
        raise(sig);
}

/* Once, under s_veh_lock. */
static void veh_install(void)
{
    static int installed;
    struct sigaction sa;
    if (installed) return;
    installed = 1;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = veh_signal;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &s_prev_segv);
    sigaction(SIGBUS, &sa, &s_prev_bus);
    sigaction(SIGTRAP, &sa, &s_prev_trap);
}

LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter)
{
    LPTOP_LEVEL_EXCEPTION_FILTER old;
    pthread_mutex_lock(&s_veh_lock);
    veh_install();
    old = __atomic_exchange_n(&s_unhandled_filter, filter, __ATOMIC_ACQ_REL);
    pthread_mutex_unlock(&s_veh_lock);
    return old;
}

PVOID AddVectoredExceptionHandler(ULONG First, PVECTORED_EXCEPTION_HANDLER Handler)
{
    PVOID result = NULL;
    if (!Handler) return NULL;
    pthread_mutex_lock(&s_veh_lock);
    if (s_veh_count < VEH_MAX) {
        veh_install();
        if (First) {
            memmove(&s_veh[1], &s_veh[0], (size_t)s_veh_count * sizeof s_veh[0]);
            s_veh[0] = Handler;
        } else {
            s_veh[s_veh_count] = Handler;
        }
        __atomic_store_n(&s_veh_count, s_veh_count + 1, __ATOMIC_RELEASE);
        result = (PVOID)Handler;
    }
    pthread_mutex_unlock(&s_veh_lock);
    return result;
}

/* ponytail: removal clears the slot rather than compacting, so a fault in
 * flight never sees the array shift; 16 registrations per process. */
ULONG RemoveVectoredExceptionHandler(PVOID h)
{
    int i;
    ULONG found = 0;
    pthread_mutex_lock(&s_veh_lock);
    for (i = 0; i < s_veh_count; i++)
        if ((PVOID)s_veh[i] == h) { s_veh[i] = NULL; found = 1; break; }
    pthread_mutex_unlock(&s_veh_lock);
    return found;
}
#else
PVOID AddVectoredExceptionHandler(ULONG First, PVECTORED_EXCEPTION_HANDLER Handler)
{ (void)First; (void)Handler; return NULL; }   /* MMIO decoding is x86-64 only */
ULONG RemoveVectoredExceptionHandler(PVOID h) { (void)h; return 0; }
LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f)
{ (void)f; return NULL; }
#endif

/* ---- Console control over signals ------------------------------------- */
static PHANDLER_ROUTINE s_console_handler;

static void console_signal(int sig)
{
    PHANDLER_ROUTINE h = __atomic_load_n(&s_console_handler, __ATOMIC_ACQUIRE);
    DWORD event = sig == SIGINT ? CTRL_C_EVENT : CTRL_CLOSE_EVENT;
    if (h && h(event)) return;
    signal(sig, SIG_DFL);
    raise(sig);
}

/* ponytail: one handler, the last one added; MM3 registers exactly one. */
BOOL SetConsoleCtrlHandler(PHANDLER_ROUTINE handler, BOOL add)
{
    if (!handler) return TRUE;
    __atomic_store_n(&s_console_handler, add ? handler : NULL, __ATOMIC_RELEASE);
    signal(SIGINT, add ? console_signal : SIG_DFL);
    signal(SIGTERM, add ? console_signal : SIG_DFL);
    signal(SIGHUP, add ? console_signal : SIG_DFL);
    return TRUE;
}

/* ---- Process and module queries --------------------------------------- */
#include <dlfcn.h>
#include <execinfo.h>
#if defined(__linux__)
#include <sys/uio.h>
#endif

HMODULE GetModuleHandleA(LPCSTR name)
{
    Dl_info info;
    if (name) return NULL;
    /* This file is linked into the executable, so its base is the module's. */
    return dladdr((void *)GetModuleHandleA, &info) ? (HMODULE)info.dli_fbase : NULL;
}

HMODULE GetModuleHandleW(LPCWSTR name)
{
    return name ? NULL : GetModuleHandleA(NULL);
}

USHORT CaptureStackBackTrace(ULONG skip, ULONG count, PVOID *frames, PULONG hash)
{
    void *all[128];
    int n = backtrace(all, 128), i, out = 0;
    if (hash) *hash = 0;
    for (i = (int)skip + 1; i < n && out < (int)count; i++)   /* +1: this frame */
        frames[out++] = all[i];
    return (USHORT)out;
}

BOOL ReadProcessMemory(HANDLE process, LPCVOID address, LPVOID buffer, SIZE_T size,
                       SIZE_T *read)
{
    ssize_t got = -1;
    (void)process;
    if (read) *read = 0;
#if defined(__linux__)
    {
        struct iovec local = { buffer, size }, remote = { (void *)address, size };
        got = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    }
#elif defined(__APPLE__)
    {
        mach_vm_size_t n = 0;
        if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)address, size,
                                   (mach_vm_address_t)buffer, &n) == KERN_SUCCESS)
            got = (ssize_t)n;
    }
#endif
    if (got < 0 || (SIZE_T)got != size) {
        SetLastError(ERROR_PARTIAL_COPY);
        if (read && got > 0) *read = (SIZE_T)got;
        return FALSE;
    }
    if (read) *read = size;
    return TRUE;
}

/* An empty value removes the variable, as on Windows. */
int _putenv_s(const char *name, const char *value)
{
    if (!name || !value) return EINVAL;
    return (*value ? setenv(name, value, 1) : unsetenv(name)) ? errno : 0;
}

int strcpy_s(char *dst, size_t size, const char *src)
{
    size_t len;
    if (!dst || !size) return EINVAL;
    if (!src) { dst[0] = '\0'; return EINVAL; }
    len = strlen(src);
    if (len >= size) { dst[0] = '\0'; return ERANGE; }
    memcpy(dst, src, len + 1);
    return 0;
}

#endif /* !_WIN32 */
