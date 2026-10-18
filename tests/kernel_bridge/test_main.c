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
#define SCRATCH_SP 0x30000u
#define THUNK_SLOTS 12

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
    check(mem != NULL, "mapped guest memory available", NULL);
    if (!mem) {
        printf("\n%d failure(s); the rest needs guest memory.\n", failures);
        return 1;
    }

    /* Imports, in the form an XBE stores them: 0x80000000 | ordinal. Slot 0 is
     * ordinal 173; the rest are the symbolic link, handle and event calls
     * checked at the end. */
    static const uint32_t ordinals[THUNK_SLOTS] = {
        173, 67, 203, 215, 187, 225, 61, 62, 59, 65, 72, 73
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

    /* IOSBs are 8-byte guest structures. The host bridge must marshal them;
     * passing the guest pointer as a native IO_STATUS_BLOCK writes Information
     * at host offset +8 and corrupts the adjacent guest canary. The zero
     * builder return below is existing unsupported-builder behavior, not
     * successful IRP creation; these checks cover only marshaling and stack ABI. */
    {
        const uint32_t build_ioctl = THUNK_VA + 6 * 4;
        const uint32_t build_fsd = THUNK_VA + 7 * 4;
        const uint32_t ios_va = 0x40800;
        const uint32_t canary = 0xA55A3CC3u;
        uint32_t a[9] = { 0 };
        uint32_t status;

        a[8] = ios_va;
        *(uint32_t *)(mem + ios_va) = 0xFFFFFFFFu;
        *(uint32_t *)(mem + ios_va + 4) = 0xFFFFFFFFu;
        *(uint32_t *)(mem + ios_va + 8) = canary;
        status = call_slot(mem, build_ioctl, 9, a);
        check(status == 0 && g_esp == SCRATCH_SP + 40,
              "IoBuildDeviceIoControlRequest dispatches with 9 stdcall args", NULL);
        check(*(uint32_t *)(mem + ios_va) == 0 &&
              *(uint32_t *)(mem + ios_va + 4) == 0,
              "IoBuildDeviceIoControlRequest writes guest IOSB status and information", NULL);
        check(*(uint32_t *)(mem + ios_va + 8) == canary,
              "IoBuildDeviceIoControlRequest preserves the adjacent guest canary", NULL);

        a[8] = 0;
        status = call_slot(mem, build_ioctl, 9, a);
        check(status == 0 && g_esp == SCRATCH_SP + 40,
              "IoBuildDeviceIoControlRequest accepts a null IOSB", NULL);

        memset(a, 0, sizeof a);
        a[6] = ios_va;
        *(uint32_t *)(mem + ios_va) = 0xFFFFFFFFu;
        *(uint32_t *)(mem + ios_va + 4) = 0xFFFFFFFFu;
        *(uint32_t *)(mem + ios_va + 8) = canary;
        status = call_slot(mem, build_fsd, 7, a);
        check(status == 0 && g_esp == SCRATCH_SP + 32,
              "IoBuildSynchronousFsdRequest dispatches with 7 stdcall args", NULL);
        check(*(uint32_t *)(mem + ios_va) == 0 &&
              *(uint32_t *)(mem + ios_va + 4) == 0,
              "IoBuildSynchronousFsdRequest writes guest IOSB status and information", NULL);
        check(*(uint32_t *)(mem + ios_va + 8) == canary,
              "IoBuildSynchronousFsdRequest preserves the adjacent guest canary", NULL);

        a[6] = 0;
        status = call_slot(mem, build_fsd, 7, a);
        check(status == 0 && g_esp == SCRATCH_SP + 32,
              "IoBuildSynchronousFsdRequest accepts a null IOSB", NULL);
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
