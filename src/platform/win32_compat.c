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
    if (ms == INFINITE) {
        pthread_cond_wait(c, m);
        return TRUE;
    }
    struct timespec ts;
    deadline_from_ms(ms, &ts);
    int rc = pthread_cond_timedwait(c, m, &ts);
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

typedef struct w32_object {
    w32_kind        kind;
    LONG            refcount;
    pthread_mutex_t lock;
    pthread_cond_t  cond;

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
    int             thread_joinable;
    DWORD           tid;
    int             exited;
    DWORD           exit_code;
    int             suspend_count;
    pthread_cond_t  gate;
    LPTHREAD_START_ROUTINE start;
    LPVOID          start_param;
    int             priority;
    PAPCFUNC        apc_func[W32_MAX_APC];
    ULONG_PTR       apc_data[W32_MAX_APC];
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
    int             waitable_triggered;
    int             waitable_armed;

    /* file mapping / fd-backed file handle */
    int             fd;
    SIZE_T          map_size;
    char           *file_path;
} w32_object;

/* pseudo handles for "current thread"/"current process" */
#define PSEUDO_CURRENT_PROCESS ((HANDLE)(LONG_PTR)-1)
#define PSEUDO_CURRENT_THREAD  ((HANDLE)(LONG_PTR)-2)
#define STILL_ACTIVE 259u

static __thread w32_object *t_self_obj = NULL;
static __thread DWORD       t_tid      = 0;
static volatile LONG        s_next_tid = 1000;

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
    o->kind     = kind;
    o->refcount = 1;
    pthread_mutex_init(&o->lock, NULL);
    pthread_cond_init(&o->cond, NULL);
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
    pthread_cond_destroy(&o->gate);
    free(o);
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
    {
        w32_object *o = obj_lookup(h);
        if (!o) return FALSE;
        if (o != &s_process_heap) obj_release(o);
    }
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
    InterlockedIncrement(&o->refcount);
    *dst = src;
    if (options & DUPLICATE_CLOSE_SOURCE)
        obj_release(o);
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

/* Run any pending user-APCs for the calling thread. Returns count run. */
static int drain_apcs(void)
{
    w32_object *o = t_self_obj;
    int run = 0;
    if (!o) return 0;
    pthread_mutex_lock(&o->lock);
    while (o->apc_count > 0) {
        PAPCFUNC  f = o->apc_func[0];
        ULONG_PTR d = o->apc_data[0];
        memmove(o->apc_func, o->apc_func + 1, sizeof(PAPCFUNC) * (o->apc_count - 1));
        memmove(o->apc_data, o->apc_data + 1, sizeof(ULONG_PTR) * (o->apc_count - 1));
        o->apc_count--;
        pthread_mutex_unlock(&o->lock);
        f(d);
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
    struct timespec now;
    if (o->waitable_triggered) return 1;
    if (!o->waitable_armed) return 0;
    clock_gettime(CLOCK_REALTIME, &now);
    if (timespec_before(&now, &o->waitable_due_time)) return 0;
    o->waitable_triggered = 1;
    return 1;
}

/*
 * Wait on a single object. The object lock must NOT be held.
 * Returns WAIT_OBJECT_0 / WAIT_TIMEOUT.
 */
static DWORD wait_single(w32_object *o, DWORD ms)
{
    struct timespec ts;
    int timed = (ms != INFINITE);
    if (timed) deadline_from_ms(ms, &ts);

    pthread_mutex_lock(&o->lock);
    DWORD result = WAIT_OBJECT_0;

    for (;;) {
        int ready = 0;
        switch (o->kind) {
        case K_EVENT:  ready = o->signaled; break;
        case K_THREAD: ready = o->exited;   break;
        case K_SEM:    ready = (o->sem_count > 0); break;
        case K_MUTEX:
            ready = (o->mtx_owner == 0 || o->mtx_owner == GetCurrentThreadId());
            break;
        case K_WAITABLE_TIMER: ready = waitable_due(o); break;
        case K_PROCESS: ready = 0; break;   /* signalled at exit; never here */
        default:       ready = 1; break;
        }
        if (ready) break;

        /* An armed timer has its own deadline. Waiting on the caller's alone
         * would sleep straight past the due time, so take whichever comes
         * first and re-test. */
        struct timespec until = ts;
        int bounded = timed;
        if (o->kind == K_WAITABLE_TIMER && o->waitable_armed &&
            (!timed || timespec_before(&o->waitable_due_time, &ts))) {
            until = o->waitable_due_time;
            bounded = 1;
        }
        int rc = bounded ? pthread_cond_timedwait(&o->cond, &o->lock, &until)
                         : pthread_cond_wait(&o->cond, &o->lock);
        if (rc == ETIMEDOUT && timed && !timespec_before(&until, &ts)) {
            result = WAIT_TIMEOUT; break;
        }
    }

    if (result == WAIT_OBJECT_0) {
        switch (o->kind) {
        case K_EVENT: if (!o->manual_reset) o->signaled = 0; break;
        case K_WAITABLE_TIMER:
            if (!o->waitable_manual_reset) { o->waitable_triggered = 0; o->waitable_armed = 0; }
            break;
        case K_SEM:   o->sem_count--; break;
        case K_MUTEX: o->mtx_owner = GetCurrentThreadId(); o->mtx_recursion++; break;
        default: break;
        }
    }
    pthread_mutex_unlock(&o->lock);
    return result;
}

DWORD WaitForSingleObject(HANDLE h, DWORD ms)
{
    if (!h || h == PSEUDO_CURRENT_THREAD || h == PSEUDO_CURRENT_PROCESS)
        return WAIT_OBJECT_0;
    {
        w32_object *o = obj_lookup(h);
        if (!o) return WAIT_FAILED;
        return wait_single(o, ms);
    }
}

DWORD WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable)
{
    if (alertable && drain_apcs() > 0)
        return WAIT_IO_COMPLETION;
    return WaitForSingleObject(h, ms);
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
    struct timespec ts;
    int timed = (ms != INFINITE);
    if (timed) deadline_from_ms(ms, &ts);

    for (;;) {
        if (alertable && drain_apcs() > 0)
            return WAIT_IO_COMPLETION;

        if (waitAll) {
            DWORD got = 0;
            for (DWORD i = 0; i < count; i++)
                if (WaitForSingleObject(handles[i], 0) == WAIT_OBJECT_0) got++;
            if (got == count) return WAIT_OBJECT_0;
        } else {
            for (DWORD i = 0; i < count; i++)
                if (WaitForSingleObject(handles[i], 0) == WAIT_OBJECT_0)
                    return WAIT_OBJECT_0 + i;
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

static void *thread_trampoline(void *arg)
{
    w32_object *o = (w32_object *)arg;
    t_self_obj = o;
    t_tid      = o->tid;

    /* CREATE_SUSPENDED gate */
    pthread_mutex_lock(&o->lock);
    while (o->suspend_count > 0)
        pthread_cond_wait(&o->gate, &o->lock);
    pthread_mutex_unlock(&o->lock);

    DWORD rc = o->start ? o->start(o->start_param) : 0;

    pthread_mutex_lock(&o->lock);
    o->exit_code = rc;
    o->exited    = 1;
    o->signaled  = 1;
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->lock);

    obj_release(o);   /* drop the trampoline's reference */
    return NULL;
}

HANDLE CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stackSize,
                    LPTHREAD_START_ROUTINE start, LPVOID param,
                    DWORD flags, LPDWORD threadId)
{
    (void)sa;
    w32_object *o = obj_alloc(K_THREAD);
    o->start         = start;
    o->start_param   = param;
    o->tid           = (DWORD)InterlockedIncrement(&s_next_tid);
    o->suspend_count = (flags & CREATE_SUSPENDED) ? 1 : 0;
    o->refcount      = 2;   /* one for caller, one for the trampoline */

    pthread_attr_t attr;
    pthread_attr_init(&attr);
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
    o->thread_joinable = 1;

    if (threadId) *threadId = o->tid;
    return (HANDLE)o;
}

VOID ExitThread(DWORD exitCode)
{
    w32_object *o = t_self_obj;
    if (o) {
        pthread_mutex_lock(&o->lock);
        o->exit_code = exitCode;
        o->exited    = 1;
        o->signaled  = 1;
        pthread_cond_broadcast(&o->cond);
        pthread_mutex_unlock(&o->lock);
        obj_release(o);
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
    w32_object *o = obj_lookup(h);
    if (!o || o->kind != K_THREAD) return FALSE;
    pthread_cancel(o->thread);
    pthread_mutex_lock(&o->lock);
    o->exit_code = exitCode;
    o->exited    = 1;
    o->signaled  = 1;
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->lock);
    return TRUE;
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
    w32_object *o = (thread == PSEUDO_CURRENT_THREAD) ? t_self_obj : obj_lookup(thread);
    if (!o || o->kind != K_THREAD) return 0;
    pthread_mutex_lock(&o->lock);
    DWORD ok = 0;
    if (o->apc_count < W32_MAX_APC) {
        o->apc_func[o->apc_count] = func;
        o->apc_data[o->apc_count] = data;
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
    if (ms == 0) { sched_yield(); return; }
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { }
}

DWORD SleepEx(DWORD ms, BOOL alertable)
{
    if (alertable && drain_apcs() > 0)
        return WAIT_IO_COMPLETION;
    Sleep(ms);
    if (alertable && drain_apcs() > 0)
        return WAIT_IO_COMPLETION;
    return 0;
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
    return (HANDLE)o;
}

BOOL SetWaitableTimer(HANDLE h, const LARGE_INTEGER *dueTime, LONG period,
                      PTIMERAPCROUTINE completion, PVOID arg, BOOL resume)
{
    w32_object *o = obj_lookup(h);
    (void)completion; (void)arg; (void)resume;
    if (!o || o->kind != K_WAITABLE_TIMER || !dueTime) return FALSE;
    pthread_mutex_lock(&o->lock);
    /* Win32 100ns units: negative is relative to now, positive is an absolute
     * FILETIME. ponytail: period is ignored -- one-shot only, revisit if a
     * title actually arms a repeating timer. */
    if (dueTime->QuadPart <= 0) {
        clock_gettime(CLOCK_REALTIME, &o->waitable_due_time);
        LONGLONG ns = -dueTime->QuadPart * 100LL;
        o->waitable_due_time.tv_sec  += (time_t)(ns / 1000000000LL);
        o->waitable_due_time.tv_nsec += (long)(ns % 1000000000LL);
        if (o->waitable_due_time.tv_nsec >= 1000000000L) {
            o->waitable_due_time.tv_sec++;
            o->waitable_due_time.tv_nsec -= 1000000000L;
        }
    } else {
        /* FILETIME epoch is 1601-01-01; Unix is 1970-01-01. */
        LONGLONG unix100ns = dueTime->QuadPart - 116444736000000000LL;
        o->waitable_due_time.tv_sec  = (time_t)(unix100ns / 10000000LL);
        o->waitable_due_time.tv_nsec = (long)((unix100ns % 10000000LL) * 100LL);
    }
    (void)period;
    o->waitable_armed = 1;
    o->waitable_triggered = 0;
    pthread_mutex_unlock(&o->lock);
    pthread_cond_broadcast(&o->cond);
    return TRUE;
}

BOOL CancelWaitableTimer(HANDLE h)
{
    w32_object *o = obj_lookup(h);
    if (!o || o->kind != K_WAITABLE_TIMER) return FALSE;
    pthread_mutex_lock(&o->lock);
    o->waitable_triggered = 0;
    o->waitable_armed = 0;
    pthread_mutex_unlock(&o->lock);
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

/* Length registry, defined with the view helpers below. Win32 frees by address
 * alone -- UnmapViewOfFile takes no length and VirtualFree(MEM_RELEASE) is
 * documented to take size 0 -- so the length has to be recoverable here or
 * munmap cannot be called at all. */
void view_register(void *addr, size_t len);
size_t view_take(const void *addr);
static int held_contains(const void *addr, size_t len);
static int held_release(void *addr, size_t len);
static void held_add(void *addr, size_t len);
static void held_drop(const void *addr, size_t len);

LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD allocationType, DWORD protect)
{
    int prot  = prot_from_page(protect);
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;

    if (prot < 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    /* MEM_COMMIT on a region already reserved by a prior VirtualAlloc:
     * just adjust protection. */
    if ((allocationType & MEM_COMMIT) && !(allocationType & MEM_RESERVE) && address) {
        if (mprotect(address, size, prot) == 0)
            return address;
        /* fall through to a fresh mapping */
    }

    /* Inside one of our own reservations the range is ours to replace, the
     * way Win32 replaces a placeholder; anywhere else a fixed address must
     * not displace someone else's mapping. */
    int replace = address && held_contains(address, size);
    if (replace)
        flags |= MAP_FIXED;
#if defined(MAP_FIXED_NOREPLACE)
    else if (address) flags |= MAP_FIXED_NOREPLACE;
#endif

    /* MEM_RESERVE alone maps no access: a reserved page is neither readable nor
     * writable until it is committed, and VirtualQuery reports it that way. */
    int map_prot = (allocationType & MEM_RESERVE) && !(allocationType & MEM_COMMIT)
                   ? PROT_NONE : (prot ? prot : PROT_READ | PROT_WRITE);
    void *p;
#if defined(__APPLE__)
    if (address && !replace) {
        p = mach_map_fixed(address, size, map_prot);
    } else
#endif
    p = mmap(address, size, map_prot, flags, -1, 0);
    if (p == MAP_FAILED) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    if (replace)
        held_drop(p, size);                 /* now an allocation of its own */
    if ((allocationType & MEM_RESERVE) && !(allocationType & MEM_COMMIT))
        held_add(p, size);                  /* and still a reservation */
    /* Remember the length: VirtualFree(MEM_RELEASE) is passed size 0 by every
     * Win32 caller, and munmap cannot be called without one. */
    view_register(p, size);
#if !defined(MAP_FIXED_NOREPLACE) && !defined(__APPLE__)
    /* Older kernels without MAP_FIXED_NOREPLACE: plain MAP_FIXED would silently
     * unmap whatever already lives there, so we pass the address as a hint and
     * treat a different result as "taken". Apple goes through mach_map_fixed
     * above, which reports that properly instead of inferring it. */
    if (address && p != address) {
        munmap(p, size);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
#endif
    return p;
}

BOOL VirtualFree(LPVOID address, SIZE_T size, DWORD freeType)
{
    if (freeType & MEM_RELEASE) {
        /* Win32 MEM_RELEASE passes size 0 and frees the whole allocation, so
         * the length comes from the registry VirtualAlloc filled in. Returning
         * TRUE without unmapping -- as this used to -- made every release a
         * silent no-op: the caller believed the address was free, the next
         * allocation there failed, and nothing connected the two. */
        /* Part of one of our reservations: keep holding it (no access, no
         * memory) so the slice can be mapped over atomically, instead of
         * opening a hole another thread's mmap can land in first. */
        if (size && held_release(address, size))
            return TRUE;
        size_t len = view_take(address);
        if (size == 0) size = len;
        if (size == 0) return FALSE;
        held_drop(address, size);
        return munmap(address, size) == 0;
    }
    if (freeType & MEM_DECOMMIT)
        return mprotect(address, size, PROT_NONE) == 0;
    return TRUE;
}

/* Host page state, read back from the kernel (/proc/self/maps on Linux,
 * mach_vm_region on Darwin). Win32 protection and commit state derive from it,
 * so there is no parallel record to drift from the page tables. */
typedef struct {
    uintptr_t start, end;   /* [start, end) */
    int       mapped;       /* 0: [addr, start) is a gap */
    int       perms;        /* PROT_* of the mapping */
} w32_host_region;

/* The region holding addr, or else the first one after it. Returns 1 when a
 * region was found, 0 when nothing maps at or after addr, -1 when the host
 * state cannot be read. */
#if defined(__APPLE__)
static int host_region_at(uintptr_t addr, w32_host_region *r)
{
    mach_vm_address_t at = (mach_vm_address_t)addr;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &at, &size, VM_REGION_BASIC_INFO_64,
                       (vm_region_info_t)&info, &count, &object) != KERN_SUCCESS)
        return 0;
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    r->start  = (uintptr_t)at;
    r->end    = (uintptr_t)(at + size);
    r->mapped = (uintptr_t)at <= addr;
    r->perms  = ((info.protection & VM_PROT_READ)    ? PROT_READ  : 0) |
                ((info.protection & VM_PROT_WRITE)   ? PROT_WRITE : 0) |
                ((info.protection & VM_PROT_EXECUTE) ? PROT_EXEC  : 0);
    return 1;
}
#else
static int host_region_at(uintptr_t addr, w32_host_region *r)
{
    char line[1024];
    FILE *maps = fopen("/proc/self/maps", "r");
    int found = 0;
    if (!maps) return -1;
    /* Lines are address-ordered. A line longer than the buffer is cut, but the
     * fields read here come first, and the tail fails the sscanf below. */
    while (fgets(line, sizeof line, maps)) {
        unsigned long lo, hi;
        char perm[5] = "";
        if (sscanf(line, "%lx-%lx %4s", &lo, &hi, perm) != 3) continue;
        if ((uintptr_t)hi <= addr) continue;
        r->start  = (uintptr_t)lo;
        r->end    = (uintptr_t)hi;
        r->mapped = (uintptr_t)lo <= addr;
        r->perms  = (perm[0] == 'r' ? PROT_READ  : 0) |
                    (perm[1] == 'w' ? PROT_WRITE : 0) |
                    (perm[2] == 'x' ? PROT_EXEC  : 0);
        found = 1;
        break;
    }
    fclose(maps);
    return found;
}
#endif

static DWORD protect_from_perms(int perms)
{
    switch (perms & (PROT_READ | PROT_WRITE | PROT_EXEC)) {
    case 0:                              return PAGE_NOACCESS;
    case PROT_READ:                      return PAGE_READONLY;
    case PROT_READ | PROT_WRITE:
    case PROT_WRITE:                     return PAGE_READWRITE;
    case PROT_EXEC:                      return PAGE_EXECUTE;
    case PROT_READ | PROT_EXEC:          return PAGE_EXECUTE_READ;
    default:                             return PAGE_EXECUTE_READWRITE;
    }
}

BOOL VirtualProtect(LPVOID address, SIZE_T size, DWORD newProtect, PDWORD oldProtect)
{
    int prot = prot_from_page(newProtect);

    if (prot < 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    w32_host_region r;
    /* Win32 reports the protection of the first page of the range. */
    int known = host_region_at((uintptr_t)address, &r) == 1 && r.mapped;
    if (oldProtect) *oldProtect = known ? protect_from_perms(r.perms) : PAGE_READWRITE;
    if (mprotect(address, size, prot) == 0) return TRUE;
    SetLastError(ERROR_INVALID_ADDRESS);
    return FALSE;
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

/* Registry of active views: UnmapViewOfFile takes no length, so we must
 * recover the mapping length here for munmap. */
typedef struct { void *addr; size_t len; } w32_view;
static w32_view        s_views[512];
static pthread_mutex_t s_views_lock = PTHREAD_MUTEX_INITIALIZER;

void view_register(void *addr, size_t len)
{
    pthread_mutex_lock(&s_views_lock);
    for (int i = 0; i < 512; i++)
        if (!s_views[i].addr) { s_views[i].addr = addr; s_views[i].len = len; break; }
    pthread_mutex_unlock(&s_views_lock);
}

/* Non-destructive counterpart to view_take, and interior-aware: VirtualQuery
 * is asked about addresses *within* a region at least as often as about its
 * base -- a translated guest VA lands in the middle of the 64 MB window.
 * Picks the containing region and reports where it starts. */
int view_lookup(const void *addr, void **base_out, size_t *len_out)
{
    int found = 0;
    pthread_mutex_lock(&s_views_lock);
    for (int i = 0; i < 512; i++) {
        if (!s_views[i].addr)
            continue;
        uintptr_t lo = (uintptr_t)s_views[i].addr;
        uintptr_t hi = lo + s_views[i].len;
        if ((uintptr_t)addr >= lo && (uintptr_t)addr < hi) {
            if (base_out) *base_out = s_views[i].addr;
            if (len_out)  *len_out  = s_views[i].len;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&s_views_lock);
    return found;
}

/* Reservations made by VirtualAlloc(MEM_RESERVE) and still held: address
 * ranges this process owns, which a fixed-address map may replace in place.
 * ponytail: a small array under the views lock; the memory layout makes a
 * handful of these. */
typedef struct { uintptr_t lo, hi; } w32_held;
static w32_held s_held[64];

static void held_add(void *addr, size_t len)
{
    pthread_mutex_lock(&s_views_lock);
    for (int i = 0; i < 64; i++)
        if (!s_held[i].hi) { s_held[i].lo = (uintptr_t)addr; s_held[i].hi = (uintptr_t)addr + len; break; }
    pthread_mutex_unlock(&s_views_lock);
}

static int held_contains(const void *addr, size_t len)
{
    uintptr_t lo = (uintptr_t)addr, hi = lo + len;
    int found = 0;
    pthread_mutex_lock(&s_views_lock);
    for (int i = 0; i < 64 && !found; i++)
        found = s_held[i].hi && lo >= s_held[i].lo && hi <= s_held[i].hi;
    pthread_mutex_unlock(&s_views_lock);
    return found;
}

/* A partial release inside a held reservation: discard the contents and keep
 * the range held. 1 if handled; 0 if not inside one, or if it is the whole
 * reservation (a real release). */
static int held_release(void *addr, size_t len)
{
    uintptr_t lo = (uintptr_t)addr, hi = lo + len;
    int partial = 0;
    pthread_mutex_lock(&s_views_lock);
    for (int i = 0; i < 64; i++)
        if (s_held[i].hi && lo >= s_held[i].lo && hi <= s_held[i].hi) {
            partial = !(lo == s_held[i].lo && hi == s_held[i].hi);
            break;
        }
    pthread_mutex_unlock(&s_views_lock);
    if (!partial) return 0;
    return mmap(addr, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE,
                -1, 0) != MAP_FAILED;
}

/* Stop holding [addr, addr+len): it was released or mapped over. A held range
 * that loses a piece from the middle keeps both ends. */
static void held_drop(const void *addr, size_t len)
{
    uintptr_t lo = (uintptr_t)addr, hi = lo + len;
    pthread_mutex_lock(&s_views_lock);
    for (int i = 0; i < 64; i++) {
        w32_held h = s_held[i];
        if (!h.hi || hi <= h.lo || lo >= h.hi) continue;
        s_held[i].lo = s_held[i].hi = 0;
        if (h.lo < lo) { s_held[i].lo = h.lo; s_held[i].hi = lo; }
        if (hi < h.hi) {
            if (!s_held[i].hi) { s_held[i].lo = hi; s_held[i].hi = h.hi; }
            else for (int j = 0; j < 64; j++)
                if (!s_held[j].hi) { s_held[j].lo = hi; s_held[j].hi = h.hi; break; }
        }
    }
    pthread_mutex_unlock(&s_views_lock);
}

size_t view_take(const void *addr)
{
    size_t len = 0;
    pthread_mutex_lock(&s_views_lock);
    for (int i = 0; i < 512; i++)
        if (s_views[i].addr == addr) { len = s_views[i].len; s_views[i].addr = NULL; break; }
    pthread_mutex_unlock(&s_views_lock);
    return len;
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
    (void)file; (void)sa; (void)protect;
    SIZE_T size = ((SIZE_T)maxSizeHigh << 32) | maxSizeLow;
    if (size == 0) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }

    int fd = anon_map_fd(name);
    if (fd < 0) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }

    w32_object *o = obj_alloc(K_FILEMAP);
    o->fd       = fd;
    o->map_size = size;
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
    w32_object *o = (w32_object *)mapping;
    if (!o || o->kind != K_FILEMAP) { SetLastError(ERROR_INVALID_HANDLE); return NULL; }

    off_t  off = ((off_t)offHigh << 32) | offLow;
    SIZE_T len = count ? count : (o->map_size - (SIZE_T)off);
    int prot   = PROT_READ | ((access != FILE_MAP_READ) ? PROT_WRITE : 0);
    int flags  = MAP_SHARED;

    /* Win32 MapViewOfFileEx *fails* when the requested address is unavailable.
     * Plain MAP_FIXED does the opposite: it silently unmaps whatever is there
     * and succeeds. The Xbox memory model maps its RAM view at an address it
     * reserved, so with a base the OS chose rather than one we picked, that
     * difference is the process quietly destroying its own libraries and heap
     * and dying somewhere unrelated a moment later. */
    int replace = baseAddr && held_contains(baseAddr, len);
    if (replace) {
        flags |= MAP_FIXED;                 /* over our own reservation */
    } else if (baseAddr) {
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
    if (replace)
        held_drop(p, len);
    view_register(p, len);
    return p;
}

LPVOID MapViewOfFile(HANDLE mapping, DWORD access, DWORD offHigh, DWORD offLow, SIZE_T count)
{
    return MapViewOfFileEx(mapping, access, offHigh, offLow, count, NULL);
}

BOOL UnmapViewOfFile(LPCVOID baseAddr)
{
    size_t len = view_take(baseAddr);
    if (len == 0) return FALSE;
    return munmap((void *)baseAddr, len) == 0;
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

    void  *base = NULL;
    size_t len  = 0;

    if (!view_lookup(address, &base, &len)) {
        /* Not ours. Report it free rather than inventing a committed page. */
        buffer->BaseAddress    = (PVOID)address;
        buffer->AllocationBase = NULL;
        buffer->RegionSize     = 0;
        buffer->State          = MEM_FREE;
        buffer->Protect        = PAGE_NOACCESS;
        buffer->Type           = 0;
        return sizeof(*buffer);
    }

    /* The state is the host's: a page with no access is a reservation (Win32
     * MEM_RESERVE, which also covers a decommitted page), anything else is
     * committed. Without /proc the registry alone says it is committed RW. */
    uintptr_t addr      = (uintptr_t)address;
    uintptr_t page_size = (uintptr_t)sysconf(_SC_PAGESIZE);
    uintptr_t page      = addr & ~(page_size - 1);
    uintptr_t limit     = (uintptr_t)base + ((len + page_size - 1) & ~(page_size - 1));
    w32_host_region r, first, next;
    int rc = host_region_at(addr, &r);

    if (rc == 0 || (rc > 0 && !r.mapped)) {
        /* Owned by this layer but not mapped on the host: nothing to report. */
        buffer->BaseAddress    = (PVOID)page;
        buffer->AllocationBase = NULL;
        buffer->State          = MEM_FREE;
        buffer->Protect        = PAGE_NOACCESS;
        return sizeof(*buffer);
    }
    if (rc < 0) {
        r.start = addr; r.end = limit; r.mapped = 1; r.perms = PROT_READ | PROT_WRITE;
    }

    /* RegionSize is the run from this page of pages with the same state and
     * protection, within the allocation. Callers sizing a copy depend on it. */
    uintptr_t end = r.end < limit ? r.end : limit;
    while (end < limit && host_region_at(end, &next) == 1 && next.mapped &&
           next.start == end && next.perms == r.perms)
        end = next.end < limit ? next.end : limit;

    if (host_region_at((uintptr_t)base, &first) == 1 && first.mapped)
        buffer->AllocationProtect = protect_from_perms(first.perms);
    else
        buffer->AllocationProtect = PAGE_READWRITE;
    buffer->BaseAddress      = (PVOID)page;
    buffer->AllocationBase   = base;
    buffer->RegionSize       = end - page;
    buffer->State            = r.perms == 0 ? MEM_RESERVE : MEM_COMMIT;
    buffer->Protect          = protect_from_perms(r.perms);
    buffer->Type             = MEM_PRIVATE;
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
