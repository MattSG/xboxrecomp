/*
 * Memory regressions: the guest heap and address space, checked against the
 * bugs they had.
 *
 * Runs the real memory layout on the small synthetic XBE from
 * tools/conformance, so no title and no game files are needed. The bridge is
 * called through the thunk dispatcher, the path a title's kernel calls take.
 *
 * One process per mode, because the switches are read once:
 *
 *   default       No switch set. Pins what every title gets today, so a change
 *                 that alters the default shows up here.
 *   model-churn     No switch set. Model-sized temporary buffers mixed with
 *                 small retained objects must not consume the entire heap.
 *   heap-reclaim  RECOMP_HEAP_RECLAIM=1 additionally enables NT release and
 *                 decommit. Reuse/splitting and contiguous frees are default.
 *
 *   1. A small request after a large free takes only what it needs. Reuse used
 *      to hand over the whole block, so a 16-byte request could take 2 MB.
 *   2. Freeing three neighbours in an awkward order merges all three. Merging
 *      left an empty slot behind that later frees did not step over, so the
 *      third block never joined the other two.
 *   3. NtFreeVirtualMemory(MEM_RELEASE) on memory NtAllocateVirtualMemory took
 *      from the heap gives it back. It handed the 32-bit guest slot to the host
 *      VirtualFree as a pointer, which failed, so none of it ever returned.
 *   5. MEM_DECOMMIT then MEM_COMMIT on heap memory gives zeroed pages, as on
 *      the console. Both were no-ops, so the old contents came back.
 *   6. MmFreeContiguousMemory gives a contiguous block back and the next
 *      request of that size reuses it; contiguous pages are reused in both modes.
 *
 *   ext-vma       RECOMP_EXT_VMA=1. A title that reserves a specific address
 *                 above RAM gets it, can commit inside it and use the memory,
 *                 is told the truth when it asks, and is refused cleanly for
 *                 an address in the unmapped slots above RAM.
 *   ext-vma-128   The same switch on a 128 MB map, where the slots above RAM
 *                 already reach past user space. The tracker must stay out of the way
 *                 rather than reserve a range of negative size.
 *   reserve-heap  Retail RAM on a 128 MB map. Above-RAM reservations keep
 *                 exact ownership through partial commit and no-op release,
 *                 and both allocators exclude the other's issued blocks.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include "guest_vmem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Provided by the generated title; this harness has none. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern RECOMP_TLS uint32_t g_eax, g_esp;

enum { S_ALLOC, S_FREE, S_QUERY, N_SLOTS };
static const uint32_t ORD[N_SLOTS] = {
    184,   /* NtAllocateVirtualMemory */
    199,   /* NtFreeVirtualMemory */
    217,   /* NtQueryVirtualMemory */
};

static uint32_t scratch;                 /* guest VA of a 64 KB block */
#define THUNK_VA (scratch)
#define STACK_VA (scratch + 0x1000)
#define OUT_VA   (scratch + 0x2000)

static uint32_t slot_va[N_SLOTS];
static int failures;

static uint32_t *G(uint32_t va)
{
    return (uint32_t *)((uintptr_t)va + xbox_GetMemoryOffset());
}

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-66s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

static uint32_t call(int slot, int nargs, const uint32_t *args)
{
    uint32_t *sp = G(STACK_VA);
    int i;

    sp[0] = 0xBEEF0001u;
    for (i = 0; i < nargs; i++)
        sp[1 + i] = args[i];
    g_esp = STACK_VA;
    g_eax = 0xDEADBEEFu;
    recomp_lookup_kernel(slot_va[slot])();
    return g_eax;
}

/* NtAllocateVirtualMemory(&base, 0, &size, type, RW). On return *base and *size
 * hold what the kernel wrote back. */
static uint32_t nt_alloc_at(uint32_t *base, uint32_t *size, uint32_t type)
{
    uint32_t args[5], st;

    *G(OUT_VA) = *base;
    *G(OUT_VA + 4) = *size;
    args[0] = OUT_VA; args[1] = 0; args[2] = OUT_VA + 4;
    args[3] = type; args[4] = 0x04;
    st = call(S_ALLOC, 5, args);
    *base = *G(OUT_VA);
    *size = *G(OUT_VA + 4);
    return st;
}

/* Reserve and commit `size` bytes anywhere. */
static uint32_t nt_alloc(uint32_t size, uint32_t *out_base)
{
    uint32_t base = 0;
    uint32_t st = nt_alloc_at(&base, &size, 0x3000);

    *out_base = base;
    return st;
}

/* NtFreeVirtualMemory(&base, &size = 0, MEM_RELEASE). */
static uint32_t nt_release(uint32_t base)
{
    uint32_t args[3];

    *G(OUT_VA) = base;
    *G(OUT_VA + 4) = 0;
    args[0] = OUT_VA; args[1] = OUT_VA + 4; args[2] = 0x8000;
    return call(S_FREE, 3, args);
}

/* NtQueryVirtualMemory(base, &info); fills mbi[7] with the guest
 * MEMORY_BASIC_INFORMATION fields. */
static uint32_t nt_query(uint32_t base, uint32_t mbi[7])
{
    uint32_t args[2], st, i;

    args[0] = base; args[1] = OUT_VA + 0x100;
    st = call(S_QUERY, 2, args);
    for (i = 0; i < 7; i++)
        mbi[i] = *G(OUT_VA + 0x100 + 4 * i);
    return st;
}

static unsigned char *load_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long n;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static void set_env(const char *name, const char *value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    if (*value)
        setenv(name, value, 1);
    else
        unsetenv(name);
#endif
}

int main(int argc, char **argv)
{
    /* This fixture owns fixed raw scratch addresses below the export page. */
    if (!xbox_ConfigureKernelData(0x00740000u)) return 90;

    const char *mode = argc > 1 ? argv[1] : "default";
    const char *path = argc > 2 ? argv[2] : "tools/conformance/test.xbe";
    int scene_churn = !strcmp(mode, "scene-churn") || !strcmp(mode, "scene-churn-baseline");
    int reclaim = !strcmp(mode, "heap-reclaim") || !strcmp(mode, "scene-churn");
    int ext128 = !strcmp(mode, "ext-vma-128");
    int ext = ext128 || !strcmp(mode, "ext-vma");
    int reserve_heap = !strcmp(mode, "reserve-heap");
    int model_churn = !strcmp(mode, "model-churn");
    int stack_config = !strcmp(mode, "stack-config");
    int stack_bad_base = !strcmp(mode, "stack-bad-base");
    int stack_bad_size = !strcmp(mode, "stack-bad-size");
    int stack_unbacked = !strcmp(mode, "stack-unbacked");
    int stack_invalid = stack_bad_base || stack_bad_size || stack_unbacked;
    int stack_layout = !strcmp(mode, "stack-layout");
    int high_image = !strcmp(mode, "image-high");
    int bad_raw = !strcmp(mode, "image-bad-raw");
    int bad_table = !strcmp(mode, "image-bad-table");
    int short_header = !strcmp(mode, "image-short-header");
    int bad_image = !strcmp(mode, "image-bad-range") || bad_raw || bad_table || short_header;
    int tls_bad_directory = !strcmp(mode, "tls-bad-directory");
    int tls_bad_range = !strcmp(mode, "tls-bad-range");
    int tls_size_overflow = !strcmp(mode, "tls-size-overflow");
    int tls_invalid = tls_bad_directory || tls_bad_range || tls_size_overflow;
    int tls_layout = !strcmp(mode, "tls-layout") || tls_invalid;
    size_t xbe_len = 0;
    unsigned char *xbe;
    char d[160];
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!reclaim && !ext && !reserve_heap && !model_churn && !scene_churn && !tls_layout && !high_image && !bad_image && !stack_layout && !stack_config && !stack_invalid && strcmp(mode, "default")) {
        printf("unknown mode '%s'\n", mode);
        return 2;
    }
    if (reclaim || reserve_heap)
        set_env("RECOMP_HEAP_RECLAIM", "1");
    else if (scene_churn)
        set_env("RECOMP_HEAP_RECLAIM", "");
    if (ext)
        set_env("RECOMP_EXT_VMA", "1");
    if (ext128 || high_image || bad_image)
        xbox_SetTotalRam(XBOX_DEVKIT_RAM);
    if (reserve_heap || scene_churn || stack_config)
        xbox_SetMapSize(XBOX_DEVKIT_RAM);

    xbe = load_file(path, &xbe_len);
    if (!xbe) {
        printf("cannot read %s -- run: python tools/conformance/mkxbe.py\n", path);
        return 2;
    }
    if (tls_layout || high_image || bad_image || stack_layout || stack_bad_base) {
        uint32_t section_va = (stack_layout || stack_bad_base) ? 0x780000u : high_image ? 0x04010000u : (!strcmp(mode, "image-bad-range") ? 0xFFFFF000u : 0x770000u);
        /* Add a data section in the former scratch range without changing text. */
        uint32_t base = *(uint32_t *)(xbe + 0x104);
        uint32_t count = *(uint32_t *)(xbe + 0x11C);
        uint32_t table = *(uint32_t *)(xbe + 0x120) - base;
        uint32_t raw = (uint32_t)((xbe_len + 4095u) & ~4095u);
        uint32_t *section;
        uint8_t *data;
        if (count != 1 || table + 112u > 0x470u) return 2;
        xbe = realloc(xbe, raw + 0x2000u);
        if (!xbe) return 2;
        memset(xbe + xbe_len, 0, raw + 0x2000u - xbe_len);
        xbe_len = raw + 0x2000u;
        section = (uint32_t *)(xbe + table + 56u);
        memset(section, 0, 56);
        section[0] = 2; section[1] = section_va; section[2] = 0x2000;
        section[3] = raw; section[4] = 0x2000; section[5] = base + 0x470;
        memcpy(xbe + 0x470, ".tls", 5);
        *(uint32_t *)(xbe + 0x11C) = 2;
        *(uint32_t *)(xbe + 0x10C) = section_va + 0x2000u - base;
        *(uint32_t *)(xbe + 0x12C) = tls_layout ? 0x770A00u : 0;
        data = xbe + raw;
        memset(data, 0xA5, 0x2000);
        memset(data + 0xA00, 0, 24);
        *(uint32_t *)(data + 0xA00) = 0x770100;
        *(uint32_t *)(data + 0xA04) = 0x770400;
        *(uint32_t *)(data + 0xA08) = 0x770A40;
        *(uint32_t *)(data + 0xA10) = 0x30;
        if (tls_bad_directory) *(uint32_t *)(xbe + 0x12C) = UINT32_MAX - 7u;
        if (tls_bad_range) *(uint32_t *)(data + 0xA04) = 0x7700FF;
        if (tls_size_overflow) *(uint32_t *)(data + 0xA10) = UINT32_MAX;
        if (bad_raw) section[3] = 0xFFFFF000u;
        if (bad_table) *(uint32_t *)(xbe + 0x120) = base - 16u;
        if (short_header) xbe_len = 16;
    }
    if (stack_layout) *(uint32_t *)(xbe + 0x130) = 0x3100;
    if (stack_bad_size) *(uint32_t *)(xbe + 0x130) = UINT32_MAX;
    if (stack_bad_base) check(xbox_ConfigureMainStack(0x780000, 0x4000), "overlap is checked against the loaded image", NULL);
    if (stack_unbacked) check(xbox_ConfigureMainStack(0x3FFF000, 0x2000), "backing is checked at initialization", NULL);
    if (stack_config) {
        check(xbox_ConfigureMainStack(0x5000000, 0x300000), "port can request a backed main stack", NULL);
        check(!xbox_ConfigureMainStack(0x5000001, 0x1000) &&
              !xbox_ConfigureMainStack(0, 17) &&
              !xbox_ConfigureMainStack(0xFFFFF000, 0x2000) &&
              !xbox_ConfigureMainStack(0x5000000, 0),
              "unaligned, wrapped and incomplete policy is rejected", NULL);
    }
    BOOL layout_ok = xbox_MemoryLayoutInit(xbe, xbe_len);
    if (tls_invalid || bad_image || stack_invalid) {
        check(!layout_ok, "invalid image/TLS/stack is rejected", NULL);
        if (layout_ok) xbox_MemoryLayoutShutdown();
        check(xbox_GetMemoryBase() == NULL && xbox_GetKernelMemoryBase() == NULL,
              "invalid TLS leaves no owned memory mapping", NULL);
        free(xbe);
        return failures ? 1 : 0;
    }
    if (!layout_ok) {
        puts("xbox_MemoryLayoutInit failed");
        return 2;
    }

    check(*G(XBOX_FS_BASE + 0x28) == 0,
          "loader does not install an application context chain", NULL);

    check(*G(XBOX_FS_BASE + 0x20) == XBOX_FS_BASE + XBOX_KPCR_PRCB_OFFSET,
          "main KPCR points at its own inline PrcbData", NULL);

    void *kernel_probe = xbox_GetKernelMemoryBase();
    if (xbox_IsContiguousMemoryAvailable())
        check(kernel_probe == G(0x80010000u), "kernel probe exposes its mapped storage", NULL);
    if (kernel_probe)
        check(*(uint32_t *)((uint8_t *)kernel_probe + 0x3C) == 0,
              "loader does not fabricate a kernel PE offset", NULL);

    if (stack_config) {
        uint32_t heap, reserve, contig;
        check(XBOX_STACK_BASE == 0x5000000 && XBOX_STACK_SIZE == 0x300000,
              "accepted policy survives rejected changes", NULL);
        check(!xbox_ConfigureMainStack(0, 0), "active layout rejects policy changes", NULL);
        check(XBOX_WORKER_STACK_BASE == 0x5300000,
              "callback pool begins after the whole main stack", NULL);
        reserve = xbox_ReserveAlloc(4096, 4096);
        heap = xbox_HeapAlloc(4096, 4096);
        check(reserve >= XBOX_WORKER_STACK_END && heap >= reserve + 4096,
              "above-RAM reservation and heap exclude both stack ranges", NULL);
        contig = xbox_ContiguousAlloc(4096, 4096);
        check(contig && xbox_ContiguousRangeAllocated(contig - 0x80000000u, 4096) &&
              xbox_ReserveRangeAllocated(reserve), "populate positive ownership caches", NULL);
        xbox_MemoryLayoutShutdown();
        check(!XBOX_STACK_BASE && !XBOX_STACK_SIZE && !XBOX_STACK_TOP &&
              xbox_worker_stack_alloc() < 0, "shutdown retires every stack range", NULL);
        check(xbox_ConfigureMainStack(0, 0), "port can restore image-derived defaults", NULL);
        xbox_SetMapSize(0);
        if (!xbox_MemoryLayoutInit(xbe, xbe_len)) { free(xbe); return 1; }
        check(XBOX_STACK_SIZE == 4096 && XBOX_STACK_BASE < 0x5000000,
              "new layout uses default minimum instead of prior port policy", NULL);
        check(!xbox_HeapBlockSize(heap) && !xbox_ReserveRangeAllocated(reserve) &&
              !xbox_ContiguousRangeAllocated(contig - 0x80000000u, 4096),
              "new layout discards old allocations and positive caches", NULL);
        check(xbox_HeapAlloc(4096, 4096) >= XBOX_WORKER_STACK_END,
              "new heap excludes the new stack layout", NULL);
        goto shutdown;
    }

    if (stack_layout) {
        check(XBOX_STACK_BASE >= g_xbox_image_hi,
              "main stack does not cover the loaded image", NULL);
        check(XBOX_STACK_SIZE == 0x4000 && g_esp == XBOX_STACK_TOP,
              "main stack uses page-rounded image stack commit", NULL);
        check(XBOX_WORKER_STACK_BASE >= XBOX_STACK_BASE + XBOX_STACK_SIZE,
              "callback pool does not overlap the main stack", NULL);
        check(*G(0x780000) == 0xA5A5A5A5u,
              "stack setup preserves image data at the former stack base", NULL);
        goto shutdown;
    }

    if (high_image) {
        check(*G(0x04010000u) == 0xA5A5A5A5u && g_xbox_image_hi == 0x04012000u,
              "devkit image section above 64 MiB is loaded", NULL);
        goto shutdown;
    }

    if (tls_layout) {
        uint32_t total = 0x334;
        uint32_t block = *G(XBOX_FS_BASE + 4) - total;
        uint8_t *bytes = (uint8_t *)G(block);
        check(*G(0x770000) == 0xA5A5A5A5 && *G(0x770100) == 0xA5A5A5A5,
              "TLS setup preserves the loaded data section", NULL);
        check(block >= g_xbox_image_hi && *G(block) == block + total,
              "TLS owns storage beyond the image with adjacent thread data", NULL);
        for (unsigned n = 4; n < 0x300; n++)
            if (bytes[n] != 0xA5) { check(0, "TLS template bytes are preserved", NULL); break; }
        for (unsigned n = 0x300; n < total + 64; n++)
            if (bytes[n] != 0) { check(0, "TLS zero-fill and thread data do not overlap", NULL); break; }
        goto shutdown;
    }

    scratch = xbox_HeapAlloc(0x10000, 4096);
    for (i = 0; i < N_SLOTS; i++)
        *G(THUNK_VA + 4 * i) = 0x80000000u | ORD[i];
    xbox_kernel_set_thunk_address(THUNK_VA, N_SLOTS);
    xbox_kernel_bridge_init();
    for (i = 0; i < N_SLOTS; i++) {
        slot_va[i] = *G(THUNK_VA + 4 * i);
        if (!recomp_lookup_kernel(slot_va[i])) {
            printf("ordinal %u did not resolve to a bridge entry point\n", ORD[i]);
            return 2;
        }
    }
    printf("mode: %s\n", mode);
    if (scene_churn) {
        /* Synthetic non-page-aligned blocks exercise the NT release bridge.
         * Keeping the whole group live catches a release no-op even when
         * individual allocation/free pairs appear stable. */
        const uint32_t sizes[] = {0x250030u, 0x2B0030u, 0x190030u, 0x1A0030u,
                                 0x130030u, 0x160030u, 0x180030u, 0x140030u};
        uint32_t blocks[8], first[8] = {0};
        uint32_t guard = xbox_HeapAlloc(4096, 16);
        unsigned completed = 0;
        int reused = 1, intact = 1, released = 1;
        *G(guard) = 0xC0FFEE42u;
        for (unsigned round = 0; round < 24; ++round) {
            unsigned loaded = 0;
            for (; loaded < 8; ++loaded) {
                uint32_t size = sizes[loaded], base = 0;
                /* Commit-only requests choose their base; alternate the Xbox
                 * MEM_NOZERO flag while checking the existing zeroing and
                 * ownership contract of the NT release path. */
                uint32_t st = nt_alloc_at(&base, &size, loaded & 1 ? 0x801000 : 0x1000);
                blocks[loaded] = base;
                if (st || !blocks[loaded]) break;
                if (!round) first[loaded] = blocks[loaded];
                else if (blocks[loaded] != first[loaded]) reused = 0;
                if (*G(blocks[loaded]) || *G(blocks[loaded] + sizes[loaded] - 4)) intact = 0;
                *G(blocks[loaded]) = 0xABCD0000u + loaded;
                *G(blocks[loaded] + sizes[loaded] - 4) = 0x12340000u + loaded;
            }
            for (unsigned j = 0; j < loaded; ++j) {
                if (*G(blocks[j]) != 0xABCD0000u + j ||
                    *G(blocks[j] + sizes[j] - 4) != 0x12340000u + j) intact = 0;
            }
            for (unsigned j = 0; j < loaded; ++j) {
                if (nt_release(blocks[j]) || xbox_HeapBlockSize(blocks[j])) released = 0;
            }
            if (*G(guard) != 0xC0FFEE42u) intact = 0;
            if (loaded != 8) break;
            ++completed;
        }
        snprintf(d, sizeof d, "completed %u of 24 whole-roster scene cycles", completed);
        check(completed == 24, "repeated race/menu NT allocations do not exhaust heap", d);
        check(released, "MEM_RELEASE removes every model from the live heap", NULL);
        check(reused, "model addresses stabilize after the first scene", NULL);
        check(intact, "reused models are zeroed and live neighbours stay intact", NULL);
        goto shutdown;
    }
    if (model_churn) {
        const uint32_t big = 2u * 1024u * 1024u;
        const uint32_t models[] = {0x2B0030u, 0x250030u, 0x190030u, 0x1A0030u};
        uint32_t retained[200] = {0};
        uint32_t a = xbox_HeapAlloc(big, 4096);
        uint32_t guard = xbox_HeapAlloc(4096, 16);
        uint32_t small, page, merged;
        unsigned completed = 0;
        int intact = 1;
        *G(guard) = 0x12345678u;
        xbox_HeapFree(a);
        small = xbox_HeapAlloc(40, 16);
        page = xbox_HeapAlloc(4096, 4096);
        check(small == a && xbox_HeapBlockSize(small) == 40,
              "a 40-byte request does not retain a freed 2 MB block", NULL);
        check(page == a + 4096 && xbox_HeapBlockSize(page) == 4096,
              "page-aligned reuse carves the unaligned free tail", NULL);
        check(*G(guard) == 0x12345678u,
              "splitting preserves the live neighbouring allocation", NULL);
        xbox_HeapFree(small);
        xbox_HeapFree(page);
        merged = xbox_HeapAlloc(big, 4096);
        check(merged == a, "aligned head and tail coalesce after both frees", NULL);
        xbox_HeapFree(merged);

        /* Model-sized temporary buffers followed by persistent small objects
         * reproduce whole-block internal fragmentation without a title leak.
         * Live requested bytes stay below one model plus 12 KB throughout. */
        for (unsigned n = 0; n < 200; ++n) {
            uint32_t size = models[n % 4];
            uint32_t model = xbox_HeapAlloc(size, 16);
            uint32_t tiny_size = 40u + (n % 3u) * 8u;
            if (!model) break;
            *G(model) = 0xA5A50000u + n;
            *G(model + size - 4) = 0x5A5A0000u + n;
            xbox_HeapFree(model);
            retained[n] = xbox_HeapAlloc(tiny_size, 16);
            if (!retained[n]) break;
            if (xbox_HeapBlockSize(retained[n]) != tiny_size) intact = 0;
            *G(retained[n]) = 0xC0FF0000u + n;
            ++completed;
        }
        for (unsigned n = 0; n < completed; ++n) {
            if (*G(retained[n]) != 0xC0FF0000u + n) intact = 0;
            xbox_HeapFree(retained[n]);
        }
        snprintf(d, sizeof d, "completed %u of 200 bounded-live-memory cycles", completed);
        check(completed == 200, "200 model loads with small retained objects avoid OOM", d);
        check(intact, "retained small objects keep exact size and live contents", NULL);
        goto shutdown;
    }
    if (reserve_heap) {
        const uint32_t ram_end = XBOX_TOTAL_RAM;
        uint32_t reserve = 0, size = 0x100000, st, base, heap, later, args[3];
        st = nt_alloc_at(&reserve, &size, 0x2000);
        check(st == 0 && reserve == ram_end && size == 0x100000,
              "large pure reserve starts above retail RAM", NULL);
        if (st != 0 || reserve != ram_end) goto shutdown;

        base = reserve + 0x1000; size = 0x1000;
        st = nt_alloc_at(&base, &size, 0x1000);
        check(st == 0 && base == reserve + 0x1000 && size == 0x1000,
              "partial commit keeps its reserved guest address", NULL);
        *G(reserve) = 0xA5A5A5A5u;
        *G(reserve + 0x1000) = 0xC0FFEE01u;
        *G(reserve + 0xFFFFC) = 0xA5A5A5A5u;

        /* A single request that previously crossed 64 MB must move past the
         * whole reservation before zero filling any byte. */
        heap = xbox_HeapAlloc(ram_end - (scratch + 0x10000) + 0x1000, 4096);
        check(heap == reserve + 0x100000,
              "heap growth crossing 64 MB skips the full reservation", NULL);
        check(*G(reserve) == 0xA5A5A5A5u && *G(reserve + 0x1000) == 0xC0FFEE01u &&
              *G(reserve + 0xFFFFC) == 0xA5A5A5A5u,
              "ordinary heap zero fill preserves committed/reserved bytes", NULL);

        *G(OUT_VA) = reserve + 0x1000; *G(OUT_VA + 4) = 0x1000;
        args[0] = OUT_VA; args[1] = OUT_VA + 4; args[2] = 0x4000;
        st = call(S_FREE, 3, args);
        check(st == 0 && xbox_ReserveRangeAllocated(reserve + 0x1000) &&
              *G(reserve + 0x1000) == 0xC0FFEE01u,
              "reserve decommit remains a no-op and retains exact ownership", NULL);
        st = nt_release(reserve);
        check(st == 0 && xbox_ReserveRangeAllocated(reserve) &&
              xbox_ReserveRangeAllocated(reserve + 0xFFFFF),
              "reserve release remains a no-op even with heap reclaim enabled", NULL);

        later = 0; size = 0x100000;
        st = nt_alloc_at(&later, &size, 0x2000);
        check(st == 0 && later >= heap + xbox_HeapBlockSize(heap),
              "later reservation skips the already-issued ordinary heap", NULL);
        check(!xbox_ReserveRangeAllocated(heap) &&
              !xbox_ReserveRangeAllocated(reserve + 0x100000),
              "skipped ordinary heap bytes are not reservation coverage", NULL);
        goto shutdown;
    }
    *G(0x00070000u) = 0x12345678u;
    *G(0x80070000u) = 0x87654321u;
    check(*G(0x00070000u) == 0x12345678u &&
          *G(0xF0070000u) == 0x87654321u,
          "contiguous and tiled memory stay distinct from RAM", NULL);

    /* Nothing above the mapped range aliases RAM: the first slot above it must
     * fault, not read the byte at the same offset in low memory. The SEH to
     * catch the fault is Windows-only. */
#ifdef _WIN32
    {
        volatile uint32_t *above = G((uint32_t)xbox_GetMappedSize() + 0x70000u);
        int faulted = 0;
        __try {
            (void)*above;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            faulted = 1;
        }
        check(faulted, "a read in the first slot above the map faults", NULL);
    }
#endif

    /* 1. A small request after a large free. */
    if (!ext) {
        const uint32_t big = 2u * 1024 * 1024;
        uint32_t a = xbox_HeapAlloc(big, 4096);
        uint32_t guard = xbox_HeapAlloc(4096, 16);
        uint32_t s1, s2;

        (void)guard;
        xbox_HeapFree(a);
        s1 = xbox_HeapAlloc(16, 16);
        s2 = xbox_HeapAlloc(16, 16);

        snprintf(d, sizeof d, "block is %u bytes, expected 16", xbox_HeapBlockSize(s1));
        check(xbox_HeapBlockSize(s1) == 16, "16-byte request takes 16 bytes of a freed 2 MB", d);
        snprintf(d, sizeof d, "second request at 0x%08X, outside 0x%08X", s2, a);
        check(s2 >= a && s2 < a + big, "the rest of the block is still available", d);
    }

    /* 2. Three neighbours freed so that the third meets an empty slot. */
    if (reclaim) {
        uint32_t x = xbox_HeapAlloc(64, 16), y = xbox_HeapAlloc(64, 16);
        uint32_t z = xbox_HeapAlloc(64, 16), w = xbox_HeapAlloc(64, 16);
        uint32_t m;

        (void)w;
        xbox_HeapFree(x);
        xbox_HeapFree(y);        /* merges into x; y's slot is left empty */
        xbox_HeapFree(z);        /* must step over that slot to reach x */
        m = xbox_HeapAlloc(192, 16);
        snprintf(d, sizeof d, "got 0x%08X, expected 0x%08X", m, x);
        check(y == x + 64 && z == y + 64 && m == x,
              "three freed neighbours merge into one 192-byte block", d);
    }

    /* 3. Releasing NtAllocateVirtualMemory memory returns it. */
    if (reclaim) {
        const uint32_t size = 2u * 1024 * 1024;
        uint32_t b1, b2, st;

        st = nt_alloc(size, &b1);
        check(st == 0 && b1 != 0, "NtAllocateVirtualMemory takes 2 MB from the heap", NULL);
        st = nt_release(b1);
        snprintf(d, sizeof d, "status 0x%08X", st);
        check(st == 0, "NtFreeVirtualMemory(MEM_RELEASE) succeeds", d);
        check(xbox_HeapBlockSize(b1) == 0, "the block is no longer live", NULL);
        st = nt_alloc(size, &b2);
        snprintf(d, sizeof d, "got 0x%08X, expected 0x%08X", b2, b1);
        check(st == 0 && b2 == b1, "the next 2 MB request reuses it", d);
    }

    /* 5. Decommitted heap memory comes back zeroed. */
    if (reclaim) {
        const uint32_t size = 0x10000;
        uint32_t b1, base, sz, args[3], st;

        nt_alloc(size, &b1);
        memset(G(b1), 0xA5, size);
        *G(OUT_VA) = b1 + 0x1000;            /* decommit the second page only */
        *G(OUT_VA + 4) = 0x1000;
        args[0] = OUT_VA; args[1] = OUT_VA + 4; args[2] = 0x4000;   /* MEM_DECOMMIT */
        st = call(S_FREE, 3, args);
        base = b1 + 0x1000; sz = 0x1000;
        nt_alloc_at(&base, &sz, 0x1000);     /* MEM_COMMIT it again */
        snprintf(d, sizeof d, "status 0x%08X, page 0x%08X, neighbour 0x%08X",
                 st, *G(b1 + 0x1000), *G(b1 + 0x2000));
        check(st == 0 && *G(b1 + 0x1000) == 0 && *G(b1 + 0x1FFC) == 0 &&
              *G(b1) == 0xA5A5A5A5u && *G(b1 + 0x2000) == 0xA5A5A5A5u,
              "a decommitted page comes back zeroed, its neighbours kept", d);
    }

    /* 6. Contiguous memory can be given back. */
    if (!ext) {
        extern int xbox_ContiguousFree(uint32_t addr);
        uint32_t c1 = xbox_ContiguousAlloc(0x20000, 4096);
        uint32_t c2;

        snprintf(d, sizeof d, "free said %d", xbox_ContiguousFree(c1));
        c2 = xbox_ContiguousAlloc(0x20000, 4096);
        check(c1 && c2 == c1, "a freed contiguous block is reused in both modes", d);
    }

    /* 4. Above RAM: [0x74000000, 0x7FFE0000) at 64 MB. */
    {
        const uint32_t hi = 0x76000000u;
        uint32_t base, size, st, mbi[7];

        if (ext128) {
            check(!guest_vmem_active(),
                  "128 MB: the slots above RAM reach past user space, tracker stays off", NULL);
        } else if (!ext && !reclaim) {
            base = hi; size = 0x100000;
            st = nt_alloc_at(&base, &size, 0x2000);
            snprintf(d, sizeof d, "status 0x%08X, base 0x%08X", st, base);
            check(!(st == 0 && base == hi),
                  "default: an explicit high base is not granted at that address", d);
        } else if (ext) {
            /* Reserve the exact address. */
            base = hi; size = 0x100000;
            st = nt_alloc_at(&base, &size, 0x2000);
            snprintf(d, sizeof d, "status 0x%08X, base 0x%08X", st, base);
            check(st == 0 && base == hi && size == 0x100000,
                  "reserve at 0x76000000 returns that address", d);
            if (st != 0 || base != hi || size != 0x100000) goto shutdown;

            /* Commit the first 64 KB and use it. */
            base = hi; size = 0x10000;
            st = nt_alloc_at(&base, &size, 0x1000);
            check(st == 0 && base == hi, "commit inside the reservation succeeds", NULL);
            if (st != 0 || base != hi) goto shutdown;
            *G(hi) = 0xC0FFEE01u;
            *G(hi + 0xFFFC) = 0xC0FFEE02u;
            check(*G(hi) == 0xC0FFEE01u && *G(hi + 0xFFFC) == 0xC0FFEE02u,
                  "the committed memory is real", NULL);

            /* Ask about both halves. */
            st = nt_query(hi, mbi);
            snprintf(d, sizeof d, "state 0x%X size 0x%X base 0x%X", mbi[4], mbi[3], mbi[1]);
            check(st == 0 && mbi[0] == hi && mbi[1] == hi && mbi[3] == 0x10000 &&
                  mbi[4] == 0x1000, "query: the committed part is MEM_COMMIT, 64 KB", d);
            st = nt_query(hi + 0x10000, mbi);
            snprintf(d, sizeof d, "state 0x%X size 0x%X base 0x%X", mbi[4], mbi[3], mbi[1]);
            check(st == 0 && mbi[0] == hi + 0x10000 && mbi[1] == hi &&
                  mbi[3] == 0xF0000 && mbi[4] == 0x2000,
                  "query: the rest is MEM_RESERVE, 960 KB, same allocation base", d);

            /* The same range again is a conflict, not a second grant. */
            base = hi; size = 0x100000;
            st = nt_alloc_at(&base, &size, 0x2000);
            snprintf(d, sizeof d, "status 0x%08X", st);
            check(st == 0xC0000018u, "reserving it again is STATUS_CONFLICTING_ADDRESSES", d);

            /* An address in the slots above RAM is not free to hand out. */
            base = 0x10000000u; size = 0x100000;
            st = nt_alloc_at(&base, &size, 0x2000);
            snprintf(d, sizeof d, "status 0x%08X", st);
            check(st == 0xC0000018u, "an address in the slots above RAM is refused", d);
            st = nt_query(0x10000000u, mbi);
            snprintf(d, sizeof d, "state 0x%X", mbi[4]);
            check(st == 0 && mbi[4] == 0x2000,
                  "query: the slots read as reserved, so a scanner moves on", d);

            /* base = 0 still comes from the heap, below the slots. */
            st = nt_alloc(0x10000, &base);
            snprintf(d, sizeof d, "status 0x%08X, base 0x%08X", st, base);
            check(st == 0 && base && base < 0x04000000u,
                  "base = 0 still comes from the heap", d);

            /* Release gives the range back. */
            st = nt_release(hi);
            snprintf(d, sizeof d, "status 0x%08X", st);
            check(st == 0, "release of the reservation succeeds", d);
            st = nt_query(hi, mbi);
            check(st == 0 && mbi[4] == 0x10000, "query: it reads as MEM_FREE again", NULL);
            base = hi; size = 0x100000;
            st = nt_alloc_at(&base, &size, 0x3000);
            check(st == 0 && base == hi && *G(hi) == 0,
                  "reserving it again works, and the memory is zero", NULL);
        }
    }

shutdown: ;
#ifdef _WIN32
    void *host_base = (void *)((uintptr_t)xbox_GetMemoryOffset());
#endif
    xbox_MemoryLayoutShutdown();
    check(xbox_GetKernelMemoryBase() == NULL, "shutdown retires the kernel probe view", NULL);
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION host_info;
    /* Released means no longer ours: free, or already reused by an unrelated
     * allocation -- the renderer's shader-compile threads keep allocating
     * while this runs, and the freed range is a natural place for them to
     * land (often at its very start). The reservation itself spans at least
     * base + mirrors, gigabytes, which nothing else here comes near. */
    check(VirtualQuery(host_base, &host_info, sizeof host_info) &&
          (host_info.State == MEM_FREE || host_info.RegionSize < ((SIZE_T)1 << 30)),
          "shutdown releases the host placeholder reservation", NULL);
#endif
    free(xbe);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
