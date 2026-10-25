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
 * Checked here with ordinal 173, MmGetPhysicalAddress. The minimal XBE fixture
 * initializes the real runtime memory map, so the contiguous alias at
 * 0x80001000 resolves to physical 0x1000.
 */
/* kernel.h brings the NT type vocabulary: <windows.h> on Windows, the
 * shim in platform/xbox_winnt.h elsewhere. Nothing below is
 * platform-specific. */
#include "kernel.h"
#include "kernel_guest_io.h"
#include "recomp_icall_feedback.h"
#include "xbox_memory_layout.h"   /* RECOMP_TLS */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCRATCH_SP 0x30000u
typedef void (*recomp_func_t)(void);
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

/* Provided by the generated title; a regression harness has no title, and a
 * static archive resolves only what gets pulled in. Nothing here calls a
 * guest function. */
static uint8_t *g_guest_mem;
static uint32_t g_complete_entry;
static uint32_t g_dispatch_mode;
static uint32_t g_start_order[8];
static uint32_t g_start_count;
static uint32_t g_nested_preserved;
static uint32_t g_start_action;
static uint32_t g_start_complete_entry;
static uint32_t g_start_packet_entry;
static uint32_t g_start_enqueue_irp;
static uint32_t g_start_enqueue_key_va;
static uint32_t g_start_completed_and_freed;
static uint32_t g_sync_pending_irp;
static uint32_t g_sync_complete_entry;
static HANDLE g_sync_dispatch_entered;
static int g_sync_event_header_expected;
static int g_sync_event_header_ok = 1;
static uint32_t g_sync_event_header_checks;
static uint32_t call_fast_slot(uint8_t *mem, uint32_t entry_va,
                               uint32_t ecx, uint32_t edx);

#define TEST_DISPATCH_VA 0x45080u
#define TEST_STARTIO_VA  0x45084u

static void test_dispatch_callback(void)
{
    uint32_t *sp = (uint32_t *)(g_guest_mem + g_esp);
    uint32_t irp_va = sp[2];
    XBOX_GUEST_IRP *irp = (XBOX_GUEST_IRP *)(g_guest_mem + irp_va);
    recomp_func_t complete;

    if (g_sync_event_header_expected) {
        uint32_t event_va = irp->user_event;
        XBOX_GUEST_DISPATCHER_HEADER *header = event_va ?
            (XBOX_GUEST_DISPATCHER_HEADER *)(g_guest_mem + event_va) : NULL;
        if (!header || header->type != 0 || header->absolute != 0 ||
            header->size != 4 || header->inserted != 0 ||
            header->signal_state != 0 ||
            header->wait_list_head[0] != event_va + 8 ||
            header->wait_list_head[1] != event_va + 8)
            g_sync_event_header_ok = 0;
        ++g_sync_event_header_checks;
    }

    irp->io_status.status = g_dispatch_mode == 1 ? 0x00000103u :
                            g_dispatch_mode == 2 ? 0xC0000001u :
                            g_dispatch_mode == 3 ? 0x80000005u : 0;
    irp->io_status.information = g_dispatch_mode == 2 ? 9 :
                                  g_dispatch_mode == 3 ? 13 : 7;
    /* Mode 2 returns success with an IOSB error; mode 3 returns the warning
     * STATUS_BUFFER_OVERFLOW. Xbox ordinal 84 returns the dispatch status on
     * immediate completion but copies IOSB.Information for either status. */
    g_eax = g_dispatch_mode == 2 ? 0 : irp->io_status.status;
    if (g_dispatch_mode == 1 && g_sync_dispatch_entered) {
        g_sync_pending_irp = irp_va;
        SetEvent(g_sync_dispatch_entered);
    } else if (g_dispatch_mode != 1) {
        uint32_t result = g_eax;
        g_ecx = irp_va;
        g_edx = 0;
        g_esp -= 4; /* kernel thunk dispatch consumes its dummy call return */
        *(uint32_t *)(g_guest_mem + g_esp) = 0;
        complete = recomp_lookup_kernel(*(uint32_t *)(g_guest_mem + g_complete_entry));
        complete();
        g_nested_preserved = g_eax == 0 && g_esp == SCRATCH_SP - 12;
        g_eax = result;
    }
    g_esp += 12; /* stdcall dispatch routine consumed return + two arguments */
}

static DWORD WINAPI complete_pending_sync_irp(LPVOID parameter)
{
    (void)parameter;
    if (WaitForSingleObject(g_sync_dispatch_entered, 2000) != WAIT_OBJECT_0)
        return 1;
    Sleep(10); /* let dispatch return STATUS_PENDING before completion */
    XBOX_GUEST_IRP *irp = (XBOX_GUEST_IRP *)(g_guest_mem + g_sync_pending_irp);
    irp->io_status.status = 0;
    irp->io_status.information = 13;
    (void)call_fast_slot(g_guest_mem, g_sync_complete_entry,
                         g_sync_pending_irp, 0);
    return 0;
}

static void test_start_io_callback(void)
{
    uint32_t *sp = (uint32_t *)(g_guest_mem + g_esp);
    uint32_t device_va = sp[1];
    uint32_t irp_va = sp[2];
    XBOX_GUEST_IRP *irp = (XBOX_GUEST_IRP *)(g_guest_mem + irp_va);
    if (g_start_count < sizeof(g_start_order) / sizeof(g_start_order[0]))
        g_start_order[g_start_count++] = irp->original_file_object;
    if (g_start_action) {
        recomp_func_t complete, start_packet;
        uint32_t saved_esp = g_esp;
        g_ecx = irp_va; g_edx = 0; g_esp -= 4;
        *(uint32_t *)(g_guest_mem + g_esp) = 0;
        complete = recomp_lookup_kernel(*(uint32_t *)(g_guest_mem +
                                      g_start_complete_entry));
        complete();
        g_start_completed_and_freed = xbox_HeapBlockSize(irp_va) == 0;

        g_esp -= 16;
        *(uint32_t *)(g_guest_mem + g_esp) = 0;
        *(uint32_t *)(g_guest_mem + g_esp + 4) = device_va;
        *(uint32_t *)(g_guest_mem + g_esp + 8) = g_start_enqueue_irp;
        *(uint32_t *)(g_guest_mem + g_esp + 12) = g_start_enqueue_key_va;
        start_packet = recomp_lookup_kernel(*(uint32_t *)(g_guest_mem +
                                          g_start_packet_entry));
        start_packet();
        g_esp = saved_esp;
        g_start_action = 0;
    }
    /* Simulate a callback clobbering volatile and callee-saved CPU state. */
    g_eax = 0xE0E0E0E0u; g_ecx = 0xC1C1C1C1u; g_edx = 0xD2D2D2D2u;
    g_ebx = 0xB3B3B3B3u; g_esi = 0x51515151u; g_edi = 0xD1D1D1D1u;
    g_esp += 12;
}

#define TEST_UNWIND_VA 0x45098u
static unsigned unwind_calls;
static uint32_t unwind_frame,unwind_flags;
static void test_unwind_callback(void)
{
    uint32_t *sp=(uint32_t *)(g_guest_mem+g_esp);
    unwind_calls++; unwind_frame=sp[2];
    unwind_flags=*(uint32_t *)(g_guest_mem+sp[1]+4);
    g_esp+=4; /* cdecl return; caller owns the four arguments */
}
#define TEST_THREAD_VA 0x45088u
static DWORD thread_id;
static uint32_t thread_context[2], thread_stack, thread_tib;
static unsigned thread_context_count;
static uint32_t thread_stack_probe;
static int thread_has_bounds;
static void test_thread_callback(void)
{
    thread_id = GetCurrentThreadId();
    thread_context[0] = *(uint32_t *)(g_guest_mem + g_esp + 4);
    thread_context[1] = thread_context_count==2 ? *(uint32_t *)(g_guest_mem + g_esp + 8) : 0;
    thread_stack = g_esp;
    thread_stack_probe=xbox_HeapBlockSize(g_esp-1024*1024);
    thread_has_bounds=xbox_GetStackBounds(g_esp,NULL,NULL);
    thread_tib = g_fs_base;
    g_ebx = 0xBAD;
    g_esp += 4 + 4*thread_context_count; /* translated stdcall return */
}

recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va)
{
    if (xbox_va == TEST_UNWIND_VA) return test_unwind_callback;
    if (xbox_va == TEST_THREAD_VA) return test_thread_callback;
    if (xbox_va == TEST_DISPATCH_VA) return test_dispatch_callback;
    if (xbox_va == TEST_STARTIO_VA) return test_start_io_callback;
    return NULL;
}
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

/* The seam, plus the guest CPU state the dispatcher reads and writes. */
extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern RECOMP_TLS uint32_t g_eax, g_esp;
extern RECOMP_TLS uint32_t g_ecx, g_edx, g_ebx, g_esi, g_edi;
extern ptrdiff_t g_xbox_mem_offset;

static int failures;

static int all_zero(const uint8_t *bytes, size_t size)
{
    for (size_t i = 0; i < size; i++)
        if (bytes[i] != 0) return 0;
    return 1;
}

static int all_value(const uint8_t *bytes, size_t size, uint8_t value)
{
    for (size_t i = 0; i < size; i++)
        if (bytes[i] != value) return 0;
    return 1;
}

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-58s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
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

    /* Two imports, in the form an XBE stores them: 0x80000000 | ordinal. */
    *(uint32_t *)(mem + THUNK_VA) = 0x80000000u | 173u;
    *(uint32_t *)(mem + THUNK_VA + 4) = 0x80000000u | 197u;
    xbox_kernel_set_thunk_address(THUNK_VA, 2);
    xbox_kernel_bridge_init();

    /* bridge_init rewrites the entry in place to a synthetic VA. */
    recomp_func_t fn = recomp_lookup_kernel(*(uint32_t *)(mem + THUNK_VA));
    check(fn != NULL, "ordinal 173 resolves to a bridge entry point",
          "the thunk entry was not rewritten to a synthetic VA");

    if (fn) {
        /* stdcall: the guest pushed a return address, then the argument. */
        uint32_t *sp = (uint32_t *)(mem + STACK_VA);
        sp[0] = 0xBEEF0001u;          /* guest return address */
        sp[1] = 0x80001000u;          /* initialized contiguous alias */
        g_esp = STACK_VA;
        g_eax = 0xDEADBEEFu;
        fn();

        char detail[128];
        snprintf(detail, sizeof detail,
                 "bridge returned 0x%08X, expected physical 0x00001000", g_eax);
        check(g_eax == 0x1000u,
              "ordinal 173 resolves the initialized contiguous alias", detail);

        /* And the argument must come off the guest stack. A wrong
         * stdcall_args_for_ordinal entry corrupts the caller's frame away
         * from the call, with nothing naming the ordinal. */
        snprintf(detail, sizeof detail, "esp is 0x%08X, expected 0x%08X",
                 g_esp, STACK_VA + 8);
        check(g_esp == STACK_VA + 8,
              "ordinal 173 pops its stdcall argument", detail);
    }

    /* Ordinal 197, NtDuplicateObject(Source, &Target, Options), on the
     * guest's pseudo-handles. They are 32-bit -1 (current process) and -2
     * (current thread); widened without sign extension they are no handle
     * at all, and the duplicate fails (Windows) or faults (POSIX shim). A
     * title duplicating NtCurrentThread() to keep a handle to itself is the
     * case that found this. */
    fn = recomp_lookup_kernel(*(uint32_t *)(mem + THUNK_VA + 4));
    check(fn != NULL, "ordinal 197 resolves to a bridge entry point", NULL);
    if (fn) {
        static const struct { uint32_t src; const char *what; } pseudo[] = {
            { 0xFFFFFFFEu, "NtDuplicateObject(NtCurrentThread()) succeeds" },
            { 0xFFFFFFFFu, "NtDuplicateObject(NtCurrentProcess()) succeeds" },
        };
        const uint32_t TARGET_VA = 0x30000;
        for (size_t k = 0; k < sizeof pseudo / sizeof pseudo[0]; k++) {
            uint32_t *sp = (uint32_t *)(mem + STACK_VA);
            sp[0] = 0xBEEF0002u;      /* guest return address */
            sp[1] = pseudo[k].src;    /* SourceHandle */
            sp[2] = TARGET_VA;        /* TargetHandle (out) */
            sp[3] = 0x2u;             /* DUPLICATE_SAME_ACCESS */
            *(uint32_t *)(mem + TARGET_VA) = 0;
            g_esp = STACK_VA;
            g_eax = 0xDEADBEEFu;
            fn();

            char detail[128];
            snprintf(detail, sizeof detail,
                     "status 0x%08X, target 0x%08X", g_eax,
                     *(uint32_t *)(mem + TARGET_VA));
            check(g_eax == 0 && *(uint32_t *)(mem + TARGET_VA) != 0,
                  pseudo[k].what, detail);
            snprintf(detail, sizeof detail, "esp is 0x%08X, expected 0x%08X",
                     g_esp, STACK_VA + 16);
            check(g_esp == STACK_VA + 16,
                  "ordinal 197 pops its three stdcall arguments", detail);
        }
    }

    VirtualFree(mem, 0, MEM_RELEASE);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
