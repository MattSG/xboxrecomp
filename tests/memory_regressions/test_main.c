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
 *   car-churn     No switch set. Model-sized temporary buffers mixed with
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
    const char *mode = argc > 1 ? argv[1] : "default";
    const char *path = argc > 2 ? argv[2] : "tools/conformance/test.xbe";
    int scene_churn = !strcmp(mode, "scene-churn") || !strcmp(mode, "scene-churn-baseline");
    int reclaim = !strcmp(mode, "heap-reclaim") || !strcmp(mode, "scene-churn");
    int ext128 = !strcmp(mode, "ext-vma-128");
    int ext = ext128 || !strcmp(mode, "ext-vma");
    int reserve_heap = !strcmp(mode, "reserve-heap");
    int car_churn = !strcmp(mode, "car-churn");
    size_t xbe_len = 0;
    unsigned char *xbe;
    char d[160];
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!reclaim && !ext && !reserve_heap && !car_churn && !scene_churn && strcmp(mode, "default")) {
        printf("unknown mode '%s'\n", mode);
        return 2;
    }
    if (reclaim || reserve_heap)
        set_env("RECOMP_HEAP_RECLAIM", "1");
    else if (scene_churn)
        set_env("RECOMP_HEAP_RECLAIM", "");
    if (ext)
        set_env("RECOMP_EXT_VMA", "1");
    if (ext128)
        xbox_SetTotalRam(XBOX_DEVKIT_RAM);
    if (reserve_heap || scene_churn)
        xbox_SetMapSize(XBOX_DEVKIT_RAM);

    xbe = load_file(path, &xbe_len);
    if (!xbe) {
        printf("cannot read %s -- run: python tools/conformance/mkxbe.py\n", path);
        return 2;
    }
    if (!xbox_MemoryLayoutInit(xbe, xbe_len)) {
        puts("xbox_MemoryLayoutInit failed");
        return 2;
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
        /* Sizes from MM3's race -> menu -> car-change OOM report. Unlike
         * car-churn, these blocks return through the NT bridge, not HeapFree.
         * Keeping the whole roster live for each scene catches a release
         * no-op even when individual car changes appear stable. */
        const uint32_t sizes[] = {2441264u, 2818096u, 1638448u, 1720368u,
                                 1261616u, 1458224u, 1635472u, 1342608u};
        uint32_t blocks[8], first[8] = {0};
        uint32_t guard = xbox_HeapAlloc(4096, 16);
        unsigned completed = 0;
        int reused = 1, intact = 1, released = 1;
        *G(guard) = 0xC0FFEE42u;
        for (unsigned round = 0; round < 24; ++round) {
            unsigned loaded = 0;
            for (; loaded < 8; ++loaded) {
                uint32_t size = sizes[loaded], base = 0;
                /* MM3's large CRT allocation requests commit at base zero,
                 * optionally with MEM_NOZERO. It returns base + 0x30 to the
                 * file loader and subtracts that header before release. */
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
    if (car_churn) {
        const uint32_t big = 2u * 1024u * 1024u;
        const uint32_t models[] = {2818096u, 2441264u, 1638448u, 1720368u};
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

            /* Commit the first 64 KB and use it. */
            base = hi; size = 0x10000;
            st = nt_alloc_at(&base, &size, 0x1000);
            check(st == 0 && base == hi, "commit inside the reservation succeeds", NULL);
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
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION host_info;
    check(VirtualQuery(host_base, &host_info, sizeof host_info) && host_info.State == MEM_FREE,
          "shutdown releases the host placeholder reservation", NULL);
#endif
    free(xbe);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
