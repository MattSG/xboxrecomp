/**
 * Xbox Memory Layout Implementation
 *
 * Maps the XBE data sections to their expected virtual addresses on Windows.
 * This is critical for the recompiled code which references globals by
 * absolute address (e.g., mov eax, [0x004D532C]).
 *
 * Implementation:
 * 1. VirtualAlloc a contiguous region at XBOX_BASE_ADDRESS
 * 2. Copy .rdata and initialized .data from the XBE
 * 3. Zero-fill the BSS region
 * 4. Set memory protection (read-only for .rdata)
 */

#include "xbox_memory_layout.h"
#include "kernel.h"
#include <stdio.h>
#include <string.h>

/* XBE header field offsets (per xboxdevwiki.net/Xbe) */
#define XBE_MAGIC_OFFSET        0x0000
#define XBE_BASE_ADDR_OFFSET    0x0104
#define XBE_HEADER_SIZE_OFFSET  0x0108
#define XBE_SECTION_COUNT_OFFSET 0x011C
#define XBE_SECTION_HEADERS_OFFSET 0x0120

/* XBE section header layout (56 bytes each) */
#define SECTHDR_FLAGS       0x00
#define SECTHDR_VA          0x04
#define SECTHDR_VSIZE       0x08
#define SECTHDR_RAW_OFFSET  0x0C
#define SECTHDR_RAW_SIZE    0x10
#define SECTHDR_NAME_ADDR   0x14
#define SECTHDR_SIZE        56

static void *g_memory_base = NULL;
static size_t g_memory_size = 0;
static ptrdiff_t g_memory_offset = 0;  /* actual_base - XBOX_BASE_ADDRESS */

/* File mapping handle for the Xbox memory region.
 * Using CreateFileMapping + MapViewOfFileEx allows mirror views to alias
 * the same physical pages as the base region, so writes to mirror addresses
 * (which wrap modulo 64 MB on real Xbox hardware) correctly modify the
 * underlying data. */
static HANDLE g_mapping_handle = NULL;

/* Mirror view pointers for cleanup */
static void *g_mirror_views[XBOX_NUM_MIRRORS] = {0};

/* Global offset accessible by recompiled code (via recomp_types.h) */
ptrdiff_t g_xbox_mem_offset = 0;

/* Global registers for recompiled code (via recomp_types.h) */
uint32_t g_eax = 0, g_ecx = 0, g_edx = 0, g_esp = 0;
double g_fp_stack[8];
int g_fp_top = 0;
uint32_t g_ebx = 0, g_esi = 0, g_edi = 0;

/* SEH frame pointer bridge (see recomp_types.h for explanation) */
uint32_t g_seh_ebp = 0;

/* ICALL trace ring buffer */
volatile uint32_t g_icall_trace[16] = {0};
volatile uint32_t g_icall_trace_idx = 0;
volatile uint64_t g_icall_count = 0;

BOOL xbox_MemoryLayoutInit(const void *xbe_data, size_t xbe_size)
{
    DWORD old_protect;
    const uint8_t *xbe = (const uint8_t *)xbe_data;

    if (g_memory_base) {
        fprintf(stderr, "xbox_MemoryLayoutInit: already initialized\n");
        return FALSE;
    }

    /*
     * Calculate the full range we need to map.
     * From XBOX_MAP_START (0x0) to the end of the furthest section.
     * This includes low memory (KPCR at 0x0-0xFF) which game code reads
     * from, the XBE sections, and the simulated stack.
     */
    /* Map the full 64MB Xbox address space (covers all sections + stack + heap) */
    g_memory_size = XBOX_TOTAL_RAM;

    /*
     * Create a file mapping backed by the page file.
     *
     * Using file mapping instead of VirtualAlloc allows us to map the same
     * physical pages at multiple virtual addresses via MapViewOfFileEx.
     * This is critical for the Xbox RAM mirror: the Xbox memory controller
     * uses a 26-bit address bus, so ALL addresses wrap modulo 64 MB.
     * Code that writes to address 0x20000448 is really writing to 0x00000448.
     * With file mapping views, we create aliased mappings at 64 MB intervals
     * that all point to the same physical memory.
     */
    g_mapping_handle = CreateFileMappingA(
        INVALID_HANDLE_VALUE,   /* page file backed */
        NULL,                   /* default security */
        PAGE_READWRITE,         /* read-write access */
        0,                      /* high DWORD of size */
        (DWORD)g_memory_size,   /* low DWORD of size (64 MB) */
        NULL                    /* unnamed mapping */
    );
    if (!g_mapping_handle) {
        fprintf(stderr, "xbox_MemoryLayoutInit: CreateFileMapping failed (error %lu)\n",
                GetLastError());
        return FALSE;
    }

    /*
     * Map the base view at the desired virtual address.
     * Try the original Xbox base address first. If that fails (common on
     * Windows 11 where low addresses are often reserved), try page-aligned
     * addresses upward until we find a free region.
     */
    {
        static const uintptr_t try_bases[] = {
            XBOX_NATIVE_BASE,       /* 4 GB - above the host's low reservations */
            XBOX_BASE_ADDRESS,      /* 0x00010000 - original Xbox address */
            0x00800000,             /* 8 MB - above typical PEB/TEB region */
            0x01000000,             /* 16 MB */
            0x02000000,             /* 32 MB */
            0x10000000,             /* 256 MB */
            0,                      /* sentinel - let OS choose */
        };

        for (int i = 0; try_bases[i] != 0 || i == 0; i++) {
            LPVOID hint = try_bases[i] ? (LPVOID)try_bases[i] : NULL;
            g_memory_base = MapViewOfFileEx(
                g_mapping_handle,
                FILE_MAP_ALL_ACCESS,
                0, 0,           /* offset into mapping */
                g_memory_size,  /* size */
                hint            /* desired base address */
            );
            if (g_memory_base) {
                if (try_bases[i] != 0 && (uintptr_t)g_memory_base != try_bases[i]) {
                    /* OS gave us a different address, retry */
                    UnmapViewOfFile(g_memory_base);
                    g_memory_base = NULL;
                    continue;
                }
                break;
            }
        }
    }

    if (!g_memory_base) {
        fprintf(stderr, "xbox_MemoryLayoutInit: failed to map base view (%zu KB)\n",
                g_memory_size / 1024);
        CloseHandle(g_mapping_handle);
        g_mapping_handle = NULL;
        return FALSE;
    }

    g_memory_offset = (uintptr_t)g_memory_base - XBOX_MAP_START;

    if (g_memory_offset == 0) {
        fprintf(stderr, "xbox_MemoryLayoutInit: mapped %zu KB at 0x%08X (original Xbox address)\n",
                g_memory_size / 1024, XBOX_MAP_START);
    } else {
        fprintf(stderr, "xbox_MemoryLayoutInit: mapped %zu KB at 0x%p (offset %+td from Xbox base)\n",
                g_memory_size / 1024, g_memory_base, g_memory_offset);
    }

    /*
     * Helper macro: convert Xbox VA to actual mapped address.
     * When g_memory_offset == 0 (ideal case), this is identity.
     */
    #define XBOX_VA(va) ((void *)((uintptr_t)(va) + g_memory_offset))

    /*
     * Copy XBE header to base address.
     * The Xbox kernel maps the XBE image header at 0x00010000.
     * Game code reads kernel thunk table, certificate data, and
     * section info from this region.
     */
    {
        /* XBE header size is at file offset 0x0108 (SizeOfImageHeader) */
        DWORD header_size = 0;
        if (xbe_size >= 0x10C) {
            header_size = *(const DWORD *)(xbe + 0x0108);
        }
        if (header_size == 0 || header_size > 0x10000)
            header_size = 0x1000;  /* fallback: 4KB */
        if (header_size > xbe_size)
            header_size = (DWORD)xbe_size;
        memcpy(XBOX_VA(XBOX_BASE_ADDRESS), xbe, header_size);
        fprintf(stderr, "  XBE header: %u bytes at %p (Xbox VA 0x%08X)\n",
                header_size, XBOX_VA(XBOX_BASE_ADDRESS), XBOX_BASE_ADDRESS);
    }

    /*
     * Dynamically load ALL XBE sections by parsing the section headers.
     *
     * This replaces the old approach of hardcoding section addresses for
     * a specific game (Burnout 3). By reading the section table from the
     * XBE header, any game's sections are loaded automatically.
     *
     * Every section is copied to its original Xbox VA:
     * - .text: needed because memory walkers may scan code pages
     * - .rdata: constants, vtables, kernel thunk table
     * - .data: global variables (initialized portion from XBE, BSS zeroed)
     * - XDK library sections (D3D, DSOUND, WMADEC, XPP, etc.)
     * - DOLBY, BINK, XTIMAGE, etc.
     */
    {
        DWORD base_addr = *(const DWORD *)(xbe + XBE_BASE_ADDR_OFFSET);
        DWORD num_sections = *(const DWORD *)(xbe + XBE_SECTION_COUNT_OFFSET);
        DWORD sect_headers_va = *(const DWORD *)(xbe + XBE_SECTION_HEADERS_OFFSET);
        DWORD sect_headers_off = sect_headers_va - base_addr;
        int sections_loaded = 0;
        size_t total_bytes = 0;

        if (num_sections > 64) num_sections = 64;  /* sanity cap */

        fprintf(stderr, "  XBE sections: %u (headers at file offset 0x%08X)\n",
                num_sections, sect_headers_off);

        for (DWORD si = 0; si < num_sections; si++) {
            if (sect_headers_off + (si + 1) * SECTHDR_SIZE > xbe_size) break;

            const uint8_t *sh = xbe + sect_headers_off + si * SECTHDR_SIZE;
            DWORD sec_va       = *(const DWORD *)(sh + SECTHDR_VA);
            DWORD sec_vsize    = *(const DWORD *)(sh + SECTHDR_VSIZE);
            DWORD sec_raw_off  = *(const DWORD *)(sh + SECTHDR_RAW_OFFSET);
            DWORD sec_raw_size = *(const DWORD *)(sh + SECTHDR_RAW_SIZE);
            DWORD sec_name_va  = *(const DWORD *)(sh + SECTHDR_NAME_ADDR);

            /* Read section name from XBE header */
            const char *sec_name = "?";
            DWORD name_off = sec_name_va - base_addr;
            if (name_off < xbe_size && name_off + 8 <= xbe_size)
                sec_name = (const char *)(xbe + name_off);

            /* Validate: section must fit within our 64MB mapped region */
            if (sec_va < XBOX_BASE_ADDRESS || sec_va + sec_vsize > XBOX_TOTAL_RAM)
                continue;

            /* Determine copy size (raw_size may exceed vsize due to alignment) */
            DWORD copy_size = (sec_raw_size < sec_vsize) ? sec_raw_size : sec_vsize;

            /* Zero the full virtual size first (handles BSS) */
            memset(XBOX_VA(sec_va), 0, sec_vsize);

            /* Copy initialized data from XBE */
            if (copy_size > 0 && sec_raw_off + copy_size <= xbe_size) {
                memcpy(XBOX_VA(sec_va), xbe + sec_raw_off, copy_size);
            }

            sections_loaded++;
            total_bytes += copy_size;

            fprintf(stderr, "  [%2u] %-12s VA=0x%08X vsize=%-8u raw=0x%08X rsize=%-8u%s\n",
                    si, sec_name, sec_va, sec_vsize, sec_raw_off, sec_raw_size,
                    (sec_raw_size < sec_vsize) ? " (BSS)" : "");
        }

        fprintf(stderr, "  Loaded %d/%u sections (%zu bytes total)\n",
                sections_loaded, num_sections, total_bytes);
    }

    /*
     * Parse the kernel thunk table address from the XBE header.
     * The XBE stores KernelImageThunkAddress at offset 0x0158, XOR-encrypted.
     * The key differs between retail and debug XBEs, and there is no flag
     * saying which was used -- decode with both and keep whichever lands in
     * the mapped address range (this is what tools/xbe_parser does).
     *
     * Debug XBEs are not an edge case here: they are the builds most worth
     * recompiling, since they still carry assert strings and symbols. Halo's
     * cachebeta.xbe is one, and assuming the retail key decoded its thunk
     * table to 0xB4F98174 instead of 0x00253090, which silently fell back to
     * the compile-time default and resolved 0 of 378 kernel imports.
     */
    if (xbe_size >= 0x015C) {
        uint32_t thunk_raw = *(const uint32_t *)(xbe + 0x0158);
        uint32_t thunk_retail = thunk_raw ^ 0x5B6D40B6;  /* retail XOR key */
        uint32_t thunk_debug  = thunk_raw ^ 0xEFB1F152;  /* debug XOR key  */
        uint32_t thunk_va;

        if (thunk_retail >= XBOX_BASE_ADDRESS && thunk_retail < XBOX_TOTAL_RAM) {
            thunk_va = thunk_retail;
        } else {
            thunk_va = thunk_debug;
        }

        /* Validate: thunk VA should be within our mapped region */
        if (thunk_va >= XBOX_BASE_ADDRESS && thunk_va < XBOX_TOTAL_RAM) {
            /* Count thunk entries by scanning until we hit 0 */
            uint32_t thunk_count = 0;
            /* XBOX_KERNEL_THUNK_TABLE_SIZE, not 366: the kernel exports 378
             * slots, and kernel.h notes 366 is short by 12. A title importing
             * a high ordinal would have had its table truncated here. */
            for (uint32_t t = 0; t < XBOX_KERNEL_THUNK_TABLE_SIZE; t++) {
                uint32_t entry = *(volatile uint32_t *)((uintptr_t)(thunk_va + t * 4) + g_memory_offset);
                if (entry == 0) break;
                thunk_count++;
            }
            xbox_kernel_set_thunk_address(thunk_va, thunk_count);
            fprintf(stderr, "  Kernel thunks: %u entries at Xbox VA 0x%08X\n",
                    thunk_count, thunk_va);
        } else {
            fprintf(stderr, "  WARNING: kernel thunk VA 0x%08X out of range (raw=0x%08X)\n",
                    thunk_va, thunk_raw);
        }
    }

    /*
     * NOTE: .rdata is NOT set read-only.
     * VirtualProtect rounds to page boundaries, and the .rdata end (0x003B2454)
     * and .data start (0x003B2360) share the same 4KB page (0x003B2000-0x003B2FFF).
     * Making .rdata read-only also makes the first ~0xCA0 bytes of .data read-only,
     * which causes game initialization code to fault when writing to .data globals
     * in that overlap range.
     */
    (void)old_protect;

    #undef XBOX_VA


    /* Set the global offset for recompiled code MEM macros */
    g_xbox_mem_offset = g_memory_offset;

    /*
     * Initialize the Xbox stack for recompiled code.
     * The stack area lives at XBOX_STACK_BASE in Xbox address space.
     * g_esp is the global stack pointer shared by all translated functions.
     */
    g_esp = XBOX_STACK_TOP;
    fprintf(stderr, "  Stack: %u KB at Xbox VA 0x%08X (ESP = 0x%08X)\n",
            XBOX_STACK_SIZE / 1024, XBOX_STACK_BASE, g_esp);

    /*
     * Populate the fake Thread Information Block (TIB) at Xbox VA 0x0.
     *
     * The original Xbox code uses fs:[offset] to read per-thread data,
     * but the recompiler drops the fs: segment prefix and generates
     * MEM32(offset) instead. Since we mapped low memory (0x0-0xFFFF),
     * we populate the TIB fields that game code accesses:
     *
     *   fs:[0x00] = SEH exception list (-1 = end of chain)
     *   fs:[0x04] = stack base (top of stack)
     *   fs:[0x08] = stack limit (bottom of stack)
     *   fs:[0x18] = self pointer (TIB address)
     *   fs:[0x20] = KPCR Prcb pointer (→ fake structure)
     *   fs:[0x28] = TLS / RW engine context pointer
     *
     * We use free space in the BSS area for the fake structures.
     */
    {
        #define XBOX_VA(va) ((void *)((uintptr_t)(va) + g_memory_offset))
        #define MEM32_INIT(va, val) (*(uint32_t *)XBOX_VA(va) = (uint32_t)(val))

        /* Fake TIB at address 0x0 */
        MEM32_INIT(0x00, 0xFFFFFFFF);       /* SEH: end of chain */
        MEM32_INIT(0x04, XBOX_STACK_TOP);   /* Stack base (high address) */
        MEM32_INIT(0x08, XBOX_STACK_BASE);  /* Stack limit (low address) */
        MEM32_INIT(0x18, 0x00000000);       /* Self pointer (TIB at VA 0) */

        /*
         * fs:[0x20] - On Xbox KPCR, this is the Prcb pointer.
         * Game code reads [fs:[0x20] + 0x250] which on the real Xbox
         * accesses a D3D cache structure. We set it to 0 so the read
         * at offset 0x250 returns 0, causing the cache init to be skipped.
         */
        MEM32_INIT(0x20, 0x00000000);

        /*
         * fs:[0x28] - Thread local storage / RW engine context.
         * The RW engine reads [fs:[0x28] + 0x28] to get a pointer
         * to its data area. We allocate a fake structure at 0x00760000
         * (in the BSS area) and a data buffer at 0x00700000.
         */
        #define FAKE_TLS_VA     0x00760000  /* Fake TLS structure (in BSS) */
        #define FAKE_RWDATA_VA  0x00700000  /* RW engine data area (in BSS) */

        MEM32_INIT(0x28, FAKE_TLS_VA);
        /* TLS[0x28] = pointer to RW data area */
        MEM32_INIT(FAKE_TLS_VA + 0x28, FAKE_RWDATA_VA);

        fprintf(stderr, "  TIB: fake TIB at VA 0x0, TLS at 0x%08X, RW data at 0x%08X\n",
                FAKE_TLS_VA, FAKE_RWDATA_VA);

        #undef FAKE_TLS_VA
        #undef FAKE_RWDATA_VA
        #undef MEM32_INIT
        #undef XBOX_VA
    }

    /*
     * Relocated fake KPCR/TIB page.
     *
     * The recompiler translates fs: reads (KPCR/TIB) to flat reads at
     * 0x0-0xFFF, and XBOX_PTR redirects those to FAKE_KPCR_VA (see
     * recomp_types.h). The real low page (0x0-0xFFF) is ordinary RAM the
     * game uses through 64 MB-mirror aliases (0x80000000-0x80001000): the
     * XPP push-buffer allocator (sub_0035B4A1) bumps down from 0x80001000
     * and 0xCCCCCCCC-fills it, which clobbered a fake TIB at VA 0x0
     * (m250=0xCCCCCCCC -> [[0x20]+0x250] ICALL crashed). On the real Xbox
     * the KPCR sits above the XPP range, so there is no clash.
     */
    {
        #ifndef FAKE_KPCR_VA
        #define FAKE_KPCR_VA 0x00762000u
        #endif
        #define XBOX_VA(va) ((void *)((uintptr_t)(va) + g_memory_offset))
        #define MEM32_INIT(va, val) (*(uint32_t *)XBOX_VA(va) = (uint32_t)(val))
        #define FAKE_TLS_VA 0x00760000

        /* Fake KPCR page: keep the fields the game's fs: reads expect. */
        memset(XBOX_VA(FAKE_KPCR_VA), 0, 0x1000);
        MEM32_INIT(FAKE_KPCR_VA + 0x00, 0xFFFFFFFF);   /* SEH end of chain */
        MEM32_INIT(FAKE_KPCR_VA + 0x04, XBOX_STACK_TOP);
        MEM32_INIT(FAKE_KPCR_VA + 0x08, XBOX_STACK_BASE);
        MEM32_INIT(FAKE_KPCR_VA + 0x18, 0x00000000);   /* self pointer */
        MEM32_INIT(FAKE_KPCR_VA + 0x20, 0x00000000);   /* Prcb -> 0: D3D cache skipped */
        MEM32_INIT(FAKE_KPCR_VA + 0x24, 0x00000000);   /* IRQL */
        MEM32_INIT(FAKE_KPCR_VA + 0x28, FAKE_TLS_VA);  /* TLS pointer */
        MEM32_INIT(FAKE_KPCR_VA + 0x250, 0x00000000);  /* D3D cache (skip) */
        /* Fake XPP device register page (guest 0xFED00000, redirected by
         * XBOX_PTR): the XPP push-buffer driver polls the busy bit at +0x04
         * and writes config at +0x48/+0x4C/+0x50; on real hardware this is
         * device MMIO. Zeroed = idle so the sub_0035A1DE wait passes. */
        #ifndef FAKE_XPP_VA
        #define FAKE_XPP_VA 0x00763000u
        #endif
        memset(XBOX_VA(FAKE_XPP_VA), 0, 0x1000);


        #undef FAKE_TLS_VA
        #undef MEM32_INIT
        #undef XBOX_VA
    }

    /*
     * The high half (guest 0x80000000-0x8C000000) is covered by mirror
     * views of the base 64 MB region (see below), exactly like the real
     * Xbox 26-bit address bus: 0x80010000 aliases 0x00010000, 0x84000000
     * aliases 0x00000000, etc. A separate VirtualAlloc region previously
     * occupied the mirror addresses (error 487) and made the DICE arena
     * land on unmapped memory at the region end (0x84000000). The fake
     * kernel PE is written through mirror 31 after the views map.
     */

    /*
     * Initialize the DICE software memory map (guest 0x46E6C0).
     *
     * The DICE engine addresses memory in 16-bit segments (high 16 bits of
     * the address) mapped through this table. With a zeroed table the mapper
     * (sub_0009C317 / callback 0x9D33E) maps identity, so the arena at
     * guest 0x81000000 (segment 0x8100) is never wrapped to physical RAM and
     * faults. On the real Xbox the 64 MB bus wrap maps segment 0x8100 to
     * segment 0x0100 (physical 16 MB). Provide that map here as a stand-in
     * for sub_0009E74B's setup (which is not reached before the crash).
     *
     * Table layout (all little-endian):
     *   +0x00 u16 range0_lo, +0x02 u16 range0_hi, +0x04 u32 range0_offset
     *   +0x06 u16 range1_lo, +0x08 u16 range1_hi, +0x0A u32 range1_offset
     */
    {
        #define MEMAP_BASE 0x46E6C0u
        #define MEMAP_WRITE16(off, v) \
            (*(volatile uint16_t *)((uintptr_t)(MEMAP_BASE + off) + g_memory_offset) = (uint16_t)(v))
        #define MEMAP_WRITE32(off, v) \
            (*(volatile uint32_t *)((uintptr_t)(MEMAP_BASE + off) + g_memory_offset) = (uint32_t)(v))
        MEMAP_WRITE16(0x00, 0x0000);           /* range0_lo */
        MEMAP_WRITE16(0x02, 0x7FFF);           /* range0_hi */
        MEMAP_WRITE32(0x04, 0x00000000);       /* range0_offset (identity) */
        MEMAP_WRITE16(0x06, 0x8000);           /* range1_lo */
        MEMAP_WRITE16(0x08, 0xFFFF);           /* range1_hi */
        MEMAP_WRITE32(0x0A, 0xFFFF8000);       /* range1_offset (-0x8000 wrap) */
        fprintf(stderr, "  DICE memory map: initialized at guest 0x%08X "
            "(segments 0x8000-0xFFFF wrap to 0x0000-0x7FFF)\n", MEMAP_BASE);
        #undef MEMAP_BASE
        #undef MEMAP_WRITE16
        #undef MEMAP_WRITE32
    }

    /* Initialize the dynamic heap. */
    fprintf(stderr, "  Heap: %u MB at Xbox VA 0x%08X-0x%08X\n",
            XBOX_HEAP_SIZE / (1024 * 1024), XBOX_HEAP_BASE,
            XBOX_HEAP_BASE + XBOX_HEAP_SIZE);

    /*
     * Map mirror views of the 64 MB region.
     *
     * On retail Xbox, physical RAM wraps at 64 MB due to the 26-bit
     * address bus. Address 0x04070000 reads the same data as 0x00070000.
     * The RenderWare engine's memory walker crosses 64 MB and accesses
     * mirrored data for an extended walk covering 256+ MB of virtual
     * addresses. Game init code also writes large data structures past
     * 64 MB that on real hardware wrap into physical RAM.
     *
     * We map additional views of the SAME file mapping section at 64 MB
     * intervals. All views alias the same physical pages, so reads and
     * writes at any mirror address correctly access the base data.
     */
    {
        int mirrors_ok = 0;
        for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
            uintptr_t mirror_base = (uintptr_t)g_memory_base +
                                    (uintptr_t)(m + 1) * g_memory_size;
            g_mirror_views[m] = MapViewOfFileEx(
                g_mapping_handle,
                FILE_MAP_ALL_ACCESS,
                0, 0,
                g_memory_size,
                (LPVOID)mirror_base
            );
            if (g_mirror_views[m]) {
                mirrors_ok++;
            } else {
                fprintf(stderr, "  Mirror %d: FAILED at %p (error %lu)\n",
                        m + 1, (void *)mirror_base, GetLastError());
                /* Diagnose the address so a view-mapping failure is explainable. */
                MEMORY_BASIC_INFORMATION mbi;
                if (VirtualQuery((LPCVOID)mirror_base, &mbi, sizeof(mbi))) {
                    fprintf(stderr, "    region: base=0x%p size=0x%zX "
                        "state=0x%X type=0x%X\n",
                        mbi.BaseAddress, mbi.RegionSize, mbi.State, mbi.Type);
                }
            }
        }
        fprintf(stderr, "  RAM mirror: %d/%d views mapped (covers %d MB)\n",
                mirrors_ok, XBOX_NUM_MIRRORS,
                (int)((mirrors_ok + 1) * g_memory_size / (1024 * 1024)));
    }

    /* Fake kernel PE at guest 0x80010000 (mirror 31, which aliases guest
     * 0x00010000 = the XBE header). RenderWare's Xbox driver code
     * (xbcache.c) reads MEM32(0x8001003C) to parse the kernel PE header
     * for CPU cache line sizing; we provide a minimal PE with 0 sections
     * so the parse gracefully skips the cache init. The overlay touches
     * only XBE header offsets 0x00-0xD4 (magic + signature), which nothing
     * re-reads at runtime; certificate/section fields at 0x104+ survive.
     * On real hardware the kernel image occupies the same wrapped pages. */
    if (g_mirror_views[31]) {
        uint8_t *kp = (uint8_t *)g_mirror_views[31] + 0x10000;
        *(uint16_t *)(kp + 0x00) = 0x5A4D;                    /* "MZ" */
        *(uint32_t *)(kp + 0x3C) = 0x80;                      /* e_lfanew */
        *(uint32_t *)(kp + 0x80) = 0x00004550;                /* "PE\0\0" */
        *(uint16_t *)(kp + 0x84) = 0x14C;                     /* Machine i386 */
        *(uint16_t *)(kp + 0x86) = 0;                         /* NumberOfSections */
        *(uint16_t *)(kp + 0x94) = 0xE0;                      /* SizeOfOptionalHeader */
        *(uint16_t *)(kp + 0x96) = 0x0102;                    /* Characteristics */
        *(uint16_t *)(kp + 0x98) = 0x10B;                     /* Optional magic PE32 */
        *(uint32_t *)(kp + 0xD0) = 0x01000000u;               /* SizeOfImage */
        fprintf(stderr, "  Fake kernel PE: written at guest 0x80010000 "
            "(mirror 31, aliases XBE header at 0x10000)\n");
    } else {
        fprintf(stderr, "  WARNING: mirror 31 (guest 0x80000000) not "
            "mapped; fake kernel PE unavailable\n");
    }

    fprintf(stderr, "xbox_MemoryLayoutInit: complete\n");
    return TRUE;
}

void xbox_MemoryLayoutShutdown(void)
{
    /* Unmap mirror views first */
    for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
        if (g_mirror_views[m]) {
            UnmapViewOfFile(g_mirror_views[m]);
            g_mirror_views[m] = NULL;
        }
    }
    /* Unmap base view */
    if (g_memory_base) {
        UnmapViewOfFile(g_memory_base);
        g_memory_base = NULL;
        g_memory_size = 0;
    }
    /* Close file mapping handle */
    if (g_mapping_handle) {
        CloseHandle(g_mapping_handle);
        g_mapping_handle = NULL;
    }
    fprintf(stderr, "xbox_MemoryLayoutShutdown: released\n");
}

BOOL xbox_IsXboxAddress(uintptr_t address)
{
    return (address >= XBOX_BASE_ADDRESS &&
            address < XBOX_BASE_ADDRESS + g_memory_size);
}

void *xbox_GetMemoryBase(void)
{
    return g_memory_base;
}

ptrdiff_t xbox_GetMemoryOffset(void)
{
    return g_memory_offset;
}

/* ── Dynamic heap allocator ────────────────────────────────
 *
 * Simple bump allocator backing the kernel-side Nt/Ke/Mm allocation
 * bridges in kernel_bridge.c (pool memory, IRPs, device/thread objects,
 * MmAllocateContiguousMemory and similar) - NOT the guest's own CRT/user
 * heap, which the recompiled game manages itself (sub_000858F3 and
 * friends in src/main.c, driven by the game's own malloc/HeapAlloc calls)
 * starting at the same XBOX_HEAP_BASE and growing upward.
 *
 * Both allocators used to start at XBOX_HEAP_BASE and grow upward, so as
 * soon as either grew past a few hundred KB they handed out addresses the
 * other already owned - this allocator's memset-on-alloc would then zero
 * live CRT-heap free-list nodes out from under it. Confirmed via a
 * hardware write watchpoint (MM3_WATCH_GUEST) on a free-list node's
 * `next` field: the corrupting write's ecx (byte count) and the
 * subsequent [HEAP] log line matched a 1 MB xbox_HeapAlloc request whose
 * range enclosed the node's live address, at a point where the CRT heap
 * had already grown into that same range. Grow this allocator downward
 * from the top of RAM instead, so the two only collide on genuine
 * exhaustion of the shared 64 MB budget, not on ordinary growth from
 * opposite ends of the same starting address. No free support (bump-only
 * for now, in either direction).
 */
static uint32_t g_heap_next = XBOX_HEAP_BASE + XBOX_HEAP_SIZE;

static int g_heap_alloc_count = 0;

uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment)
{
    uint32_t result;
    if (alignment < 4) alignment = 4;

    /* Enforce minimum allocation size.
     * The Xbox D3D8 code sometimes computes resource sizes from GPU
     * capabilities that return 0 (since we don't have real NV2A hardware),
     * resulting in zero-size allocations. With a bump allocator, these all
     * return the same address, causing overlapping structures. Enforce a
     * minimum of 4096 bytes so each allocation gets its own memory. */
    if (size < 4096) size = 4096;

    /* Align the *end* of the block downward from the current top, then
     * derive its start - mirrors the old upward bump but growing the
     * other way. */
    if ((uint64_t)size > (uint64_t)g_heap_next) {
        result = 0; /* underflow guard; falls into the OOM path below */
    } else {
        result = (g_heap_next - size) & ~(alignment - 1);
    }

    if (result < XBOX_HEAP_BASE || (uint64_t)result + (uint64_t)size >
        (uint64_t)XBOX_HEAP_BASE + XBOX_HEAP_SIZE) {
        /* 64-bit bound check: the 32-bit result+size wraps for requests
         * near 4 GB, which previously bypassed this check and let memset
         * walk the mirror region to the 0xA4000000 ceiling and fault.
         * Reject oversized requests; the game pool checks the NTSTATUS. */
        fprintf(stderr, "xbox_HeapAlloc: out of memory (requested %u, result 0x%08X, used %u/%u)\n",
                size, result,
                (uint32_t)(XBOX_HEAP_BASE + XBOX_HEAP_SIZE - g_heap_next),
                XBOX_HEAP_SIZE);
        /* run-371 (read-only): the requested sizes (0xF80290 / 0x104D2C0 /
         * 0x104D2A0) are stale heap-region pointers passed as sizes. Pin the
         * guest caller: [g_esp] = guest return address of the thunk call,
         * native bt names the bridge, penter ring names the recomp callers. */
        {
            uint32_t ret = (g_esp >= 0x1000u && g_esp < 0x04000000u)
                ? *(volatile uint32_t *)((uintptr_t)g_esp + g_xbox_mem_offset) : 0;
            void *bt[8];
            USHORT bn = RtlCaptureStackBackTrace(1, 8, bt, NULL);
            fprintf(stderr, "  [HEAPOOM] ic=%llu size=%u align=%u esp=0x%08X guest_ret=0x%08X\n",
                    (unsigned long long)g_icall_count, size, alignment, g_esp, ret);
            fprintf(stderr, "  [HEAPOOM] args:");
            for (int ai = 0; ai < 8; ai++) {
                uint32_t a = 0;
                if (g_esp >= 0x1000u && g_esp + 4u + (uint32_t)ai * 4u < 0x04000000u)
                    a = *(volatile uint32_t *)((uintptr_t)g_esp + 4u +
                        (uint32_t)ai * 4u + g_xbox_mem_offset);
                fprintf(stderr, " %08X", a);
            }
            fprintf(stderr, "\n  [HEAPOOM] nat:");
            for (USHORT bi = 0; bi < bn; bi++)
                fprintf(stderr, " %zX", (uintptr_t)bt[bi] -
                    (uintptr_t)GetModuleHandle(NULL));
            fprintf(stderr, "\n  [HEAPOOM] ring:");
            extern volatile uintptr_t g_penter_trace[256];
            extern volatile uint32_t g_penter_trace_idx;
            for (int ri = 0; ri < 12; ri++) {
                uint32_t idx = (g_penter_trace_idx - 1 - (uint32_t)ri) & 255u;
                fprintf(stderr, " %zX", g_penter_trace[idx]);
            }
            fprintf(stderr, "\n");
            fflush(stderr);
        }
        return 0;
    }

    g_heap_next = result;

    /* Zero-fill the allocated block (Xbox memory is always zeroed) */
    memset((void *)((uintptr_t)result + g_memory_offset), 0, size);

    g_heap_alloc_count++;
    fprintf(stderr, "  [HEAP] #%d: size=%u align=%u → 0x%08X..0x%08X (used %u/%u)\n",
            g_heap_alloc_count, size, alignment, result, result + size,
            (uint32_t)(XBOX_HEAP_BASE + XBOX_HEAP_SIZE - g_heap_next), XBOX_HEAP_SIZE);
    fflush(stderr);

    return result;
}

void xbox_HeapFree(uint32_t xbox_va)
{
    /* No-op for bump allocator */
    (void)xbox_va;
}

HANDLE xbox_GetMappingHandle(void)
{
    return g_mapping_handle;
}
