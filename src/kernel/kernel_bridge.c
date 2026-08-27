/**
 * kernel_bridge.c - Bridge between translated game code and kernel functions
 *
 * Problem:
 *   Translated game code calls kernel functions via indirect calls through
 *   the kernel thunk table at VA 0x0036B7C0. In the XBE file, these entries
 *   contain unresolved ordinals (0x80000000 | ordinal). On real Xbox hardware,
 *   the kernel loader replaces these with actual function pointers before the
 *   game runs.
 *
 * Solution:
 *   1. After xbox_MemoryLayoutInit copies .rdata, call xbox_kernel_bridge_init()
 *   2. Replace each ordinal entry in Xbox memory with a synthetic VA
 *   3. When RECOMP_ICALL encounters a synthetic VA, route it to a per-ordinal
 *      bridge function that reads args from the simulated Xbox stack, translates
 *      pointer arguments from Xbox VA→native, and calls the kernel function.
 *
 * Synthetic VA scheme:
 *   Each thunk slot i gets VA 0xFE000000 + i*4
 *   The lookup function checks this range and dispatches appropriately.
 *
 * Why per-ordinal bridges instead of a generic trampoline:
 *   Kernel functions receive Xbox pointers (32-bit VAs) that must be translated
 *   to native pointers by adding g_xbox_mem_offset. Different functions have
 *   different parameter layouts (pointer vs value), so each needs its own bridge.
 */

#include "kernel.h"
#include "xbox_memory_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <float.h>
#include <io.h>       /* _open_osfhandle */
#include <fcntl.h>    /* _O_RDONLY, etc. */
#include <setjmp.h>
#include <intrin.h>
#include <windows.h>

/* Access to recompiled code globals */
extern uint32_t g_eax, g_ecx, g_edx, g_esp;
extern uint32_t g_ebx, g_esi, g_edi;
extern uint32_t g_seh_ebp;
extern volatile uint64_t g_icall_count;
extern volatile uintptr_t g_penter_last_rva;
extern volatile uintptr_t g_penter_caller_rva;
extern uintptr_t g_fake_kpcr_native;
extern const char *recomp_probe_fn_name(uintptr_t rva);
extern ptrdiff_t g_xbox_mem_offset;
extern void d3d8_PresentFrame(void);

/* Xbox file I/O bridge (src/xbox_file_bridge.c) */
extern int xbox_file_register(FILE *fp);
extern FILE *xbox_file_lookup(int handle);
extern void xbox_file_close(int handle);
extern size_t xbox_file_read(int handle, void *buf, size_t size);
extern void xbox_handle_register_file(HANDLE xbox_handle, FILE *fp);
extern FILE *xbox_handle_lookup_file(int xbox_handle);

/* Dispatch table lookup (for function pointer args) */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);

/* Memory access - same as recomp_types.h MEM32 but without the #define guard */
#define BRIDGE_MEM32(addr) (*(volatile uint32_t *)((uintptr_t)(addr) + g_xbox_mem_offset))

/* Translate Xbox VA to native pointer (NULL-safe: 0 → NULL) */
#define XBOX_TO_NATIVE(va) ((va) ? (void*)((uintptr_t)(va) + g_xbox_mem_offset) : NULL)

/* ── Synthetic VA range (for function exports) ─────────── */

#define KERNEL_VA_BASE  0xFE000000u
#define KERNEL_VA_END   (KERNEL_VA_BASE + XBOX_KERNEL_THUNK_TABLE_SIZE * 4)

/* ── Kernel data exports ──────────────────────────────────
 *
 * Some kernel ordinals are DATA exports (structs/variables), not functions.
 * The game reads their thunk entries and dereferences the result to access
 * the data. These cannot use synthetic VAs — they must point to real,
 * dereferenceable addresses in the Xbox VA space.
 *
 * We allocate a "kernel data area" at XBOX_KERNEL_DATA_BASE and populate
 * it with the expected structures.
 */

#define BRIDGE_MEM16(addr) (*(volatile uint16_t *)((uintptr_t)(addr) + g_xbox_mem_offset))
#define BRIDGE_MEM8(addr)  (*(volatile uint8_t  *)((uintptr_t)(addr) + g_xbox_mem_offset))

/**
 * Get the Xbox VA of data for a kernel DATA export ordinal.
 * Returns 0 if the ordinal is not a data export (i.e., it's a function).
 */
static uint32_t kernel_data_va_for_ordinal(ULONG ordinal)
{
    switch (ordinal) {
    case  17: return XBOX_KERNEL_DATA_BASE + KDATA_EVENT_OBJ_TYPE;
    case  65: return XBOX_KERNEL_DATA_BASE + KDATA_IO_COMPLETION_TYPE;
    case  71: return XBOX_KERNEL_DATA_BASE + KDATA_IO_DEVICE_TYPE;
    case 156: return XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT;
    case 164: return XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE;
    case 259: return XBOX_KERNEL_DATA_BASE + KDATA_THREAD_OBJ_TYPE;
    case 322: return XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO;
    case 323: return XBOX_KERNEL_DATA_BASE + KDATA_HD_KEY;
    case 324: return XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION;
    case 325: return XBOX_KERNEL_DATA_BASE + KDATA_SIGNATURE_KEY;
    case 326: return XBOX_KERNEL_DATA_BASE + KDATA_LAN_KEY;
    case 327: return XBOX_KERNEL_DATA_BASE + KDATA_ALT_SIGNATURE_KEYS;
    case 328: return XBOX_KERNEL_DATA_BASE + KDATA_XE_IMAGE_FILENAME;
    case 355: return XBOX_KERNEL_DATA_BASE + KDATA_LAN_KEY;         /* alias */
    case 356: return XBOX_KERNEL_DATA_BASE + KDATA_ALT_SIGNATURE_KEYS; /* alias */
    case 357: return XBOX_KERNEL_DATA_BASE + KDATA_XE_PUBLIC_KEY;
    default:  return 0;  /* Not a data export */
    }
}

/**
 * Initialize kernel data export values at the kernel data area.
 * Called during bridge init, after Xbox memory is mapped.
 */
static void kernel_data_init(void)
{
    /* XboxHardwareInfo (ordinal 322) - XBOX_HARDWARE_INFO
     *   +0: ULONG Flags (0 = retail, 0x20 = devkit)
     *   +4: UCHAR GpuRevision
     *   +5: UCHAR McpRevision
     */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 0) = 0;   /* Retail */
    BRIDGE_MEM8(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 4) = 0xA1; /* NV2A A1 */
    BRIDGE_MEM8(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 5) = 0xB1; /* MCPX B1 */

    /* XboxKrnlVersion (ordinal 324) - XBOX_KRNL_VERSION
     *   +0: USHORT Major (1)
     *   +2: USHORT Minor (0)
     *   +4: USHORT Build (5849 = XDK version)
     *   +6: USHORT Qfe (0)
     */
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 0) = 1;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 2) = 0;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 4) = 5849;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 6) = 0;

    /* KeTickCount (ordinal 156) - initialized to current tick count.
     * A background thread in main.c updates this every ~1ms. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT) = GetTickCount();

    /* LaunchDataPage (ordinal 164) - NULL (no launch data) */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE) = 0;

    /* PsThreadObjectType (ordinal 259) - type object (stub: 0) */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_THREAD_OBJ_TYPE) = 0;

    /* ExEventObjectType (ordinal 17) - type object (stub: 0) */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_EVENT_OBJ_TYPE) = 0;

    /* IoCompletionObjectType (ordinal 65) - type object (stub: 0) */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_IO_COMPLETION_TYPE) = 0;

    /* IoDeviceObjectType (ordinal 71) - type object (stub: 0) */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_IO_DEVICE_TYPE) = 0;

    /* XboxHDKey (ordinal 323) - 16 bytes of zeros (no key) */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_HD_KEY) + g_xbox_mem_offset), 0, 16);

    /* XboxSignatureKey (ordinal 325) - 16 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_SIGNATURE_KEY) + g_xbox_mem_offset), 0, 16);

    /* XboxLANKey (ordinals 326, 355) - 16 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_LAN_KEY) + g_xbox_mem_offset), 0, 16);

    /* XboxAlternateSignatureKeys (ordinals 327, 356) - 256 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_ALT_SIGNATURE_KEYS) + g_xbox_mem_offset), 0, 256);

    /* XePublicKeyData (ordinal 357) - 284 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_XE_PUBLIC_KEY) + g_xbox_mem_offset), 0, 284);

    fprintf(stderr, "  Kernel data exports: initialized at Xbox VA 0x%08X\n",
            XBOX_KERNEL_DATA_BASE);
}

/* ── Per-slot ordinal and bridge function ────────────────── */

/* Ordinal for each slot (read from Xbox memory during init) */
static ULONG g_slot_ordinals[XBOX_KERNEL_THUNK_TABLE_SIZE];

/* Log counter - limit output to avoid flooding */
static int g_kernel_call_count = 0;

/* Read Xbox stack arg as uint32_t.
 * After kernel_thunk_dispatch pops the dummy return address (g_esp += 4),
 * arg0 is at g_esp+0, arg1 at g_esp+4, etc. */
#define STACK_ARG(n) ((uint32_t)BRIDGE_MEM32(g_esp + (n) * 4))

/* ── Per-ordinal bridge functions ─────────────────────────
 *
 * Each bridge reads args from the Xbox stack, translates pointer
 * args from Xbox VA→native, calls the kernel function, and stores
 * the result in g_eax.
 *
 * Xbox cdecl: args pushed right-to-left, caller cleans stack.
 * Xbox stdcall: args pushed right-to-left, callee cleans stack.
 * In our case the caller (translated code) does "PUSH32" for each arg
 * before calling, and the kernel function's ret-N is handled by the
 * translated code's own stack adjustment.
 */

/* ── PsCreateSystemThreadEx (ordinal 255) ────────────────
 * NTSTATUS PsCreateSystemThreadEx(
 *   PHANDLE ThreadHandle,      // arg0: Xbox VA → pointer
 *   ULONG ThreadExtraSize,     // arg1: value
 *   ULONG KernelStackSize,     // arg2: value
 *   ULONG TlsDataSize,         // arg3: value
 *   PULONG ThreadId,           // arg4: Xbox VA → pointer (can be NULL)
 *   PVOID StartContext1,       // arg5: Xbox VA → opaque
 *   PVOID StartContext2,       // arg6: Xbox VA → opaque
 *   BOOLEAN CreateSuspended,   // arg7: value
 *   BOOLEAN DebugStack,        // arg8: value
 *   PXBOX_SYSTEM_ROUTINE StartRoutine  // arg9: Xbox function pointer
 * )
 *
 * For static recompilation, we don't create a real thread.
 * Instead we call the StartRoutine synchronously via RECOMP_ICALL.
 * This is correct because on Xbox, the entry point creates a system
 * thread and returns, and the thread runs the actual game.
 */
static int g_thread_call_count = 0;
/* Fiber-based guest thread scheduling (single worker).
 *
 * The recomp engine runs on ONE host thread; the register globals
 * (g_eax..g_edi, g_esp, g_seh_ebp) are shared by all guest threads, like real
 * x86 registers are per-CPU. Guest workers created by PsCreateSystemThreadEx
 * therefore run as Windows fibers on the same host thread, and the register
 * globals are saved/restored around each fiber switch. The worker blends one
 * screen, then its KeDelayExecutionThread parks it until the delay elapses;
 * the main fiber resumes it at the next kernel thunk call. Single worker
 * only: g_worker is one slot and g_worker_exit_jmp is one jmp_buf (nested
 * workers would need a slot table). */
static jmp_buf g_worker_exit_jmp;
static volatile int g_worker_active = 0;

typedef struct {
    LPVOID fiber;            /* worker fiber handle (NULL until created) */
    recomp_func_t fn;
    uint32_t ctx1, ctx2;
    volatile int parked;     /* 1 while the worker sleeps on KeDelay */
    volatile int done;       /* 1 after the worker routine returned */
    DWORD wake_tick;         /* GetTickCount() when the delay elapses */
    /* Saved guest register set for this guest thread. */
    uint32_t eax, ecx, edx, ebx, esi, edi, esp, seh_ebp;
} worker_state_t;

static worker_state_t g_worker;
static worker_state_t g_main_state;
static LPVOID g_main_fiber = NULL;

static void worker_load_tib(const worker_state_t *w)
{
    if (g_fake_kpcr_native)
        *(uint32_t *)(g_fake_kpcr_native + 0x04) =
            (w == &g_worker) ? XBOX_WORKER_STACK_TOP : 0x00F7FFF0u;
}


/* Diagnostic: prove whether fiber resume restores the exact guest regset
 * saved at the switch (run-360 register-provenance question). Bounded. */
static uint32_t s_fsw_log = 0;
static void fsw_log(const char *op, const worker_state_t *w)
{
    if (getenv("MM3_TRACE_FSW") && s_fsw_log < 200000) {
        s_fsw_log++;
        fprintf(stderr, "[FSW] %s %s kc=%u eax=%08X ecx=%08X edx=%08X "
            "ebx=%08X esi=%08X edi=%08X esp=%08X ebp=%08X\n", op,
            (w == &g_main_state) ? "main" : "work",
            (unsigned)g_kernel_call_count, w->eax, w->ecx, w->edx, w->ebx,
            w->esi, w->edi, w->esp, w->seh_ebp);
        fflush(stderr);
    }
}

static void worker_save_regs(worker_state_t *w)
{
    w->eax = g_eax; w->ecx = g_ecx; w->edx = g_edx;
    w->ebx = g_ebx; w->esi = g_esi; w->edi = g_edi;
    w->esp = g_esp; w->seh_ebp = g_seh_ebp;
    fsw_log("save", w);
}

static void worker_load_regs(const worker_state_t *w)
{
    g_eax = w->eax; g_ecx = w->ecx; g_edx = w->edx;
    g_ebx = w->ebx; g_esi = w->esi; g_edi = w->edi;
    g_esp = w->esp; g_seh_ebp = w->seh_ebp;
    worker_load_tib(w);
    fsw_log("load", w);
}

/* Park the worker fiber and resume the main one. */
static void worker_switch_to_main(void)
{
    worker_save_regs(&g_worker);
    worker_load_regs(&g_main_state);
    SwitchToFiber(g_main_fiber);
}

/* Park the main fiber and resume the worker one (loads worker regs; the
 * worker restores the main register set before switching back). */
static void worker_switch_to_worker(void)
{
    worker_save_regs(&g_main_state);
    worker_load_regs(&g_worker);
    SwitchToFiber(g_worker.fiber);
}

static void worker_resume_if_due(void)
{
    if (g_worker_active && g_worker.fiber && g_worker.parked && !g_worker.done &&
        (int)(GetTickCount() - g_worker.wake_tick) >= 0) {
        worker_switch_to_worker();
        if (g_worker.done) {
            DeleteFiber(g_worker.fiber);
            g_worker.fiber = NULL;
        }
    }
}

static void WINAPI worker_fiber_main(LPVOID param)
{
    (void)param;
    g_worker_active = 1;
    /* Reserve the TIB/stack-base metadata immediately below the stack top;
     * the worker's first pushes must not overwrite FS:[4]-0x14. */
    g_esp = XBOX_WORKER_STACK_TOP - 0x20u;
    g_seh_ebp = g_esp;

    BRIDGE_MEM32(XBOX_WORKER_STACK_TOP - 0x14u) =
        XBOX_WORKER_STACK_TOP - 0x10u;


    /* Xbox thread start routines receive (StartContext1, StartContext2). */
    g_esp -= 4; BRIDGE_MEM32(g_esp) = g_worker.ctx2;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = g_worker.ctx1;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;

    if (setjmp(g_worker_exit_jmp) == 0) {
        g_worker.fn();
        fprintf(stderr, "  [KERNEL] worker routine returned\n");
    } else {
        fprintf(stderr, "  [KERNEL] worker unwound via PsTerminateSystemThread\n");
    }
    fflush(stderr);
    g_worker_active = 0;
    g_worker.done = 1;
    g_worker.parked = 0;
    worker_switch_to_main();   /* never returns */
}

static void bridge_PsCreateSystemThreadEx(void)
{
    uint32_t xbox_handle_ptr = STACK_ARG(0);
    uint32_t start_context1  = STACK_ARG(5);
    uint32_t start_context2  = STACK_ARG(6);
    uint32_t start_routine   = STACK_ARG(9);
    int is_first_call = (g_thread_call_count == 0);
    g_thread_call_count++;

    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx #%d: routine=0x%08X ctx1=0x%08X ctx2=0x%08X\n",
            g_thread_call_count, start_routine, start_context1, start_context2);
    fflush(stderr);

    /* Write a fake handle to the output pointer */
    if (xbox_handle_ptr) {
        BRIDGE_MEM32(xbox_handle_ptr) = 0xBEEF0001;  /* fake handle */
    }

    /* Run the start routine through the recomp dispatch.
     * Xbox thread start routines receive two parameters:
     *   void ThreadRoutine(PVOID StartContext1, PVOID StartContext2)
     * We push both onto the simulated stack (right-to-left).
     *
     * First call: the game's main thread entry point. Must run synchronously
     * and inherit the current register state (this IS the game starting).
     *
     * Subsequent calls: worker threads run as fibers on the same host
     * thread with their own guest stack (XBOX_WORKER_STACK_*); the pump
     * resumes immediately, matching real Xbox concurrency. */
    if (start_routine) {
        recomp_func_t fn = recomp_lookup(start_routine);
        if (!fn) fn = recomp_lookup_manual(start_routine);
        if (fn) {
            if (is_first_call) {
                /* The first routine is the game entry and inherits the
                 * current guest register state. */
                g_esp -= 4; BRIDGE_MEM32(g_esp) = start_context2;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = start_context1;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
                fn();
                g_esp += 12;
            } else {
                fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: spawning worker 0x%08X (ctx=0x%08X)\n",
                        start_routine, start_context1);
                fflush(stderr);

                if (!g_main_fiber)
                    g_main_fiber = ConvertThreadToFiber(NULL);
                g_worker.fn = fn;
                g_worker.ctx1 = start_context1;
                g_worker.ctx2 = start_context2;
                g_worker.parked = 0;
                g_worker.done = 0;
                g_worker.fiber = CreateFiber(0, worker_fiber_main, NULL);
                if (g_worker.fiber) {
                    worker_switch_to_worker();
                    /* Worker parked on KeDelay or finished; main resumes here. */
                    if (g_worker.done) {
                        DeleteFiber(g_worker.fiber);
                        g_worker.fiber = NULL;
                    }
                } else {
                    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: CreateFiber failed\n");
                    fflush(stderr);
                }
            }
        } else {
            fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: start routine 0x%08X not found in dispatch!\n",
                    start_routine);
        }
    }

    g_eax = 0; /* STATUS_SUCCESS */
}

/* ── NtClose (ordinal 187) ───────────────────────────────
 * NTSTATUS NtClose(HANDLE Handle)
 * Handle is a value (not a pointer), so safe for generic call.
 */
/* Handle-table helpers; defined further below. Xbox memory slots are 32-bit
 * but native HANDLEs are 64-bit pointers, so handles are kept in a table and
 * referenced by tagged 32-bit tokens. */
#define BRIDGE_HANDLE_TAG  0x48000000u
#define BRIDGE_HANDLE_MASK 0x00FFFFFFu
#define BRIDGE_HANDLE_MAX  16384
static HANDLE s_handle_table[BRIDGE_HANDLE_MAX];
static int     s_handle_file_slot[BRIDGE_HANDLE_MAX];

static void   bridge_write_handle(uint32_t handle_va, HANDLE h);
static HANDLE bridge_take_handle(uint32_t token);

static void bridge_NtClose(void)
{
    uint32_t raw_handle = STACK_ARG(0);
    int file_slot = 0;

    /* Close real handles but skip fake/synthetic ones */
    if (raw_handle && raw_handle != 0xDEAD0001u && raw_handle != 0xBEEF0010u) {
        if ((raw_handle & 0xFF000000u) == BRIDGE_HANDLE_TAG) {
            uint32_t ti = raw_handle & BRIDGE_HANDLE_MASK;
            if (ti > 0 && ti < BRIDGE_HANDLE_MAX) {
                file_slot = s_handle_file_slot[ti];
                if (file_slot) {
                    xbox_file_close(file_slot);
                    s_handle_file_slot[ti] = 0;
                }
            }
        }
        HANDLE h = bridge_take_handle(raw_handle);
        fprintf(stderr, "  [NTC] close tok=0x%08X h=%p file_slot=%d\n", raw_handle, h, file_slot);
        if (h && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
    g_eax = 0; /* STATUS_SUCCESS */
}

/* ── MmAllocateContiguousMemory (ordinal 165) ─────────────
 * PVOID MmAllocateContiguousMemory(ULONG NumberOfBytes)
 */
static void bridge_MmAllocateContiguousMemory(void)
{
    uint32_t size = STACK_ARG(0);

    /* Allocate from Xbox heap so MEM32(result) works correctly */
    uint32_t xbox_va = xbox_HeapAlloc(size, 4096);

    if (g_kernel_call_count <= 100) {
        fprintf(stderr, "  [KERNEL] MmAllocateContiguousMemory: size=%u → Xbox VA 0x%08X\n",
                size, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

/* ── MmAllocateSystemMemory (ordinal 167) ────────────────
 * PVOID MmAllocateSystemMemory(ULONG NumberOfBytes)
 *
 * Boot path sub_000824F6 allocates 64KB of system memory; NULL means boot
 * failure and routes to the BadGameDisc screen. Allocate from the Xbox heap
 * like the other Mm bridges.
 */
static void bridge_MmAllocateSystemMemory(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t xbox_va = xbox_HeapAlloc(size, 4096);

    if (g_kernel_call_count <= 100) {
        fprintf(stderr, "  [KERNEL] MmAllocateSystemMemory: size=%u → Xbox VA 0x%08X\n",
                size, xbox_va);
        fflush(stderr);
    }
    g_eax = xbox_va;
}

/* ── MmAllocateContiguousMemoryEx (ordinal 166) ───────────
 * PVOID MmAllocateContiguousMemoryEx(SIZE_T size, ULONG_PTR low, ULONG_PTR high,
 *                                     ULONG alignment, ULONG protect)
 */
static void bridge_MmAllocateContiguousMemoryEx(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t low = STACK_ARG(1);
    uint32_t high = STACK_ARG(2);
    uint32_t align = STACK_ARG(3);
    uint32_t prot = STACK_ARG(4);
    /* Whole-RAM D3D surface-heap reservation: sub_00346450 (loc_003466AF)
     * requests the full 64 MB arena (size 0x4000000, align 0x4000, tag 0x404)
     * as the device surface heap. On real Xbox this succeeds against all of
     * physical RAM and D3D surfaces live in the non-cached 0x80000000 alias;
     * the bump-only ~50.8 MB heap cannot express it, so return the 64 MB
     * mirror base directly and let the game's descriptor sub-allocator manage
     * the arena (the mirror aliases the whole base RAM, like real hardware). */
    if (size >= XBOX_TOTAL_RAM) {
        if (g_kernel_call_count <= 100) {
            fprintf(stderr, "  [KERNEL] MmAllocateContiguousMemoryEx: size=%u align=%u"
                " -> surface arena 0x80000000\n", size, align);
            fflush(stderr);
        }
        g_eax = 0x80000000u;
        return;
    }

    /* Allocate from Xbox heap with requested alignment */
    if (align < 4096) align = 4096;
    uint32_t xbox_va = xbox_HeapAlloc(size, align);

    if (g_kernel_call_count <= 100) {
        fprintf(stderr, "  [KERNEL] MmAllocateContiguousMemoryEx: size=%u align=%u → Xbox VA 0x%08X\n",
                size, align, xbox_va);
        fflush(stderr);
    }
    g_eax = xbox_va;
}

/* ── MmFreeContiguousMemory (ordinal 171) ─────────────────
 * VOID MmFreeContiguousMemory(PVOID BaseAddress)
 */
static void bridge_MmFreeContiguousMemory(void)
{
    uint32_t addr = STACK_ARG(0);
    xbox_HeapFree(addr);
    g_eax = 0;
}

/* ── NtAllocateVirtualMemory (ordinal 184) ────────────────
 * NTSTATUS NtAllocateVirtualMemory(PVOID *BaseAddress, ULONG ZeroBits,
 *     PULONG AllocationSize, ULONG AllocationType, ULONG Protect)
 */
static void bridge_NtAllocateVirtualMemory(void)
{
    uint32_t base_ptr = STACK_ARG(0);  /* PVOID* in Xbox VA */
    uint32_t zero_bits = STACK_ARG(1);
    uint32_t size_ptr = STACK_ARG(2);  /* PULONG in Xbox VA */
    uint32_t alloc_type = STACK_ARG(3);
    uint32_t protect = STACK_ARG(4);

    /* Read the requested size from Xbox memory */
    uint32_t size = size_ptr ? BRIDGE_MEM32(size_ptr) : 0;
    /* Read the base address hint (0 = let kernel choose) */
    uint32_t base_hint = base_ptr ? BRIDGE_MEM32(base_ptr) : 0;

    if (g_kernel_call_count <= 200) {
        fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory: base=0x%08X size=%u type=0x%X prot=0x%X\n",
                base_hint, size, alloc_type, protect);
        fflush(stderr);
    }

    if (size == 0) {
        g_eax = 0xC0000045u; /* STATUS_INVALID_PAGE_PROTECTION */
        return;
    }

    /*
     * Xbox NtAllocateVirtualMemory supports two modes:
     * - MEM_RESERVE (0x2000): Reserve virtual address space
     * - MEM_COMMIT  (0x1000): Commit pages within a reserved region
     * - MEM_RESERVE|MEM_COMMIT (0x3000): Both in one call
     *
     * Our Xbox heap (bump allocator) always commits memory immediately,
     * so MEM_COMMIT on an already-reserved region is a no-op.
     * Only allocate new memory when MEM_RESERVE is requested.
     */
    if (base_hint != 0 && (alloc_type & 0x2000) == 0) {
        /* MEM_COMMIT only, on an already-reserved region.
         * The memory is already committed by our bump allocator.
         * Don't change the base address - just return success. */
        if (g_kernel_call_count <= 200) {
            fprintf(stderr, "  [KERNEL] → MEM_COMMIT on existing region 0x%08X, no-op\n", base_hint);
            fflush(stderr);
        }
        g_eax = 0; /* STATUS_SUCCESS */
        return;
    }

    /* MEM_RESERVE at a mirror-backed guest address: the 26-bit bus wrap
     * already makes every address below the mirror ceiling real physical
     * RAM, exactly like the real Xbox. The DICE pool carves its large
     * regions here (pool cursor 0x84000000); grant the request at the
     * requested base instead of failing on the tiny bump heap and forcing
     * the pool's broken fake-large-node fallback (garbage descriptors ->
     * runaway 12-byte copy past the mirror seam). */
    if (base_hint != 0 && (alloc_type & 0x2000) &&
        base_hint < (uint32_t)((XBOX_NUM_MIRRORS + 1) * XBOX_TOTAL_RAM)) {
        if (g_kernel_call_count <= 200) {
            fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory: mirror-backed region base=0x%08X size=%u (wrapped RAM)\n",
                    base_hint, size);
            fflush(stderr);
        }
        if (base_ptr) BRIDGE_MEM32(base_ptr) = base_hint;
        if (size_ptr) BRIDGE_MEM32(size_ptr) = size;
        g_eax = 0; /* STATUS_SUCCESS */
        return;
    }

    /* Allocate from Xbox heap (MEM_RESERVE or MEM_RESERVE|MEM_COMMIT) */
    uint32_t xbox_va = xbox_HeapAlloc(size, 4096);
    if (!xbox_va) {
        fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory: HEAP REJECT size=%u base_hint=0x%08X type=0x%X prot=0x%X\n",
                size, base_hint, alloc_type, protect);
        fflush(stderr);
        g_eax = 0xC0000017u; /* STATUS_NO_MEMORY */
        return;
    }

    /* Write back the allocated address and actual size */
    if (base_ptr) BRIDGE_MEM32(base_ptr) = xbox_va;
    if (size_ptr) BRIDGE_MEM32(size_ptr) = size;

    g_eax = 0; /* STATUS_SUCCESS */
}

/* ── NtFreeVirtualMemory (ordinal 199) ────────────────────
 * NTSTATUS NtFreeVirtualMemory(PVOID *BaseAddress, PULONG FreeSize,
 *     ULONG FreeType)
 */
static void bridge_NtFreeVirtualMemory(void)
{
    uint32_t base_ptr = STACK_ARG(0);
    uint32_t size_ptr = STACK_ARG(1);
    uint32_t free_type = STACK_ARG(2);

    /* Mirror-backed guest addresses are wrapped RAM that must stay mapped;
     * releasing them is a success no-op (real Xbox RAM is never released). */
    if (base_ptr) {
        uint32_t base_va = BRIDGE_MEM32(base_ptr);
        if (base_va != 0 &&
            base_va < (uint32_t)((XBOX_NUM_MIRRORS + 1) * XBOX_TOTAL_RAM)) {
            g_eax = 0; /* STATUS_SUCCESS */
            return;
        }
    }

    g_eax = (uint32_t)xbox_NtFreeVirtualMemory(
        XBOX_TO_NATIVE(base_ptr), XBOX_TO_NATIVE(size_ptr), free_type);
}

/* ── NtQueryVirtualMemory (ordinal 217) ────────────────────
 * NTSTATUS NtQueryVirtualMemory(PVOID BaseAddress,
 *     PMEMORY_BASIC_INFORMATION MemoryInformation,
 *     ULONG MemoryInformationLength, PULONG ReturnLength)
 *
 * The game passes an XBOX VA as BaseAddress (e.g. the image-base
 * region during heap init in sub_000854CF). The host VirtualQuery
 * must receive the NATIVE address (VA + g_xbox_mem_offset), not the
 * raw guest VA — otherwise it fails with STATUS_INVALID_PARAMETER and
 * the game's heap init returns 0, leaving 0x46A154=0 and driving the
 * allocator pool-grow recursion (sub_000858F3). */
static void bridge_NtQueryVirtualMemory(void)
{
    uint32_t base_va = STACK_ARG(0);
    uint32_t info_ptr = STACK_ARG(1);
    uint32_t info_len = STACK_ARG(2);
    uint32_t ret_len_ptr = STACK_ARG(3);

    /* Translate the guest VA to the native mapping (critical). */
    void *native_base = XBOX_TO_NATIVE(base_va);

    if (g_kernel_call_count <= 200) {
        fprintf(stderr, "  [KERNEL] NtQueryVirtualMemory: base=0x%08X (native %p) len=%u\n",
                base_va, native_base, info_len);
        fflush(stderr);
    }

    NTSTATUS status = xbox_NtQueryVirtualMemory(
        native_base, XBOX_TO_NATIVE(info_ptr), info_len,
        (PULONG)XBOX_TO_NATIVE(ret_len_ptr));

    if (g_kernel_call_count <= 200) {
        fprintf(stderr, "  [KERNEL] NtQueryVirtualMemory: → status=0x%08X\n",
                (uint32_t)status);
        fflush(stderr);
    }

    g_eax = (uint32_t)status;
}

/* ── ExAllocatePool / ExAllocatePoolWithTag (ordinals 15, 16) ─
 * Must allocate from Xbox heap so the returned pointer is an Xbox VA
 * that can be accessed via MEM32(). Native HeapAlloc returns 64-bit
 * pointers that get truncated and produce garbage Xbox VAs.
 */
static void bridge_ExAllocatePool(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t xbox_va = xbox_HeapAlloc(size, 16);

    if (g_kernel_call_count <= 200) {
        fprintf(stderr, "  [KERNEL] ExAllocatePool: size=%u → Xbox VA 0x%08X\n",
                size, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

static void bridge_ExAllocatePoolWithTag(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t tag = STACK_ARG(1);
    uint32_t xbox_va = xbox_HeapAlloc(size, 16);

    if (g_kernel_call_count <= 200) {
        fprintf(stderr, "  [KERNEL] ExAllocatePoolWithTag: size=%u tag='%c%c%c%c' → Xbox VA 0x%08X\n",
                size,
                (char)(tag & 0xFF), (char)((tag >> 8) & 0xFF),
                (char)((tag >> 16) & 0xFF), (char)((tag >> 24) & 0xFF),
                xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

/* ── KfRaiseIrql / KfLowerIrql (ordinals 160, 161) ────── */
static void bridge_KfRaiseIrql(void)
{
    uint32_t new_irql = STACK_ARG(0);
    g_eax = (uint32_t)xbox_KfRaiseIrql((UCHAR)new_irql);
}

static void bridge_KfLowerIrql(void)
{
    uint32_t new_irql = STACK_ARG(0);
    xbox_KfLowerIrql((UCHAR)new_irql);
    g_eax = 0;
}

/* ── KeRaiseIrqlToDpcLevel (ordinal 129) ─────────────────── */
static void bridge_KeRaiseIrqlToDpcLevel(void)
{
    g_eax = (uint32_t)xbox_KeRaiseIrqlToDpcLevel();
}

/* MM3's imported slot 23 resolves ordinal 49 here.  The XBE uses this
 * software-interrupt request while bringing up the callback scheduler. */
static void bridge_HalRequestSoftwareInterrupt(void)
{
    xbox_HalRequestSoftwareInterrupt((KIRQL)STACK_ARG(0));
}

/* ── HalGetInterruptVector (ordinal 44) ─────────────────── */
static void bridge_HalGetInterruptVector(void)
{
    uint32_t level = STACK_ARG(0);
    uint32_t irql_ptr = STACK_ARG(1);
    KIRQL irql = PASSIVE_LEVEL;
    g_eax = xbox_HalGetInterruptVector(level, irql_ptr ? &irql : NULL);
    if (irql_ptr)
        BRIDGE_MEM8(irql_ptr) = irql;
}

/* ── RtlInitializeCriticalSection / Enter / Leave (ordinals 291, 277, 294) ─ */
static void bridge_RtlInitializeCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    xbox_RtlInitializeCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

static void bridge_RtlEnterCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    xbox_RtlEnterCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

static void bridge_RtlLeaveCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    xbox_RtlLeaveCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

/* ── KeQueryPerformanceCounter / Frequency (ordinals 126, 127) ─ */
static void bridge_KeQueryPerformanceCounter(void)
{
    LARGE_INTEGER li = xbox_KeQueryPerformanceCounter();
    g_eax = (uint32_t)li.LowPart;
    g_edx = (uint32_t)li.HighPart;
}

static void bridge_KeQueryPerformanceFrequency(void)
{
    LARGE_INTEGER li = xbox_KeQueryPerformanceFrequency();
    g_eax = (uint32_t)li.LowPart;
    g_edx = (uint32_t)li.HighPart;
}

/* ── KeQuerySystemTime (ordinal 128) ─────────────────────── */
static void bridge_KeQuerySystemTime(void)
{
    uint32_t time_ptr = STACK_ARG(0);
    xbox_KeQuerySystemTime(XBOX_TO_NATIVE(time_ptr));
    g_eax = 0;
}

/* ── MmQueryStatistics (ordinal 181) ─────────────────────── */
static void bridge_MmQueryStatistics(void)
{
    uint32_t stats_ptr = STACK_ARG(0);
    g_eax = (uint32_t)xbox_MmQueryStatistics(XBOX_TO_NATIVE(stats_ptr));
}

static void bridge_MmQueryAddressProtect(void)
{
    uint32_t address = STACK_ARG(0);
    g_eax = (uint32_t)xbox_MmQueryAddressProtect(XBOX_TO_NATIVE(address));
}

/* ── NtCreateEvent (ordinal 189) ─────────────────────────── */
static void bridge_NtCreateEvent(void)
{
    uint32_t handle_ptr = STACK_ARG(0);
    uint32_t obj_attr_ptr = STACK_ARG(1);
    uint32_t event_type = STACK_ARG(2);
    uint32_t initial_state = STACK_ARG(3);

    /* Use local HANDLE to avoid 8-byte write to 4-byte Xbox memory slot.
     * On x64, HANDLE is 8 bytes but Xbox expects 4-byte handles. */
    HANDLE local_handle = NULL;
    NTSTATUS status = xbox_NtCreateEvent(
        &local_handle,
        XBOX_TO_NATIVE(obj_attr_ptr),
        event_type, initial_state);

    if (handle_ptr) {
        bridge_write_handle(handle_ptr, local_handle);
    }

    fprintf(stderr, "  [BRIDGE] NtCreateEvent: handle_ptr=0x%08X type=%u init=%u → status=0x%08X handle=0x%08X\n",
            handle_ptr, event_type, initial_state, (uint32_t)status,
            (uint32_t)(uintptr_t)local_handle);

    g_eax = (uint32_t)status;
}

/* ── KeSetEvent (ordinal 145) ────────────────────────────── */
static void bridge_KeSetEvent(void)
{
    uint32_t event_ptr = STACK_ARG(0);
    uint32_t increment = STACK_ARG(1);
    uint32_t wait = STACK_ARG(2);

    g_eax = (uint32_t)xbox_KeSetEvent((PVOID)(uintptr_t)event_ptr, increment, (BOOLEAN)wait);
}

/* ── KeWaitForSingleObject (ordinal 159) ─────────────────── */
static void bridge_KeWaitForSingleObject(void)
{
    static unsigned wait_log_count;
    static int cf_trace = -1;
    uint32_t object = STACK_ARG(0);
    uint32_t wait_reason = STACK_ARG(1);
    uint32_t wait_mode = STACK_ARG(2);
    uint32_t alertable = STACK_ARG(3);
    uint32_t timeout_ptr = STACK_ARG(4);
    uint32_t esp_before = g_esp;
    if (cf_trace < 0) cf_trace = getenv("MM3_CF_TRACE") ? 1 : 0;

    if ((g_icall_count >= 12295ULL && g_icall_count <= 12305ULL) ||
        (g_icall_count >= 321678ULL && g_icall_count <= 321679ULL) ||
        (g_icall_count >= 326430ULL && g_icall_count <= 326450ULL)) {
        void *frames[6];
        USHORT n = CaptureStackBackTrace(0, 6, frames, NULL);
        HMODULE mod = GetModuleHandle(NULL);
        fprintf(stderr, "[FRONTIER-KEWAIT-BEGIN] ic=%llu object=%08X reason=%08X mode=%08X alert=%08X timeout=%08X esp=%08X ebp=%08X\n",
                (unsigned long long)g_icall_count, object, wait_reason,
                wait_mode, alertable, timeout_ptr, g_esp, g_seh_ebp);
        for (USHORT i = 0; i < n; ++i)
            fprintf(stderr, "[FRONTIER-KEWAIT-FRAME] i=%u host_rva=%zX\n", i,
                    (uintptr_t)frames[i] - (uintptr_t)mod);
    }
    g_eax = (uint32_t)xbox_KeWaitForSingleObject(
        (PVOID)(uintptr_t)object, wait_reason, wait_mode,
        (BOOLEAN)alertable, XBOX_TO_NATIVE(timeout_ptr));
    if (cf_trace && g_icall_count >= 321600ULL && g_icall_count <= 321700ULL) {
        fprintf(stderr, "[CF] ic=%llu RET fn=KeWaitForSingleObject "
                        "ret=unknown stack20=%08X esp_before=%08X esp_after=%08X "
                        "expected_delta=0 actual_delta=%d guest=FE0000F0\n",
                (unsigned long long)g_icall_count,
                BRIDGE_MEM32(esp_before + 20), esp_before, g_esp,
                (int)g_esp - (int)esp_before);
    }
    if ((g_icall_count >= 12295ULL && g_icall_count <= 12305ULL) ||
        (g_icall_count >= 321678ULL && g_icall_count <= 321679ULL) ||
        (g_icall_count >= 326430ULL && g_icall_count <= 326450ULL))
        fprintf(stderr, "[FRONTIER-KEWAIT] ic=%llu object=%08X reason=%08X mode=%08X alert=%08X timeout=%08X status=%08X esp=%08X ebp=%08X\n",
                (unsigned long long)g_icall_count, object, wait_reason,
                wait_mode, alertable, timeout_ptr, (unsigned)g_eax,
                g_esp, g_seh_ebp);
    if (wait_log_count++ < 8)
        fprintf(stderr, "[KEWAIT] object=%08X timeout=%08X status=%08X\n",
                object, timeout_ptr, (unsigned)g_eax);
}

/* ── KeDelayExecutionThread (ordinal 99/256) ────────────── */
static void bridge_KeDelayExecutionThread(void)
{
    uint32_t wait_mode   = STACK_ARG(0);
    uint32_t alertable   = STACK_ARG(1);
    uint32_t interval_va = STACK_ARG(2);
    static unsigned trace_delay;

    if (g_worker_active) {
        /* Worker context: the whole recomp runs on one host thread, so a
         * native Sleep here would stall the pump too. Park the worker and
         * let the main fiber resume it when the delay has elapsed. */
        LARGE_INTEGER *iv = (LARGE_INTEGER *)XBOX_TO_NATIVE(interval_va);
        DWORD ms = 0;
        if (iv && iv->QuadPart < 0) {
            LONGLONG rel = -iv->QuadPart;
            ms = (DWORD)(rel / 10000);
            if (ms == 0 && rel > 0) ms = 1;
        }
        if (getenv("MM3_TRACE_DELAY") && g_icall_count >= 300000ULL &&
            trace_delay++ < 128) {
            fprintf(stderr, "[DELAY] ic=%llu tid=%lu interval_va=%08X "
                    "quad=%lld ms=%lu parked=%d active=%d esp=%08X\n",
                    (unsigned long long)g_icall_count,
                    (unsigned long)GetCurrentThreadId(), interval_va,
                    iv ? (long long)iv->QuadPart : 0LL,
                    (unsigned long)ms, g_worker.parked, g_worker_active,
                    g_esp);
            fflush(stderr);
        }
        if (ms > 0) {
            g_worker.wake_tick = GetTickCount() + ms;
            g_worker.parked = 1;
            worker_switch_to_main();   /* resumes when the delay is due */
            g_worker.parked = 0;
            if (getenv("MM3_TRACE_DELAY") && g_icall_count >= 300000ULL &&
                trace_delay++ < 128) {
                fprintf(stderr, "[DELAY-RESUME] ic=%llu tid=%lu parked=%d "
                        "done=%d eax=%08X esp=%08X\n",
                        (unsigned long long)g_icall_count,
                        (unsigned long)GetCurrentThreadId(),
                        g_worker.parked, g_worker.done, g_eax, g_esp);
                fflush(stderr);
            }
        }
        g_eax = 0;  /* STATUS_SUCCESS */
        return;
    }
    g_eax = (uint32_t)xbox_KeDelayExecutionThread(
        (KPROCESSOR_MODE)wait_mode, (BOOLEAN)alertable, XBOX_TO_NATIVE(interval_va));
}

/* ── KeStallExecutionProcessor (ordinal 151) ────────────── */
static void bridge_KeStallExecutionProcessor(void)
{
    xbox_KeStallExecutionProcessor(STACK_ARG(0));
    g_eax = 0;
}

/* ── NtYieldExecution (ordinal 238) ──────────────────────── */
static void bridge_NtYieldExecution(void)
{
    g_eax = (uint32_t)xbox_NtYieldExecution();
}

/* ── MmGetPhysicalAddress (ordinal 173) ──────────────────── */
static void bridge_MmGetPhysicalAddress(void)
{
    uint32_t addr = STACK_ARG(0);
    /* Xbox uses identity mapping (physical == virtual) for the lower 64MB.
     * Just return the Xbox VA as-is. Don't call xbox_MmGetPhysicalAddress
     * which would return a native pointer. */
    g_eax = addr;
}

/* ── MmSetAddressProtect (ordinal 182) ───────────────────── */
static void bridge_MmSetAddressProtect(void)
{
    uint32_t addr = STACK_ARG(0);
    uint32_t size = STACK_ARG(1);
    uint32_t prot = STACK_ARG(2);

    xbox_MmSetAddressProtect(XBOX_TO_NATIVE(addr), size, prot);
    g_eax = 0;
}

/* ── AV pack helpers (ordinals 1-4) ───────────────────────── */
static void bridge_AvGetSavedDataAddress(void)
{
    g_eax = xbox_AvGetSavedDataAddress();
}

static void bridge_AvSendTVEncoderOption(void)
{
    uint32_t addr = STACK_ARG(0);
    uint32_t option = STACK_ARG(1);
    uint32_t param = STACK_ARG(2);
    uint32_t result = STACK_ARG(3);

    xbox_AvSendTVEncoderOption(XBOX_TO_NATIVE(addr), option, param,
                               result ? (PULONG)XBOX_TO_NATIVE(result) : NULL);
    if (getenv("MM3_TRACE_AV_OPTION") && option == 6) {
        fprintf(stderr, "[AV-OPTION] ic=%llu addr=%08X option=%08X param=%08X "
            "result=%08X value=%08X\n",
            (unsigned long long)g_icall_count, addr, option, param, result,
            result ? BRIDGE_MEM32(result) : 0);
        fflush(stderr);
    }
    g_eax = 0;
}

static void bridge_AvSetSavedDataAddress(void)
{
    xbox_AvSetSavedDataAddress(STACK_ARG(0));
    g_eax = 0;
}

static void bridge_AvSetDisplayMode(void)
{
    uint32_t addr = STACK_ARG(0);
    uint32_t step = STACK_ARG(1);
    uint32_t mode = STACK_ARG(2);
    uint32_t format = STACK_ARG(3);
    uint32_t pitch = STACK_ARG(4);
    uint32_t fb = STACK_ARG(5);

    if ((g_icall_count >= 321670ULL && g_icall_count <= 321690ULL) ||
        (g_icall_count >= 326600ULL && g_icall_count <= 326900ULL) ||
        g_icall_count >= 347500ULL)
        fprintf(stderr, "[FRONTIER-AV] ic=%llu addr=%08X step=%08X mode=%08X format=%08X pitch=%08X fb=%08X esp=%08X\n",
                (unsigned long long)g_icall_count, addr, step, mode, format,
                pitch, fb, g_esp);

    xbox_AvSetDisplayMode(XBOX_TO_NATIVE(addr), step, mode, format, pitch, fb);
    if (step == 0)
        d3d8_PresentFrame();
    g_eax = 0;
}

/* ── PsTerminateSystemThread (ordinal 258) ───────────────
 * VOID PsTerminateSystemThread(NTSTATUS ExitStatus)
 *
 * On real Xbox, this terminates the calling thread (never returns).
 * In our recompiled version, we call ExitThread to match the behavior.
 */
static void bridge_PsTerminateSystemThread(void)
{
    uint32_t exit_status = STACK_ARG(0);

    if (g_worker_active) {
        fprintf(stderr, "  [KERNEL] PsTerminateSystemThread: status=0x%08X - worker unwind\n", exit_status);
    fflush(stderr);
        g_worker_active = 0;
        longjmp(g_worker_exit_jmp, 1);
    }

    fprintf(stderr, "  [KERNEL] PsTerminateSystemThread: status=0x%08X - calling ExitThread\n", exit_status);
    fflush(stderr);
    ExitThread(exit_status);
    /* Never returns */
}

/* -- KeBugCheck / KeBugCheckEx (ordinals 95/96) -----------
 * VOID KeBugCheck(ULONG BugCheckCode)
 * VOID KeBugCheckEx(ULONG BugCheckCode, PVOID P1, PVOID P2, PVOID P3, PVOID P4)
 *
 * On real Xbox these halt the system and never return. The title only calls
 * them on fatal/assert paths. Emulate the original semantics: log the code
 * and halt the host run instead of returning 0 into poisoned guest state.
 */
static void bridge_KeBugCheck(void)
{
    uint32_t code = STACK_ARG(0);
    uintptr_t caller = (uintptr_t)_ReturnAddress();
    uintptr_t module = (uintptr_t)GetModuleHandleW(NULL);
    fprintf(stderr, "\n  [KERNEL] KeBugCheck: code=0x%08X ic=%llu caller_rva=0x%zX "
        "eip_hint=%08X esp=%08X eax=%08X ecx=%08X edx=%08X "
        "- guest requested fatal halt, terminating run\n", code,
        (unsigned long long)g_icall_count, (size_t)(caller - module),
        *(uint32_t *)(uintptr_t)(g_esp + g_xbox_mem_offset), g_esp,
        g_eax, g_ecx, g_edx);
    fprintf(stderr, "  [BUGCHECK-STATE] stack=%08X/%08X/%08X/%08X "
        "46e900=%08X 46e904=%08X 362140=%08X 3c0000=%08X 3bfffc=%08X "
        "dice=e584:%08X e588:%08X e6a4:%08X e6c0:%08X\n",
        STACK_ARG(0), STACK_ARG(1), STACK_ARG(2), STACK_ARG(3),
        BRIDGE_MEM32(0x46E900), BRIDGE_MEM32(0x46E904),
        BRIDGE_MEM32(0x362140), BRIDGE_MEM32(0x3C0000),
        BRIDGE_MEM32(0x3BFFFC), BRIDGE_MEM32(0x46E584),
        BRIDGE_MEM32(0x46E588), BRIDGE_MEM32(0x46E6A4),
        BRIDGE_MEM32(0x46E6C0));
    fflush(stderr);
    ExitProcess(1);
}

static void bridge_KeBugCheckEx(void)
{
    uint32_t code = STACK_ARG(0);
    uint32_t p1 = STACK_ARG(1);
    uint32_t p2 = STACK_ARG(2);
    uint32_t p3 = STACK_ARG(3);
    uint32_t p4 = STACK_ARG(4);
    fprintf(stderr, "\n  [KERNEL] KeBugCheckEx: code=0x%08X p1=0x%08X p2=0x%08X p3=0x%08X p4=0x%08X - guest requested fatal halt, terminating run\n",
            code, p1, p2, p3, p4);
    fflush(stderr);
    ExitProcess(1);
}

/* ── HalReadSMCTrayState (ordinal 47) ─────────────────────
 * VOID HalReadSMCTrayState(PDWORD TrayState, PDWORD TrayStateChangeCount)
 *
 * Returns DVD tray state. 0x10 = no disc, 0x14 = tray closed with disc.
 */
static void bridge_HalReadSMCTrayState(void)
{
    uint32_t state_ptr = STACK_ARG(0);
    uint32_t count_ptr = STACK_ARG(1);

    if (state_ptr) BRIDGE_MEM32(state_ptr) = 0x10;  /* No disc */
    if (count_ptr) BRIDGE_MEM32(count_ptr) = 0;
    g_eax = 0;
}

/* ── KeInitializeDpc (ordinal 107) ────────────────────────
 * VOID KeInitializeDpc(PKDPC Dpc, PKDEFERRED_ROUTINE DeferredRoutine,
 *                       PVOID DeferredContext)
 *
 * Initializes a DPC object. The Xbox KDPC structure is 32 bytes.
 * We zero it and set the routine and context pointers.
 */
static void bridge_KeInitializeDpc(void)
{
    uint32_t dpc_va = STACK_ARG(0);
    uint32_t routine = STACK_ARG(1);
    uint32_t context = STACK_ARG(2);

    /* Zero the structure (32 bytes) */
    memset(XBOX_TO_NATIVE(dpc_va), 0, 32);

    /* Set Type (0x13 = DpcObject) and fields */
    BRIDGE_MEM16(dpc_va + 0) = 0x13;   /* Type */
    BRIDGE_MEM32(dpc_va + 12) = routine; /* DeferredRoutine */
    BRIDGE_MEM32(dpc_va + 16) = context; /* DeferredContext */
    g_eax = 0;
}

static void bridge_KeInitializeInterrupt(void)
{
    uint32_t interrupt_va = STACK_ARG(0);
    xbox_KeInitializeInterrupt(
        (PXBOX_KINTERRUPT)XBOX_TO_NATIVE(interrupt_va),
        (PVOID)(uintptr_t)STACK_ARG(1),
        (PVOID)(uintptr_t)STACK_ARG(2),
        STACK_ARG(3), (KIRQL)STACK_ARG(4), STACK_ARG(5),
        (BOOLEAN)STACK_ARG(6));
    g_eax = 0;
}

static void bridge_KeConnectInterrupt(void)
{
    uint32_t interrupt_va = STACK_ARG(0);
    g_eax = xbox_KeConnectInterrupt(
        (PXBOX_KINTERRUPT)XBOX_TO_NATIVE(interrupt_va));
}

static void bridge_KeDisconnectInterrupt(void)
{
    uint32_t interrupt_va = STACK_ARG(0);
    g_eax = xbox_KeDisconnectInterrupt(
        (PXBOX_KINTERRUPT)XBOX_TO_NATIVE(interrupt_va));
}

static void bridge_KeInsertQueueDpc(void)
{
    uint32_t dpc_va = STACK_ARG(0);
    g_eax = xbox_KeInsertQueueDpc(
        (PXBOX_KDPC)XBOX_TO_NATIVE(dpc_va),
        (PVOID)(uintptr_t)STACK_ARG(1),
        (PVOID)(uintptr_t)STACK_ARG(2));
}

/* ── KeInitializeTimerEx (ordinal 113) ────────────────────
 * VOID KeInitializeTimerEx(PKTIMER Timer, TIMER_TYPE Type)
 *
 * Initializes a timer object. Xbox KTIMER is 40 bytes.
 */
static void bridge_KeInitializeTimerEx(void)
{
    uint32_t timer_va = STACK_ARG(0);
    uint32_t type = STACK_ARG(1);

    /* Zero the structure (40 bytes) */
    memset(XBOX_TO_NATIVE(timer_va), 0, 40);

    /* Set Type (0x08 = TimerNotificationObject, 0x09 = TimerSynchronizationObject) */
    BRIDGE_MEM16(timer_va + 0) = (uint16_t)(0x08 + (type & 1));
    g_eax = 0;
}

/* ── KeSetTimer / KeSetTimerEx (ordinal 149/150) ──────────
 * BOOLEAN KeSetTimer(PKTIMER Timer, LARGE_INTEGER DueTime, PKDPC Dpc)
 *
 * Sets a timer. We don't actually start timers - just record the state.
 * Returns FALSE (timer was not already set).
 */
static void bridge_KeSetTimer(void)
{
    /* Timer functionality is not needed for basic execution.
     * Return FALSE = timer was not previously set. */
    g_eax = 0;
}

/* ── ExQueryPoolBlockSize (ordinal 24) ────────────────────
 * ULONG ExQueryPoolBlockSize(PVOID PoolBlock)
 *
 * Returns the size of a pool memory block.
 * Since we use HeapAlloc, we can query the Windows heap.
 */
static void bridge_ExQueryPoolBlockSize(void)
{
    uint32_t block = STACK_ARG(0);
    /* Return a reasonable default size. Actual pool blocks are managed
     * by the kernel; for recompilation, returning 0 might be OK since
     * code usually uses this for debugging/stats. */
    g_eax = 0;
}

/* ── RtlNtStatusToDosError (ordinal 301) ─────────────────
 * ULONG RtlNtStatusToDosError(NTSTATUS Status)
 *
 * Converts an NTSTATUS to a Win32 error code.
 */
static void bridge_RtlNtStatusToDosError(void)
{
    uint32_t status = STACK_ARG(0);

    /* Simple mapping of common status codes */
    switch (status) {
    case 0x00000000: g_eax = 0; break;          /* STATUS_SUCCESS → ERROR_SUCCESS */
    case 0xC0000034: g_eax = 2; break;          /* STATUS_OBJECT_NAME_NOT_FOUND → ERROR_FILE_NOT_FOUND */
    case 0xC000003A: g_eax = 3; break;          /* STATUS_OBJECT_PATH_NOT_FOUND → ERROR_PATH_NOT_FOUND */
    case 0xC0000022: g_eax = 5; break;          /* STATUS_ACCESS_DENIED → ERROR_ACCESS_DENIED */
    case 0xC0000008: g_eax = 6; break;          /* STATUS_INVALID_HANDLE → ERROR_INVALID_HANDLE */
    case 0xC0000017: g_eax = 8; break;          /* STATUS_NO_MEMORY → ERROR_NOT_ENOUGH_MEMORY */
    case 0xC000000D: g_eax = 87; break;         /* STATUS_INVALID_PARAMETER → ERROR_INVALID_PARAMETER */
    default:         g_eax = 317; break;         /* ERROR_MR_MID_NOT_FOUND (generic) */
    }
}

/* ── File I/O bridge helpers ─────────────────────────────── */

/*
 * Xbox structures use 32-bit pointers. On Win64, the C structs
 * (XBOX_OBJECT_ATTRIBUTES, etc.) have 64-bit pointers, so we can't
 * cast Xbox memory to them directly. Instead, parse the 32-bit
 * Xbox layout manually:
 *
 * XBOX_OBJECT_ATTRIBUTES (12 bytes):
 *   offset 0: RootDirectory  (uint32_t)
 *   offset 4: ObjectName     (uint32_t, Xbox VA to ANSI_STRING)
 *   offset 8: Attributes     (uint32_t)
 *
 * XBOX_ANSI_STRING (8 bytes):
 *   offset 0: Length          (uint16_t)
 *   offset 2: MaximumLength   (uint16_t)
 *   offset 4: Buffer          (uint32_t, Xbox VA to char[])
 *
 * XBOX_IO_STATUS_BLOCK (8 bytes):
 *   offset 0: Status          (uint32_t)
 *   offset 4: Information     (uint32_t)
 */

/* Extract the ANSI path string from an Xbox OBJECT_ATTRIBUTES */
static const char* bridge_get_xbox_path(uint32_t obj_attrs_va)
{
    uint32_t ansi_str_va, buf_va;
    if (!obj_attrs_va) return NULL;
    ansi_str_va = BRIDGE_MEM32(obj_attrs_va + 4);
    if (!ansi_str_va) return NULL;
    buf_va = BRIDGE_MEM32(ansi_str_va + 4);
    if (!buf_va) return NULL;
    return (const char*)XBOX_TO_NATIVE(buf_va);
}

/* Write NTSTATUS + Information into Xbox IO_STATUS_BLOCK */
static void bridge_write_iostatus(uint32_t ios_va, NTSTATUS status, uint32_t info)
{
    if (ios_va) {
        BRIDGE_MEM32(ios_va + 0) = (uint32_t)status;
        BRIDGE_MEM32(ios_va + 4) = info;
    }
}

/*
 * Handle table.
 *
 * Xbox memory only has 32-bit handle slots, but native HANDLEs are 64-bit
 * pointers (win32_compat objects, or real Win32 handles on Windows). Map
 * 32-bit tokens <-> native HANDLEs so a handle survives a round-trip through
 * Xbox memory. Tokens carry a tag in the high byte so they never collide
 * with the synthetic handles (0xDEAD0001 / 0xBEEF0010) used elsewhere.
 */
static uint32_t bridge_handle_token(HANDLE h)
{
    int i;
    if (!h || h == INVALID_HANDLE_VALUE) return 0;
    for (i = 1; i < BRIDGE_HANDLE_MAX; i++)
        if (s_handle_table[i] == h) return BRIDGE_HANDLE_TAG | (uint32_t)i;
    for (i = 1; i < BRIDGE_HANDLE_MAX; i++)
        if (s_handle_table[i] == NULL) {
            s_handle_table[i] = h;
            return BRIDGE_HANDLE_TAG | (uint32_t)i;
        }
    fprintf(stderr, "  [BRIDGE] handle table full\n");
    return 0;
}

/* Store a native HANDLE into a 32-bit Xbox memory slot (as a token). */
static void bridge_write_handle(uint32_t handle_va, HANDLE h)
{
    if (handle_va)
        BRIDGE_MEM32(handle_va) = bridge_handle_token(h);
}

/* Resolve a 32-bit handle token to a native HANDLE.
 * Tokens are tagged with BRIDGE_HANDLE_TAG in the high byte;
 * untagged values pass through as synthetic/dummy handles.
 * This treats the argument as a TOKEN VALUE, not a VA pointer. */
static HANDLE bridge_read_handle(uint32_t token)
{
    if ((token & 0xFF000000u) == BRIDGE_HANDLE_TAG) {
        uint32_t i = token & BRIDGE_HANDLE_MASK;
        return (i > 0 && i < BRIDGE_HANDLE_MAX) ? s_handle_table[i] : NULL;
    }
    /* Untagged value: synthetic/dummy handle -- pass through unchanged. */
    return (HANDLE)(uintptr_t)token;
}

/* Resolve a token to a HANDLE and release its table slot (for NtClose). */
static HANDLE bridge_take_handle(uint32_t token)
{
    if ((token & 0xFF000000u) == BRIDGE_HANDLE_TAG) {
        uint32_t i = token & BRIDGE_HANDLE_MASK;
        if (i > 0 && i < BRIDGE_HANDLE_MAX) {
            HANDLE h = s_handle_table[i];
            s_handle_table[i] = NULL;
            return h;
        }
    }
    return NULL;   /* untagged -> not a table handle, do not close */
}

/* Build a native OBJECT_ATTRIBUTES wrapping the translated Xbox path. */
static void bridge_build_oa(uint32_t obj_attrs_va,
                            XBOX_OBJECT_ATTRIBUTES* oa, XBOX_ANSI_STRING* name)
{
    const char* path = bridge_get_xbox_path(obj_attrs_va);
    name->Buffer        = (PCHAR)path;
    name->Length        = path ? (USHORT)strlen(path) : 0;
    name->MaximumLength = (USHORT)(name->Length + 1);
    oa->RootDirectory = NULL;
    oa->ObjectName    = name;
    oa->Attributes    = 0;
}

/* Open a file by delegating to the ported xbox_NtCreateFile kernel HLE. */
static NTSTATUS bridge_create_file_impl(
    uint32_t handle_va, ACCESS_MASK access, uint32_t obj_attrs_va,
    uint32_t iostatus_va, ULONG file_attrs, ULONG share,
    ULONG disposition, ULONG options)
{
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING       name;
    XBOX_IO_STATUS_BLOCK   ios;
    HANDLE   h  = NULL;
    NTSTATUS st;

    bridge_build_oa(obj_attrs_va, &oa, &name);
    if (!name.Buffer) {
        bridge_write_iostatus(iostatus_va, STATUS_OBJECT_PATH_NOT_FOUND, 0);
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }
    memset(&ios, 0, sizeof(ios));

    DWORD gle = 0;
    st = xbox_NtCreateFile(&h, access, &oa, &ios, NULL,
                           file_attrs, share, disposition, options);
    gle = GetLastError();

    {
        static int s_res_log = 0;
        if (s_res_log < 20) {
            fprintf(stderr, "[FILE] result path='%s' st=0x%08X h=%p access=0x%X share=0x%X disp=%d opts=0x%X gle=%u seh_ebp=0x%08X\n",
                    name.Buffer ? name.Buffer : "(null)", (uint32_t)st, h,
                    access, share, disposition, options, gle, g_seh_ebp);
            s_res_log++;
        }
    }

    if (NT_SUCCESS(st)) {
        /* Always store the native Windows HANDLE for kernel I/O
         * (NtReadFile, NtClose, etc.). The handle-table token
         * is tagged and stored in Xbox memory. */
        bridge_write_handle(handle_va, h);
        fprintf(stderr, "  [HANDLE] open path='%s' va=0x%08X tok=0x%08X h=%p\n",
                name.Buffer ? name.Buffer : "(null)", handle_va,
                handle_va ? BRIDGE_MEM32(handle_va) : 0, h);

        /* Also register a CRT FILE* for fread/fread_s interception.
         * Uses a duplicated handle so CRT owns the dup while the
         * kernel keeps the original for ReadFile-based NtReadFile. */
        if (h && h != INVALID_HANDLE_VALUE) {
            HANDLE dup_h = NULL;
            if (DuplicateHandle(GetCurrentProcess(), h,
                    GetCurrentProcess(), &dup_h,
                    0, FALSE, DUPLICATE_SAME_ACCESS)) {
                int fd = _open_osfhandle((intptr_t)dup_h, _O_RDONLY);
                if (fd != -1) {
                    FILE *fp = _fdopen(fd, "rb");
                    if (fp) {
                        int slot = xbox_file_register(fp);
                        if (slot) {
                            xbox_handle_register_file(slot, fp);
                            uint32_t tok = handle_va ? BRIDGE_MEM32(handle_va) : 0;
                            if ((tok & 0xFF000000u) == BRIDGE_HANDLE_TAG) {
                                uint32_t ti = tok & BRIDGE_HANDLE_MASK;
                                if (ti > 0 && ti < BRIDGE_HANDLE_MAX)
                                    s_handle_file_slot[ti] = slot;
                            }
                        } else {
                            fclose(fp);
                        }
                    } else {
                        _close(fd);
                    }
                } else {
                    CloseHandle(dup_h);
                }
            }
        }

        bridge_write_iostatus(iostatus_va, ios.Status, (uint32_t)ios.Information);
    } else {
        bridge_write_iostatus(iostatus_va, st, 0);
    }
    return st;
}

/* ── NtCreateFile (ordinal 190, 9 args = 36 bytes) ─────── */
static void bridge_NtCreateFile(void)
{
    uint32_t handle_va   = STACK_ARG(0);  /* PHANDLE */
    uint32_t access      = STACK_ARG(1);  /* ACCESS_MASK */
    uint32_t obj_attrs   = STACK_ARG(2);  /* POBJECT_ATTRIBUTES */
    uint32_t iostatus    = STACK_ARG(3);  /* PIO_STATUS_BLOCK */
    /* arg4: AllocationSize - ignored */
    uint32_t file_attrs  = STACK_ARG(5);  /* FileAttributes */
    uint32_t share       = STACK_ARG(6);  /* ShareAccess */
    uint32_t disposition = STACK_ARG(7);  /* CreateDisposition */
    uint32_t options     = STACK_ARG(8);  /* CreateOptions */

    g_eax = (uint32_t)bridge_create_file_impl(
        handle_va, access, obj_attrs, iostatus,
        file_attrs, share, disposition, options);
}

/* ── NtOpenFile (ordinal 202, 6 args = 24 bytes) ──────── */
static void bridge_NtOpenFile(void)
{
    uint32_t handle_va = STACK_ARG(0);  /* PHANDLE */
    uint32_t access    = STACK_ARG(1);  /* ACCESS_MASK */
    uint32_t obj_attrs = STACK_ARG(2);  /* POBJECT_ATTRIBUTES */
    uint32_t iostatus  = STACK_ARG(3);  /* PIO_STATUS_BLOCK */
    uint32_t share     = STACK_ARG(4);  /* ShareAccess */
    uint32_t options   = STACK_ARG(5);  /* OpenOptions */

    /* Log the path being opened */
    {
        const char* path = bridge_get_xbox_path(obj_attrs);
        fprintf(stderr, "  [KERNEL] NtOpenFile: path='%s' access=0x%X\n",
            path ? path : "(null)", access);
        if (path) {
            static int s_hex = 0;
            if (s_hex < 24) {
                const uint8_t *pb = (const uint8_t *)path;
                fprintf(stderr, "  [KERNEL] NtOpenFile pathhex:");
                for (int i = 0; i < 32; i++) fprintf(stderr, " %02X", pb[i]);
                fprintf(stderr, "\n");
                s_hex++;
            }
        }
        if (!path && getenv("MM3_TRACE_PATHS")) {
            fprintf(stderr, "[NTOPTRACE] obj_attrs=0x%08X ansi=0x%08X buf=0x%08X\n",
                obj_attrs,
                obj_attrs ? BRIDGE_MEM32(obj_attrs + 4) : 0,
                obj_attrs ? BRIDGE_MEM32(BRIDGE_MEM32(obj_attrs + 4) + 4) : 0);
        }
    }

    /* NtOpenFile = NtCreateFile with FILE_OPEN disposition */
    g_eax = (uint32_t)bridge_create_file_impl(
        handle_va, access, obj_attrs, iostatus,
        0, share, 1 /* FILE_OPEN */, options);

    fprintf(stderr, "  [KERNEL] NtOpenFile: result=0x%08X handle_va=0x%08X\n",
        g_eax, handle_va);
}

/* ── NtReadFile (ordinal 219, 8 args = 32 bytes) ──────── */
static void bridge_NtReadFile(void)
{
    HANDLE   handle    = bridge_read_handle(STACK_ARG(0));
    uint32_t iostatus  = STACK_ARG(4);
    uint32_t buffer_va = STACK_ARG(5);
    uint32_t length    = STACK_ARG(6);
    uint32_t offset_va = STACK_ARG(7);
    XBOX_IO_STATUS_BLOCK ios;
    LARGE_INTEGER  off;
    PLARGE_INTEGER poff = NULL;
    LARGE_INTEGER  cur = {0};
    LARGE_INTEGER  zero = {0};

    memset(&ios, 0, sizeof(ios));
    if (offset_va) {
        off.LowPart  = BRIDGE_MEM32(offset_va);
        off.HighPart = (LONG)BRIDGE_MEM32(offset_va + 4);
        poff = &off;
    }
    if (handle && handle != INVALID_HANDLE_VALUE)
        SetFilePointerEx(handle, zero, &cur, FILE_CURRENT);
    g_eax = (uint32_t)xbox_NtReadFile(handle, NULL, NULL, NULL, &ios,
                XBOX_TO_NATIVE(buffer_va), length, poff);
    bridge_write_iostatus(iostatus, ios.Status, (uint32_t)ios.Information);
    {
        /* MM3 run-1048 (read-only): loader-window read trace. Logs the
         * guest-requested ByteOffset (off) plus the host file pointer before
         * the read (cur) so a VFS/zip stream rewind shows up as either an
         * explicit guest seek or a repeating host read position. Window
         * ic>=400000, capped at 3000 lines. Env-gated via MM3_TRACE_READS. */
        static int s_read_log = 0;
        if (getenv("MM3_TRACE_READS") && g_icall_count >= 400000ULL &&
            s_read_log < 3000) {
            uint32_t rb0 = 0, rb1 = 0;
            if (buffer_va < 0x04000000u) {
                rb0 = BRIDGE_MEM32(buffer_va);
                rb1 = BRIDGE_MEM32(buffer_va + 4);
            }
            fprintf(stderr, "[READ] h=%p off=%lld cur=%lld len=%u st=0x%08X "
                    "info=%u buf=%08X %08X ic=%llu fnrva=%zX\n",
                    handle, poff ? (long long)off.QuadPart : -1LL,
                    (long long)cur.QuadPart, length, (uint32_t)ios.Status,
                    (uint32_t)ios.Information, rb0, rb1,
                    (unsigned long long)g_icall_count,
                    (size_t)g_penter_last_rva);
            s_read_log++;
        }
    }
}

/* ── NtWriteFile (ordinal 236, 8 args = 32 bytes) ─────── */
static void bridge_NtWriteFile(void)
{
    HANDLE   handle    = bridge_read_handle(STACK_ARG(0));
    uint32_t iostatus  = STACK_ARG(4);
    uint32_t buffer_va = STACK_ARG(5);
    uint32_t length    = STACK_ARG(6);
    uint32_t offset_va = STACK_ARG(7);
    XBOX_IO_STATUS_BLOCK ios;
    LARGE_INTEGER  off;
    PLARGE_INTEGER poff = NULL;

    memset(&ios, 0, sizeof(ios));
    if (offset_va) {
        off.LowPart  = BRIDGE_MEM32(offset_va);
        off.HighPart = (LONG)BRIDGE_MEM32(offset_va + 4);
        poff = &off;
    }
    g_eax = (uint32_t)xbox_NtWriteFile(handle, NULL, NULL, NULL, &ios,
                XBOX_TO_NATIVE(buffer_va), length, poff);
    bridge_write_iostatus(iostatus, ios.Status, (uint32_t)ios.Information);
}

/* ── NtQueryInformationFile (ordinal 211, 5 args = 20 bytes) */
static void bridge_NtQueryInformationFile(void)
{
    HANDLE   handle    = bridge_read_handle(STACK_ARG(0));
    uint32_t ios_va    = STACK_ARG(1);
    uint32_t info_va   = STACK_ARG(2);
    uint32_t length    = STACK_ARG(3);
    uint32_t infoclass = STACK_ARG(4);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    g_eax = (uint32_t)xbox_NtQueryInformationFile(handle, &ios,
                XBOX_TO_NATIVE(info_va), length,
                (XBOX_FILE_INFORMATION_CLASS)infoclass);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
    {
        static int s_q_log = 0;
        if (s_q_log < 20) {
            uint64_t eof = 0;
            DWORD qgle = GetLastError();
            uint32_t qb[4] = {0, 0, 0, 0};
            if (info_va < 0x04000000u)
            {
                /* FileNetworkOpenInformation.EndOfFile is at +0x28. */
                eof = (uint64_t)BRIDGE_MEM32(info_va + 0x28) |
                      ((uint64_t)BRIDGE_MEM32(info_va + 0x2C) << 32);
                qb[0] = BRIDGE_MEM32(info_va + 0x28);
                qb[1] = BRIDGE_MEM32(info_va + 0x2C);
                qb[2] = BRIDGE_MEM32(info_va + 0x30);
                qb[3] = BRIDGE_MEM32(info_va + 0x34);
            }
            fprintf(stderr, "[QFILE] tok=0x%08X h=%p class=%u len=%u "
                    "st=0x%08X eof64=%llu gle=%u buf=%08X %08X %08X %08X "
                    "ic=%llu\n",
                    STACK_ARG(0), handle, infoclass, length,
                    (uint32_t)ios.Status, (unsigned long long)eof, qgle,
                    qb[0], qb[1], qb[2], qb[3],
                    (unsigned long long)g_icall_count);
            s_q_log++;
        }
    }
}

/* ── NtSetInformationFile (ordinal 226, 5 args = 20 bytes) ─ */
static void bridge_NtSetInformationFile(void)
{
    HANDLE   handle    = bridge_read_handle(STACK_ARG(0));
    uint32_t ios_va    = STACK_ARG(1);
    uint32_t info_va   = STACK_ARG(2);
    uint32_t length    = STACK_ARG(3);
    uint32_t infoclass = STACK_ARG(4);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    g_eax = (uint32_t)xbox_NtSetInformationFile(handle, &ios,
                XBOX_TO_NATIVE(info_va), length,
                (XBOX_FILE_INFORMATION_CLASS)infoclass);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
    {
        /* MM3 run-1048 (read-only): file-seek trace for the loader window.
         * Logs FilePositionInformation targets so a stream rewind is visible
         * as an explicit guest seek. Env-gated via MM3_TRACE_READS. */
        static int s_seek_log = 0;
        if (getenv("MM3_TRACE_READS") && g_icall_count >= 400000ULL &&
            infoclass == XboxFilePositionInformation && s_seek_log < 500) {
            LONGLONG target =
                ((LONGLONG)(LONG)BRIDGE_MEM32(info_va + 4) << 32) |
                (LONGLONG)BRIDGE_MEM32(info_va);
            fprintf(stderr, "[SEEK] h=%p off=%lld ic=%llu fnrva=%zX "
                    "ra=%08X esp=%08X\n",
                    handle, (long long)target,
                    (unsigned long long)g_icall_count,
                    (size_t)g_penter_last_rva,
                    (unsigned)BRIDGE_MEM32((uint32_t)g_esp),
                    (uint32_t)g_esp);
            s_seek_log++;
        }
    }
}

/* ── NtQueryVolumeInformationFile (ordinal 218, 5 args = 20 bytes) */
static void bridge_NtQueryVolumeInformationFile(void)
{
    HANDLE   handle    = bridge_read_handle(STACK_ARG(0));
    uint32_t ios_va    = STACK_ARG(1);
    uint32_t info_va   = STACK_ARG(2);
    uint32_t length    = STACK_ARG(3);
    uint32_t infoclass = STACK_ARG(4);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    g_eax = (uint32_t)xbox_NtQueryVolumeInformationFile(handle, &ios,
                XBOX_TO_NATIVE(info_va), length,
                (XBOX_FS_INFORMATION_CLASS)infoclass);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtQueryFullAttributesFile (ordinal 210, 2 args = 8 bytes) */
static void bridge_NtQueryFullAttributesFile(void)
{
    uint32_t obj_attrs = STACK_ARG(0);
    uint32_t info_va   = STACK_ARG(1);
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING       name;

    bridge_build_oa(obj_attrs, &oa, &name);
    if (!name.Buffer) { g_eax = STATUS_OBJECT_PATH_NOT_FOUND; return; }
    g_eax = (uint32_t)xbox_NtQueryFullAttributesFile(&oa,
                (PXBOX_FILE_NETWORK_OPEN_INFORMATION)XBOX_TO_NATIVE(info_va));
}

/* ── NtFlushBuffersFile (ordinal 198, 2 args = 8 bytes) ─── */
static void bridge_NtFlushBuffersFile(void)
{
    HANDLE   handle = bridge_read_handle(STACK_ARG(0));
    uint32_t ios_va = STACK_ARG(1);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    g_eax = (uint32_t)xbox_NtFlushBuffersFile(handle, &ios);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtDeleteFile (ordinal 195, 1 arg = 4 bytes) ─────── */
static void bridge_NtDeleteFile(void)
{
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING       name;

    bridge_build_oa(STACK_ARG(0), &oa, &name);
    if (!name.Buffer) { g_eax = STATUS_OBJECT_PATH_NOT_FOUND; return; }
    g_eax = (uint32_t)xbox_NtDeleteFile(&oa);
}

/* ── NtQueryDirectoryFile (ordinal 207, 9 args = 36 bytes) ─ */
static void bridge_NtQueryDirectoryFile(void)
{
    HANDLE   handle      = bridge_read_handle(STACK_ARG(0));
    uint32_t ios_va      = STACK_ARG(4);
    uint32_t info_va     = STACK_ARG(5);
    uint32_t length      = STACK_ARG(6);
    uint32_t filename_va = STACK_ARG(7);  /* PXBOX_ANSI_STRING */
    uint32_t restart     = STACK_ARG(8);  /* BOOLEAN */
    XBOX_IO_STATUS_BLOCK ios;
    XBOX_ANSI_STRING     fn;
    PXBOX_ANSI_STRING    pfn = NULL;

    memset(&ios, 0, sizeof(ios));
    if (filename_va) {
        /* Xbox ANSI_STRING: 0=Length(u16), 2=MaximumLength(u16), 4=Buffer(u32) */
        uint32_t fn_buf  = BRIDGE_MEM32(filename_va + 4);
        fn.Length        = BRIDGE_MEM16(filename_va);
        fn.MaximumLength = BRIDGE_MEM16(filename_va + 2);
        fn.Buffer        = fn_buf ? (PCHAR)XBOX_TO_NATIVE(fn_buf) : NULL;
        if (fn.Buffer) pfn = &fn;
    }
    g_eax = (uint32_t)xbox_NtQueryDirectoryFile(handle, NULL, NULL, NULL, &ios,
                XBOX_TO_NATIVE(info_va), length, pfn, (BOOLEAN)restart);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtOpenSymbolicLinkObject (ordinal 203, 2 args = 8 bytes) */
static void bridge_NtOpenSymbolicLinkObject(void)
{
    uint32_t handle_va = STACK_ARG(0);
    /* arg1: POBJECT_ATTRIBUTES - ignored, we return a synthetic handle.
     * Written raw (untagged) so NtClose recognises it and skips it. */
    if (handle_va) BRIDGE_MEM32(handle_va) = 0xDEAD0001u;
    g_eax = STATUS_SUCCESS;
}

/* ── NtQuerySymbolicLinkObject (ordinal 215, 3 args = 12 bytes) */
static void bridge_NtQuerySymbolicLinkObject(void)
{
    /* uint32_t handle = STACK_ARG(0); */
    uint32_t target_va = STACK_ARG(1);
    uint32_t retlen_va = STACK_ARG(2);
    const char* target = "\\Device\\CdRom0";
    USHORT len = (USHORT)strlen(target);

    if (target_va) {
        uint16_t max_len = BRIDGE_MEM16(target_va + 2);
        uint32_t buf_va  = BRIDGE_MEM32(target_va + 4);
        if (buf_va && len < max_len) {
            memcpy(XBOX_TO_NATIVE(buf_va), target, len + 1);
            BRIDGE_MEM16(target_va) = len;
        }
    }
    if (retlen_va) BRIDGE_MEM32(retlen_va) = (uint32_t)len;
    g_eax = STATUS_SUCCESS;
}

/* ── IoCreateFile (ordinal 67, 10 args = 40 bytes) ────── */
static void bridge_IoCreateFile(void)
{
    /* Same as NtCreateFile with an extra Options arg at the end */
    uint32_t handle_va   = STACK_ARG(0);
    uint32_t access      = STACK_ARG(1);
    uint32_t obj_attrs   = STACK_ARG(2);
    uint32_t iostatus    = STACK_ARG(3);
    uint32_t file_attrs  = STACK_ARG(5);
    uint32_t share       = STACK_ARG(6);
    uint32_t disposition = STACK_ARG(7);
    uint32_t options     = STACK_ARG(8);

    g_eax = (uint32_t)bridge_create_file_impl(
        handle_va, access, obj_attrs, iostatus,
        file_attrs, share, disposition, options);
}

/* ── NtDeviceIoControlFile (ordinal 196, 10 args = 40 bytes) */
static void bridge_NtDeviceIoControlFile(void)
{
    uint32_t ioctl = STACK_ARG(5);
    uint32_t ios_va = STACK_ARG(4);
    fprintf(stderr, "  [FILE] NtDeviceIoControlFile(0x%X) - stub\n", ioctl);
    bridge_write_iostatus(ios_va, 0xC00000BBu, 0);
    g_eax = 0xC00000BBu; /* STATUS_NOT_IMPLEMENTED */
}

/* ── NtFsControlFile (ordinal 200, 10 args = 40 bytes) ──── */
static void bridge_NtFsControlFile(void)
{
    uint32_t fsctl = STACK_ARG(5);
    uint32_t ios_va = STACK_ARG(4);
    fprintf(stderr, "  [FILE] NtFsControlFile(0x%X) - stub\n", fsctl);
    bridge_write_iostatus(ios_va, 0xC00000BBu, 0);
    g_eax = 0xC00000BBu;
}

/* ── NtCreateDirectoryObject (ordinal 188) ──────────────── */
static void bridge_NtCreateDirectoryObject(void)
{
    /* Return STATUS_SUCCESS with a fake handle */
    uint32_t handle_ptr = STACK_ARG(0);
    if (handle_ptr) BRIDGE_MEM32(handle_ptr) = 0xBEEF0010;
    g_eax = 0;  /* STATUS_SUCCESS */
}

/* ── RtlInitAnsiString (ordinal 289, 2 args = 8 bytes) ───
 * VOID RtlInitAnsiString(PANSI_STRING Dest, PCSZ Source)
 * MM3's XPP memory-unit driver builds "\Device\MU_n" names with it.
 */
static void bridge_RtlInitAnsiString(void)
{
    uint32_t dst = STACK_ARG(0);
    uint32_t src = STACK_ARG(1);
    if (!dst || !src) {
        g_eax = 0;
        return;
    }
    size_t len = 0;
    const uint8_t *p = (const uint8_t *)XBOX_TO_NATIVE(src);
    while (p[len]) len++;
    if (len > 0xFFFF) len = 0xFFFF;
    if (getenv("MM3_TRACE_ANSI")) {
        static int s_ansi = 0;
        if (dst < 0x04000000u) {
            const uint8_t *dp = (const uint8_t *)XBOX_TO_NATIVE(dst);
            fprintf(stderr, "[ANSI]   dst=%08X struct: %02X%02X %02X%02X %02X%02X%02X%02X\n",
                    dst, dp[0], dp[1], dp[2], dp[3], dp[4], dp[5], dp[6], dp[7]);
        }
        if (src >= 0x01080000u && src < 0x010A0000u) {
            const uint8_t *sp = (const uint8_t *)XBOX_TO_NATIVE(src - 0x20 < src ? src - 0x20 : 0);
            (void)sp;
            fprintf(stderr, "[ANSI]   around src:");
            for (int k = -8; k < 24; k += 4) {
                uint32_t a = (uint32_t)((int32_t)src + k * 4);
                const uint8_t *ap = (const uint8_t *)XBOX_TO_NATIVE(a);
                fprintf(stderr, " %02X%02X%02X%02X", ap[0], ap[1], ap[2], ap[3]);
            }
            fprintf(stderr, "\n");
        }
        s_ansi++;
        if (s_ansi < 24) {
            extern volatile uint32_t g_icall_trace[16], g_icall_trace_idx;
            { uint32_t pi = g_icall_trace_idx; fprintf(stderr, "[ANSI]   ring:");
              for (int k = 0; k < 8; k++) fprintf(stderr, " %08X", g_icall_trace[(pi - 1 - k) & 15]);
              fprintf(stderr, " esp=0x%08X\n", g_esp); }
            fprintf(stderr, "[ANSI] src=0x%08X ret=0x%08X len=%u str=%.40s "
                    "hostrva=0x%llX seh_ebp=0x%08X\n",
                    src, BRIDGE_MEM32(g_esp - 4), (uint32_t)len, (const char*)p,
                    (unsigned long long)((uintptr_t)_ReturnAddress() - (uintptr_t)GetModuleHandle(NULL)),
                    g_seh_ebp);
            if (src >= 0x00700000u && src < 0x01000000u) {
                const uint8_t *bp = (const uint8_t *)XBOX_TO_NATIVE(src);
                fprintf(stderr, "[ANSI]   dump:");
                for (int j = 0; j < 48; j += 4)
                    fprintf(stderr, " %02X%02X%02X%02X", bp[j], bp[j+1], bp[j+2], bp[j+3]);
                fprintf(stderr, "\n");
                s_ansi++;
            }
        }
    }
    BRIDGE_MEM16(dst) = (uint16_t)len;
    BRIDGE_MEM16(dst + 2) = (uint16_t)(len + 1);
    BRIDGE_MEM32(dst + 4) = src;
    g_eax = 0;
}
/* ── IoCreateSymbolicLink (ordinal 63) ───────────────────── */
/* IoAllocateIrp (ordinal 59, 2 args = 8 bytes).
 * Drivers in the XPP/resource path require a real guest-resident IRP;
 * returning the generic null stub makes their request path immediately fail.
 * Keep the allocation opaque here: callers own the Xbox-layout fields and
 * the existing IoInitializeIrp bridge handles explicit initialization. */
static void bridge_IoAllocateIrp(void)
{
    uint32_t stack_size = STACK_ARG(0);
    uint32_t bytes = 0x100u + (stack_size > 0x40u ? 0x40u : stack_size) * 0x24u;
    uint32_t irp = xbox_HeapAlloc(bytes, 16);
    if (irp)
        memset(XBOX_TO_NATIVE(irp), 0, bytes);
    g_eax = irp;
}

static void bridge_IoCreateSymbolicLink(void)
{
    g_eax = 0;  /* STATUS_SUCCESS */
}

/* ── IoCreateDevice (ordinal 65, 6 args = 24 bytes) ───────
 * NTSTATUS IoCreateDevice(DriverObject, ExtensionSize, DeviceName,
 *                         DeviceType, Exclusive, DeviceObject)
 * MM3's XPP memory-unit driver calls this in a loop (up to 8 MU devices) and
 * reads [DeviceObject+0x18] as the DeviceExtension pointer, so the object is
 * allocated from the Xbox heap with the extension at offset 0x18.
 */
static void bridge_IoCreateDevice(void)
{
    uint32_t ext_size = STACK_ARG(1);
    uint32_t dev_type = STACK_ARG(3);
    uint32_t dev_out  = STACK_ARG(5);

    uint32_t total = 0x18 + ext_size;
    uint32_t device = xbox_HeapAlloc(total, 16);
    if (!device) {
        if (dev_out) BRIDGE_MEM32(dev_out) = 0;
        g_eax = 0xC000009Au; /* STATUS_INSUFFICIENT_RESOURCES */
        return;
    }
    memset(XBOX_TO_NATIVE(device), 0, total);
    BRIDGE_MEM16(device) = (uint16_t)dev_type;
    BRIDGE_MEM32(device + 4) = total;
    BRIDGE_MEM32(device + 0x18) = device + 0x18; /* DeviceExtension */
    if (dev_out) BRIDGE_MEM32(dev_out) = device;
    g_eax = 0; /* STATUS_SUCCESS */

    if (g_kernel_call_count <= 200) {
        fprintf(stderr, "  [KERNEL] IoCreateDevice: ext=0x%X type=0x%X -> 0x%08X\n",
                ext_size, dev_type, device);
        fflush(stderr);
    }
}

/* ── ObReferenceObjectByHandle (ordinal 246) ─────────────── */
static void bridge_ObReferenceObjectByHandle(void)
{
    /* Xbox: NTSTATUS ObReferenceObjectByHandle(HANDLE Handle, PVOID ObjectType, PVOID* Object)
     * 3 args (not 6 like Windows NT) */
    uint32_t handle = STACK_ARG(0);
    uint32_t obj_type = STACK_ARG(1);
    uint32_t object_ptr = STACK_ARG(2);
    uint32_t object = 0;
    /* The fake system-thread handle written by bridge_PsCreateSystemThreadEx.
     * Workers run synchronously and have already completed by the time the
     * game waits, so hand out a guest-resident "ready" thread object:
     * [+4] != 0 (signaled) and [+0x120] != 0x103 (wait result), which is
     * exactly what sub_00083989 / sub_001E7CF6 poll for. */
    if (handle == 0xBEEF0001u) {
        static uint32_t s_ready_thread_obj = 0;
        if (!s_ready_thread_obj) {
            s_ready_thread_obj = xbox_HeapAlloc(0x128, 4);
            BRIDGE_MEM8(s_ready_thread_obj + 4) = 1;        /* signaled */
            BRIDGE_MEM32(s_ready_thread_obj + 0x120) = 0;   /* STATUS_SUCCESS */
        }
        object = s_ready_thread_obj;
    }
    if (object_ptr) BRIDGE_MEM32(object_ptr) = object;
    g_eax = 0;  /* STATUS_SUCCESS */
}

/* ── RtlRaiseException (ordinal 302) ─────────────────────
 * VOID RtlRaiseException(PEXCEPTION_RECORD ExceptionRecord)
 *
 * Called by CRT / SEH code to raise structured exceptions.
 * On Xbox this triggers the kernel exception dispatcher.
 * For recompilation, we log and continue (no real SEH dispatch yet).
 */
static void bridge_RtlRaiseException(void)
{
    uint32_t record_ptr = STACK_ARG(0);
    uint32_t code = record_ptr ? BRIDGE_MEM32(record_ptr) : 0;

    static int raise_count = 0;
    raise_count++;
    if (raise_count <= 10) {
        fprintf(stderr, "  [KERNEL] RtlRaiseException: record=0x%08X code=0x%08X (#%d)\n",
                record_ptr, code, raise_count);
        fflush(stderr);
    }

    /* Handle float exceptions by clearing the FPU status.
     *
     * On the real Xbox, RtlRaiseException dispatches through the SEH chain.
     * For float exceptions (0xC0000090-0xC0000096), the CRT exception handler
     * clears the x87/SSE status word and continues execution. Without clearing,
     * the caller re-checks the FPU status, sees the exception still pending,
     * and re-raises in an infinite loop.
     *
     * _clearfp() clears both x87 and SSE exception flags on Windows x64.
     */
    if (code >= 0xC0000090u && code <= 0xC0000096u) {
        _clearfp();
    }

    g_eax = 0;
}

/* -- RtlUnwind (ordinal 312) ---------------------------------
 * VOID RtlUnwind(PVOID TargetFrame, PVOID TargetIp,
 *                PVOID ExceptionRecord, PVOID ReturnValue)
 *
 * MM3 uses this through sub_000BBB60 as a stdcall "pop args and resume at
 * TargetIp" helper. In the generated model, kernel_thunk_dispatch already
 * pops the dummy return address and the four stdcall args, and the caller
 * continues at the label immediately after its RECOMP_ITAIL, which is the
 * original TargetIp. Therefore the guest-visible effect is simply a normal
 * return; do not delegate to host RtlUnwind (it would unwind the native
 * stack, not the simulated Xbox stack). */
static void bridge_RtlUnwind(void)
{
    g_eax = 0;
}

/* ── MmMapIoSpace (ordinal 177) ──────────────────────────
 * PVOID MmMapIoSpace(ULONG_PTR PhysicalAddress, ULONG NumberOfBytes, ULONG Protect)
 *
 * Maps physical I/O memory (GPU registers, etc.) into virtual address space.
 * Allocate from Xbox heap so the returned pointer is a valid Xbox VA.
 */
static void bridge_MmMapIoSpace(void)
{
    uint32_t phys_addr = STACK_ARG(0);
    uint32_t num_bytes = STACK_ARG(1);
    uint32_t protect = STACK_ARG(2);
    uint32_t xbox_va = xbox_HeapAlloc(num_bytes, 4096);

    fprintf(stderr, "  [KERNEL] MmMapIoSpace: phys=0x%08X size=%u → Xbox VA 0x%08X\n",
            phys_addr, num_bytes, xbox_va);
    fflush(stderr);

    g_eax = xbox_va;
}

/* ── MmClaimGpuInstanceMemory (ordinal 168) ─────────────── */
static void bridge_MmClaimGpuInstanceMemory(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t padding_ptr = STACK_ARG(1);
    uint32_t xbox_va = xbox_HeapAlloc(size, 4096);
    if (padding_ptr)
        BRIDGE_MEM32(padding_ptr) = 0;
    g_eax = xbox_va;
}

/* ── MmPersistContiguousMemory (ordinal 178) ─────────────
 * VOID MmPersistContiguousMemory(PVOID BaseAddress, ULONG NumberOfBytes, BOOLEAN Persist)
 *
 * Marks contiguous memory as persistent across reboots (for save data).
 * No-op for recompilation.
 */
static void bridge_MmPersistContiguousMemory(void)
{
    /* No-op stub */
    g_eax = 0;
}

/* ── Generic fallback for simple value-only functions ────── */
/* ── MmLockUnlockBufferPages (ordinal 175) ────────────────
 * NTSTATUS MmLockUnlockBufferPages(PVOID BaseAddress, ULONG NumberOfBytes,
 *                                  BOOLEAN Lock)
 * Locks/unlocks DMA-visible pages. No real DMA in recompilation; accept and
 * report success so the XPP USB init path can proceed.
 */
static void bridge_MmLockUnlockBufferPages(void)
{
    g_eax = 0; /* STATUS_SUCCESS */
}
static void bridge_generic_stub(void)
{
    /* Success-returning stub for functions whose callers only check for 0.
     * Deliberately silent: the caller (kernel_thunk_dispatch) warns for
     * ordinals with no bridge at all, which is the case worth hearing about. */
    g_eax = 0;
}

/* ── Dispatch table: ordinal → bridge function + stack arg bytes ── */

typedef void (*bridge_func_t)(void);

/**
 * stdcall arg byte count for each kernel ordinal.
 * On x86 stdcall, the callee cleans (ret N). Our bridges must do the same
 * via g_esp += N after execution so the simulated stack stays balanced.
 *
 * Special cases:
 *   - KfRaiseIrql/KfLowerIrql: fastcall (arg in ecx), 0 stack bytes
 *   - KeSetTimer: DueTime is LARGE_INTEGER (8 bytes on stack) + Timer + Dpc
 */
static int stdcall_args_for_ordinal(ULONG ordinal)
{
    switch (ordinal) {
    /* ── Display / AV ── */
    case   1: return  0;  /* AvGetSavedDataAddress(void) */
    case   2: return 16;  /* AvSendTVEncoderOption(4) */
    case   3: return 24;  /* AvSetDisplayMode(6) */
    case   4: return  4;  /* AvSetSavedDataAddress(1) */

    /* ── Unknown stubs ── */
    case   8: return  0;  /* Unknown_8(void) */
    case  23: return  0;  /* Unknown_23(void) */
    case  42: return  0;  /* Unknown_42(void) */

    /* ── Pool Allocator ── */
    case  15: return  8;  /* ExAllocatePool(2) */
    case  16: return  8;  /* ExAllocatePoolWithTag(2) */
    /* case  17: DATA export - ExEventObjectType */
    case  24: return  4;  /* ExQueryPoolBlockSize(1) */

    /* ── HAL ── */
    case  40: return  4;  /* HalClearSoftwareInterrupt(1) */
    case  41: return  8;  /* HalDisableSystemInterrupt(2) */
    case  44: return  8;  /* HalGetInterruptVector(2) */
    case  46: return 12;  /* MM3 ISR IAT alias for KeInsertQueueDpc(3) */
    case  47: return 24;  /* HalReadWritePCISpace(6) */
    case  49: return  4;  /* HalRequestSoftwareInterrupt(1) */
    case 358: return  0;  /* HalIsResetOrShutdownPending(void) */

    /* ── I/O Manager ── */
    case  59: return  8;  /* IoAllocateIrp(2) */
    case  62: return 36;  /* IoBuildDeviceIoControlRequest(9) */
    case  65: return 24;  /* IoCreateDevice(6) */
    /* case  65: DATA export - IoCompletionObjectType */
    case  67: return 40;  /* IoCreateFile(10) */
    case  69: return  4;  /* IoDeleteDevice(1) */
    /* case  71: DATA export - IoDeviceObjectType */
    case  74: return 12;  /* IoInitializeIrp(3) */
    case  81: return 20;  /* IoSetIoCompletion(5) */
    case  83: return  8;  /* IoStartNextPacket(2) */
    case  84: return 12;  /* IoStartNextPacketByKey(3) */
    case  85: return 16;  /* IoStartPacket(4) */
    case  86: return 32;  /* IoSynchronousDeviceIoControlRequest(8) */
    case  87: return 20;  /* IoSynchronousFsdRequest(5) */
    case 359: return  4;  /* IoMarkIrpMustComplete(1) */

    /* ── Kernel Synchronization ── */
    case  95: return  4;  /* KeBugCheck(1) - stdcall, never returns on real Xbox */
    case  96: return 20;  /* KeBugCheckEx(5) - stdcall, never returns on real Xbox */
    case  97: return  4;  /* KeCancelTimer(1) */
    case  98: return  4;  /* MM3/XDK5233 calls slot-102 ordinal 98 as KeConnectInterrupt(1) */
    case  99: return 12;  /* KeDelayExecutionThread(3) - arg-bytes must match the real ordinal */
    case 100: return  4;  /* KeConnectInterrupt(1) */
    case 107: return 12;  /* KeInitializeDpc(3) */
    case 109: return 28;  /* KeInitializeInterrupt(7) */
    case 113: return  8;  /* KeInitializeTimerEx(2) */
    case 119: return 12;  /* KeInsertQueueDpc(3) */
    case 124: return  4;  /* KeQueryBasePriorityThread(1) */
    case 126: return  0;  /* KeQueryPerformanceCounter(void) */
    case 127: return  0;  /* KeQueryPerformanceFrequency(void) */
    case 128: return  4;  /* KeQuerySystemTime(1) */
    case 129: return  0;  /* KeRaiseIrqlToDpcLevel(void) */
    case 137: return  4;  /* KeRemoveQueueDpc(1) */
    case 139: return  4;  /* KeRestoreFloatingPointState(1) */
    case 142: return  4;  /* KeSaveFloatingPointState(1) */
    case 143: return  8;  /* KeSetBasePriorityThread(2) */
    case 145: return 12;  /* KeSetEvent(3) */
    case 149: return 16;  /* KeSetTimer(Timer+DueTime[8]+Dpc) */
    case 150: return 20;  /* KeSetTimerEx(Timer+DueTime[8]+Period+Dpc) */
    case 151: return  4;  /* KeStallExecutionProcessor(1) */
    case 153: return 12;  /* KeSynchronizeExecution(3) */
    /* case 156: DATA export - KeTickCount */
    case 158: return 32;  /* KeWaitForMultipleObjects(8) */
    case 159: return 20;  /* KeWaitForSingleObject(5) */
    case 160: return  0;  /* KfRaiseIrql (fastcall: arg in ecx) */
    case 161: return  0;  /* KfLowerIrql (fastcall: arg in ecx) */

    /* ── Launch Data ── */
    /* case 164: DATA export - LaunchDataPage */

    /* ── Memory Management ── */
    case 165: return  4;  /* MmAllocateContiguousMemory(1) */
    case 166: return 20;  /* MmAllocateContiguousMemoryEx(5) */
    case 167: return  4;  /* MmAllocateSystemMemory(1) */
    case 168: return  8;  /* MmClaimGpuInstanceMemory(2) */
    case 169: return  8;  /* MmCreateKernelStack(2) */
    case 170: return  8;  /* MmDeleteKernelStack(2) */
    case 171: return  4;  /* MmFreeContiguousMemory(1) */
    case 172: return  4;  /* MmFreeSystemMemory(1) */
    case 173: return  4;  /* MmGetPhysicalAddress(1) */
    case 175: return 12;  /* MmLockUnlockBufferPages(3) */
    case 176: return  8;  /* MmLockUnlockPhysicalPage(2) */
    case 177: return 12;  /* MmMapIoSpace(3) */
    case 178: return 12;  /* MmPersistContiguousMemory(3) */
    case 179: return  4;  /* MmQueryAddressProtect(1) */
    case 180: return  4;  /* MmQueryAllocationSize(1) */
    case 181: return  4;  /* MmQueryStatistics(1) */
    case 182: return 12;  /* MmSetAddressProtect(3) */

    /* ── NT Virtual Memory ── */
    case 184: return 20;  /* NtAllocateVirtualMemory(5) */

    /* ── NT File I/O & Handle ── */
    case 187: return  4;  /* NtClose(1) */
    case 189: return 16;  /* NtCreateEvent(4) */
    case 190: return 36;  /* NtCreateFile(9) */
    case 193: return 16;  /* NtCreateSemaphore(4) */
    case 195: return  4;  /* NtDeleteFile(1) */
    case 196: return 40;  /* NtDeviceIoControlFile(10) */
    case 197: return 12;  /* NtDuplicateObject(3) */
    case 198: return  8;  /* NtFlushBuffersFile(2) */
    case 199: return 12;  /* NtFreeVirtualMemory(3) */
    case 200: return 40;  /* NtFsControlFile(10) */
    case 202: return 24;  /* NtOpenFile(6) */
    case 203: return  8;  /* NtOpenSymbolicLinkObject(2) */
    case 207: return 36;  /* NtQueryDirectoryFile(9) */
    case 210: return  8;  /* NtQueryFullAttributesFile(2) */
    case 211: return 20;  /* NtQueryInformationFile(5) */
    case 215: return 12;  /* NtQuerySymbolicLinkObject(3) */
    case 217: return 16;  /* NtQueryVirtualMemory(4) */
    case 218: return 20;  /* NtQueryVolumeInformationFile(5) */
    case 219: return 32;  /* NtReadFile(8) */
    case 222: return 12;  /* NtReleaseSemaphore(3) */
    case 225: return  8;  /* NtSetEvent(2) */
    case 226: return 20;  /* NtSetInformationFile(5) */
    case 228: return  8;  /* NtSetSystemTime(2) */
    case 233: return 20;  /* NtWaitForMultipleObjectsEx(5) */
    case 234: return 12;  /* NtWaitForSingleObject(3) */
    case 236: return 32;  /* NtWriteFile(8) */
    case 238: return  0;  /* NtYieldExecution(void) */

    /* ── Object Manager ── */
    case 246: return 12;  /* ObReferenceObjectByHandle(3) - Xbox: Handle,Type,Object* */
    case 247: return 20;  /* ObReferenceObjectByName(5) */
    case 250: return  0;  /* ObfDereferenceObject (fastcall: arg in ecx) */

    /* ── Network / PHY ── */
    case 252: return  4;  /* PhyGetLinkState(1) */
    case 253: return  8;  /* PhyInitialize(2) */

    /* ── Threading ── */
    case 255: return 40;  /* PsCreateSystemThreadEx(10) */
    case 256: return 12;  /* KeDelayExecutionThread(3) */
    case 258: return  4;  /* PsTerminateSystemThread(1) */
    /* case 259: DATA export - PsThreadObjectType */

    /* ── Runtime Library ── */
    case 260: return 12;  /* RtlAnsiStringToUnicodeString(3) */
    case 269: return 12;  /* RtlCompareMemoryUlong(3) */
    case 277: return  4;  /* RtlEnterCriticalSection(1) */
    case 279: return 12;  /* RtlEqualString(3) */
    case 289: return  8;  /* RtlInitAnsiString(2) */
    case 291: return  4;  /* RtlInitializeCriticalSection(1) */
    case 294: return  4;  /* RtlLeaveCriticalSection(1) */
    case 301: return  4;  /* RtlNtStatusToDosError(1) */
    case 302: return  4;  /* RtlRaiseException(1) */
    case 304: return  8;  /* RtlTimeFieldsToTime(2) */
    case 305: return  8;  /* RtlTimeToTimeFields(2) */
    case 308: return 12;  /* RtlUnicodeStringToAnsiString(3) */
    case 312: return 16;  /* RtlUnwind(4) */
    case 354: return 12;  /* RtlRip(3) */

    /* ── Xbox Identity (data exports) ── */
    /* cases 322-328, 355-357: DATA exports */

    /* ── Port I/O ── */
    case 335: return 12;  /* WRITE_PORT_BUFFER_USHORT(3) */
    case 336: return 12;  /* WRITE_PORT_BUFFER_ULONG(3) */

    /* ── Crypto ── */
    case 337: return  4;  /* XcSHAInit(1) */
    case 338: return 12;  /* XcSHAUpdate(3) */
    case 339: return  8;  /* XcSHAFinal(2) */
    case 340: return 12;  /* XcRC4Key(3) */
    case 344: return 12;  /* XcPKDecPrivate(3) */
    case 345: return  4;  /* XcPKGetKeyLen(1) */
    case 346: return 12;  /* XcVerifyPKCS1Signature(3) */
    case 347: return 20;  /* XcModExp(5) */
    case 349: return 12;  /* XcKeyTable(3) */
    case 353: return  8;  /* XcUpdateCrypto(2) */

    default:  return  0;  /* DATA exports or truly unknown */
    }
}

static bridge_func_t bridge_for_ordinal(ULONG ordinal)
{
    switch (ordinal) {
    /* Threading */
    case 255: return bridge_PsCreateSystemThreadEx;
    case 258: return bridge_PsTerminateSystemThread;
    case  99: return bridge_KeDelayExecutionThread;
    case 256: return bridge_KeDelayExecutionThread;
    case 151: return bridge_KeStallExecutionProcessor;

    /* File/Handle */
    case 187: return bridge_NtClose;
    case 190: return bridge_NtCreateFile;
    case 195: return bridge_NtDeleteFile;
    case 196: return bridge_NtDeviceIoControlFile;
    case 198: return bridge_NtFlushBuffersFile;
    case 200: return bridge_NtFsControlFile;
    case 202: return bridge_NtOpenFile;
    case 203: return bridge_NtOpenSymbolicLinkObject;
    case 207: return bridge_NtQueryDirectoryFile;
    case 210: return bridge_NtQueryFullAttributesFile;
    case 211: return bridge_NtQueryInformationFile;
    case 218: return bridge_NtQueryVolumeInformationFile;
    case 219: return bridge_NtReadFile;
    case 226: return bridge_NtSetInformationFile;
    case 236: return bridge_NtWriteFile;

    /* Memory - contiguous */
    case 165: return bridge_MmAllocateContiguousMemory;
    case 166: return bridge_MmAllocateContiguousMemoryEx;
    case 167: return bridge_MmAllocateSystemMemory;
    case 171: return bridge_MmFreeContiguousMemory;
    case 173: return bridge_MmGetPhysicalAddress;
    case 182: return bridge_MmSetAddressProtect;
    case 181: return bridge_MmQueryStatistics;
    case 179: return bridge_MmQueryAddressProtect;

    /* Memory - virtual */
    case 184: return bridge_NtAllocateVirtualMemory;
    case 199: return bridge_NtFreeVirtualMemory;
    case 217: return bridge_NtQueryVirtualMemory;

    /* Pool */
    case  15: return bridge_ExAllocatePool;
    case  16: return bridge_ExAllocatePoolWithTag;
    case  24: return bridge_ExQueryPoolBlockSize;

    /* IRQL */
    case 160: return bridge_KfRaiseIrql;
    case 161: return bridge_KfLowerIrql;
    case 129: return bridge_KeRaiseIrqlToDpcLevel;

    /* Critical sections */
    case 291: return bridge_RtlInitializeCriticalSection;
    case 277: return bridge_RtlEnterCriticalSection;
    case 294: return bridge_RtlLeaveCriticalSection;

    /* Timing */
    case 126: return bridge_KeQueryPerformanceCounter;
    case 127: return bridge_KeQueryPerformanceFrequency;
    case 128: return bridge_KeQuerySystemTime;
    case 149: return bridge_KeSetTimer;
    case 150: return bridge_KeSetTimer;  /* KeSetTimerEx */

    /* DPC / Timer init */
    case 107: return bridge_KeInitializeDpc;
    case 109: return bridge_KeInitializeInterrupt;
    case 119: return bridge_KeInsertQueueDpc;
    /* This XBE's IAT maps the ISR's 0x3620AC entry to ordinal 46. */
    case 46: return bridge_KeInsertQueueDpc;
    case 113: return bridge_KeInitializeTimerEx;
    case  98: return bridge_KeConnectInterrupt;
    case 100: return bridge_KeDisconnectInterrupt;

    /* Synchronization */
    case  95: return bridge_KeBugCheck;
    case  96: return bridge_KeBugCheckEx;
    case 189: return bridge_NtCreateEvent;
    case 145: return bridge_KeSetEvent;
    case 159: return bridge_KeWaitForSingleObject;
    case 238: return bridge_NtYieldExecution;

    /* Hardware */
    case  47: return bridge_HalReadSMCTrayState;
    case  49: return bridge_HalRequestSoftwareInterrupt;
    case  44: return bridge_HalGetInterruptVector;

    /* Display */
    case   1: return bridge_AvGetSavedDataAddress;
    case   2: return bridge_AvSendTVEncoderOption;
    case   3: return bridge_AvSetDisplayMode;
    case   4: return bridge_AvSetSavedDataAddress;

    /* I/O */
    case  59: return bridge_IoAllocateIrp;
    case  63: return bridge_IoCreateSymbolicLink;
    case  65: return bridge_IoCreateDevice;
    case  67: return bridge_IoCreateFile;
    case 188: return bridge_NtCreateDirectoryObject;
    case 246: return bridge_ObReferenceObjectByHandle;

    case 175: return bridge_MmLockUnlockBufferPages;
    case 168: return bridge_MmClaimGpuInstanceMemory;
    /* Memory - I/O mapping */
    case 177: return bridge_MmMapIoSpace;
    case 178: return bridge_MmPersistContiguousMemory;

    /* RTL */
    case 289: return bridge_RtlInitAnsiString;
    case 301: return bridge_RtlNtStatusToDosError;
    case 302: return bridge_RtlRaiseException;
    case 312: return bridge_RtlUnwind;

    default:  return NULL;
    }
}

/* ── Per-slot bridge functions (resolved at init) ────────── */

static bridge_func_t g_slot_bridges[XBOX_KERNEL_THUNK_TABLE_SIZE];
static int g_slot_arg_bytes[XBOX_KERNEL_THUNK_TABLE_SIZE];

/* Current dispatching slot */
static int g_kernel_dispatch_slot = -1;
static XBOX_THREAD_LOCAL int g_guest_work_depth;

void xbox_kernel_pump_guest_work(void)
{
    worker_resume_if_due();
    static int trace_frontier = -1;
    if (trace_frontier < 0) trace_frontier = getenv("MM3_TRACE_PUMP") ? 1 : 0;
    static int trace_irq_dpc = -1;
    if (trace_irq_dpc < 0) trace_irq_dpc = getenv("MM3_TRACE_IRQ_DPC") ? 1 : 0;
    int trace_here = trace_frontier && g_icall_count >= 11800ULL && g_icall_count <= 12200ULL;
    if (trace_here) fprintf(stderr, "[PUMP] enter ic=%llu depth=%d esp=%08X\n",
        (unsigned long long)g_icall_count, g_guest_work_depth, g_esp);
    if (g_guest_work_depth++) {
        g_guest_work_depth--;
        if (trace_here) fprintf(stderr, "[PUMP] nested-return ic=%llu\n",
            (unsigned long long)g_icall_count);
        return;
    }
    if (trace_here) fprintf(stderr, "[PUMP] before-irq-take ic=%llu\n",
        (unsigned long long)g_icall_count);
    PXBOX_KINTERRUPT interrupt = xbox_kernel_take_interrupt();
    if (trace_here) fprintf(stderr, "[PUMP] after-irq-take ic=%llu irq=%p\n",
        (unsigned long long)g_icall_count, (void *)interrupt);
    if (interrupt && interrupt->ServiceRoutine) {
        uint32_t saved_eax = g_eax, saved_ecx = g_ecx, saved_edx = g_edx;
        uint32_t saved_ebx = g_ebx, saved_esi = g_esi, saved_edi = g_edi;
        uint32_t saved_esp = g_esp, saved_seh = g_seh_ebp;
        uint32_t routine_va = (uint32_t)(uintptr_t)interrupt->ServiceRoutine;
        recomp_func_t isr = recomp_lookup(routine_va);
        if (!isr) isr = recomp_lookup_manual(routine_va);
        if (trace_irq_dpc)
            fprintf(stderr, "[IRQ] accept routine=%08X context=%p esp=%08X\n",
                    routine_va, interrupt->ServiceContext, saved_esp);
        if (isr) {
            if (trace_here) fprintf(stderr, "[PUMP] isr-enter ic=%llu routine=%08X\n",
                (unsigned long long)g_icall_count, routine_va);
            /* Generated ISR expects ESP to point at the dummy return slot,
             * followed by (Interrupt, ServiceContext). */
            /* Reserve dummy return + two 32-bit arguments below the
             * interrupted stack. Writing arguments above saved_esp would
             * overwrite the interrupted guest frame. */
            g_esp = saved_esp - 12;
            BRIDGE_MEM32(g_esp) = 0;
            BRIDGE_MEM32(g_esp + 4) = (uint32_t)(uintptr_t)interrupt;
            BRIDGE_MEM32(g_esp + 8) = (uint32_t)(uintptr_t)interrupt->ServiceContext;
            isr();
            if (trace_here) fprintf(stderr, "[PUMP] isr-exit ic=%llu routine=%08X\n",
                (unsigned long long)g_icall_count, routine_va);
            g_esp = saved_esp;
            if (trace_irq_dpc)
                fprintf(stderr, "[IRQ] return routine=%08X esp=%08X\n", routine_va, g_esp);
        } else {
            if (trace_irq_dpc)
                fprintf(stderr, "[IRQ] unresolved routine=%08X\n", routine_va);
        }
        g_eax = saved_eax; g_ecx = saved_ecx; g_edx = saved_edx;
        g_ebx = saved_ebx; g_esi = saved_esi; g_edi = saved_edi;
        g_esp = saved_esp; g_seh_ebp = saved_seh;
    }

    PXBOX_KDPC dpc;
    PVOID arg1, arg2;
    if (trace_here) fprintf(stderr, "[PUMP] before-dpc-loop ic=%llu\n",
        (unsigned long long)g_icall_count);
    while (xbox_kernel_take_guest_dpc(&dpc, &arg1, &arg2)) {
        if (trace_here) fprintf(stderr, "[PUMP] dpc-take ic=%llu dpc=%p\n",
            (unsigned long long)g_icall_count, (void *)dpc);
        if (!dpc) continue;
        uint32_t dpc_va = (uint32_t)((uintptr_t)dpc - (uintptr_t)g_xbox_mem_offset);
        uint32_t routine_va = BRIDGE_MEM32(dpc_va + 12);
        recomp_func_t fn = recomp_lookup(routine_va);
        if (!fn) fn = recomp_lookup_manual(routine_va);
        if (trace_irq_dpc)
            fprintf(stderr, "[DPC] dispatch routine=%08X esp=%08X\n",
                    routine_va, g_esp);
        if (fn) {
            uint32_t saved_eax = g_eax, saved_ecx = g_ecx, saved_edx = g_edx;
            uint32_t saved_ebx = g_ebx, saved_esi = g_esi, saved_edi = g_edi;
            uint32_t saved_seh = g_seh_ebp;
            uint32_t saved_esp = g_esp;
            uint32_t context_va = BRIDGE_MEM32(dpc_va + 16);
            int trace_dpc_state = 0;
            const char *trace_dpc_env = getenv("MM3_TRACE_DPC_STATE");
            if (trace_dpc_env && routine_va == 0x00348120u)
                trace_dpc_state = 1;
            if (trace_dpc_state) {
                uint32_t dpc_obj = BRIDGE_MEM32(context_va);
                fprintf(stderr, "[DPC-STATE] pre routine=%08X dpc_va=%08X ctx=%08X "
                    "obj=%08X obj100=%08X obj820=%08X obj824=%08X "
                    "obj600100=%08X ic=%llu\n", routine_va, dpc_va, context_va,
                    dpc_obj, BRIDGE_MEM32(dpc_obj + 0x100u),
                    BRIDGE_MEM32(dpc_obj + 0x820u),
                    BRIDGE_MEM32(dpc_obj + 0x824u),
                    BRIDGE_MEM32(dpc_obj + 0x600100u),
                    (unsigned long long)g_icall_count);
            }
            /* Reserve dummy return + four DPC arguments below the saved
             * guest stack; never write the synthetic frame over its caller. */
            g_esp = saved_esp - 20;
            BRIDGE_MEM32(g_esp) = 0;
            BRIDGE_MEM32(g_esp + 4) = dpc_va;
            BRIDGE_MEM32(g_esp + 8) = context_va;
            BRIDGE_MEM32(g_esp + 12) = (uint32_t)(uintptr_t)arg1;
            BRIDGE_MEM32(g_esp + 16) = (uint32_t)(uintptr_t)arg2;
            fn();
            if (trace_dpc_state) {
                uint32_t dpc_obj = BRIDGE_MEM32(context_va);
                fprintf(stderr, "[DPC-STATE] post routine=%08X obj=%08X "
                    "obj100=%08X obj820=%08X obj824=%08X obj600100=%08X "
                    "ic=%llu\n", routine_va, dpc_obj,
                    BRIDGE_MEM32(dpc_obj + 0x100u),
                    BRIDGE_MEM32(dpc_obj + 0x820u),
                    BRIDGE_MEM32(dpc_obj + 0x824u),
                    BRIDGE_MEM32(dpc_obj + 0x600100u),
                    (unsigned long long)g_icall_count);
            }
            if (trace_here) fprintf(stderr, "[PUMP] dpc-exit ic=%llu routine=%08X\n",
                (unsigned long long)g_icall_count, routine_va);
            g_eax = saved_eax; g_ecx = saved_ecx; g_edx = saved_edx;
            g_ebx = saved_ebx; g_esi = saved_esi; g_edi = saved_edi;
            g_seh_ebp = saved_seh;
            g_esp = saved_esp;
        } else {
            if (trace_irq_dpc)
                fprintf(stderr, "[DPC] unresolved routine=%08X\n", routine_va);
        }
    }
    if (trace_here) fprintf(stderr, "[PUMP] exit ic=%llu\n",
        (unsigned long long)g_icall_count);
    g_guest_work_depth--;
}

static void kernel_thunk_dispatch(void)
{
    int slot = g_kernel_dispatch_slot;
    bridge_func_t bridge;
    ULONG ordinal;
    uint32_t esp_before = g_esp;

    if (slot < 0 || slot >= XBOX_KERNEL_THUNK_TABLE_SIZE) {
        fprintf(stderr, "  [KERNEL] bad slot %d\n", slot);
        g_eax = 0;
        g_esp += 4;  /* pop dummy return address */
        return;
    }

    ordinal = g_slot_ordinals[slot];
    bridge = g_slot_bridges[slot];


    if (getenv("MM3_TRACE_KERNEL_WINDOW") &&
        ((g_icall_count >= 12055ULL && g_icall_count <= 12062ULL) ||
         (g_icall_count >= 12080ULL && g_icall_count <= 12092ULL) ||
         (g_icall_count >= 325900ULL && g_icall_count <= 326120ULL))) {
        fprintf(stderr, "[KERNEL-WINDOW] ic=%llu slot=%d ordinal=%u bridge=%p "
            "esp=%08X a0=%08X a1=%08X a2=%08X a3=%08X "
            "penter=%p caller=%p\n",
            (unsigned long long)g_icall_count, slot, ordinal, (void *)bridge,
            g_esp, STACK_ARG(0), STACK_ARG(1), STACK_ARG(2), STACK_ARG(3),
            (void *)g_penter_last_rva, (void *)g_penter_caller_rva);
        fflush(stderr);
    }

    if (slot == 75 || slot == 94 || ordinal == 95 || ordinal == 226 ||
        ordinal == 302 || ordinal == 312 || ordinal == 354) {
        uintptr_t ra = (uintptr_t)_ReturnAddress();
        /* The log runs before kernel_thunk_dispatch pops the dummy return
         * address, so the real guest argument vector starts at g_esp+4. */
        fprintf(stderr, "[KERNEL-SPECIAL] ordinal=%u slot=%d ic=%llu guest_esp=%08X "
            "dispatch_caller_rva=%zX penter_rva=%zX eax=%08X ecx=%08X edx=%08X "
            "a0=%08X a1=%08X a2=%08X a3=%08X a4=%08X\n",
            ordinal, slot,
            (unsigned long long)g_icall_count, g_esp,
            (size_t)(ra - (uintptr_t)GetModuleHandleW(NULL)),
            (size_t)g_penter_last_rva, g_eax, g_ecx, g_edx,
            STACK_ARG(1), STACK_ARG(2), STACK_ARG(3), STACK_ARG(4), STACK_ARG(5));
        {
            void *frames[6];
            USHORT n = CaptureStackBackTrace(0, 6, frames, NULL);
            HMODULE mod = GetModuleHandleW(NULL);
            for (USHORT i = 0; i < n; ++i)
                fprintf(stderr, "[KERNEL-SPECIAL-STACK] %u rva=%zX\n", i,
                    (size_t)((uintptr_t)frames[i] - (uintptr_t)mod));
        }
        fflush(stderr);
    }

    if (getenv("MM3_TRACE_LOCK_STACK") && (ordinal == 277 || ordinal == 294)) {
        static int s_lock_stack_n;
        if (g_kernel_call_count > 500000 && s_lock_stack_n++ < 24) {
            uintptr_t ra = (uintptr_t)_ReturnAddress();
            fprintf(stderr, "[LOCK-STACK] #%u ordinal=%u ic=%llu ra=%zX esp=%08X eax=%08X ecx=%08X edx=%08X\n",
                    s_lock_stack_n, ordinal, (unsigned long long)g_icall_count,
                    (size_t)(ra - (uintptr_t)GetModuleHandleW(NULL)), g_esp, g_eax, g_ecx, g_edx);
            void *frames[12];
            USHORT n = CaptureStackBackTrace(0, 12, frames, NULL);
            HMODULE mod = GetModuleHandleW(NULL);
            for (USHORT i = 0; i < n; ++i)
                fprintf(stderr, "[LOCK-STACK-FRAME] %u rva=%zX\n", i,
                    (size_t)((uintptr_t)frames[i] - (uintptr_t)mod));
            fflush(stderr);
        }
    }


    if (getenv("MM3_TRACE_LOCK_CALLER") && (ordinal == 277 || ordinal == 294)) {
        static int s_lock_caller_n;
        int late = g_kernel_call_count > 500000;
        if ((!late && s_lock_caller_n < 12) || (late && s_lock_caller_n < 36)) {
            s_lock_caller_n++;
            fprintf(stderr, "[LOCK-CALLER] #%d ordinal=%u ic=%llu caller=%zX fn=%s "
                    "esp=%08X eax=%08X ecx=%08X edx=%08X\n",
                    s_lock_caller_n, ordinal,
                    (unsigned long long)g_icall_count,
                    (size_t)g_penter_caller_rva,
                    recomp_probe_fn_name(g_penter_caller_rva),
                    g_esp, g_eax, g_ecx, g_edx);
            fflush(stderr);
        }
    }


    /* Guest thread owns register globals here; host producers only queue. */
    xbox_kernel_pump_guest_work();

    g_kernel_call_count++;

    if (g_kernel_call_count <= 500) {
        fprintf(stderr, "  [KERNEL] #%d: ordinal %u (slot %d) ab=%d esp=0x%08X\n",
                g_kernel_call_count, ordinal, slot, g_slot_arg_bytes[slot], g_esp);
        fflush(stderr);
    }

    /* Diagnostic: dump the 0x3929D0/0x3929EC lock-list state once when the
     * EnterCS/LeaveCS recursion signature appears (esp deep in the thread
     * stack while EnterCS is called repeatedly). NON-M4. */
    if (ordinal == 277 && g_esp < 0x0277F000u && g_esp > 0x02700000u) {
        static int s_lock_dump = 0;
        if (s_lock_dump < 3) {
            fprintf(stderr, "[LOCKDUMP] #%d esp=0x%08X seh_ebp=0x%08X\n",
                g_kernel_call_count, g_esp, g_seh_ebp);
            {
                extern ptrdiff_t g_xbox_mem_offset;
                uint32_t *m = (uint32_t *)((uintptr_t)g_xbox_mem_offset);
                fprintf(stderr, "[LOCKDUMP]   MEM32(0x46A154)=0x%08X MEM32(0x3C01C8)=0x%08X\n",
                    *(volatile uint32_t*)((uintptr_t)0x46A154u + g_xbox_mem_offset),
                    *(volatile uint32_t*)((uintptr_t)0x3C01C8u + g_xbox_mem_offset));
                fprintf(stderr, "[LOCKDUMP]   IAT: 0x361FA8=0x%08X 0x361FB0=0x%08X 0x361FB4=0x%08X\n",
                    *(volatile uint32_t*)((uintptr_t)0x361FA8u + g_xbox_mem_offset),
                    *(volatile uint32_t*)((uintptr_t)0x361FB0u + g_xbox_mem_offset),
                    *(volatile uint32_t*)((uintptr_t)0x361FB4u + g_xbox_mem_offset));
                fprintf(stderr, "[LOCKDUMP]   slot44 ordinal=%u slot20 ordinal=%u\n",
                    g_slot_ordinals[44], g_slot_ordinals[20]);
                uint32_t base10118 = *(volatile uint32_t*)((uintptr_t)0x10118u + g_xbox_mem_offset);
                fprintf(stderr, "[LOCKDUMP]   MEM32(0x10118)=0x%08X\n", base10118);
                if (base10118 && base10118 < 0x01000000u) {
                    for (int i = 0; i < 6; i++) {
                        uint32_t v = *(volatile uint32_t*)((uintptr_t)(base10118 + i*4) + g_xbox_mem_offset);
                        fprintf(stderr, "[LOCKDUMP]     [0x%08X+%d]=0x%08X\n", base10118, i*4, v);
                    }
                }
            }
            /* Native caller chain: who is calling EnterCS recursively? */
            void *frames[20];
            USHORT n = CaptureStackBackTrace(0, 20, frames, NULL);
            HMODULE mod = GetModuleHandle(NULL);
            for (USHORT i = 0; i < n; i++) {
                fprintf(stderr, "[LOCKDUMP]   [%u] module+0x%zX\n", i,
                    (uintptr_t)frames[i] - (uintptr_t)mod);
            }
            s_lock_dump++;
            fflush(stderr);
        }
    }
        DWORD now = GetTickCount();
        {
        static DWORD last_summary_tick = 0;
        if (last_summary_tick == 0) last_summary_tick = now;
        if (now - last_summary_tick >= 2000 && g_kernel_call_count > 200) {
            fprintf(stderr, "  [KERNEL] summary: %d total calls, latest ordinal %u (slot %d) esp=0x%08X ra=%zX\n",
                    g_kernel_call_count, ordinal, slot, g_esp,
                    (size_t)((uintptr_t)_ReturnAddress() - (uintptr_t)GetModuleHandleW(NULL)));
            fflush(stderr);
            last_summary_tick = now;
        }
    }

    /* Pop the dummy return address that PUSH32(esp, 0) pushed before RECOMP_ICALL.
     * On real x86, "call [thunk]" pushes a real return address and "ret" pops it.
     * In our model, the bridge is called directly (not via the simulated stack),
     * so we must manually consume the dummy return address. */
    g_esp += 4;

    /* Resume a parked worker whose delay has elapsed before servicing this
     * thunk: the pump calls kernel thunks regularly, which is what paces the
     * render worker on the same host thread. */
    worker_resume_if_due();

    if (bridge) {
        uint32_t dev_before = 0;
        if (getenv("MM3_TRACE_KERNEL_WINDOW"))
            dev_before = BRIDGE_MEM32(0x00351F48u);
        bridge();
        if (getenv("MM3_TRACE_KERNEL_WINDOW")) {
            uint32_t dev_after = BRIDGE_MEM32(0x00351F48u);
            if (dev_before != dev_after || ordinal == 100 || ordinal == 166 ||
                (g_icall_count >= 325900ULL && g_icall_count <= 326120ULL))
                fprintf(stderr, "[KERNEL-WINDOW-RET] ic=%llu ordinal=%u "
                    "dev=%08X->%08X eax=%08X esp=%08X\n",
                    (unsigned long long)g_icall_count, ordinal, dev_before,
                    dev_after, g_eax, g_esp);
        }
    } else {
        /* No specific bridge - return 0. Warn once per ordinal rather than
         * gating on g_kernel_call_count: a missing bridge is rare and is
         * usually the reason a game misbehaves, so it must not be swallowed
         * by the general call-trace throttle. Bounded to one line per slot. */
        static uint8_t warned[XBOX_KERNEL_THUNK_TABLE_SIZE];
        if (!warned[slot]) {
            warned[slot] = 1;
            fprintf(stderr, "  [KERNEL] WARNING: no bridge for ordinal %u (slot %d), returning 0\n",
                    ordinal, slot);
            fflush(stderr);
        }
        g_eax = 0;
    }

    /* Clean stdcall args from the simulated stack.
     * On real x86, stdcall callee does "ret N" to pop the return address
     * and N bytes of arguments. We already popped the dummy return address
     * above; now pop the args. */
    g_esp += g_slot_arg_bytes[slot];

    /* Detect ESP corruption: after the thunk, ESP should be near esp_before
     * (the dummy return + args were popped). Large deviations indicate a bug. */
    if (g_esp < 0x00770000 || g_esp > 0x03000000) {
        fprintf(stderr, "  [KERNEL] ESP CORRUPTION after ordinal %u (slot %d): "
            "before=0x%08X after=0x%08X delta=%d\n",
            ordinal, slot, esp_before, g_esp, (int)(g_esp - esp_before));
        fflush(stderr);
    }

    if (g_kernel_call_count <= 200) {
        fprintf(stderr, "  [KERNEL] → returned 0x%08X\n", g_eax);
        fflush(stderr);
    }

    /* ESP-guard: catch corruption immediately */
    if (g_esp < 0x00770000 || g_esp > 0x02780FFF) {
        fprintf(stderr, "  [FATAL] ESP corrupt after kernel call #%d: esp=0x%08X\n",
            g_kernel_call_count, g_esp);
        fflush(stderr);
        __debugbreak();
        ExitProcess(1);
    }
}

/* ── Dispatch lookup ────────────────────────────────────── */

/**
 * Look up a kernel thunk by synthetic VA.
 * Called as a fallback when recomp_lookup() returns NULL.
 */
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va)
{
    if (xbox_va >= KERNEL_VA_BASE && xbox_va < KERNEL_VA_END) {
        int slot = (xbox_va - KERNEL_VA_BASE) / 4;
        if (slot >= 0 && slot < XBOX_KERNEL_THUNK_TABLE_SIZE) {
            g_kernel_dispatch_slot = slot;
            return kernel_thunk_dispatch;
        }
    }
    return NULL;
}

/* ── Initialization ─────────────────────────────────────── */

/*
 * Where this title's kernel thunk table lives. Defaults to the compile-time
 * constant, but every XBE puts it somewhere different (it comes from the
 * header's KernelImageThunkAddress), so xbox_MemoryLayoutInit() parses the
 * real address out of the binary and overrides it here.
 *
 * Halo build 2276 puts it at 0x00253090 against the default's 0x0036B7C0 --
 * without the override the bridge patches ordinals into whatever happens to
 * live at the wrong address and every kernel call goes somewhere arbitrary.
 */
static uint32_t g_thunk_table_base  = XBOX_KERNEL_THUNK_TABLE_BASE;
static uint32_t g_thunk_table_count = XBOX_KERNEL_THUNK_TABLE_SIZE;

void xbox_kernel_set_thunk_address(uint32_t xbox_va, uint32_t count)
{
    if (!xbox_va) {
        return;
    }

    g_thunk_table_base = xbox_va;

    /* count indexes g_slot_* arrays, which are sized by the macro. A title
     * importing more slots than the real kernel exports would run off them. */
    if (count && count <= XBOX_KERNEL_THUNK_TABLE_SIZE) {
        g_thunk_table_count = count;
    } else if (count > XBOX_KERNEL_THUNK_TABLE_SIZE) {
        fprintf(stderr,
                "  Kernel thunk bridge: XBE declares %u thunk slots, clamping to %d\n",
                count, XBOX_KERNEL_THUNK_TABLE_SIZE);
        g_thunk_table_count = XBOX_KERNEL_THUNK_TABLE_SIZE;
    }
}

/* Override the stdcall arg-byte count for a specific ordinal. The default
 * stdcall_args_for_ordinal table is a best-guess from one XDK build; some
 * titles call a thunk with a different number of arguments than the default
 * (observed: MM3 calls the ordinal-47 and ordinal-67 thunks with 2 args, but
 * the default table says 24/40, so kernel_thunk_dispatch over-popped the
 * simulated stack by 16 bytes and corrupted the caller's callee-saved
 * restore). Call after xbox_kernel_bridge_init(). */
void xbox_kernel_set_ordinal_arg_bytes(ULONG ordinal, int bytes)
{
    int i;
    int n = 0;
    for (i = 0; i < g_thunk_table_count; i++) {
        if (g_slot_ordinals[i] == ordinal) {
            g_slot_arg_bytes[i] = bytes;
            n++;
        }
    }
    if (n) {
        fprintf(stderr, "  Kernel thunk bridge: overrode ordinal %u arg bytes to %d (%d slots)\n",
                ordinal, bytes, n);
    } else {
        fprintf(stderr, "  Kernel thunk bridge: WARNING ordinal %u not found for arg override\n",
                ordinal);
    }
}

/**
 * Resolve the kernel thunk table in Xbox memory.
 *
 * Must be called AFTER xbox_MemoryLayoutInit() so Xbox memory is mapped.
 *
 * Reads the actual ordinals from the XBE memory thunk table (0x80000000|ordinal),
 * resolves each to a per-ordinal bridge function, and replaces the entry
 * with a synthetic VA for dispatch.
 */
void xbox_kernel_bridge_init(void)
{
    int i;
    int resolved = 0;
    int bridged = 0;
    int unbridged = 0;
    DWORD old_protect;

    fprintf(stderr, "  Kernel thunk bridge: resolving %d entries at 0x%08X\n",
            g_thunk_table_count, g_thunk_table_base);

    /* The thunk table lives in .rdata which is marked PAGE_READONLY.
     * Temporarily make it writable so we can patch the ordinals. */
    VirtualProtect(
        (LPVOID)((uintptr_t)g_thunk_table_base + g_xbox_mem_offset),
        g_thunk_table_count * 4,
        PAGE_READWRITE,
        &old_protect
    );

    /* Initialize kernel data export values first */
    kernel_data_init();

    for (i = 0; i < g_thunk_table_count; i++) {
        uint32_t va = g_thunk_table_base + i * 4;
        uint32_t current = BRIDGE_MEM32(va);

        if (current & 0x80000000) {
            /* Read the actual ordinal from Xbox memory */
            ULONG ordinal = current & 0x7FFFFFFF;
            g_slot_ordinals[i] = ordinal;

            /* Check if this is a data export */
            uint32_t data_va = kernel_data_va_for_ordinal(ordinal);
            if (data_va) {
                /* DATA export: point thunk to actual data in mapped memory.
                 * This allows the game to dereference the thunk entry. */
                BRIDGE_MEM32(va) = data_va;
                resolved++;
                bridged++;
                continue;
            }

            /* FUNCTION export: use synthetic VA for dispatch */
            g_slot_bridges[i] = bridge_for_ordinal(ordinal);
            g_slot_arg_bytes[i] = stdcall_args_for_ordinal(ordinal);
            if (i == 105 || i == 106) {
                fprintf(stderr, "  [THUNK-DIAG] slot=%d ordinal=%u bridge=%p args=%d\n",
                        i, ordinal, (void *)g_slot_bridges[i],
                        g_slot_arg_bytes[i]);
            }
            if (g_slot_bridges[i]) {
                bridged++;
            } else {
                unbridged++;
            }

            /* Replace Xbox memory entry with synthetic VA */
            uint32_t synthetic = KERNEL_VA_BASE + i * 4;
            BRIDGE_MEM32(va) = synthetic;
            resolved++;
        }
    }

    /* Restore original protection */
    VirtualProtect(
        (LPVOID)((uintptr_t)g_thunk_table_base + g_xbox_mem_offset),
        g_thunk_table_count * 4,
        old_protect,
        &old_protect
    );

    fprintf(stderr, "  Kernel thunk bridge: %d/%d resolved (%d bridged, %d stub)\n",
            resolved, g_thunk_table_count, bridged, unbridged);
    fprintf(stderr, "  Synthetic VA range: 0x%08X-0x%08X\n",
            KERNEL_VA_BASE, KERNEL_VA_BASE + (resolved - 1) * 4);
}
