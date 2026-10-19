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

#define TEST_DISPATCH_VA 0x45080u
#define TEST_STARTIO_VA  0x45084u

static void test_dispatch_callback(void)
{
    uint32_t *sp = (uint32_t *)(g_guest_mem + g_esp);
    uint32_t irp_va = sp[2];
    XBOX_GUEST_IRP *irp = (XBOX_GUEST_IRP *)(g_guest_mem + irp_va);
    recomp_func_t complete;

    irp->io_status.status = g_dispatch_mode == 1 ? 0x00000103u : 0;
    irp->io_status.information = 7;
    g_eax = irp->io_status.status;
    if (g_dispatch_mode == 0) {
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

recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va)
{
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

/* Guest-side helpers for the checks after ordinal 173. */
#define THUNK_SLOTS 23

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

static uint32_t call_fast_slot(uint8_t *mem, uint32_t entry_va,
                               uint32_t ecx, uint32_t edx)
{
    recomp_func_t fn = recomp_lookup_kernel(*(uint32_t *)(mem + entry_va));
    g_ecx = ecx; g_edx = edx; g_esp = SCRATCH_SP - 4; g_eax = 0xDEADBEEFu;
    *(uint32_t *)(mem + g_esp) = 0xBEEF0001u;
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
    uint32_t device_for_io = 0;

    uint8_t *xbe = (uint8_t *)calloc(1, 0x1000);
    uint32_t xbe_word;
    if (xbe) {
        xbe_word = 0x00010000u; memcpy(xbe + 0x104, &xbe_word, 4); /* base */
        xbe_word = 0x1000u; memcpy(xbe + 0x108, &xbe_word, 4); /* header size */
        xbe_word = 0; memcpy(xbe + 0x11c, &xbe_word, 4); /* no sections */
        xbe_word = 0x00010120u; memcpy(xbe + 0x120, &xbe_word, 4); /* section table */
        xbe_word = 0x00010180u; memcpy(xbe + 0x118, &xbe_word, 4); /* certificate */
        xbe_word = 0x00010200u ^ 0x5B6D40B6u;
        memcpy(xbe + 0x158, &xbe_word, 4); /* retail encoded thunk VA */
    }
    check(xbe != NULL && xbox_MemoryLayoutInit(xbe, 0x1000),
          "minimal XBE fixture initializes guest memory and heap", NULL);
    free(xbe);
    uint8_t *mem = (uint8_t *)xbox_GetMemoryBase();
    g_guest_mem = mem;
    check(mem != NULL, "mapped guest memory available", NULL);
    if (!mem) {
        printf("\n%d failure(s); the rest needs guest memory.\n", failures);
        return 1;
    }

    /* Imports, in the form an XBE stores them: 0x80000000 | ordinal. Slot 0 is
     * ordinal 173; the rest are the symbolic link, handle and event calls
     * checked at the end. */
    static const uint32_t ordinals[THUNK_SLOTS] = {
        173, 67, 203, 215, 187, 225, 61, 62, 59, 65, 72, 73,
        60, 83, 81, 82, 86, 87, 74, 108, 84, 85, 159
    };
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

    /* IoAllocateIrp returns a guest VA to a fixed-width Xbox IRP followed by
     * 0x18-byte stack locations; IoFreeIrp releases that same guest block. */
    {
        uint32_t alloc = THUNK_VA + 8 * 4, free_irp = THUNK_VA + 10 * 4;
        uint32_t a[1] = { 3 }, irp_va = call_slot(mem, alloc, 1, a);
        XBOX_GUEST_IRP *irp = (XBOX_GUEST_IRP *)(mem + irp_va);
        check(irp_va != 0 && irp->type == XBOX_IO_TYPE_IRP &&
              irp->size == XBOX_IRP_SIZE + 3 * XBOX_IO_STACK_LOCATION_SIZE &&
              irp->stack_count == 3 && irp->current_location == 4 &&
              irp->current_stack_location == irp_va + irp->size &&
              g_esp == SCRATCH_SP + 8,
              "IoAllocateIrp creates initialized Xbox IRP and pops one arg", NULL);
        check(all_zero(mem + irp_va + XBOX_IRP_SIZE,
                       3 * XBOX_IO_STACK_LOCATION_SIZE),
              "IoAllocateIrp zeroes each guest stack slot", NULL);
        a[0] = 0;
        check(call_slot(mem, alloc, 1, a) == 0 && g_esp == SCRATCH_SP + 8,
              "IoAllocateIrp rejects zero stack count with one-arg cleanup", NULL);
        a[0] = irp_va;
        check(call_slot(mem, free_irp, 1, a) == 0 && g_esp == SCRATCH_SP + 8 &&
              xbox_HeapBlockSize(irp_va) == 0,
              "IoFreeIrp dispatches with one-arg cleanup", NULL);
        a[0] = 127;
        irp_va = call_slot(mem, alloc, 1, a);
        irp = (XBOX_GUEST_IRP *)(mem + irp_va);
        check(irp_va != 0 && irp->stack_count == 127 &&
              irp->size == XBOX_IRP_SIZE + 127 * XBOX_IO_STACK_LOCATION_SIZE &&
              irp->current_stack_location == irp_va + irp->size,
              "IoAllocateIrp accepts maximum positive CCHAR stack count", NULL);
        a[0] = irp_va;
        (void)call_slot(mem, free_irp, 1, a);
        a[0] = 0x80;
        check(call_slot(mem, alloc, 1, a) == 0 && g_esp == SCRATCH_SP + 8,
              "IoAllocateIrp rejects negative CCHAR stack count", NULL);
    }

    /* Initializing a caller-provided packet must populate guest addresses,
     * fit its stack array, and leave the canary beyond PacketSize untouched. */
    {
        const uint32_t init = THUNK_VA + 11 * 4, packet_va = 0x42000;
        const uint16_t packet_size = XBOX_IRP_SIZE + 3 * XBOX_IO_STACK_LOCATION_SIZE;
        const uint32_t canary = 0xC0FFEE11u;
        uint32_t a[3] = { packet_va, packet_size, 3 };
        memset(mem + packet_va, 0xCC, packet_size);
        *(uint32_t *)(mem + packet_va + packet_size) = canary;
        check(call_slot(mem, init, 3, a) == 0 && g_esp == SCRATCH_SP + 16,
              "IoInitializeIrp dispatches and pops three args", NULL);
        XBOX_GUEST_IRP *irp = (XBOX_GUEST_IRP *)(mem + packet_va);
        check(irp->type == XBOX_IO_TYPE_IRP && irp->size == packet_size &&
              irp->stack_count == 3 && irp->current_location == 4 &&
              irp->current_stack_location == packet_va + packet_size,
              "IoInitializeIrp writes Xbox header and end-of-stack guest VA", NULL);
        check(all_zero(mem + packet_va + XBOX_IRP_SIZE,
                       3 * XBOX_IO_STACK_LOCATION_SIZE),
              "IoInitializeIrp clears each guest stack slot", NULL);
        check(*(uint32_t *)(mem + packet_va + packet_size) == canary,
              "IoInitializeIrp preserves stack-boundary canary", NULL);
        a[2] = 0xFFFFFFFFu;
        memset(mem + packet_va, 0xCC, packet_size);
        check(call_slot(mem, init, 3, a) == 0 &&
              all_value(mem + packet_va, packet_size, 0xCC),
              "IoInitializeIrp rejects negative stack count without writing", NULL);
        a[2] = 3;
        a[1] = (uint32_t)(packet_size - 1);
        memset(mem + packet_va, 0xCC, packet_size);
        check(call_slot(mem, init, 3, a) == 0 &&
              all_value(mem + packet_va, packet_size, 0xCC),
              "IoInitializeIrp refuses undersized packet without writing", NULL);
    }

    /* Device extension begins after the complete Xbox DEVICE_OBJECT, which
     * includes DeviceQueue, DeviceLock and StartIoKey. */
    {
        const uint32_t create = THUNK_VA + 9 * 4, out_va = 0x44020;
        uint32_t a[6] = { 0x44000, 12, 0, 7, 0, out_va };
        uint32_t status = call_slot(mem, create, 6, a);
        uint32_t device_va = *(uint32_t *)(mem + out_va);
        device_for_io = device_va;
        XBOX_GUEST_DEVICE_OBJECT *device = (XBOX_GUEST_DEVICE_OBJECT *)(mem + device_va);
        check(status == 0 && g_esp == SCRATCH_SP + 28 && device_va != 0,
              "IoCreateDevice allocates through ordinal 65 with six args", NULL);
        check(device->type == XBOX_IO_TYPE_DEVICE && device->size == 0x48 + 12 &&
              device->reference_count == 0 &&
              device->device_extension == device_va + XBOX_DEVICE_OBJECT_SIZE &&
              device->device_type == 7 && device->stack_size == 1 &&
              device->mounted_or_self_device == 0 &&
              device->flags == XBOX_DEVICE_FLAG_INITIALIZING &&
              all_zero(mem + device->device_extension, 12),
              "IoCreateDevice uses complete guest device layout and extension offset", NULL);
        check(device->device_queue.type == XBOX_DEVICE_QUEUE_OBJECT &&
              device->device_queue.size == 0x0c &&
              device->device_queue.device_list_head[0] == device_va + 0x2c &&
              device->device_queue.device_list_head[1] == device_va + 0x2c,
              "IoCreateDevice initializes embedded Xbox queue", NULL);
        check(device->device_lock.type == XBOX_EVENT_SYNCHRONIZATION_OBJECT &&
              device->device_lock.size == 4 && device->device_lock.signal_state == 1 &&
              device->device_lock.wait_list_head[0] == device_va + 0x3c &&
              device->device_lock.wait_list_head[1] == device_va + 0x3c,
              "IoCreateDevice initializes physical device lock event", NULL);
        /* Malformed non-NULL names must fail before allocation or exposing a
         * device. Failures after validating DeviceObject zero that output. */
        a[2] = 0x7FFFFFF0u;
        *(uint32_t *)(mem + out_va) = 0xA11CE55Au;
        status = call_slot(mem, create, 6, a);
        check(status == 0xC0000005u && *(uint32_t *)(mem + out_va) == 0 &&
              g_esp == SCRATCH_SP + 28,
              "IoCreateDevice rejects unmapped ANSI_STRING descriptor", NULL);
        a[2] = 0x44040;
        *(uint16_t *)(mem + 0x44040) = 3;
        *(uint16_t *)(mem + 0x44042) = 2;
        *(uint32_t *)(mem + 0x44044) = 0;
        *(uint32_t *)(mem + out_va) = 0xA11CE55Au;
        status = call_slot(mem, create, 6, a);
        check(status == 0xC000000Du && *(uint32_t *)(mem + out_va) == 0 &&
              g_esp == SCRATCH_SP + 28,
              "IoCreateDevice rejects Length greater than MaximumLength", NULL);
        *(uint16_t *)(mem + 0x44040) = 1;
        *(uint16_t *)(mem + 0x44042) = 1;
        *(uint32_t *)(mem + out_va) = 0xA11CE55Au;
        status = call_slot(mem, create, 6, a);
        check(status == 0xC000000Du && *(uint32_t *)(mem + out_va) == 0 &&
              g_esp == SCRATCH_SP + 28,
              "IoCreateDevice rejects nonempty ANSI_STRING with NULL Buffer", NULL);
        *(uint32_t *)(mem + 0x44044) = 0x7FFFFFF0u;
        *(uint32_t *)(mem + out_va) = 0xA11CE55Au;
        status = call_slot(mem, create, 6, a);
        check(status == 0xC0000005u && *(uint32_t *)(mem + out_va) == 0 &&
              g_esp == SCRATCH_SP + 28,
              "IoCreateDevice rejects unmapped ANSI_STRING Buffer", NULL);
        a[2] = 0;
        a[1] = UINT32_MAX - XBOX_DEVICE_OBJECT_SIZE;
        *(uint32_t *)(mem + out_va) = 0xA11CE55Au;
        status = call_slot(mem, create, 6, a);
        check(status == 0xC000000Du && *(uint32_t *)(mem + out_va) == 0 &&
              g_esp == SCRATCH_SP + 28,
              "IoCreateDevice rejects overflowing extension size before allocation", NULL);
        a[1] = 12; a[5] = UINT32_MAX;
        status = call_slot(mem, create, 6, a);
        check(status == 0xC000000Du && g_esp == SCRATCH_SP + 28,
              "IoCreateDevice rejects out-of-range guest output pointer", NULL);
    }

    /* The builders create real fixed-width guest packets. Raw offsets below
     * are independently grounded in MM3's driver code (default.xbe SHA-256
     * 2b04b66c43e7f37bbcebbfb5b72ccb96a2aa99cc2c60c53eb7d3530bcc2a3d79):
     * 0x35B2B3 reads stack+0x0c and 0x35B2B6 reads +0x04; 0x35B2D3 reads
     * +0x10; 0x35B2F4 tests Flags+2 and 0x35B2FA reads Buffer+8. At 0x35B1DE
     * it reads IoControlCode+0x10. This static disassembly establishes the
     * title's expected byte offsets, not runtime reachability or hardware
     * conformance. These tests verify guest packet ABI and dispatch only; the
     * separate native kernel_io entrypoints remain unsupported and no test
     * here claims a real backing-device transfer. */
    {
        const uint32_t build_ioctl = THUNK_VA + 6 * 4;
        const uint32_t build_fsd = THUNK_VA + 7 * 4;
        const uint32_t ios_va = 0x40800, event_va = 0x40900;
        const uint32_t buffer_va = 0x40A00, offset_va = 0x40B00;
        uint32_t a[9] = { 0 }, irp_va;
        XBOX_GUEST_IRP *irp;
        XBOX_GUEST_IO_STACK_LOCATION *slot;
        uint32_t init_event[3] = { event_va, 1, 0 };
        uint32_t init_event_thunk = THUNK_VA + 19 * 4;

        check(device_for_io != 0, "I/O builders have a real guest device", NULL);
        call_slot(mem, init_event_thunk, 3, init_event);
        *(uint32_t *)(mem + offset_va) = 0x11223344u;
        *(uint32_t *)(mem + offset_va + 4) = 0x55667788u;
        a[0] = 0x222000u; a[1] = device_for_io;
        a[2] = buffer_va; a[3] = 4; a[4] = buffer_va + 0x10; a[5] = 8;
        a[6] = 0; a[7] = event_va; a[8] = ios_va;
        irp_va = call_slot(mem, build_ioctl, 9, a);
        irp = (XBOX_GUEST_IRP *)(mem + irp_va);
        slot = (XBOX_GUEST_IO_STACK_LOCATION *)(mem + irp->current_stack_location -
                                                 XBOX_IO_STACK_LOCATION_SIZE);
        check(irp_va && g_esp == SCRATCH_SP + 40 &&
              irp->type == XBOX_IO_TYPE_IRP && irp->user_iosb == ios_va &&
              irp->user_event == event_va && irp->user_buffer == buffer_va + 0x10,
              "IoBuildDeviceIoControlRequest creates synchronous guest IRP", NULL);
        check(slot->major_function == XBOX_MAJOR_DEVICE_CONTROL &&
              slot->parameters[0] == 8 && slot->parameters[1] == 4 &&
              slot->parameters[2] == buffer_va &&
              slot->parameters[3] == 0x222000u,
              "DeviceIoControl stack parameters use verified Xbox offsets", NULL);
        uint32_t slot_va = irp->current_stack_location - XBOX_IO_STACK_LOCATION_SIZE;
        check(*(uint32_t *)(mem + slot_va + 4) == 8 &&
              *(uint32_t *)(mem + slot_va + 8) == 4 &&
              *(uint32_t *)(mem + slot_va + 12) == buffer_va &&
              *(uint32_t *)(mem + slot_va + 16) == 0x222000u,
              "DeviceIoControl raw stack bytes match observed Xbox driver offsets", NULL);

        memset(a, 0, sizeof a);
        a[0] = XBOX_MAJOR_READ; a[1] = device_for_io; a[2] = buffer_va;
        a[3] = 16; a[4] = offset_va; a[5] = event_va; a[6] = ios_va;
        irp_va = call_slot(mem, build_fsd, 7, a);
        irp = (XBOX_GUEST_IRP *)(mem + irp_va);
        slot = (XBOX_GUEST_IO_STACK_LOCATION *)(mem + irp->current_stack_location -
                                                 XBOX_IO_STACK_LOCATION_SIZE);
        check(irp_va && g_esp == SCRATCH_SP + 32 &&
              (irp->flags & (XBOX_IRP_FLAG_SYNCHRONOUS_API |
                             XBOX_IRP_FLAG_READ_OPERATION)) ==
                (XBOX_IRP_FLAG_SYNCHRONOUS_API | XBOX_IRP_FLAG_READ_OPERATION),
              "IoBuildSynchronousFsdRequest builds read IRP", NULL);
        check(slot->major_function == XBOX_MAJOR_READ &&
              slot->parameters[0] == 16 &&
              slot->parameters[1] == buffer_va &&
              slot->parameters[2] == 0x11223344u &&
              slot->parameters[3] == 0x55667788u,
              "Read stack Length and LARGE_INTEGER offsets match Xbox layout", NULL);
        slot_va = irp->current_stack_location - XBOX_IO_STACK_LOCATION_SIZE;
        check(*(uint32_t *)(mem + slot_va + 4) == 16 &&
              *(uint32_t *)(mem + slot_va + 8) == buffer_va &&
              *(uint32_t *)(mem + slot_va + 12) == 0x11223344u &&
              *(uint32_t *)(mem + slot_va + 16) == 0x55667788u,
              "Read raw stack bytes match observed Xbox driver offsets", NULL);
    }

    /* Async builder IRPs are caller-owned, and packet queue insertion keeps
     * keyed FIFO ordering while invoking StartIo outside the queue lock. */
    {
        const uint32_t build_async = THUNK_VA + 12 * 4;
        const uint32_t start_packet = THUNK_VA + 13 * 4;
        const uint32_t start_next = THUNK_VA + 14 * 4;
        const uint32_t start_next_by_key = THUNK_VA + 15 * 4;
        const uint32_t alloc = THUNK_VA + 8 * 4;
        const uint32_t free_irp = THUNK_VA + 10 * 4;
        const uint32_t driver_va = 0x45040;
        const uint32_t key_va = 0x40C00;
        uint32_t a6[6] = { XBOX_MAJOR_WRITE, 0, 0x40A00, 12, 0x40B00, 0x40800 };
        uint32_t irp_va, free_arg[1];
        uint32_t alloc_arg[1] = { 1 }, pkt[3];
        XBOX_GUEST_IRP *async_irp;
        XBOX_GUEST_DEVICE_OBJECT *device =
            (XBOX_GUEST_DEVICE_OBJECT *)(mem + device_for_io);
        XBOX_GUEST_DRIVER_OBJECT *driver =
            (XBOX_GUEST_DRIVER_OBJECT *)(mem + driver_va);

        a6[1] = device_for_io;
        irp_va = call_slot(mem, build_async, 6, a6);
        async_irp = (XBOX_GUEST_IRP *)(mem + irp_va);
        XBOX_GUEST_IO_STACK_LOCATION *async_slot =
            (XBOX_GUEST_IO_STACK_LOCATION *)(mem + async_irp->current_stack_location -
                                              XBOX_IO_STACK_LOCATION_SIZE);
        check(irp_va && g_esp == SCRATCH_SP + 28 &&
              !(async_irp->flags & XBOX_IRP_FLAG_SYNCHRONOUS_API) &&
              async_slot->major_function == XBOX_MAJOR_WRITE &&
              async_slot->parameters[0] == 12 &&
              async_slot->parameters[1] == 0x40A00,
              "IoBuildAsynchronousFsdRequest builds caller-owned write packet", NULL);
        free_arg[0] = irp_va;
        (void)call_slot(mem, free_irp, 1, free_arg);

        memset(driver, 0, sizeof(*driver));
        driver->driver_start_io = TEST_STARTIO_VA;
        device->driver_object = driver_va;
        uint32_t queue_irps[3];
        for (uint32_t i = 0; i < 3; ++i) {
            queue_irps[i] = call_slot(mem, alloc, 1, alloc_arg);
            ((XBOX_GUEST_IRP *)(mem + queue_irps[i]))->original_file_object =
                (i + 1) * 100;
        }
        g_start_count = 0;
        *(uint32_t *)(mem + key_va) = 30;
        pkt[0] = device_for_io; pkt[1] = queue_irps[0]; pkt[2] = key_va;
        call_slot(mem, start_packet, 3, pkt);
        *(uint32_t *)(mem + key_va) = 10;
        pkt[1] = queue_irps[1]; call_slot(mem, start_packet, 3, pkt);
        *(uint32_t *)(mem + key_va) = 20;
        pkt[1] = queue_irps[2]; call_slot(mem, start_packet, 3, pkt);
        check(g_start_count == 1 && g_start_order[0] == 100 &&
              device->current_irp == queue_irps[0] && device->device_queue.busy == 1 &&
              g_esp == SCRATCH_SP + 16,
              "IoStartPacket starts active IRP and queues two keyed packets", NULL);
        /* Duplicate insertion of an already queued IRP must be a no-op. */
        call_slot(mem, start_packet, 3, pkt);
        uint32_t by_key[2] = { device_for_io, 15 };
        call_slot(mem, start_next_by_key, 2, by_key);
        check(g_start_count == 2 && g_start_order[1] == 300 &&
              device->current_irp == queue_irps[2] &&
              !((XBOX_GUEST_IRP *)(mem + queue_irps[2]))->tail_overlay.device_queue_entry.device_queue_inserted &&
              g_esp == SCRATCH_SP + 12,
              "IoStartNextPacketByKey selects first queued key at or above request", NULL);
        uint32_t by_device[1] = { device_for_io };
        call_slot(mem, start_next, 1, by_device);
        check(g_start_count == 3 && g_start_order[2] == 200 &&
              device->current_irp == queue_irps[1] && g_esp == SCRATCH_SP + 8,
              "IoStartNextPacket dequeues next keyed IRP with one-arg cleanup", NULL);
        call_slot(mem, start_next, 1, by_device);
        check(!device->current_irp && !device->device_queue.busy &&
              *(uint32_t *)(mem + device_for_io + 0x2c) == device_for_io + 0x2c &&
              g_esp == SCRATCH_SP + 8,
              "IoStartNextPacket empties queue and clears busy state", NULL);
        {
            uint32_t head_va = device_for_io + 0x2c;
            uint32_t entry_va = queue_irps[0] + offsetof(XBOX_GUEST_IRP, tail_overlay);
            uint32_t saved_head[2] = { *(uint32_t *)(mem + head_va),
                                        *(uint32_t *)(mem + head_va + 4) };
            *(uint32_t *)(mem + head_va) = 0xFFFFFFF0u;
            *(uint32_t *)(mem + head_va + 4) = head_va;
            call_slot(mem, start_next, 1, by_device);
            check(*(uint32_t *)(mem + head_va) == 0xFFFFFFF0u &&
                  *(uint32_t *)(mem + head_va + 4) == head_va &&
                  !device->current_irp && !device->device_queue.busy,
                  "IoStartNextPacket rejects unmapped queue link without mutation", NULL);

            *(uint32_t *)(mem + head_va) = entry_va;
            *(uint32_t *)(mem + head_va + 4) = entry_va;
            *(uint32_t *)(mem + entry_va) = entry_va;
            *(uint32_t *)(mem + entry_va + 4) = head_va;
            *(uint8_t *)(mem + entry_va + 12) = 1;
            call_slot(mem, start_next, 1, by_device);
            check(*(uint32_t *)(mem + head_va) == entry_va &&
                  *(uint32_t *)(mem + head_va + 4) == entry_va &&
                  *(uint32_t *)(mem + entry_va) == entry_va &&
                  *(uint32_t *)(mem + entry_va + 4) == head_va &&
                  *(uint8_t *)(mem + entry_va + 12) == 1 &&
                  !device->current_irp && !device->device_queue.busy,
                  "IoStartNextPacket rejects cyclic queue without mutation", NULL);

            *(uint32_t *)(mem + head_va) = saved_head[0];
            *(uint32_t *)(mem + head_va + 4) = saved_head[1];
            *(uint32_t *)(mem + entry_va) = entry_va;
            *(uint32_t *)(mem + entry_va + 4) = entry_va;
            *(uint8_t *)(mem + entry_va + 12) = 0;
        }
        for (uint32_t i = 0; i < 3; ++i) {
            free_arg[0] = queue_irps[i];
            (void)call_slot(mem, free_irp, 1, free_arg);
        }
    }

    /* The synchronous convenience exports need a guest-thread wait contract
     * that this bridge does not provide. They fail explicitly without
     * translating/dereferencing guest pointers as host addresses. */
    {
        const uint32_t sync_ioctl = THUNK_VA + 20 * 4;
        const uint32_t sync_fsd = THUNK_VA + 21 * 4;
        uint32_t ioctl_args[8] = {
            0x222000u, device_for_io, 0x40A00, 4, 0x40A10, 8, 0x40C20, 0
        };
        uint32_t fsd_args[5] = { XBOX_MAJOR_READ, device_for_io, 0x40A00,
                                 4, 0x40B00 };
        *(uint32_t *)(mem + 0x40C20) = 0xA55AA55Au;
        check(call_slot(mem, sync_ioctl, 8, ioctl_args) == 0xC0000002u &&
              *(uint32_t *)(mem + 0x40C20) == 0xA55AA55Au &&
              g_esp == SCRATCH_SP + 36,
              "IoSynchronousDeviceIoControlRequest fails explicitly with 8-arg cleanup", NULL);
        check(call_slot(mem, sync_fsd, 5, fsd_args) == 0xC0000002u &&
              g_esp == SCRATCH_SP + 24,
              "IoSynchronousFsdRequest fails explicitly with 5-arg cleanup", NULL);
    }

    /* Synchronous builder ownership metadata must grow with live requests,
     * unregister on explicit IoFreeIrp, and allow the heap VA to be reused. */
    {
        enum { LIVE_SYNC_IRPS = 260 };
        const uint32_t build_fsd = THUNK_VA + 7 * 4;
        const uint32_t free_irp = THUNK_VA + 10 * 4;
        uint32_t irps[LIVE_SYNC_IRPS];
        uint32_t args[7] = { XBOX_MAJOR_READ, device_for_io, 0x40A00, 4,
                             0x40B00, 0, 0 };
        uint32_t free_arg[1];
        int all_built = 1, all_freed = 1;
        for (uint32_t i = 0; i < LIVE_SYNC_IRPS; ++i) {
            irps[i] = call_slot(mem, build_fsd, 7, args);
            if (!irps[i]) all_built = 0;
        }
        for (uint32_t i = 0; i < LIVE_SYNC_IRPS; ++i) {
            free_arg[0] = irps[i];
            if (!irps[i] || call_slot(mem, free_irp, 1, free_arg) != 0 ||
                xbox_HeapBlockSize(irps[i]) != 0)
                all_freed = 0;
        }
        check(all_built && all_freed,
              "synchronous IRP ownership handles 260 live allocations and frees", NULL);

        uint32_t first = call_slot(mem, build_fsd, 7, args);
        free_arg[0] = first;
        (void)call_slot(mem, free_irp, 1, free_arg);
        uint32_t reused = call_slot(mem, build_fsd, 7, args);
        check(first && reused == first && xbox_HeapBlockSize(reused) != 0,
              "IoFreeIrp unregisters ownership before guest VA reuse", NULL);
        free_arg[0] = reused;
        (void)call_slot(mem, free_irp, 1, free_arg);
    }

    /* Real driver order can complete/free the active synchronous IRP, queue a
     * new packet while DeviceObject still names that just-completed IRP, then
     * call IoStartNextPacket. The queue path must not dereference the stale
     * CurrentIrp during this documented transition window. */
    {
        const uint32_t build_fsd = THUNK_VA + 7 * 4;
        const uint32_t alloc = THUNK_VA + 8 * 4;
        const uint32_t start_packet = THUNK_VA + 13 * 4;
        const uint32_t start_next = THUNK_VA + 14 * 4;
        const uint32_t free_irp = THUNK_VA + 10 * 4;
        uint32_t sync_args[7] = { XBOX_MAJOR_READ, device_for_io, 0x40A00,
                                  4, 0x40B00, 0, 0 };
        uint32_t alloc_args[1] = { 1 }, packet_args[3];
        uint32_t next_irp = call_slot(mem, alloc, 1, alloc_args);
        uint32_t active_irp = call_slot(mem, build_fsd, 7, sync_args);
        uint32_t empty_queue[1] = { device_for_io };
        XBOX_GUEST_DEVICE_OBJECT *device =
            (XBOX_GUEST_DEVICE_OBJECT *)(mem + device_for_io);
        ((XBOX_GUEST_IRP *)(mem + next_irp))->original_file_object = 902;
        ((XBOX_GUEST_IRP *)(mem + active_irp))->original_file_object = 901;
        g_start_count = 0;
        g_start_action = 1;
        g_start_complete_entry = THUNK_VA + 17 * 4;
        g_start_packet_entry = start_packet;
        g_start_enqueue_irp = next_irp;
        g_start_enqueue_key_va = 0;
        g_start_completed_and_freed = 0;
        packet_args[0] = device_for_io;
        packet_args[1] = active_irp;
        packet_args[2] = 0;
        call_slot(mem, start_packet, 3, packet_args);
        check(g_start_completed_and_freed && g_start_count == 1 &&
              device->current_irp == active_irp && device->device_queue.busy == 1 &&
              ((XBOX_GUEST_IRP *)(mem + next_irp))->tail_overlay.device_queue_entry.device_queue_inserted,
              "StartPacket queues behind just-completed active IRP", NULL);
        call_slot(mem, start_next, 1, empty_queue);
        check(g_start_count == 2 && g_start_order[1] == 902 &&
              device->current_irp == next_irp && device->device_queue.busy == 1 &&
              !((XBOX_GUEST_IRP *)(mem + next_irp))->tail_overlay.device_queue_entry.device_queue_inserted,
              "StartNextPacket starts queued request after completion transition", NULL);
        call_slot(mem, start_next, 1, empty_queue);
        uint32_t free_args[1] = { next_irp };
        (void)call_slot(mem, free_irp, 1, free_args);
    }

    /* IofCallDriver enters the driver table through a guest x86 stdcall frame;
     * completion writes the caller's guest IOSB and signals its guest event. */
    {
        const uint32_t build_fsd = THUNK_VA + 7 * 4;
        const uint32_t call_driver = THUNK_VA + 16 * 4;
        const uint32_t complete = THUNK_VA + 17 * 4;
        const uint32_t alloc = THUNK_VA + 8 * 4;
        const uint32_t driver_va = 0x45040, event_va = 0x40900;
        const uint32_t ios_va = 0x40800, buffer_va = 0x40A00;
        uint32_t a[7] = { XBOX_MAJOR_READ, 0, buffer_va, 4, 0, event_va, ios_va };
        uint32_t irp_va, invalid_dispatch_va;
        XBOX_GUEST_DRIVER_OBJECT *driver = (XBOX_GUEST_DRIVER_OBJECT *)(mem + driver_va);
        XBOX_GUEST_IRP *irp;

        memset(driver, 0, sizeof(*driver));
        driver->major_function[XBOX_MAJOR_READ] = TEST_DISPATCH_VA;
        ((XBOX_GUEST_DEVICE_OBJECT *)(mem + device_for_io))->driver_object = driver_va;
        a[1] = device_for_io;
        g_complete_entry = complete;
        *(uint32_t *)(mem + ios_va) = 0xFFFFFFFFu;
        *(uint32_t *)(mem + ios_va + 4) = 0xFFFFFFFFu;
        *(uint32_t *)(mem + event_va + 4) = 0;
        g_dispatch_mode = 0;
        irp_va = call_slot(mem, build_fsd, 7, a);
        g_ebx = 0xBEBEBEBEu; g_esi = 0x51515151u; g_edi = 0xD1D1D1D1u;
        uint32_t dispatch_status = call_fast_slot(mem, call_driver, device_for_io, irp_va);
        char dispatch_detail[160];
        snprintf(dispatch_detail, sizeof dispatch_detail,
                 "status=%08x esp=%08x ebx=%08x esi=%08x edi=%08x nested=%u",
                 dispatch_status, g_esp, g_ebx, g_esi, g_edi, g_nested_preserved);
        check(dispatch_status == 0 &&
              g_esp == SCRATCH_SP && g_ebx == 0xBEBEBEBEu &&
              g_esi == 0x51515151u && g_edi == 0xD1D1D1D1u,
              "IofCallDriver dispatches guest callback and restores caller state", dispatch_detail);
        check(*(uint32_t *)(mem + ios_va) == 0 &&
              *(uint32_t *)(mem + ios_va + 4) == 7 &&
              *(int32_t *)(mem + event_va + 4) == 1 && g_nested_preserved,
              "nested IofCompleteRequest writes IOSB and signals event", NULL);
        uint32_t wait_args[5] = { event_va, 0, 0, 0, 0 };
        check(call_slot(mem, THUNK_VA + 22 * 4, 5, wait_args) == 0 &&
              *(int32_t *)(mem + event_va + 4) == 0,
              "KeWaitForSingleObject consumes completed synchronization event", NULL);

        *(uint32_t *)(mem + ios_va) = 0xFFFFFFFFu;
        *(uint32_t *)(mem + ios_va + 4) = 0xFFFFFFFFu;
        *(int32_t *)(mem + event_va + 4) = 0;
        g_dispatch_mode = 1;
        irp_va = call_slot(mem, build_fsd, 7, a);
        check(call_fast_slot(mem, call_driver, device_for_io, irp_va) == 0x103u &&
              *(uint32_t *)(mem + ios_va) == 0xFFFFFFFFu &&
              *(uint32_t *)(mem + ios_va + 4) == 0xFFFFFFFFu &&
              *(int32_t *)(mem + event_va + 4) == 0,
              "pending dispatch retains IRP without premature completion", NULL);
        irp = (XBOX_GUEST_IRP *)(mem + irp_va);
        irp->io_status.status = 0;
        irp->io_status.information = 11;
        check(call_fast_slot(mem, complete, irp_va, 0) == 0 &&
              *(uint32_t *)(mem + ios_va) == 0 &&
              *(uint32_t *)(mem + ios_va + 4) == 11 &&
              *(int32_t *)(mem + event_va + 4) == 1,
              "later completion publishes retained pending IRP result", NULL);
        check(call_slot(mem, THUNK_VA + 22 * 4, 5, wait_args) == 0 &&
              *(int32_t *)(mem + event_va + 4) == 0,
              "KeWaitForSingleObject consumes pending completion event", NULL);
        check(xbox_HeapBlockSize(irp_va) == 0,
              "synchronous builder IRP is released after completion", NULL);

        /* A major-function entry may point directly to an imported kernel
         * thunk such as IoInvalidDeviceRequest, not only generated code. */
        invalid_dispatch_va = *(uint32_t *)(mem + THUNK_VA + 18 * 4);
        driver->major_function[XBOX_MAJOR_READ] = invalid_dispatch_va;
        uint32_t stack_args[1] = { 1 };
        irp_va = call_slot(mem, alloc, 1, stack_args);
        irp = (XBOX_GUEST_IRP *)(mem + irp_va);
        irp->user_iosb = ios_va;
        irp->user_event = event_va;
        ((XBOX_GUEST_IO_STACK_LOCATION *)(mem + irp->current_stack_location -
          XBOX_IO_STACK_LOCATION_SIZE))->major_function = XBOX_MAJOR_READ;
        *(int32_t *)(mem + event_va + 4) = 0;
        *(uint32_t *)(mem + ios_va + 4) = 0xFFFFFFFFu;
        check(call_fast_slot(mem, call_driver, device_for_io, irp_va) == 0xC0000010u &&
              irp->io_status.status == 0xC0000010u && irp->io_status.information == 0 &&
              *(uint32_t *)(mem + ios_va) == 0xC0000010u &&
              *(uint32_t *)(mem + ios_va + 4) == 0 &&
              *(int32_t *)(mem + event_va + 4) == 1,
              "IoInvalidDeviceRequest thunk rejects and completes guest IRP", NULL);
        uint32_t free_args[1] = { irp_va };
        free_args[0] = irp_va;
        (void)call_slot(mem, THUNK_VA + 10 * 4, 1, free_args);
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

    xbox_MemoryLayoutShutdown();
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
