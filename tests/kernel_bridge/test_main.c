/*
 * The kernel bridge.
 *
 * A title does not call the kernel by name. Its indirect calls land on a
 * thunk table that xbox_kernel_bridge_init() has rewritten to synthetic VAs,
 * and kernel_thunk_dispatch turns each of those into a bridge_* function.
 * That is the path the runtime actually executes, and it is easy to leave
 * untested: bridge_* functions are static and read guest CPU state rather
 * than taking arguments, so they cannot be called directly.
 *
 * They are reachable all the same. recomp_lookup_kernel is not static: given
 * a buffer standing in for guest memory it selects the slot and hands back
 * the dispatcher. tests/kernel_directory uses the same seam for the directory
 * ABI; this one covers kernel memory ordinals.
 *
 * Checked here with ordinal 173, MmGetPhysicalAddress, which had two
 * implementations that disagreed -- the bridge translating through the
 * contiguous window and xbox_MmGetPhysicalAddress returning its argument
 * unchanged. The bridge now calls the latter, and that delegation is what
 * these checks pin: asserting only on xbox_MmGetPhysicalAddress leaves a
 * bridge that stops delegating entirely green.
 */
/* kernel.h brings the NT type vocabulary: <windows.h> on Windows, the
 * shim in platform/xbox_winnt.h elsewhere. Nothing below is
 * platform-specific. */
#include "kernel.h"
#include "xbox_memory_layout.h"   /* RECOMP_TLS */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Provided by the generated title; a regression harness has no title, and a
 * static archive resolves only what gets pulled in. Nothing here calls a
 * guest function. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

/* The seam, plus the guest CPU state the dispatcher reads and writes. */
extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern RECOMP_TLS uint32_t g_eax, g_esp;
extern ptrdiff_t g_xbox_mem_offset;

static int failures;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-58s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

/* Guest-side helpers for the checks after ordinal 173. */
#define SCRATCH_SP 0x30000u
#define THUNK_SLOTS 6

/* Calls the bridge in the thunk entry at entry_va with nargs stdcall arguments
 * and returns EAX. The lookup happens here, on every call: recomp_lookup_kernel
 * hands back one shared dispatcher and records the slot as a side effect, so a
 * function looked up earlier would run whichever slot was looked up last. */
static uint32_t call_slot(uint8_t *mem, uint32_t entry_va, uint32_t nargs,
                          const uint32_t *args)
{
    uint32_t *sp = (uint32_t *)(mem + SCRATCH_SP);
    recomp_func_t fn = recomp_lookup_kernel(*(uint32_t *)(mem + entry_va));
    uint32_t i;

    sp[0] = 0xBEEF0001u;                  /* guest return address */
    for (i = 0; i < nargs; i++)
        sp[1 + i] = args[i];
    g_esp = SCRATCH_SP;
    g_eax = 0xDEADBEEFu;
    fn();
    return g_eax;
}

/* A guest ANSI_STRING at at, its text at buf. */
static void put_ansi(uint8_t *mem, uint32_t at, uint32_t buf, const char *s)
{
    uint16_t len = (uint16_t)strlen(s);
    *(uint16_t *)(mem + at) = len;
    *(uint16_t *)(mem + at + 2) = (uint16_t)(len + 1);
    *(uint32_t *)(mem + at + 4) = buf;
    memcpy(mem + buf, s, len + 1);
}

/* A guest OBJECT_ATTRIBUTES at oa naming s. Layout: Root, ObjectName, Attributes. */
static void put_oa(uint8_t *mem, uint32_t oa, uint32_t name, uint32_t buf, const char *s)
{
    put_ansi(mem, name, buf, s);
    *(uint32_t *)(mem + oa) = 0;
    *(uint32_t *)(mem + oa + 4) = name;
    *(uint32_t *)(mem + oa + 8) = 0x40;
}

/* NtQuerySymbolicLinkObject on handle; the target lands in out on success. */
static uint32_t query_link(uint8_t *mem, uint32_t query, uint32_t handle,
                           char *out, size_t out_size)
{
    uint32_t a[3], status;

    *(uint16_t *)(mem + 0x40400) = 0;      /* ANSI_STRING: Length */
    *(uint16_t *)(mem + 0x40402) = 128;    /* MaximumLength */
    *(uint32_t *)(mem + 0x40404) = 0x40420;
    a[0] = handle; a[1] = 0x40400; a[2] = 0x40410;
    status = call_slot(mem, query, 3, a);
    out[0] = '\0';
    if (status == 0)
        snprintf(out, out_size, "%s", (char *)(mem + 0x40420));
    return status;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    const uint32_t THUNK_VA = 0x10000, STACK_VA = 0x20000;

    uint8_t *mem = VirtualAlloc(NULL, 16 * 1024 * 1024,
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    check(mem != NULL, "guest memory stand-in allocated", NULL);
    if (!mem) {
        printf("\n%d failure(s); the rest needs guest memory.\n", failures);
        return 1;
    }
    g_xbox_mem_offset = (ptrdiff_t)mem;

    /* Imports, in the form an XBE stores them: 0x80000000 | ordinal. Slot 0 is
     * ordinal 173; the rest are the symbolic link, handle and event calls
     * checked at the end. */
    static const uint32_t ordinals[THUNK_SLOTS] = { 173, 67, 203, 215, 187, 225 };
    for (uint32_t i = 0; i < THUNK_SLOTS; i++)
        *(uint32_t *)(mem + THUNK_VA + 4 * i) = 0x80000000u | ordinals[i];
    xbox_kernel_set_thunk_address(THUNK_VA, THUNK_SLOTS);
    xbox_kernel_bridge_init();

    /* bridge_init rewrites each entry in place to a synthetic VA. */
    recomp_func_t fn = recomp_lookup_kernel(*(uint32_t *)(mem + THUNK_VA));
    check(fn != NULL, "ordinal 173 resolves to a bridge entry point",
          "the thunk entry was not rewritten to a synthetic VA");

    if (fn) {
        /* stdcall: the guest pushed a return address, then the argument. */
        uint32_t *sp = (uint32_t *)(mem + STACK_VA);
        sp[0] = 0xBEEF0001u;          /* guest return address */
        sp[1] = 0x80001000u;          /* inside the contiguous window */
        g_esp = STACK_VA;
        g_eax = 0xDEADBEEFu;
        fn();

        char detail[128];
        snprintf(detail, sizeof detail,
                 "bridge returned 0x%08X, expected 0x00001000", g_eax);
        check(g_eax == 0x00001000u,
              "ordinal 173 translates through the contiguous window", detail);

        /* And the argument must come off the guest stack. A wrong
         * stdcall_args_for_ordinal entry corrupts the caller's frame away
         * from the call, with nothing naming the ordinal. */
        snprintf(detail, sizeof detail, "esp is 0x%08X, expected 0x%08X",
                 g_esp, STACK_VA + 8);
        check(g_esp == STACK_VA + 8,
              "ordinal 173 pops its stdcall argument", detail);
    }

    /* Symbolic links. A name the title registered opens to that target, an
     * unknown name is not found and writes no handle, and a link handle
     * reports the target it was opened for, not a fixed one. */
    {
        const uint32_t create = THUNK_VA + 4;
        const uint32_t open = THUNK_VA + 8;
        const uint32_t query = THUNK_VA + 12;
        const uint32_t close = THUNK_VA + 16;
        const uint32_t set_ev = THUNK_VA + 20;
        uint32_t a[3], status, handle, handle_d;
        char detail[192], target[128];

        put_ansi(mem, 0x40000, 0x40100, "\\??\\Q:");
        put_ansi(mem, 0x40008, 0x40120, "\\Device\\Harddisk0\\Partition1\\");
        a[0] = 0x40000; a[1] = 0x40008;
        status = call_slot(mem, create, 2, a);
        check(status == 0 && g_esp == SCRATCH_SP + 12,
              "IoCreateSymbolicLink registers \\??\\Q:", "registration failed");

        put_oa(mem, 0x40010, 0x40200, 0x40220, "\\??\\Nope:");
        *(uint32_t *)(mem + 0x40300) = 0xFFFFFFFFu;
        a[0] = 0x40300; a[1] = 0x40010;
        status = call_slot(mem, open, 2, a);
        snprintf(detail, sizeof detail, "returned 0x%08X, expected 0xC0000034", status);
        check(status == 0xC0000034u,
              "NtOpenSymbolicLinkObject: unknown name is not found", detail);
        check(*(uint32_t *)(mem + 0x40300) == 0xFFFFFFFFu,
              "  unknown name writes no handle", NULL);

        put_oa(mem, 0x40030, 0x40240, 0x40260, "\\??\\Q:");
        *(uint32_t *)(mem + 0x40300) = 0;
        a[0] = 0x40300; a[1] = 0x40030;
        status = call_slot(mem, open, 2, a);
        handle = *(uint32_t *)(mem + 0x40300);
        snprintf(detail, sizeof detail, "returned 0x%08X, handle 0x%08X", status, handle);
        check(status == 0 && handle != 0,
              "NtOpenSymbolicLinkObject: registered name opens", detail);

        status = query_link(mem, query, handle, target, sizeof target);
        snprintf(detail, sizeof detail, "returned 0x%08X, target \"%s\"", status, target);
        check(status == 0 && strcmp(target, "\\Device\\Harddisk0\\Partition1\\") == 0,
              "NtQuerySymbolicLinkObject: handle reports its own target", detail);

        put_oa(mem, 0x40050, 0x40280, 0x402A0, "\\??\\D:");
        *(uint32_t *)(mem + 0x40300) = 0;
        a[0] = 0x40300; a[1] = 0x40050;
        status = call_slot(mem, open, 2, a);
        handle_d = *(uint32_t *)(mem + 0x40300);
        status = query_link(mem, query, handle_d, target, sizeof target);
        snprintf(detail, sizeof detail, "returned 0x%08X, target \"%s\"", status, target);
        check(status == 0 && strcmp(target, "\\Device\\CdRom0") == 0,
              "built-in D: reports \\Device\\CdRom0", detail);

        a[0] = handle;
        status = call_slot(mem, close, 1, a);
        check(status == 0, "NtClose releases a link handle", NULL);
        status = query_link(mem, query, handle, target, sizeof target);
        snprintf(detail, sizeof detail, "returned 0x%08X, expected 0xC0000008", status);
        check(status == 0xC0000008u, "a closed link handle is invalid", detail);

        /* Untagged values. The bridge issues only tokens, sentinels and pseudo
         * handles. A host event made here is a real handle, and reaching
         * SetEvent untagged would signal it: that must be refused. */
        {
            HANDLE ev = CreateEventA(NULL, TRUE, FALSE, NULL);
            check(ev != NULL, "host event for the untagged-handle check", NULL);
            if (ev) {
                a[0] = (uint32_t)(uintptr_t)ev; a[1] = 0x40700;
                status = call_slot(mem, set_ev, 2, a);
                snprintf(detail, sizeof detail, "returned 0x%08X, event signalled",
                         status);
                check(status != 0 && WaitForSingleObject(ev, 0) != WAIT_OBJECT_0,
                      "untagged host handle is refused, not signalled", detail);
                CloseHandle(ev);
            }
            a[0] = 0x1234; a[1] = 0x40700;
            status = call_slot(mem, set_ev, 2, a);
            snprintf(detail, sizeof detail, "returned 0x%08X", status);
            check(status != 0, "untagged small value is refused", detail);
        }
    }

    VirtualFree(mem, 0, MEM_RELEASE);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
