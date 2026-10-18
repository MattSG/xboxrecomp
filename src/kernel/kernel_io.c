/*
 * kernel_io.c - I/O Manager Stubs
 *
 * Implements IoCreateDevice, IoDeleteDevice, IRP management, and I/O
 * completion stubs.
 *
 * The Xbox I/O manager is used internally by XDK libraries (DSOUND, D3D,
 * storage drivers). Since we replace those libraries entirely with Win32
 * equivalents, most I/O manager functions are stubs that return success.
 */

#include "kernel.h"
#include "kernel_guest_io.h"
#include "xbox_memory_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Type Object Pointers
 *
 * Exported type objects for device and completion port types.
 * Game code rarely uses these directly - they're mainly for the
 * XDK libraries we're replacing.
 * ============================================================================ */

static ULONG g_device_object_type_data     = 0x44455643; /* 'DEVC' */
static ULONG g_completion_object_type_data = 0x434F4D50; /* 'COMP' */
static ULONG g_file_object_type_data       = 0x46494C45; /* 'FILE' */

PVOID xbox_IoDeviceObjectType     = &g_device_object_type_data;
PVOID xbox_IoCompletionObjectType = &g_completion_object_type_data;
PVOID xbox_IoFileObjectType       = &g_file_object_type_data;

/* ============================================================================
 * Device Management
 *
 * IoCreateDevice/IoDeleteDevice create kernel device objects. Since we
 * don't have real kernel drivers, we allocate dummy objects that satisfy
 * the interface without actual device functionality.
 * ============================================================================ */

/* Minimal device object - just enough to not crash if someone reads fields */
typedef struct _XBOX_FAKE_DEVICE {
    ULONG Type;
    ULONG Size;
    PVOID DeviceExtension;
    PVOID DriverObject;
} XBOX_FAKE_DEVICE;

NTSTATUS __stdcall xbox_IoCreateDevice(
    PVOID DriverObject,
    ULONG DeviceExtensionSize,
    PXBOX_ANSI_STRING DeviceName,
    ULONG DeviceType,
    BOOLEAN Exclusive,
    PVOID* DeviceObject)
{
    XBOX_FAKE_DEVICE* device;
    ULONG total_size;

    (void)DriverObject;
    (void)DeviceType;
    (void)Exclusive;

    if (!DeviceObject)
        return STATUS_INVALID_PARAMETER;

    total_size = sizeof(XBOX_FAKE_DEVICE) + DeviceExtensionSize;
    device = (XBOX_FAKE_DEVICE*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total_size);
    if (!device) {
        *DeviceObject = NULL;
        return STATUS_NO_MEMORY;
    }

    device->Type = DeviceType;
    device->Size = total_size;
    device->DriverObject = DriverObject;
    device->DeviceExtension = (DeviceExtensionSize > 0)
        ? (PVOID)((UCHAR*)device + sizeof(XBOX_FAKE_DEVICE))
        : NULL;

    *DeviceObject = device;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_IO,
        "IoCreateDevice: '%.*s' type=%u ext_size=%u → %p",
        DeviceName ? DeviceName->Length : 0,
        DeviceName ? DeviceName->Buffer : "<null>",
        DeviceType, DeviceExtensionSize, device);

    return STATUS_SUCCESS;
}

VOID __stdcall xbox_IoDeleteDevice(PVOID DeviceObject)
{
    if (DeviceObject) {
        xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_IO,
            "IoDeleteDevice: %p", DeviceObject);
        HeapFree(GetProcessHeap(), 0, DeviceObject);
    }
}

/* ============================================================================
 * IRP Management Stubs
 *
 * IRPs (I/O Request Packets) are the core mechanism for driver I/O on Xbox.
 * Since we don't use real drivers, these are all stubs.
 * ============================================================================ */

int xbox_IoInitializeIrpGuest(uint32_t guest_va, PVOID Irp, USHORT PacketSize, CCHAR StackSize)
{
    uint32_t required;
    XBOX_GUEST_IRP *irp = (XBOX_GUEST_IRP *)Irp;

    if (!irp || StackSize <= 0)
        return 0;
    required = XBOX_IRP_SIZE + (uint32_t)(uint8_t)StackSize * XBOX_IO_STACK_LOCATION_SIZE;
    if (PacketSize < required || guest_va > UINT32_MAX - required)
        return 0;

    memset(irp, 0, PacketSize);
    irp->type = XBOX_IO_TYPE_IRP;
    irp->size = PacketSize;
    irp->stack_count = StackSize;
    irp->current_location = (int8_t)(StackSize + 1);
    irp->current_stack_location = guest_va + XBOX_IRP_SIZE +
        (uint32_t)(uint8_t)StackSize * XBOX_IO_STACK_LOCATION_SIZE;
    return 1;
}

VOID __stdcall xbox_IoInitializeIrp(PVOID Irp, USHORT PacketSize, CCHAR StackSize)
{
    uint32_t guest_va;
    if (!Irp)
        return;
    guest_va = (uint32_t)((uintptr_t)Irp - (uintptr_t)xbox_GetMemoryOffset());
    (void)xbox_IoInitializeIrpGuest(guest_va, Irp, PacketSize, StackSize);
}

VOID __stdcall xbox_IoStartNextPacket(PVOID DeviceObject, BOOLEAN Cancelable)
{
    (void)DeviceObject;
    (void)Cancelable;
}

VOID __stdcall xbox_IoStartNextPacketByKey(PVOID DeviceObject, BOOLEAN Cancelable, ULONG Key)
{
    (void)DeviceObject;
    (void)Cancelable;
    (void)Key;
}

VOID __stdcall xbox_IoStartPacket(PVOID DeviceObject, PVOID Irp, PULONG Key, PVOID CancelFunction)
{
    (void)DeviceObject;
    (void)Irp;
    (void)Key;
    (void)CancelFunction;
}

VOID __stdcall xbox_IoMarkIrpMustComplete(PVOID Irp)
{
    (void)Irp;
}

/* ============================================================================
 * I/O Completion Ports
 * ============================================================================ */

NTSTATUS __stdcall xbox_IoSetIoCompletion(
    PVOID IoCompletion,
    PVOID KeyContext,
    PVOID ApcContext,
    NTSTATUS IoStatus,
    ULONG_PTR IoStatusInformation)
{
    (void)IoCompletion;
    (void)KeyContext;
    (void)ApcContext;
    (void)IoStatus;
    (void)IoStatusInformation;

    xbox_log(XBOX_LOG_TRACE, XBOX_LOG_IO,
        "IoSetIoCompletion: completion=%p status=0x%08X (stubbed)",
        IoCompletion, IoStatus);

    return STATUS_SUCCESS;
}

/* ============================================================================
 * Synchronous Device I/O
 *
 * These functions perform synchronous I/O through the driver stack.
 * Stubbed since we replace all Xbox drivers.
 * ============================================================================ */

NTSTATUS __stdcall xbox_IoSynchronousDeviceIoControlRequest(
    ULONG IoControlCode,
    PVOID DeviceObject,
    PVOID InputBuffer,
    ULONG InputBufferLength,
    PVOID OutputBuffer,
    ULONG OutputBufferLength,
    PULONG ReturnedOutputBufferLength,
    BOOLEAN InternalDeviceIoControl)
{
    (void)IoControlCode;
    (void)DeviceObject;
    (void)InputBuffer;
    (void)InputBufferLength;
    (void)OutputBuffer;
    (void)OutputBufferLength;
    (void)InternalDeviceIoControl;

    if (ReturnedOutputBufferLength)
        *ReturnedOutputBufferLength = 0;

    xbox_log(XBOX_LOG_TRACE, XBOX_LOG_IO,
        "IoSynchronousDeviceIoControlRequest: ioctl=0x%08X device=%p (stubbed)",
        IoControlCode, DeviceObject);

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_IoBuildDeviceIoControlRequest(
    ULONG IoControlCode,
    PVOID DeviceObject,
    PVOID InputBuffer,
    ULONG InputBufferLength,
    PVOID OutputBuffer,
    ULONG OutputBufferLength,
    BOOLEAN InternalDeviceIoControl,
    HANDLE Event,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock)
{
    (void)IoControlCode;
    (void)DeviceObject;
    (void)InputBuffer;
    (void)InputBufferLength;
    (void)OutputBuffer;
    (void)OutputBufferLength;
    (void)InternalDeviceIoControl;
    (void)Event;

    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = 0;
    }

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_IoSynchronousFsdRequest(
    ULONG MajorFunction,
    PVOID DeviceObject,
    PVOID Buffer,
    ULONG Length,
    PLARGE_INTEGER StartingOffset)
{
    (void)MajorFunction;
    (void)DeviceObject;
    (void)Buffer;
    (void)Length;
    (void)StartingOffset;

    xbox_log(XBOX_LOG_TRACE, XBOX_LOG_IO,
        "IoSynchronousFsdRequest: major=%u device=%p (stubbed)",
        MajorFunction, DeviceObject);

    return STATUS_SUCCESS;
}

PVOID __stdcall xbox_IoBuildSynchronousFsdRequest(
    ULONG MajorFunction,
    PVOID DeviceObject,
    PVOID Buffer,
    ULONG Length,
    PLARGE_INTEGER StartingOffset,
    HANDLE Event,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock)
{
    (void)DeviceObject;
    (void)Buffer;
    (void)Length;
    (void)StartingOffset;
    (void)Event;

    /*
     * Returns a built IRP for the caller to hand to IofCallDriver. We have no
     * driver stack, so there is nothing meaningful to build. Returning NULL is
     * the documented allocation-failure path, which callers already handle;
     * inventing a fake IRP would only push the failure to whoever dereferences
     * it. IoStatusBlock is filled in so a caller that reads it without checking
     * the return sees a coherent result.
     */
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = 0;
    }

    xbox_log(XBOX_LOG_WARN, XBOX_LOG_IO,
        "IoBuildSynchronousFsdRequest: major=%u len=%u - no driver stack, returning NULL",
        MajorFunction, Length);

    return NULL;
}

/* ============================================================================
 * Driver Dispatch
 *
 * IofCallDriver/IofCompleteRequest are __fastcall on Xbox (the 'f' suffix).
 * Getting the convention wrong here corrupts the stack at every call site.
 * ============================================================================ */

NTSTATUS __fastcall xbox_IofCallDriver(PVOID DeviceObject, PVOID Irp)
{
    (void)DeviceObject;
    (void)Irp;

    xbox_log(XBOX_LOG_TRACE, XBOX_LOG_IO,
        "IofCallDriver: device=%p irp=%p (stubbed)", DeviceObject, Irp);

    return STATUS_SUCCESS;
}

VOID __fastcall xbox_IofCompleteRequest(PVOID Irp, CCHAR PriorityBoost)
{
    (void)Irp;
    (void)PriorityBoost;
}

NTSTATUS __stdcall xbox_IoInvalidDeviceRequest(PVOID DeviceObject, PVOID Irp)
{
    (void)DeviceObject;
    (void)Irp;

    /*
     * The default dispatch entry for major functions a driver does not handle.
     * Its whole contract is to reject, so this is a real implementation rather
     * than a stub.
     */
    return STATUS_INVALID_DEVICE_REQUEST;
}

/* ============================================================================
 * Symbolic Links
 *
 * Games create links like \??\D: -> \Device\Cdrom0. Actual path translation is
 * table-driven in kernel_path.c, which already knows the standard Xbox drive
 * letters, so these only need to record the mapping and stay consistent:
 * creating a duplicate must collide, and deleting an unknown link must fail.
 * ============================================================================ */

#define XBOX_MAX_SYMLINKS 32

typedef struct _XBOX_SYMLINK {
    char link[64];
    char target[128];
    BOOL used;
} XBOX_SYMLINK;

static XBOX_SYMLINK g_symlinks[XBOX_MAX_SYMLINKS];

/* Copy a counted, non-NUL-terminated ANSI_STRING into a C buffer. */
static void xbox_copy_ansi(char* dst, size_t dst_size, PXBOX_ANSI_STRING src)
{
    size_t n;

    if (!dst || dst_size == 0)
        return;
    dst[0] = '\0';
    if (!src || !src->Buffer)
        return;

    n = (size_t)src->Length;
    if (n >= dst_size)
        n = dst_size - 1;
    memcpy(dst, src->Buffer, n);
    dst[n] = '\0';
}

static XBOX_SYMLINK* xbox_find_symlink(const char* name)
{
    int i;
    for (i = 0; i < XBOX_MAX_SYMLINKS; i++) {
        if (g_symlinks[i].used && strcmp(g_symlinks[i].link, name) == 0)
            return &g_symlinks[i];
    }
    return NULL;
}

const char* xbox_LookupSymbolicLink(const char* link)
{
    XBOX_SYMLINK* entry;

    if (!link)
        return NULL;
    entry = xbox_find_symlink(link);
    return entry ? entry->target : NULL;
}

NTSTATUS __stdcall xbox_IoCreateSymbolicLink(
    PXBOX_ANSI_STRING SymbolicLinkName,
    PXBOX_ANSI_STRING DeviceName)
{
    char link[64], target[128];
    int i;

    if (!SymbolicLinkName)
        return STATUS_INVALID_PARAMETER;

    xbox_copy_ansi(link, sizeof(link), SymbolicLinkName);
    xbox_copy_ansi(target, sizeof(target), DeviceName);

    if (link[0] == '\0')
        return STATUS_INVALID_PARAMETER;

    if (xbox_find_symlink(link)) {
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_IO,
            "IoCreateSymbolicLink: '%s' already exists", link);
        return STATUS_OBJECT_NAME_COLLISION;
    }

    for (i = 0; i < XBOX_MAX_SYMLINKS; i++) {
        if (!g_symlinks[i].used) {
            g_symlinks[i].used = TRUE;
            strcpy(g_symlinks[i].link, link);
            strcpy(g_symlinks[i].target, target);
            xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_IO,
                "IoCreateSymbolicLink: '%s' -> '%s'", link, target);
            return STATUS_SUCCESS;
        }
    }

    xbox_log(XBOX_LOG_ERROR, XBOX_LOG_IO,
        "IoCreateSymbolicLink: table full (%d), dropping '%s'",
        XBOX_MAX_SYMLINKS, link);
    return STATUS_INSUFFICIENT_RESOURCES;
}

NTSTATUS __stdcall xbox_IoDeleteSymbolicLink(PXBOX_ANSI_STRING SymbolicLinkName)
{
    char link[64];
    XBOX_SYMLINK* entry;

    if (!SymbolicLinkName)
        return STATUS_INVALID_PARAMETER;

    xbox_copy_ansi(link, sizeof(link), SymbolicLinkName);
    entry = xbox_find_symlink(link);
    if (!entry) {
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_IO,
            "IoDeleteSymbolicLink: '%s' not found", link);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_IO, "IoDeleteSymbolicLink: '%s'", link);
    entry->used = FALSE;
    entry->link[0] = '\0';
    entry->target[0] = '\0';
    return STATUS_SUCCESS;
}

/* ============================================================================
 * Object handles for names opened through the Ob* calls.
 *
 * NtOpenSymbolicLinkObject and NtCreateDirectoryObject hand out a handle that
 * names the object. Values sit in 0xDE500000 + slot, a range no host handle
 * or bridge token uses, so the bridge recognises them by value and passes them
 * through unchanged. A link handle keeps the target it had at open time, so a
 * later IoDeleteSymbolicLink does not invalidate a handle already in use.
 * ============================================================================ */

#define XBOX_OBJ_HANDLE_BASE 0xDE500000u
#define XBOX_MAX_OBJ_HANDLES 32

typedef struct {
    BOOL used;
    int  is_link;               /* 1 = symbolic link, 0 = directory */
    char name[128];
    char target[128];           /* link target; empty for directories */
} XBOX_OBJ_HANDLE;

static XBOX_OBJ_HANDLE g_obj_handles[XBOX_MAX_OBJ_HANDLES + 1];

/* Drive links the runtime knows without a title registering them. The targets
 * are the device paths kernel_path.c documents for each drive letter. */
static const struct { const char* key; const char* target; } s_builtin_links[] = {
    { "D:", "\\Device\\CdRom0" },
    { "T:", "\\Device\\Harddisk0\\Partition1\\TDATA" },
    { "U:", "\\Device\\Harddisk0\\Partition1\\UDATA" },
};

/* Target of the link named by an OBJECT_ATTRIBUTES name, or NULL. The name
 * may carry the "\??\" prefix or not; the table keys carry it. */
static const char* xbox_open_link_target(const char* name)
{
    const char* key = name;
    char full[160];
    const char* target;
    size_t i;

    if (strncmp(key, "\\??\\", 4) == 0)
        key += 4;
    if (strlen(key) >= sizeof(full) - 4)
        return NULL;
    strcpy(full, "\\??\\");
    strcat(full, key);
    target = xbox_LookupSymbolicLink(full);
    if (target)
        return target;
    for (i = 0; i < sizeof(s_builtin_links) / sizeof(s_builtin_links[0]); i++)
        if (_stricmp(key, s_builtin_links[i].key) == 0)
            return s_builtin_links[i].target;
    return NULL;
}

static HANDLE xbox_obj_handle_alloc(const char* name, int is_link, const char* target)
{
    int i;
    for (i = 1; i <= XBOX_MAX_OBJ_HANDLES; i++) {
        if (!g_obj_handles[i].used) {
            g_obj_handles[i].used = TRUE;
            g_obj_handles[i].is_link = is_link;
            strncpy(g_obj_handles[i].name, name, sizeof(g_obj_handles[i].name) - 1);
            g_obj_handles[i].name[sizeof(g_obj_handles[i].name) - 1] = '\0';
            strncpy(g_obj_handles[i].target, target ? target : "",
                    sizeof(g_obj_handles[i].target) - 1);
            g_obj_handles[i].target[sizeof(g_obj_handles[i].target) - 1] = '\0';
            return (HANDLE)(uintptr_t)(XBOX_OBJ_HANDLE_BASE + (uint32_t)i);
        }
    }
    return NULL;
}

/* Slot of an object handle, or 0 when h is not one this table issued. */
static int xbox_obj_handle_slot(HANDLE h)
{
    uintptr_t v = (uintptr_t)h;
    int i;
    if (v < XBOX_OBJ_HANDLE_BASE + 1u || v > XBOX_OBJ_HANDLE_BASE + XBOX_MAX_OBJ_HANDLES)
        return 0;
    i = (int)(v - XBOX_OBJ_HANDLE_BASE);
    return g_obj_handles[i].used ? i : 0;
}

NTSTATUS xbox_OpenSymbolicLink(const char* name, HANDLE* out)
{
    const char* target;
    HANDLE h;

    if (!name || !out)
        return STATUS_INVALID_PARAMETER;
    target = xbox_open_link_target(name);
    if (!target) {
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_IO,
            "NtOpenSymbolicLinkObject: '%s' not found", name);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    h = xbox_obj_handle_alloc(name, 1, target);
    if (!h) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_IO,
            "NtOpenSymbolicLinkObject: handle table full, dropping '%s'", name);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_IO,
        "NtOpenSymbolicLinkObject: '%s' -> '%s'", name, target);
    *out = h;
    return STATUS_SUCCESS;
}

/* Target recorded for a link handle, or NULL if h is not a link handle. */
const char* xbox_SymbolicLinkHandleTarget(HANDLE h)
{
    int i = xbox_obj_handle_slot(h);
    if (!i || !g_obj_handles[i].is_link)
        return NULL;
    return g_obj_handles[i].target;
}

NTSTATUS xbox_CreateDirectoryHandle(const char* name, HANDLE* out)
{
    HANDLE h;

    if (!out)
        return STATUS_INVALID_PARAMETER;
    h = xbox_obj_handle_alloc(name ? name : "", 0, NULL);
    if (!h) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_IO,
            "NtCreateDirectoryObject: handle table full, dropping '%s'",
            name ? name : "");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    *out = h;
    return STATUS_SUCCESS;
}

/* 1 if h is an object handle this table issued and it is still open. */
int xbox_IsObjectHandle(HANDLE h)
{
    return xbox_obj_handle_slot(h) != 0;
}

/* Releases an object handle. 1 if h was one of ours, 0 if it was not. */
int xbox_CloseObjectHandle(HANDLE h)
{
    int i = xbox_obj_handle_slot(h);
    if (!i)
        return 0;
    g_obj_handles[i].used = FALSE;
    return 1;
}

/* ============================================================================
 * Unsupported IOCTL and FS control codes.
 *
 * A title that polls a code this layer does not implement calls it every
 * frame. The first call per code is logged; later calls only count, and the
 * totals are printed at exit. Guest threads call in concurrently, so the table
 * is guarded by g_ioctl_notes_lock; the log lines are printed under it too,
 * which keeps a code from being logged twice on a race.
 * ============================================================================ */

#define XBOX_IOCTL_NOTE_MAX 16

static struct { ULONG code; const char* what; unsigned long long count; }
    g_ioctl_notes[XBOX_IOCTL_NOTE_MAX];
static int g_ioctl_notes_used;
static int g_ioctl_notes_registered;
static SRWLOCK g_ioctl_notes_lock = SRWLOCK_INIT;

static void xbox_ioctl_notes_report(void)
{
    int i;
    AcquireSRWLockExclusive(&g_ioctl_notes_lock);
    if (g_ioctl_notes_used) {
        fprintf(stderr, "  [FILE] unsupported control codes (calls after the first):\n");
        for (i = 0; i < g_ioctl_notes_used; i++)
            fprintf(stderr, "  [FILE]   %s(0x%X) x%llu\n", g_ioctl_notes[i].what,
                    (unsigned)g_ioctl_notes[i].code, g_ioctl_notes[i].count);
        fflush(stderr);
    }
    ReleaseSRWLockExclusive(&g_ioctl_notes_lock);
}

void xbox_NoteUnsupportedIoctl(const char* what, ULONG code)
{
    int i;

    AcquireSRWLockExclusive(&g_ioctl_notes_lock);
    for (i = 0; i < g_ioctl_notes_used; i++) {
        if (g_ioctl_notes[i].code == code && strcmp(g_ioctl_notes[i].what, what) == 0) {
            g_ioctl_notes[i].count++;
            ReleaseSRWLockExclusive(&g_ioctl_notes_lock);
            return;
        }
    }
    if (!g_ioctl_notes_registered) {
        g_ioctl_notes_registered = 1;
        atexit(xbox_ioctl_notes_report);
    }
    if (g_ioctl_notes_used == XBOX_IOCTL_NOTE_MAX) {
        fprintf(stderr, "  [FILE] %s(0x%X) - unsupported (note table full)\n",
                what, (unsigned)code);
    } else {
        g_ioctl_notes[g_ioctl_notes_used].code = code;
        g_ioctl_notes[g_ioctl_notes_used].what = what;
        g_ioctl_notes[g_ioctl_notes_used].count = 1;
        g_ioctl_notes_used++;
        fprintf(stderr, "  [FILE] %s(0x%X) - unsupported\n", what, (unsigned)code);
        fflush(stderr);
    }
    ReleaseSRWLockExclusive(&g_ioctl_notes_lock);
}
