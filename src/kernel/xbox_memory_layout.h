/**
 * Xbox Memory Layout Compatibility
 *
 * The Xbox has 64MB of unified memory shared between CPU and GPU.
 * Memory is identity-mapped (physical == virtual for most of it).
 * Game code and data are linked to specific address ranges which vary
 * per game. Section addresses are parsed dynamically from the XBE header
 * at runtime, so this module works with ANY Xbox game.
 *
 * On Windows, we:
 * 1. Create a 64MB file mapping (CreateFileMapping)
 * 2. Map the base view; reserve, but do not map, the space above it
 * 3. Parse the XBE section table and copy sections to their Xbox VAs
 * 4. Set up simulated stack, heap, TIB, and kernel data area
 *
 * An address above the mapped range is not an alias of RAM. It is unmapped,
 * so a stray guest access faults and the fault handler reports the address.
 */

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

/* Start of mapped region - includes low memory (KPCR at 0x0) because
 * game code reads from addresses like 0x20 and 0x28 (Xbox kernel structures). */
#define XBOX_MAP_START          0x00000000

/* Xbox physical memory. 64 MB is the retail default; debug/beta builds ship for
 * 128 MB devkits and allocate accordingly (Halo's cachebeta pre-allocates ~57 MB
 * plus its debug arrays, which only fits on a devkit). Runtime-overridable via
 * xbox_SetTotalRam() before xbox_MemoryLayoutInit(); see g_xbox_total_ram. */
#define XBOX_TOTAL_RAM          (64 * 1024 * 1024)  /* 64 MB (default) */
#define XBOX_DEVKIT_RAM         (128 * 1024 * 1024) /* 128 MB (debug kit) */
#define XBOX_GPU_RESERVED       (4 * 1024 * 1024)   /* ~4 MB for GPU */

/* Actual mapped RAM for this run. Defaults to XBOX_TOTAL_RAM; a title with a
 * devkit build calls xbox_SetTotalRam(XBOX_DEVKIT_RAM) before init. Heap top
 * derives from this, not from the compile-time constant. */
extern size_t g_xbox_total_ram;

/* How much guest address space to map, when that must exceed RAM.
 *
 * These are not the same quantity and conflating them is a bug. RAM is what
 * the console has and what the heap is carved out of; the mapped range is how
 * much guest address space is backed by distinct host pages. Anything a title
 * allocates above the mapped range is not backed by anything: it faults, which
 * is what an unmapped virtual address does on the console too.
 *
 * A port can request a larger backed virtual range when guest reservations
 * exceed installed RAM. These pages must remain distinct rather than aliasing
 * live RAM allocations. The port supplies the required range before init.
 *
 * Raising g_xbox_total_ram instead does not work: the heap top and anything
 * the guest is told about memory derive from that, so the title sizes itself
 * differently and faults during CRT init. Growing only the mapping leaves both
 * alone.
 *
 * Zero means "same as RAM", which is the existing behaviour for every title
 * that does not ask. Set before xbox_MemoryLayoutInit(). */
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

/* Register a device-field counter before guest execution. It advances once
 * per executed non-replay flip, never from wall-clock time. Caller supplies
 * the image-specific pointer global/offset. Returns 0, or -1 if full. */
int xbox_Nv2aFrameCounter(uint32_t device_ptr_va, uint32_t counter_off);
void xbox_Nv2aFrameCounterFlip(void);

/* Tell the runtime where the display framebuffer is (from AvSetDisplayMode). */
void xbox_SetDisplayFramebuffer(uint32_t fb_va, uint32_t pitch);
/* Read it back: 0 until the title sets a mode. Pitch is optional. */
uint32_t xbox_GetDisplayFramebuffer(uint32_t *pitch);

/* Allocate from the contiguous (physical-mirror) arena. Returns a guest VA
 * below 256 MB, or 0 when the arena is exhausted. */
uint32_t xbox_ContiguousAlloc(uint32_t size, uint32_t alignment);
int xbox_ContiguousFree(uint32_t xbox_va);              /* 0: not ours */
uint32_t xbox_ContiguousBlockSize(uint32_t xbox_va);
int xbox_ContiguousRangeAllocated(uint32_t physical_offset, uint32_t size);

/* Register a caller-described device PUT and indirect fence target before
 * guest execution. Registration alone never publishes completion. Configure
 * CommandFence below for execution-driven semaphore completion.
 * Returns 0, or -1 if full; invalid chains are skipped. */
int xbox_Nv2aMirrorFence(uint32_t device_ptr_va,
                         uint32_t put_off, uint32_t get_ptr_off);
/* Configure an existing mirror before entering guest code. It then accepts
 * only matching executed NV097 semaphore releases, never the next counter. */
int xbox_Nv2aCommandFence(uint32_t device_ptr_va,
                          uint32_t dma_context, uint32_t semaphore_offset);
/* Executor callback; completion retains the target/value until readback. */
void xbox_Nv2aSemaphoreRelease(uint32_t dma_context,
                                uint32_t semaphore_offset, uint32_t value);
/* Publish an owned USER PUT with its PFIFO cache/busy state first. Other
 * addresses retain the original software-channel store. */
void xbox_Nv2aDmaPublish(uint32_t put_va, uint32_t value);
/* Host wait for exact CACHE1 PUT == GET and PGRAPH idle. The caller keeps
 * the original final reads/branches; unknown apertures retain guest polling. */
void xbox_Nv2aDmaWait(uint32_t pfifo_base_va);

/* Opt-in parser diagnostics only. Roots come from registered fence mirrors;
 * the raw device bytes let a title's producer layout be decoded off-line. */
#define XBOX_NV2A_CAPTURE_MIRRORS 4
#define XBOX_NV2A_CAPTURE_DEVICE_BYTES 0x2200
typedef struct {
    uint32_t device_ptr_va, device_va, device_valid, device_bytes;
    uint32_t put_offset, get_ptr_offset, put_value, fence_va, fence_value, fence_valid;
    uint32_t channel_va, channel_valid, channel_put, channel_get;
    uint32_t pfifo_va, pfifo_valid, pfifo_subroutine;
    uint32_t command_mode, command_dma, command_offset, command_pending;
    uint32_t command_value, command_fence_va, command_queue_count;
    uint8_t device[XBOX_NV2A_CAPTURE_DEVICE_BYTES];
} XBOX_NV2A_CAPTURE_MIRROR;
typedef struct {
    uint32_t version, bytes, mmio_valid, user_put, user_get;
    uint32_t pfifo_put, pfifo_get, pfifo_subroutine;
    uint32_t get_held, get_held_value, scan_incomplete;
    uint32_t notify_count, notify_param, notify_device_ptr, notify_event_offset;
    uint32_t counter_mirrors, frame_counters, mirror_count;
    XBOX_NV2A_CAPTURE_MIRROR mirrors[XBOX_NV2A_CAPTURE_MIRRORS];
} XBOX_NV2A_CAPTURE_STATE;
void xbox_Nv2aCaptureSnapshot(XBOX_NV2A_CAPTURE_STATE *out);

/* The KEVENT at *(device_ptr_va) + event_off is the title D3D's GPU-notify
 * event; KeWaitForSingleObject on it waits for the executor to consume all
 * submitted push-buffer instead (see xbox_memory_layout.c). NotifyWait
 * returns 0 when object_va is not that event. */
int xbox_Nv2aNotifyEvent(uint32_t device_ptr_va, uint32_t event_off, uint32_t nop_param);
/* Record an executed matching NO_OPERATION. Its notification ends the wait
 * only after the associated consumed GET and prior readbacks are published. */
void xbox_Nv2aNop(uint32_t param);
int xbox_Nv2aNotifyWait(uint32_t object_va);
/* Host wait for a direct guest fence poll. The caller retains its original
 * final read/unsigned branch; this helper never writes guest state. */
void xbox_Nv2aFenceWait(uint32_t fence_va, uint32_t submitted,
                         uint32_t allowed_distance);

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

/**
 * Get the offset from Xbox VA to actual mapped address.
 * actual_address = xbox_va + offset
 * Returns 0 if memory is mapped at original Xbox addresses (ideal case).
 */
ptrdiff_t xbox_GetMemoryOffset(void);
/* Bytes of guest address space mapped, for bounds-checking a guest pointer
 * that came out of guest memory rather than from this side. */
size_t xbox_GetMappedSize(void);
int xbox_IsContiguousMemoryAvailable(void);
/* Runtime virtualized physical address space. Low RAM IDs are placed after
 * the complete guest VA range so a raw guest VA can never alias an ID. The
 * resolver returns 1 for low RAM and 2 for the backed contiguous section
 * (including currently unallocated offsets). Consumers may apply additional
 * allocation or legacy guest-VA precedence rules. */
uint32_t xbox_GetRamPhysicalBase(void);
int xbox_PhysicalAddressResolve(uint32_t physical, uint32_t bytes,
                                uint32_t *guest_va);
/* Independent, owned views of physical RAM/device sections. Native pointers
 * are returned; guest bridges subtract the memory offset. Unmap only accepts
 * the exact pointer and byte count issued by MapPhysicalMemory. */
void *xbox_MapPhysicalMemory(uint32_t physical, uint32_t bytes, uint32_t protect);
int xbox_UnmapPhysicalMemory(void *address, uint32_t bytes);
int xbox_IoMappingQuery(const void *address, uint32_t *physical,
                        uint32_t *protect, uint32_t *remaining);
/* 0: not an alias; 1: translated; -1: protection denies this access.
 * access is 0 for read, 1 for write, 8 for execute (Windows AV convention). */
int xbox_IoMappingResolve(uint32_t guest_va, int access, uint32_t *physical);
/* 0: not ours; 1: changed; -1: invalid range/protection or host failure. */
int xbox_IoMappingSetProtect(void *address, uint32_t bytes, uint32_t protect);

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
#define XBOX_KERNEL_DATA_BASE   0x00740000
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
/* HAL disk identity strings (ordinals 41/42). Each is an XBOX_ANSI_STRING
 * (Length, MaximumLength, Buffer VA) followed by the string bytes it points to,
 * because HalRandGather dereferences Buffer to read the text as entropy. */
#define KDATA_DISK_MODEL_STR    0x420  /* XBOX_ANSI_STRING (8 bytes) */
#define KDATA_DISK_MODEL_BUF    0x430  /* model text (up to 48 bytes) */
#define KDATA_DISK_SERIAL_STR   0x460  /* XBOX_ANSI_STRING (8 bytes) */
#define KDATA_DISK_SERIAL_BUF   0x470  /* serial text (up to 32 bytes) */
#define KDATA_DISK_CACHE_PARTS  0x4A0  /* HalDiskCachePartitionCount (4 bytes) */
/* XeImageFileName's text. The exported symbol at KDATA_XE_IMAGE_FILENAME is
 * an XBOX_ANSI_STRING, and a title dereferences its Buffer -- the CRT reads
 * it to work out the running image's path. The struct was declared without
 * anything to point at, so Buffer held whatever was in that page. */
#define KDATA_XE_IMAGE_BUF      0x4B0  /* image path text (up to 64 bytes) */

/** Size of the simulated Xbox stack (8 MB).
 *  Increased from 1 MB because failed RECOMP_ICALL indirect calls
 *  can leak stdcall args onto the stack each frame. An 8 MB stack
 *  provides enough headroom for extended gameplay sessions. */

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

#define XBOX_STACK_SIZE     (8 * 1024 * 1024)

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
/* Xbox FS addresses a KPCR: 0x28-byte prefix plus the 0x258-byte KPRCB.
 * DpcRoutineActive is a ULONG at PrcbData + 0x30, beyond the NT_TIB. */
#define XBOX_KPCR_SIZE              0x280u
#define XBOX_KPCR_IRQL_OFFSET       0x24u
#define XBOX_KPCR_DPC_ACTIVE_OFFSET 0x58u
#define XBOX_TIB_MAIN       0x00004000
extern RECOMP_TLS uint32_t g_fs_base;
#define XBOX_FS_BASE        g_fs_base

#define XBOX_STACK_BASE     0x00780000

/** Initial ESP value (top of stack, 16-byte aligned). */
#define XBOX_STACK_TOP      (XBOX_STACK_BASE + XBOX_STACK_SIZE - 16)

/* ================================================================
 * Worker stack slices (host-tick-driven titles)
 * ================================================================
 *
 * A host thread calling guest code needs a guest stack. These optional slices
 * provide one for host-driven entry/tick calls. The loader places the pool
 * after the loaded image and excludes it from heap allocations. Guest-created
 * threads use xbox_AllocThreadStack instead.
 */
#define XBOX_WORKER_STACK_SIZE   (256 * 1024)
extern uint32_t g_xbox_worker_stack_base;
#define XBOX_WORKER_STACK_BASE   g_xbox_worker_stack_base
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
#define XBOX_HEAP_BASE      (XBOX_STACK_BASE + XBOX_STACK_SIZE)  /* 0x00F80000 */

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

/** No static mirror/guard region. */
#define XBOX_MIRROR_SIZE    0
#define XBOX_GUARD_SIZE     0

/** Number of map-sized slots reserved above the mapped range (1.75 GB at a
 *  128 MB map). They are reserved, never mapped, so an access faults: the
 *  slots used to alias low RAM, which hid real corruption. See
 *  xbox_memory_layout.c. */
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

#ifdef __cplusplus
}
#endif


/* Carve a simulated stack for a spawned thread. Returns the Xbox VA of the
 * stack top, or 0 when the pool is exhausted. */
uint32_t xbox_AllocThreadStack(void);

/* A TIB and TLS block for a newly spawned guest thread, copied from the
 * template the loader built. Returns the new TIB's Xbox VA, or 0. */
uint32_t xbox_AllocThreadTib(void);

/**
 * Return a worker's stack when the worker ends. Takes the value
 * xbox_AllocThreadStack returned. Without this the pool counts threads ever
 * created rather than threads alive, and a title that cycles workers exhausts
 * it -- after which PsCreateSystemThreadEx runs them inline, which deadlocks
 * any caller that then waits for the worker it thought it had spawned.
 */
void xbox_FreeThreadStack(uint32_t stack_top);

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

#endif /* XBOX_MEMORY_LAYOUT_H */
