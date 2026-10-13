/*
 * Kernel regressions: three fixes, each checked against the bug it fixed.
 *
 * Like tests/kernel_bridge, this calls the kernel through the thunk dispatcher
 * -- the path a title's kernel calls take -- with a plain buffer standing in
 * for guest memory. No title and no game files.
 *
 * Each check fails without its fix and passes with it:
 *
 *   1. STATUS_CONFLICTING_ADDRESSES (0xC0000018) maps to ERROR_INVALID_ADDRESS
 *      (487), in both places the mapping exists (the bridge's ordinal 301 and
 *      xbox_RtlNtStatusToDosError). 487 is the only error on which the MSVC
 *      CRT heap tries another address when it grows; the generic 317 made it
 *      stop growing.
 *
 *   2. NtCurrentThread() (-2) and NtCurrentProcess() (-1) reach Win32 as
 *      pseudo-handles. Zero-extended on a 64-bit host they are not, and
 *      DuplicateHandle rejects them, so NtDuplicateObject failed.
 *
 *   3. The kernel call counter does not wrap at 2^31. The log gate is "count
 *      <= budget", which a wrapped negative count satisfies, so every call
 *      after the wrap wrote log lines. The counter is private to the bridge,
 *      so the check drives 2^31 + 200000 calls of a cheap ordinal and counts
 *      the "[KERNEL] #" lines written. Slow (tens of seconds), so it only runs
 *      when the program is given the argument "wrap" (see CMakeLists.txt).
 */
#include "kernel.h"
#include "xbox_memory_layout.h"   /* RECOMP_TLS */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Provided by the generated title; this harness has none. Nothing here calls a
 * guest function. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern RECOMP_TLS uint32_t g_eax, g_esp;
extern ptrdiff_t g_xbox_mem_offset;

#define THUNK_VA 0x10000u
#define STACK_VA 0x20000u
#define OUT_VA   0x30000u

/* The ordinals used; thunk slot i is at THUNK_VA + 4*i. */
enum { S_MMGPA, S_DUP, S_STATUS2DOS, N_SLOTS };
static const uint32_t ORD[N_SLOTS] = {
    173,   /* MmGetPhysicalAddress (cheap; used to drive the counter) */
    197,   /* NtDuplicateObject */
    301,   /* RtlNtStatusToDosError */
};

static uint8_t *mem;
static uint32_t slot_va[N_SLOTS];
static int failures;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-62s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

/* recomp_lookup_kernel() selects the slot when it is called, so resolve right
 * before every call instead of caching the returned pointer. */
static recomp_func_t resolve(int slot)
{
    return recomp_lookup_kernel(slot_va[slot]);
}

/* stdcall call through the dispatcher; returns eax. */
static uint32_t call(int slot, int nargs, const uint32_t *args)
{
    uint32_t *sp = (uint32_t *)(mem + STACK_VA);
    int i;

    sp[0] = 0xBEEF0001u;                 /* guest return address */
    for (i = 0; i < nargs; i++)
        sp[1 + i] = args[i];
    g_esp = STACK_VA;
    g_eax = 0xDEADBEEFu;
    resolve(slot)();
    return g_eax;
}

int main(int argc, char **argv)
{
    char d[160];
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);

    mem = VirtualAlloc(NULL, 16 * 1024 * 1024, MEM_RESERVE | MEM_COMMIT,
                       PAGE_READWRITE);
    if (!mem) {
        puts("could not allocate the guest memory stand-in");
        return 2;
    }
    g_xbox_mem_offset = (ptrdiff_t)mem;

    for (i = 0; i < N_SLOTS; i++)
        *(uint32_t *)(mem + THUNK_VA + 4 * i) = 0x80000000u | ORD[i];
    xbox_kernel_set_thunk_address(THUNK_VA, N_SLOTS);
    xbox_kernel_bridge_init();
    for (i = 0; i < N_SLOTS; i++) {
        slot_va[i] = *(uint32_t *)(mem + THUNK_VA + 4 * i);
        if (!resolve(i)) {
            printf("ordinal %u did not resolve to a bridge entry point\n", ORD[i]);
            return 2;
        }
    }

    /* 1. STATUS_CONFLICTING_ADDRESSES -> 487, in both places. */
    {
        ULONG r = xbox_RtlNtStatusToDosError((NTSTATUS)0xC0000018u);
        uint32_t arg = 0xC0000018u;
        uint32_t b = call(S_STATUS2DOS, 1, &arg);

        snprintf(d, sizeof d, "returned %lu, expected 487", (unsigned long)r);
        check(r == 487, "xbox_RtlNtStatusToDosError(0xC0000018) == 487", d);
        snprintf(d, sizeof d, "returned %u, expected 487", b);
        check(b == 487, "bridge RtlNtStatusToDosError(0xC0000018) == 487", d);
    }

    /* 2. Pseudo-handles duplicate. */
    {
        static const uint32_t token[2] = { 0xFFFFFFFEu, 0xFFFFFFFFu };
        static const char *const what[2] = {
            "NtDuplicateObject(NtCurrentThread()) succeeds",
            "NtDuplicateObject(NtCurrentProcess()) succeeds",
        };

        for (i = 0; i < 2; i++) {
            uint32_t args[3];
            uint32_t st;

            args[0] = token[i];
            args[1] = OUT_VA;
            args[2] = 0x2;                       /* DUPLICATE_SAME_ACCESS */
            *(uint32_t *)(mem + OUT_VA) = 0;
            st = call(S_DUP, 3, args);
            snprintf(d, sizeof d, "status 0x%08X, handle 0x%08X", st,
                     *(uint32_t *)(mem + OUT_VA));
            check(st == 0 && *(uint32_t *)(mem + OUT_VA) != 0, what[i], d);
        }
    }

    /* 3. No call logging past 2^31 kernel calls (slow; opt-in). */
    if (argc > 1 && !strcmp(argv[1], "wrap")) {
        const char *logf = "kernel_regressions_wrap_stderr.txt";
        const uint64_t total = 2147483648ull + 200000ull;
        const uint32_t arg = 0x80001000u;
        uint32_t *sp = (uint32_t *)(mem + STACK_VA);
        recomp_func_t fn;
        uint64_t n;
        unsigned long lines = 0;
        FILE *f;
        char line[512];

        fflush(stderr);
        if (!freopen(logf, "w", stderr)) {
            puts("cannot redirect stderr");
            return 2;
        }
        setvbuf(stderr, NULL, _IOFBF, 1 << 20);

        fn = resolve(S_MMGPA);
        for (n = 0; n < total; n++) {
            sp[0] = 0xBEEF0001u;
            sp[1] = arg;
            g_esp = STACK_VA;
            fn();
        }
        fflush(stderr);
        fclose(stderr);

        f = fopen(logf, "r");
        if (f) {
            while (fgets(line, sizeof line, f))
                if (strstr(line, "[KERNEL] #"))
                    lines++;
            fclose(f);
            remove(logf);
        }
        /* The first calls are logged up to the budget (default 200); anything
         * beyond that means the gate reopened after the wrap. */
        snprintf(d, sizeof d, "%lu call-log lines for %llu calls; at most 200 expected",
                 lines, (unsigned long long)total);
        check(f != NULL && lines <= 200,
              "no call logging after 2^31 kernel calls", d);
    } else {
        puts("  (2^31 call-counter check skipped; run with 'wrap' to include it)");
    }

    /* XNet's secure LAN key exchange: Diffie-Hellman (Oakley group 1) and
     * 3DES-CBC. Both were stubs, so system link joins never authenticated. */
    {
        static const ULONG x[24] = {0x52E6B438u,0xF2A74DE4u,0x269E0D37u,0x6513270Eu,0xA6A3A450u};
        static const ULONG p[24] = {0xFFFFFFFFu,0xFFFFFFFFu,0xA63A3620u,0xF44C42E9u,0x625E7EC6u,
            0xE485B576u,0x6D51C245u,0x4FE1356Du,0xF25F1437u,0x302B0A6Du,0xCD3A431Bu,0xEF9519B3u,
            0x8E3404DDu,0x514A0879u,0x3B139B22u,0x020BBEA6u,0x8A67CC74u,0x29024E08u,0x80DC1CD1u,
            0xC4C6628Bu,0x2168C234u,0xC90FDAA2u,0xFFFFFFFFu,0xFFFFFFFFu};
        static const ULONG want[24] = {0x5BFC9A40u,0x120F66C2u,0xA50FD791u,0xDF04292Cu,0x2F465E6Bu,
            0xD2C159CDu,0x7BA6E2EEu,0x51A1E9E0u,0x1CADAFCBu,0x5BFB66B7u,0x5DDA6E18u,0x7B0DE322u,
            0x7721E7F3u,0x42159C99u,0x613292F6u,0x61F70A07u,0xB3239620u,0xFD246058u,0x1AFB5E17u,
            0x1CDF9A38u,0x58AE590Fu,0x315848D6u,0x08F9E148u,0x5A288908u};
        ULONG g[24] = {2}, r[24];
        check(xbox_XcModExp(r, g, (PULONG)x, (PULONG)p, 24) == 1 && !memcmp(r, want, sizeof r),
              "XcModExp: 2^x mod the Oakley group 1 prime", "differs from Python's pow()");

        /* FIPS 81 / the classic DES vector, then 3DES with K1=K2=K3 == DES. */
        static const UCHAR key[24] = {0x13,0x34,0x57,0x79,0x9B,0xBC,0xDF,0xF1, 0x13,0x34,0x57,0x79,
                                      0x9B,0xBC,0xDF,0xF1, 0x13,0x34,0x57,0x79,0x9B,0xBC,0xDF,0xF1};
        static const UCHAR pt[8] = {0x01,0x23,0x45,0x67,0x89,0xAB,0xCD,0xEF};
        static const UCHAR ct[8] = {0x85,0xE8,0x13,0x54,0x0F,0x0A,0xB4,0x05};
        UCHAR table[384], out[8], back[8];
        xbox_XcKeyTable(0, table, key);
        xbox_XcBlockCrypt(0, out, (PVOID)pt, table, 1);
        xbox_XcBlockCrypt(0, back, out, table, 0);
        check(!memcmp(out, ct, 8) && !memcmp(back, pt, 8), "XcBlockCrypt: DES known answer",
              "wrong ciphertext or round trip");
        xbox_XcKeyTable(1, table, key);
        xbox_XcBlockCrypt(1, out, (PVOID)pt, table, 1);
        check(!memcmp(out, ct, 8), "XcBlockCrypt: 3DES with equal keys is DES", "differs");

        UCHAR msg[24] = "secure system link data", enc[24], dec[24], iv[8] = {1,2,3,4,5,6,7,8}, iv2[8];
        memcpy(iv2, iv, 8);
        xbox_XcBlockCryptCBC(1, 24, enc, msg, table, 1, iv);
        memcpy(dec, enc, 24);
        xbox_XcBlockCryptCBC(1, 24, dec, dec, table, 0, iv2);   /* in place, like XNet */
        check(!memcmp(dec, msg, 24) && !memcmp(iv, iv2, 8) && memcmp(enc + 8, enc + 16, 8),
              "XcBlockCryptCBC: 3DES round trip", "plaintext or feedback differs");

        UCHAR par[2] = {0x00, 0x13};
        xbox_XcDESKeyParity(par, 2);
        check(par[0] == 0x01 && par[1] == 0x13, "XcDESKeyParity: odd parity", "wrong low bits");
    }

    VirtualFree(mem, 0, MEM_RELEASE);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
