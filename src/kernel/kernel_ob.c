/* Native object/handle adapters. These do not implement a guest object
 * namespace, typed validation or guest reference-counted lifetime.
 * Guest ABI bridges need their own supported object representation. */

#include "kernel.h"

/* Opaque native type identity markers. These are not guest type-object
 * layouts; ObReferenceObjectByHandle below does not validate types. */

static ULONG g_thread_object_type_data = 0x54485244; /* 'THRD' */
static ULONG g_event_object_type_data  = 0x45564E54; /* 'EVNT' */

PVOID xbox_PsThreadObjectType = &g_thread_object_type_data;
PVOID xbox_ExEventObjectType  = &g_event_object_type_data;

/* Reference operations are no-ops in this native adapter. Explicit handle
 * closure owns lifetime; this is not Xbox object reference counting. */

VOID __fastcall xbox_ObfReferenceObject(PVOID Object)
{
    /* No native reference-count state is maintained here. */
    (void)Object;
    XBOX_TRACE(XBOX_LOG_OB, "ObfReferenceObject: %p", Object);
}

VOID __fastcall xbox_ObfDereferenceObject(PVOID Object)
{
    /* Keep explicit handle closure separate from this no-op adapter. */
    (void)Object;
    XBOX_TRACE(XBOX_LOG_OB, "ObfDereferenceObject: %p", Object);
}

/* ============================================================================
 * ObReferenceObjectByHandle
 *
 * Retrieves the object pointer for a given handle. On Xbox, this returns
 * a pointer to the kernel object structure. On Windows, we simply pass
 * the handle through as the "object pointer" since our Ke* functions
 * treat object pointers as HANDLEs.
 * ============================================================================ */

NTSTATUS __stdcall xbox_ObReferenceObjectByHandle(
    HANDLE Handle,
    PVOID ObjectType,
    PVOID* Object)
{
    (void)ObjectType;

    if (!Object)
        return STATUS_INVALID_PARAMETER;

    if (!Handle || Handle == INVALID_HANDLE_VALUE) {
        *Object = NULL;
        return STATUS_INVALID_HANDLE;
    }

    /* Return the handle as the "object pointer" */
    *Object = (PVOID)Handle;

    XBOX_TRACE(XBOX_LOG_OB, "ObReferenceObjectByHandle: handle=%p → object=%p",
        Handle, *Object);

    return STATUS_SUCCESS;
}

/* ============================================================================
 * ObReferenceObjectByName
 *
 * Looks up a named kernel object. On Xbox, named objects include devices,
 * symbolic links, etc. Since we don't maintain a kernel object namespace,
 * this is stubbed to return not-found for most cases.
 * ============================================================================ */

NTSTATUS __stdcall xbox_ObReferenceObjectByName(
    PXBOX_ANSI_STRING ObjectName,
    ULONG Attributes,
    PVOID ObjectType,
    PVOID ParseContext,
    PVOID* Object)
{
    (void)Attributes;
    (void)ObjectType;
    (void)ParseContext;

    if (!Object)
        return STATUS_INVALID_PARAMETER;

    *Object = NULL;

    xbox_log(XBOX_LOG_WARN, XBOX_LOG_OB,
        "ObReferenceObjectByName: '%.*s' (not found - stubbed)",
        ObjectName ? ObjectName->Length : 0,
        ObjectName ? ObjectName->Buffer : "<null>");

    return STATUS_OBJECT_NAME_NOT_FOUND;
}
