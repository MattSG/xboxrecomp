/* Guest memory integration: image-derived sections, caller-configured RAM
 * and backing size, shared thread state and owned allocation/mapping APIs.
 * Device aliases and extended reservations have explicit ownership; unbacked
 * guest addresses do not implicitly alias ordinary RAM. */

#ifndef XBOX_MEMORY_LAYOUT_H
#define XBOX_MEMORY_LAYOUT_H

#include "platform/xbox_winnt.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Xbox memory map constants
 * ================================================================ */

/* Base address of all XBE files in Xbox memory */
#define XBOX_BASE_ADDRESS       0x00010000

/* Guest VA origin; low guard and KPCR storage are configured separately. */
#define XBOX_MAP_START          0x00000000

/* Installed guest RAM budget. Configure before memory initialization. */
#define XBOX_TOTAL_RAM          (64 * 1024 * 1024)  /* 64 MB (default) */
#define XBOX_DEVKIT_RAM         (128 * 1024 * 1024) /* 128 MB (debug kit) */
#define XBOX_GPU_RESERVED       (4 * 1024 * 1024)   /* ~4 MB for GPU */

/* Installed RAM and backed guest address space are separate quantities. */
extern size_t g_xbox_total_ram;

/* Backed guest VA span; zero selects the installed RAM size. Set before init.
 * Pages above reported RAM remain distinct storage. The heap may use this span
 * while coordinating ownership with reservations and loaded image ranges. */
extern size_t g_xbox_map_size;
void xbox_SetMapSize(size_t bytes);

/* Carve a pure address-space reservation from the mapped range above RAM.
 * Returns 0 if the mapping is no larger than RAM, or if it is exhausted.
 * See the implementation for why reservations must not come from the heap. */
uint32_t xbox_ReserveAlloc(uint32_t size, uint32_t align);
/* 1 if va lies in memory xbox_ReserveAlloc has handed out (above RAM). */
int xbox_ReserveRangeAllocated(uint32_t va);

/* Bounds of the guest's executable sections, derived from the XBE at load.
 *
 * recomp_types.h declares these too, for RECOMP_ICALL_IS_CODE. They are
 * repeated here so hand-written host code -- a fault handler wanting to tell a
 * guest return address on the stack from ordinary data, say -- can use them
 * without including the generated-code header, which redefines `eax` and
 * friends as macros.
 */
/* Full extent of the loaded XBE image -- every section, not just the
 * executable ones. Anything writing guest memory on the title's behalf must
 * stay out of this range. */
extern uint32_t g_xbox_image_lo;
extern uint32_t g_xbox_image_hi;

extern uint32_t g_xbox_code_lo;
extern uint32_t g_xbox_code_hi;
void xbox_SetTotalRam(size_t bytes);

/* NOTE: Section addresses (.text, .rdata, .data, etc.) are NOT hardcoded.
 * They are parsed from the XBE header at runtime in xbox_MemoryLayoutInit().
 * This allows the toolkit to work with ANY Xbox game without modification. */

/* ================================================================
 * Memory initialization
 * ================================================================ */

/**
 * Initialize the Xbox memory layout.
 *
 * Reserves the virtual address range 0x00010000 through 0x0076F000
 * and maps the XBE sections to their expected addresses:
 * - .rdata: copied from XBE, read-only
 * - .data: initialized portion copied from XBE, BSS zeroed
 *
 * Note: .text is NOT mapped here - the recompiled code is native
 * Windows code and doesn't need to be at the original address.
 * The data sections DO need to be at their original addresses
 * because the recompiled code references globals by absolute address.
 *
 * @param xbe_data  Pointer to the loaded XBE file contents.
 * @param xbe_size  Size of the XBE file.
 * @return TRUE on success, FALSE on failure.
 */
BOOL xbox_MemoryLayoutInit(const void *xbe_data, size_t xbe_size);

/**
 * Release the reserved Xbox memory layout.
 */
/**
 * Mirror a GPU completion fence the title spins on.
 *
 * The NV2A tables in xbox_memory_layout.c acknowledge handshakes that live at
 * fixed aperture offsets. Some titles instead wait on a semaphore the GPU
 * writes into contiguous memory: D3D seeds it, submits work, then spins until
 * it reaches the submitted count. Wreckless does this at guest 0x000FE920,
 * waiting on the 96-byte MmAllocateContiguousMemoryEx block its device struct
 * points at.
 *
 * Nothing executes the push buffer -- the D3D11 layer draws -- so everything
 * submitted is complete, and advancing the fence is the same honest
 * acknowledgement the register tables make.
 *
 * The fence has no fixed address; it is reached through the title's device
 * struct, so it is registered as the chain of indirections to follow:
 *
 *     device = MEM32(device_ptr_va)
 *     fence  = MEM32(device + get_ptr_off)
 *     MEM32(fence) = MEM32(device + put_off)
 *
 * Every step is bounds-checked each poll, so registering a chain that is not
 * yet initialised (or never becomes valid) is harmless.
 *
 * Returns 0 on success, -1 if the table is full.
 */
/* Advance a frame/swap counter inside the D3D device at ~60 Hz.
 *
 * A title that waits a frame reads the device's swap count and spins until it
 * moves. Nothing here presents, so without this the count never changes and
 * the wait never ends. Followed through the device pointer, like the fence,
 * because the device is allocated at runtime.
 *
 * Returns 0 on success, -1 if the table is full. */
/* Mirror one field of the device struct onto another, for the counter pair a
 * swap throttle waits on: the title bumps "frames submitted" itself and waits
 * for "frames completed", which only the GPU moves on hardware. Unlike
 * xbox_Nv2aFrameCounter this cannot run ahead of the title, so the unsigned
 * submitted - completed it gets compared against cannot underflow. */
int xbox_Nv2aMirrorCounter(uint32_t device_ptr_va,
                           uint32_t src_off, uint32_t dst_off);

int xbox_Nv2aFrameCounter(uint32_t device_ptr_va, uint32_t counter_off);
/* Advance those counters now, because a swap really completed. Called by the
 * pushbuffer executor on FLIP_STALL; while these arrive the 60 Hz fallback
 * stands down, so the count follows what was actually drawn. A counter the
 * title is seen to move itself is no longer bumped by either. */
void xbox_Nv2aFrameCounterFlip(void);
/* A word of the NV2A register file (offset from the register base); NULL until
 * the aperture is mapped. */
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset);
/* Registered counters the title turned out to move itself (its vblank DPC),
 * which the toolkit has stopped bumping; for the [GPU] report. */
uint32_t xbox_Nv2aFrameCountersOwned(void);

/* Tell the runtime where the display framebuffer is (from AvSetDisplayMode). */
void xbox_SetDisplayFramebuffer(uint32_t fb_va, uint32_t pitch);
/* Read it back: 0 until the title sets a mode. Pitch is optional. */
uint32_t xbox_GetDisplayFramebuffer(uint32_t *pitch);

/* Allocate from the contiguous (physical-mirror) arena. Returns a guest VA
 * below 256 MB, or 0 when the arena is exhausted. */
uint32_t xbox_ContiguousAlloc(uint32_t size, uint32_t alignment);
/* The same, constrained to physical addresses [phys_lo, phys_hi] (inclusive),
 * as MmAllocateContiguousMemoryEx asks. Alignment is rounded up to a page and
 * to a power of two. */
uint32_t xbox_ContiguousAllocEx(uint32_t size, uint32_t alignment,
                                uint32_t phys_lo, uint32_t phys_hi);
/* Record a block the caller placed itself, so it is not handed out again. */
void xbox_ContiguousClaim(uint32_t va, uint32_t size);
/* Release a block (RECOMP_HEAP_RECLAIM only); 0 if va is not in the window. */
int xbox_ContiguousFree(uint32_t va);
/* Bytes from va to the end of its block, 0 if va is not allocated. */
uint32_t xbox_ContiguousBlockSize(uint32_t va);
/* High-water mark (never decreases) and current usage, in bytes. */
uint32_t xbox_ContiguousAllocatedBytes(void);
/* One byte per 4 KB page of the window, nonzero while the page is allocated
 * (or claimed). Read without the arena lock; a device model's view. */
const volatile uint8_t *xbox_ContiguousPageMap(void);
/* MmGetPhysicalAddress named this VA; recorded if it is a heap page below
 * 64 MB (the image and stacks are not recorded). */
void xbox_PhysNoteRamName(uint32_t va);
/* One byte per 4 KB physical page below 64 MB, nonzero while the title holds
 * a RAM name for a heap page there (see xbox_PhysNoteRamName); cleared when
 * no live heap block covers the page any more. Read without a lock; a device
 * model's view. */
const volatile uint8_t *xbox_PhysRamNameMap(void);
uint32_t xbox_ContiguousInUseBytes(void);

int xbox_Nv2aMirrorFence(uint32_t device_ptr_va,
                         uint32_t put_off, uint32_t get_ptr_off);

/* Where the mirrored fence word is (0 if none is registered or it does not
 * resolve into the contiguous window). For the pushbuffer executor. */
uint32_t xbox_Nv2aFenceWordVa(void);

void xbox_MemoryLayoutShutdown(void);

/**
 * Check if an address falls within the Xbox memory map.
 */
BOOL xbox_IsXboxAddress(uintptr_t address);

/**
 * Get the base pointer for direct memory access.
 * Returns NULL if memory layout is not initialized.
 */
void *xbox_GetMemoryBase(void);
/* Runtime-owned 4 KiB kernel probe page, zeroed at initialization. NULL when
 * unavailable; invalid after memory shutdown. Contains no emulated PE image.
 * Ports may populate compatibility data before guest execution. */
void *xbox_GetKernelMemoryBase(void);

/**
 * Get the offset from Xbox VA to actual mapped address.
 * actual_address = xbox_va + offset
 * Returns 0 if memory is mapped at original Xbox addresses (ideal case).
 */
ptrdiff_t xbox_GetMemoryOffset(void);
/* Bytes of guest address space mapped, for bounds-checking a guest pointer
 * that came out of guest memory rather than from this side. */
size_t xbox_GetMappedSize(void);
void xbox_ProtectMirrorsForDebug(void);

/* Trapped hardware registers (APU, AC'97 bus master, USB OHCI). A title's
 * fault handler calls one of these first and resumes the thread when it
 * returns 1; only 0 is a crash. void * keeps windows.h and ucontext.h out of
 * here.
 *   xbox_VehMmioFault   Win32 VEH: the PEXCEPTION_POINTERS.
 *   xbox_PosixMmioFault POSIX SIGBUS/SIGSEGV handler: its ucontext_t * and
 *                       si_addr. Always 0 on a host with no decoder. */
#if defined(_WIN32)
int xbox_VehMmioFault(void *exception_pointers);
#else
int xbox_PosixMmioFault(void *ucv, uintptr_t fault);
#endif

/* Dump the guest call stack and abort if the title has not exited within
 * RECOMP_WATCHDOG_SECS seconds. Call from the thread that runs guest code;
 * does nothing unless that variable is set. */
void xbox_WatchdogStart(void);

/* Print the globals named by RECOMP_PEEK, tagged with `label`. No-op when
 * RECOMP_PEEK is unset. Called at a hang and at an early exit. */
void xbox_PeekSample(const char *label);
void xbox_WatchInit(void);

/* ================================================================
 * Xbox stack for recompiled code
 * ================================================================ */

/* ================================================================
 * Kernel data export area
 * ================================================================ */

/** Base VA for kernel data exports (XboxHardwareInfo, XboxKrnlVersion, etc.)
 *  These are kernel exports that are DATA, not functions. The game reads
 *  their thunk entries and dereferences them to access the data. */
/* Runtime, not a constant: the toolkit's own guest-side structures (this one,
 * the fake TIB/TLS data and the main stack) live in a region that starts above
 * the title image -- see g_xbox_region_base below. 0x00740000 for any image
 * that ends below 0x00700000. */
#define XBOX_KERNEL_DATA_BASE   (g_xbox_region_base + XBOX_REGION_KDATA_OFF)
#define XBOX_KERNEL_DATA_SIZE   4096   /* 4 KB - plenty for all data exports */

/* Offsets within the kernel data area */
#define KDATA_HARDWARE_INFO     0x000  /* XBOX_HARDWARE_INFO (8 bytes) */
#define KDATA_KRNL_VERSION      0x010  /* XBOX_KRNL_VERSION (8 bytes) */
#define KDATA_TICK_COUNT        0x020  /* KeTickCount (4 bytes) */
#define KDATA_LAUNCH_DATA_PAGE  0x030  /* LaunchDataPage (4 bytes, pointer) */
#define KDATA_THREAD_OBJ_TYPE   0x040  /* PsThreadObjectType (4 bytes) */
#define KDATA_EVENT_OBJ_TYPE    0x050  /* ExEventObjectType (4 bytes) */
#define KDATA_XE_IMAGE_FILENAME 0x060  /* XeImageFileName (ANSI_STRING) */
#define KDATA_IO_COMPLETION_TYPE 0x070 /* IoCompletionObjectType (4 bytes) */
#define KDATA_IO_DEVICE_TYPE    0x080  /* IoDeviceObjectType (4 bytes) */
/* Object-type exports a title may compare against each other, so each needs a
 * distinct non-zero value rather than a shared placeholder. */
#define KDATA_MUTANT_OBJ_TYPE   0x090  /* ExMutantObjectType (4 bytes) */
#define KDATA_SEMAPHORE_OBJ_TYPE 0x0A0 /* ExSemaphoreObjectType (4 bytes) */
#define KDATA_TIMER_OBJ_TYPE    0x0B0  /* ExTimerObjectType (4 bytes) */
#define KDATA_FILE_OBJ_TYPE     0x0C0  /* IoFileObjectType (4 bytes) */
#define KDATA_TIME_INCREMENT    0x0D0  /* KeTimeIncrement (4 bytes) */
#define KDATA_BOOT_SMC_VIDEO    0x0E0  /* HalBootSMCVideoMode (4 bytes) */
#define KDATA_IDEX_CHANNEL      0x500  /* IDE_CHANNEL_OBJECT (512-byte reserved region) */
#define KDATA_HD_KEY            0x100  /* XboxHDKey (16 bytes) */
#define KDATA_SIGNATURE_KEY     0x110  /* XboxSignatureKey (16 bytes) */
#define KDATA_LAN_KEY           0x120  /* XboxLANKey (16 bytes) */
#define KDATA_ALT_SIGNATURE_KEYS 0x130 /* XboxAlternateSignatureKeys (256 bytes) */
#define KDATA_XE_PUBLIC_KEY     0x300  /* XePublicKeyData (284 bytes) */
/* Guest counted-string descriptors: two 16-bit counts and a 32-bit buffer VA.
 * Bytes/capacity are owned guest heap copies of port-supplied native exports. */
#define KDATA_DISK_MODEL_STR    0x420
#define KDATA_DISK_SERIAL_STR   0x460
#define KDATA_DISK_CACHE_PARTS  0x4A0

/* Thread-local storage class for the recompiled register set. Must match
 * templates/runtime/recomp_types.h -- a mismatch is a link-time surprise. */
#if defined(_MSC_VER)
#  define RECOMP_TLS __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#  define RECOMP_TLS __thread
#else
#  define RECOMP_TLS _Thread_local
#endif

/* SSE register storage, shared with the generated code. Defined in both this
 * header and templates/runtime/recomp_types.h -- a translation unit can end up
 * including both, so the guard keeps that from being a redefinition. Keep the
 * two identical: the generated code and the runtime have to agree on the
 * layout, and nothing else checks. */
#ifndef RECOMP_MMX_DEFINED
#define RECOMP_MMX_DEFINED
typedef union RecompMmx {
    int8_t   b[8];
    uint8_t  ub[8];
    int16_t  w[4];
    uint16_t uw[4];
    int32_t  d[2];
    uint32_t ud[2];
    uint64_t q;
} RecompMmx;
#endif

#ifndef RECOMP_XMM_DEFINED
#define RECOMP_XMM_DEFINED
typedef union RecompXmm {
    float    f[4];
    double   d[2];
    uint32_t u[4];
    int32_t  i[4];
    uint64_t q[2];
} RecompXmm;
#endif

/* The toolkit region: scratch structures, then the main stack, then the heap.
 *
 * These used to be fixed -- scratch at 0x00700000, stack at 0x00780000, heap
 * at 0x00F80000 -- which is right only while the title's image ends below
 * 0x00700000. A title whose 70-odd sections run past 0x03000000 had the
 * stack, the kernel data exports and the heap inside its .data/BSS, and the
 * first zeroed heap allocation wiped its globals.
 *
 * xbox_MemoryLayoutInit now places the region at the first 64 KB boundary
 * above the highest loaded section, never below 0x00700000, so a title that
 * fit before sees exactly the old addresses. Offsets inside the region are
 * the old ones relative to 0x00700000. Valid once xbox_MemoryLayoutInit has
 * loaded the sections; nothing reads them earlier. */
#define XBOX_REGION_MIN         0x00700000u
#define XBOX_REGION_KDATA_OFF   0x00040000u  /* kernel data exports */
#define XBOX_REGION_TLS_OFF     0x00060000u  /* fake TLS structure (fs:[0x28]) */
#define XBOX_REGION_PRCB_OFF    0x00061000u  /* zeroed KPCR Prcb stand-in */
#define XBOX_REGION_TLSBLK_OFF  0x00070000u  /* image TLS block */
#define XBOX_REGION_STACK_OFF   0x00080000u  /* main stack */
extern uint32_t g_xbox_region_base;
extern uint32_t g_xbox_stack_base;
extern uint32_t g_xbox_stack_size;

/* 8 MB, unless that would leave less than 16 MB of heap -- then it shrinks,
 * to no less than 1 MB (hardware gives the main thread the XBE's stack
 * commit, typically 64 KB). */
#define XBOX_STACK_SIZE     g_xbox_stack_size

/** Base VA of the stack area (above last XBE section). */
/* Where the fake TIB lives -- the linear address fs: is based at.
 *
 * Deliberately not 0. The TIB used to sit on page zero, because the lifter
 * dropped the fs prefix and fs:[N] became linear [N]. That made a null
 * dereference read or write the TIB instead of faulting: a null check of the
 * form `cmp byte [ecx], 0` saw the exception-chain head's 0xFF and passed, and
 * a store through a null pointer quietly overwrote that head. Both then
 * surfaced somewhere else entirely. With the TIB up here, page zero is left
 * unmapped and either mistake faults where it happens.
 *
 * Sits below every XBE's image base (0x00010000), so it displaces nothing.
 *
 * 0x4000 rather than 0x1000 because protection is applied at *host* page
 * granularity. Apple Silicon pages are 16 KB, so RECOMP_TRAP_NULL asking to
 * protect guest page zero actually covers guest 0..0x3FFF -- which reached a
 * TIB at 0x1000 and killed the run, so the guard disabled itself on every
 * such host and the diagnostic quietly did nothing. At 0x4000 the largest
 * page any supported host uses fits below the TIB and the guard installs.
 * Nothing else lives in the low 64 KB, and no guest code names the address:
 * fs: resolves through g_fs_base.
 *
 * Per-thread, because a TIB is. It used to be one constant address for the
 * whole process, which meant every guest thread shared one SEH chain head
 * and -- through fs:[4] -- one CRT per-thread data block. Half-Life 2
 * deadlocked on that: two threads in _lock() each holding the CRT lock the
 * other wanted, because the bookkeeping that decides who owns what was
 * shared between them.
 *
 * XBOX_TIB_MAIN is where the first thread's TIB is built; every spawned
 * thread gets its own from xbox_AllocThreadTib() and points g_fs_base at
 * it. */
#define XBOX_TIB_MAIN       0x00004000
extern RECOMP_TLS uint32_t g_fs_base;
#define XBOX_FS_BASE        g_fs_base

#define XBOX_STACK_BASE     g_xbox_stack_base

/** Initial ESP value (top of stack, 16-byte aligned). */
#define XBOX_STACK_TOP      (XBOX_STACK_BASE + XBOX_STACK_SIZE - 16)

/* ================================================================
 * Worker stack slices (host-tick-driven titles)
 * ================================================================
 *
 * A second way to drive a recompiled title, ported from the Burnout 3 fork as
 * the runtimes reunite (see docs/technical/burnout3-reunification.md).
 *
 * The default model (Halo, Crimson Skies) runs the game's entry routine inline
 * and it drives its own main loop. Some titles instead return from their entry
 * after spawning an init thread, and expect the *host* to drive the per-frame
 * tick -- Burnout 3 is tick-driven, not main-loop-driven. To call recompiled
 * code from the host's own message-loop thread, that thread needs a guest stack
 * (its g_esp starts at 0), which is what a worker slice provides.
 *
 * These slices carve the low end of the same 8 MB stack region xbox_AllocThreadStack
 * uses, and a title uses one model or the other -- never both -- so they do not
 * coexist at runtime. For a title that never calls xbox_worker_stack_alloc
 * (every default-model title), this is unused address space and dead code, so
 * adding it changes nothing for them.
 */
#define XBOX_WORKER_STACK_SIZE   (256 * 1024)
#define XBOX_WORKER_STACK_BASE   XBOX_STACK_BASE             /* 0x00780000 for small images */
#define XBOX_WORKER_STACK_COUNT  16                          /* 4 MB total */
#define XBOX_WORKER_STACK_END    (XBOX_WORKER_STACK_BASE + \
                                  XBOX_WORKER_STACK_SIZE * XBOX_WORKER_STACK_COUNT)

/** Top (initial esp) of worker stack slice n, 16-byte aligned, growing down. */
#define XBOX_WORKER_STACK_TOP(n) (XBOX_WORKER_STACK_BASE + \
                                  XBOX_WORKER_STACK_SIZE * ((n) + 1) - 16)

/* ================================================================
 * Xbox dynamic heap (for MmAllocateContiguousMemory, etc.)
 * ================================================================ */

/** Base VA of the dynamic heap area (above stack). */
#define XBOX_HEAP_BASE      (XBOX_STACK_BASE + XBOX_STACK_SIZE)  /* 0x00F80000 for small images */

/** Exclusive top of the dynamic heap: the end of RAM for this run. Runtime,
 *  not a macro, because RAM size is now configurable (retail 64 MB vs devkit
 *  128 MB). The total mapped region (data + stack + heap) equals RAM so the
 *  engine's memory probing stops at the correct boundary. */
/* The heap runs to the end of the *mapped* range, not the end of RAM.
 *
 * When a title maps more address space than it has RAM, a large
 * MEM_RESERVE has to come from somewhere. Carving it out of a separate
 * arena above the heap looked tidy and was wrong: the guest CRT's own
 * bookkeeping never learns about that region, so realloc's block lookup
 * fails for a pointer in it and the copy is skipped -- a grown buffer
 * comes back empty with the old one still intact. Half-Life 2 loses a
 * 129-node CUtlRBTree that way, and the tree then self-cycles.
 *
 * Letting the ordinary heap serve the whole mapped range keeps every
 * allocation inside one allocator the guest already understands.
 */
#define XBOX_HEAP_TOP       ((uint32_t)(g_xbox_map_size ? g_xbox_map_size \
                                                        : g_xbox_total_ram))

/** No static mirror/guard region. RAM mirror is handled via file mapping
 *  views that alias the same physical pages as the base 64 MB region. */
#define XBOX_MIRROR_SIZE    0
#define XBOX_GUARD_SIZE     0

/** Number of 64 MB mirror views to pre-map (covers 1.75 GB of address space). */
#define XBOX_NUM_MIRRORS    28

/* Tiled / write-combined aperture. The NV2A shows physical RAM again here, and
 * titles render through it: physical page P is at XBOX_TILED_BASE + P. Aliases
 * the RAM mapping rather than getting its own storage, because a title writes a
 * surface through the tiled address and reads it back through the normal one. */
#define XBOX_TILED_BASE     0xF0000000u

/**
 * Allocate from the Xbox heap. Returns an Xbox VA, or 0 on failure.
 * Alignment must be a power of 2 (minimum 4).
 * Thread-safe: yes; a lock guards the block table.
 */
uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);

/**
 * Free a block from the Xbox heap and coalesce adjacent free blocks.
 */
void xbox_HeapFree(uint32_t xbox_va);

/**
 * Bytes remaining in the heap block containing this guest address, or 0 if the
 * heap never handed it out. Backs MmQueryAllocationSize and
 * ExQueryPoolBlockSize -- the host cannot answer either, since VirtualQuery on
 * the translated address describes the whole guest mapping.
 */
uint32_t xbox_HeapBlockSize(uint32_t xbox_va);

/**
 * True when RECOMP_HEAP_RECLAIM is set: NtFreeVirtualMemory returns heap memory
 * to the heap and zeroes decommitted pages. Off by default. Ordinary heap
 * allocation always splits reused blocks, independently of this switch.
 */
int xbox_HeapReclaimEnabled(void);

/**
 * Get the file mapping handle for the Xbox memory region.
 * Returns NULL if file mapping is not available.
 */
HANDLE xbox_GetMappingHandle(void);



/* Optional page-aligned worker minimum, configured before memory init.
 * Zero restores the image-derived default. Invalid/active changes preserve
 * policy. Requests/defaults are page-rounded; backing/heap capacity bounds
 * allocation, without a fixed thread-count cap. Startup/reset is quiesced. */
BOOL xbox_ConfigureThreadStack(uint32_t minimum_bytes);
/* Zero request uses XBE StackCommit (at least one page), enlarged by any port
 * minimum. Returns an owned guest top, 16-byte aligned, or zero on failure. */
uint32_t xbox_AllocThreadStackSized(uint32_t requested_bytes);
uint32_t xbox_AllocThreadStack(void); /* equivalent to a zero-sized request */

/* A TIB and TLS block for a newly spawned guest thread, copied from the
 * template the loader built. Returns the new TIB's Xbox VA, or 0. */
uint32_t xbox_AllocThreadTib(void);

/* Release the exact top returned by either allocator. Unowned, interior,
 * duplicate or zero values are ignored; no other heap block is released.
 * Addresses are invalid after memory shutdown, even if later reused. */
void xbox_FreeThreadStack(uint32_t stack_top);
/* Exact-size guest kernel stack, returning the exclusive high address.
 * Delete requires its matching high/low pair; ordinary heaps/worker stacks
 * cannot be released through this API. Zero size fails. No guard/debugger
 * placement semantics are supplied. Memory teardown remains quiesced. */
uint32_t xbox_AllocKernelStack(uint32_t bytes);
void xbox_FreeKernelStack(uint32_t high,uint32_t low);
/* Allocation containing a guest SP: main, owned worker/kernel or retained
 * callback lease. Bounds are [low, high); unknown/freed addresses return FALSE
 * and zero optional outputs. Retain stack/lease lifetime while using bounds;
 * this is an ownership lookup, not a host-thread identity or backing probe. */
BOOL xbox_GetStackBounds(uint32_t address, uint32_t *low, uint32_t *high);


/* Guest stack slices for host callbacks: DPCs, interrupts and host-driven
 * guest calls. See XBOX_WORKER_STACK_* and docs/runtime/README.md. */
/* Startup/quiesced reset; zero disables leases. Base is caller-owned guest RAM. */
void xbox_worker_stack_reset(uint32_t base);
int  xbox_worker_stack_alloc(void);   /* slice index, or -1 if none free */
void xbox_worker_stack_free(int slot);
void   xbox_set_game_thread(void *h);  /* HANDLE, recorded for the host watchdog */
void  *xbox_thread_debug_handle(void); /* the game thread, or NULL under inline model */

/* Both guest thread-create APIs default to independent spawned threads.
 * Ports may explicitly opt into first-call INLINE compatibility before guest
 * execution starts; later calls still use the worker policy. */
#define XBOX_THREAD_MODE_INLINE 0
#define XBOX_THREAD_MODE_SPAWN  1
void xbox_SetThreadMode(int mode);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_MEMORY_LAYOUT_H */
