/*
 * kernel_thread.c - Xbox Threading Subsystem
 *
 * Implements Xbox thread creation, termination, delays, and priority
 * management using Win32 threading APIs.
 *
 * Xbox threading model:
 *   - PsCreateSystemThreadEx creates kernel-mode threads (→ CreateThread)
 *   - Thread start routines are __stdcall with a single PVOID context
 *   - Time intervals use NT 100-nanosecond units (negative = relative)
 *   - Thread priorities use NT KPRIORITY increments
 */

#include "kernel.h"
#include "xbox_memory_layout.h"   /* XBOX_WORKER_STACK_* + worker-stack decls */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Thread Start Wrapper
 *
 * Xbox start routines are __stdcall void(*)(PVOID), but Win32 CreateThread
 * expects DWORD WINAPI (*)(LPVOID). We wrap the Xbox routine to bridge
 * the calling convention and return type.
 * ============================================================================ */

typedef struct _XBOX_THREAD_START_INFO {
    PXBOX_START_ROUTINE StartRoutine;
    PXBOX_SYSTEM_ROUTINE SystemRoutine;
    PVOID StartContext;
} XBOX_THREAD_START_INFO;

static DWORD WINAPI xbox_thread_wrapper(LPVOID parameter)
{
    XBOX_THREAD_START_INFO info=*(XBOX_THREAD_START_INFO *)parameter;
    /* Both callbacks may terminate the host thread without returning. */
    HeapFree(GetProcessHeap(),0,parameter);
    if (info.SystemRoutine) info.SystemRoutine(info.StartRoutine,info.StartContext);
    else info.StartRoutine(info.StartContext);
    return 0;
}

NTSTATUS __stdcall xbox_PsCreateSystemThreadEx(
    PHANDLE ThreadHandle, ULONG ThreadExtraSize, ULONG KernelStackSize,
    ULONG TlsDataSize, PULONG ThreadId, PXBOX_START_ROUTINE StartRoutine,
    PVOID StartContext, BOOLEAN CreateSuspended, BOOLEAN DebuggerThread,
    PXBOX_SYSTEM_ROUTINE SystemRoutine)
{
    XBOX_THREAD_START_INFO *info;
    DWORD id=0;
    HANDLE thread;
    if (ThreadHandle) *ThreadHandle=NULL;
    if (ThreadId) *ThreadId=0;
    if (!ThreadHandle || !StartRoutine) return STATUS_INVALID_PARAMETER;
    if (ThreadExtraSize || TlsDataSize || DebuggerThread) return STATUS_NOT_SUPPORTED;
    info=HeapAlloc(GetProcessHeap(),0,sizeof(*info));
    if (!info) return STATUS_NO_MEMORY;
    info->StartRoutine=StartRoutine; info->SystemRoutine=SystemRoutine;
    info->StartContext=StartContext;
    thread=CreateThread(NULL,KernelStackSize,xbox_thread_wrapper,info,
                        CreateSuspended?CREATE_SUSPENDED:0,&id);
    if (!thread) {
        HeapFree(GetProcessHeap(),0,info);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    *ThreadHandle=thread;
    if (ThreadId) *ThreadId=id;
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_PsCreateSystemThread(
    PHANDLE ThreadHandle, PULONG ThreadId, PXBOX_START_ROUTINE StartRoutine,
    PVOID StartContext, BOOLEAN DebuggerThread)
{
    return xbox_PsCreateSystemThreadEx(ThreadHandle,0,0,0,ThreadId,
                                     StartRoutine,StartContext,FALSE,DebuggerThread,NULL);
}

/* ============================================================================
 * PsTerminateSystemThread
 *
 * Terminates the calling thread. On Xbox this is the standard way for
 * system threads to exit. Maps directly to ExitThread.
 * ============================================================================ */

VOID __stdcall xbox_PsTerminateSystemThread(NTSTATUS ExitStatus)
{
    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_THREAD,
        "PsTerminateSystemThread: thread %u exiting with status 0x%08X",
        GetCurrentThreadId(), ExitStatus);

    ExitThread((DWORD)ExitStatus);

}

/* ============================================================================
 * KeDelayExecutionThread
 *
 * Delays the current thread. The interval uses NT 100-nanosecond units:
 *   - Negative values = relative delay (most common)
 *   - Positive values = absolute time (rare)
 *   - Zero = yield
 *
 * Maps to: SleepEx (for alertable waits) or Sleep
 * ============================================================================ */

NTSTATUS __stdcall xbox_KeDelayExecutionThread(
    KPROCESSOR_MODE WaitMode,
    BOOLEAN Alertable,
    PLARGE_INTEGER Interval)
{
    DWORD ms;

    (void)WaitMode;

    if (!Interval)
        return STATUS_INVALID_PARAMETER;

    if (Interval->QuadPart == 0) {
        /* Zero interval = yield the thread's time slice */
        SwitchToThread();
        return STATUS_SUCCESS;
    }

    if (Interval->QuadPart < 0) {
        /* Negative = relative time in 100ns units. Convert to milliseconds. */
        LONGLONG relative_100ns = -Interval->QuadPart;
        ms = (DWORD)(relative_100ns / 10000);
        /* Ensure at least 1ms for very short intervals */
        if (ms == 0 && relative_100ns > 0)
            ms = 1;
    } else {
        /* Positive = absolute time. Calculate relative delay from now. */
        LARGE_INTEGER now;
        GetSystemTimeAsFileTime((LPFILETIME)&now);
        LONGLONG diff = Interval->QuadPart - now.QuadPart;
        if (diff <= 0)
            return STATUS_SUCCESS; /* Already past */
        ms = (DWORD)(diff / 10000);
    }

    if (Alertable) {
        DWORD result = SleepEx(ms, TRUE);
        if (result == WAIT_IO_COMPLETION)
            return STATUS_ALERTED;
    } else {
        Sleep(ms);
    }

    return STATUS_SUCCESS;
}

/* ============================================================================
 * Thread Priority
 *
 * Xbox uses NT KPRIORITY base priority increments relative to the process.
 * We map these to Win32 thread priority levels.
 * ============================================================================ */

/*
 * Map Xbox priority increment to Win32 priority level.
 * Xbox base priorities typically range from -2 to +2 for game threads.
 */
static int xbox_priority_to_win32(LONG increment)
{
    if (increment <= -15)       return THREAD_PRIORITY_IDLE;
    else if (increment <= -2)   return THREAD_PRIORITY_LOWEST;
    else if (increment == -1)   return THREAD_PRIORITY_BELOW_NORMAL;
    else if (increment == 0)    return THREAD_PRIORITY_NORMAL;
    else if (increment == 1)    return THREAD_PRIORITY_ABOVE_NORMAL;
    else if (increment <= 2)    return THREAD_PRIORITY_HIGHEST;
    else                        return THREAD_PRIORITY_TIME_CRITICAL;
}

static LONG win32_priority_to_xbox(int priority)
{
    switch (priority) {
        case THREAD_PRIORITY_IDLE:          return -15;
        case THREAD_PRIORITY_LOWEST:        return -2;
        case THREAD_PRIORITY_BELOW_NORMAL:  return -1;
        case THREAD_PRIORITY_NORMAL:        return 0;
        case THREAD_PRIORITY_ABOVE_NORMAL:  return 1;
        case THREAD_PRIORITY_HIGHEST:       return 2;
        case THREAD_PRIORITY_TIME_CRITICAL: return 15;
        default:                            return 0;
    }
}

LONG __stdcall xbox_KeSetBasePriorityThread(PVOID Thread, LONG Increment)
{
    HANDLE hThread = (HANDLE)Thread;
    LONG previous;

    /* Get previous priority before setting new one */
    int prev_win32 = GetThreadPriority(hThread);
    previous = win32_priority_to_xbox(prev_win32);

    int new_win32 = xbox_priority_to_win32(Increment);
    SetThreadPriority(hThread, new_win32);

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_THREAD,
        "KeSetBasePriorityThread: thread=%p, increment=%d (win32=%d), prev=%d",
        Thread, Increment, new_win32, previous);

    return previous;
}

LONG __stdcall xbox_KeQueryBasePriorityThread(PVOID Thread)
{
    HANDLE hThread = (HANDLE)Thread;
    int win32_priority = GetThreadPriority(hThread);
    return win32_priority_to_xbox(win32_priority);
}

/* ============================================================================
 * KeAlertThread
 *
 * Sends an alert to a thread, which can wake it from an alertable wait.
 * On Xbox, this sets the alerted flag on the thread object.
 * We approximate this with QueueUserAPC using a no-op APC routine.
 * ============================================================================ */

static VOID CALLBACK xbox_alert_apc(ULONG_PTR dwParam)
{
    (void)dwParam;
    /* No-op - the purpose is just to wake the thread from alertable wait */
}

NTSTATUS __stdcall xbox_KeAlertThread(PVOID Thread, KPROCESSOR_MODE AlertMode)
{
    HANDLE hThread = (HANDLE)Thread;

    (void)AlertMode;

    if (!QueueUserAPC(xbox_alert_apc, hThread, 0)) {
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_THREAD,
            "KeAlertThread: QueueUserAPC failed (error %u)", GetLastError());
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}

/* ============================================================================
 * NtYieldExecution
 *
 * Yields the current thread's remaining time slice.
 * Maps directly to SwitchToThread.
 * ============================================================================ */

NTSTATUS __stdcall xbox_NtYieldExecution(void)
{
    SwitchToThread();
    return STATUS_SUCCESS;
}

/* ============================================================================
 * NtDuplicateObject
 *
 * Duplicates a kernel handle. On Xbox this is simpler than Win32 since
 * there's only one process. Maps to DuplicateHandle within the same process.
 * ============================================================================ */

NTSTATUS __stdcall xbox_NtDuplicateObject(
    HANDLE SourceHandle,
    PHANDLE TargetHandle,
    ULONG Options)
{
    HANDLE hProcess = GetCurrentProcess();
    DWORD dwOptions = 0;

    if (!TargetHandle)
        return STATUS_INVALID_PARAMETER;

    /* Xbox DUPLICATE_CLOSE_SOURCE = 0x1, same as Win32 */
    if (Options & 0x1)
        dwOptions |= DUPLICATE_CLOSE_SOURCE;
    /* Xbox DUPLICATE_SAME_ACCESS = 0x2, same as Win32 */
    if (Options & 0x2)
        dwOptions |= DUPLICATE_SAME_ACCESS;

    if (!DuplicateHandle(hProcess, SourceHandle, hProcess, TargetHandle,
                         0, FALSE, dwOptions)) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_THREAD,
            "NtDuplicateObject: DuplicateHandle failed (error %u)", GetLastError());
        return STATUS_UNSUCCESSFUL;
    }

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_THREAD,
        "NtDuplicateObject: source=%p → target=%p (options=0x%X)",
        SourceHandle, *TargetHandle, Options);

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtSuspendThread(
    HANDLE ThreadHandle,
    PULONG PreviousSuspendCount)
{
    DWORD prev;

    /*
     * SuspendThread returns the previous suspend count, or (DWORD)-1 on
     * failure. The Xbox call reports that count through an out-parameter, so
     * the two are not interchangeable: -1 must become an error status rather
     * than a suspend count of 0xFFFFFFFF.
     */
    prev = SuspendThread(ThreadHandle);
    if (prev == (DWORD)-1) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_THREAD,
            "NtSuspendThread: SuspendThread failed (error %u)", GetLastError());
        return STATUS_UNSUCCESSFUL;
    }

    if (PreviousSuspendCount)
        *PreviousSuspendCount = (ULONG)prev;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_THREAD,
        "NtSuspendThread: thread=%p previous_count=%u", ThreadHandle, prev);

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtResumeThread(
    HANDLE ThreadHandle,
    PULONG PreviousSuspendCount)
{
    DWORD prev;

    /* Mirror of NtSuspendThread: (DWORD)-1 is failure, not a count. */
    prev = ResumeThread(ThreadHandle);
    if (prev == (DWORD)-1) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_THREAD,
            "NtResumeThread: ResumeThread failed (error %u)", GetLastError());
        return STATUS_UNSUCCESSFUL;
    }

    if (PreviousSuspendCount)
        *PreviousSuspendCount = (ULONG)prev;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_THREAD,
        "NtResumeThread: thread=%p previous_count=%u", ThreadHandle, prev);

    return STATUS_SUCCESS;
}


/* ================================================================
 * Worker stack slices + game-thread tracking (host-tick-driven titles)
 * ================================================================
 *
 * Optional guest stack slices for host threads entering recompiled code,
 * plus a handle to the latest spawned guest thread for host diagnostics.
 * See XBOX_WORKER_STACK_* in xbox_memory_layout.h.
 */

/* The game's own thread, kept so a wedged boot can be inspected from the host
 * watchdog. Set when a title's game thread is spawned; NULL under the explicitly selected
 * inline model, where there is no separate thread to sample. */
static HANDLE g_game_thread = NULL;

void  xbox_set_game_thread(void *h) { g_game_thread = (HANDLE)h; }
void *xbox_thread_debug_handle(void) { return (void *)g_game_thread; }

/* One bit per worker stack slice. Interlocked because a host-driven title can
 * allocate a slice from the host thread while recompiled code allocates one for
 * a spawned worker, so the allocator itself must be thread-safe. */
static volatile LONG g_worker_stack_used[XBOX_WORKER_STACK_COUNT];
/* No slices are owned until the loader or standalone host supplies a range. */
uint32_t g_xbox_worker_stack_base;

void xbox_worker_stack_reset(uint32_t base)
{
    g_xbox_worker_stack_base = base;
    for (int i = 0; i < XBOX_WORKER_STACK_COUNT; i++)
        g_worker_stack_used[i] = 0;
}

/* Internal bounds lookup for a retained callback lease. Reset is quiesced. */
BOOL xbox_worker_stack_bounds(uint32_t address, uint32_t *low, uint32_t *high)
{
    uint32_t base=g_xbox_worker_stack_base;
    uint64_t end=(uint64_t)base+XBOX_WORKER_STACK_SIZE*XBOX_WORKER_STACK_COUNT;
    unsigned slot;
    if (!base || address<base || address>=end || end>UINT32_MAX) return FALSE;
    slot=(address-base)/XBOX_WORKER_STACK_SIZE;
    if (!InterlockedCompareExchange(&g_worker_stack_used[slot],0,0)) return FALSE;
    *low=base+slot*XBOX_WORKER_STACK_SIZE;
    *high=*low+XBOX_WORKER_STACK_SIZE;
    return TRUE;
}

int xbox_worker_stack_alloc(void)
{
    int i;
    if (!g_xbox_worker_stack_base) return -1;
    for (i = 0; i < XBOX_WORKER_STACK_COUNT; i++) {
        if (InterlockedCompareExchange(&g_worker_stack_used[i], 1, 0) == 0)
            return i;
    }
    return -1;  /* all slices in use */
}

void xbox_worker_stack_free(int slot)
{
    if (slot >= 0 && slot < XBOX_WORKER_STACK_COUNT)
        InterlockedExchange(&g_worker_stack_used[slot], 0);
}

/* ============================================================================
 * One host core for the title's threads
 *
 * The console has one CPU, and titles lean on it: two threads of equal
 * priority only interleave at a quantum boundary or a wait, so a short
 * read-modify-write on a shared global never sees the other thread's half.
 * A title's level load can spawn a loader thread that, with the main
 * thread, adds entries to one global pool with "slot = count; fill; count =
 * slot + 1". On two host cores the two loops run at the same time and lose
 * each other's entries. Pinning every guest thread to one core gives the
 * console's interleaving back: the host still preempts at its tick, but
 * inside a window of a few instructions that is as rare as it was on the
 * hardware.
 *
 * Pinned: the main guest thread and every PsCreateSystemThreadEx worker,
 * the threads the title schedules. Not pinned: the kernel timer thread,
 * which runs the title's DPCs and timer routines, and the device models'
 * threads (APU, OHCI, nv2a), which deliver ISRs while the one-CPU gate is
 * free. They do run lifted code, but they stand in for interrupts, which
 * on the console ran between any two instructions of the thread they
 * interrupted; the gate already orders them against guest code at
 * DISPATCH_LEVEL, and putting them on the guest core would queue a DPC
 * behind a main thread spinning in its frame wait. The GPU and the audio
 * keep their cores for the same reason.
 *
 * Opt-in, like the one-CPU gate it was made beside: RECOMP_GUEST_CPUS=one
 * pins to the lowest core the process may use, =<n> picks the core (cpu 0
 * takes most of the IRQ work on Linux). Unset or =all, the title's threads
 * go wherever the host scheduler puts them, as before.
 */
DWORD_PTR xbox_GuestCpuMask(void)
{
    const char *mode = getenv("RECOMP_GUEST_CPUS");
    DWORD_PTR proc = 0, sys = 0;

    if (!mode || !*mode || !strcmp(mode, "all"))
        return 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) || !proc)
        return 0;
    if (*mode >= '0' && *mode <= '9') {
        long n = strtol(mode, NULL, 10);
        if (n >= 0 && n < (long)(8 * sizeof proc) && (proc & ((DWORD_PTR)1 << n)))
            return (DWORD_PTR)1 << n;
        fprintf(stderr, "  [KERNEL] RECOMP_GUEST_CPUS=%s: not a core the process"
                        " may use (mask %#lx); taking the lowest\n", mode,
                (unsigned long)proc);
    }
    /* The lowest core the process may use: fixed, so every thread lands
     * on the same one whatever core it was created from. */
    return proc & (DWORD_PTR)(-(intptr_t)proc);
}

void xbox_GuestThreadPin(const char *role)
{
    static int announced, failed;
    DWORD_PTR mask = xbox_GuestCpuMask();
    int core = 0;

    if (!mask)
        return;
    while (!((mask >> core) & 1))
        core++;
    if (!SetThreadAffinityMask(GetCurrentThread(), mask)) {
        if (!failed) {
            failed = 1;
            fprintf(stderr, "  [KERNEL] guest threads: this host sets no thread"
                            " affinity; the title's threads stay on every core\n");
            fflush(stderr);
        }
        return;
    }
    if (!announced) {
        announced = 1;
        fprintf(stderr, "  [KERNEL] guest threads on one host core (cpu %d),"
                        " as on the console (RECOMP_GUEST_CPUS=%s)\n",
                core, getenv("RECOMP_GUEST_CPUS"));
        fflush(stderr);
    }
    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_THREAD, "guest thread %s on cpu %d",
             role ? role : "?", core);
}
