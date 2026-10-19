/* Fixed-width Xbox kernel I/O layouts. Sized against XboxDev/nxdk master at
 * 14d5ee97e73347c973f1f57b68b79ec08c9e77f2, compiled for its i386-pc-win32
 * target; see build/audit-continuation/reference/xbox-io-layout.md. These are
 * guest memory objects, not
 * host WDM structures: every pointer field is a 32-bit Xbox virtual address. */
#ifndef XBOX_KERNEL_GUEST_IO_H
#define XBOX_KERNEL_GUEST_IO_H

#include <stddef.h>
#include <stdint.h>

enum {
    XBOX_IO_TYPE_DEVICE = 3,
    XBOX_IO_TYPE_IRP = 6,
    XBOX_DEVICE_QUEUE_OBJECT = 0x14,
    XBOX_EVENT_SYNCHRONIZATION_OBJECT = 1,
    XBOX_DEVICE_TYPE_CD_ROM2 = 0x02,
    XBOX_DEVICE_TYPE_DISK2 = 0x07,
    XBOX_DEVICE_TYPE_MEMORY_UNIT = 0x3a,
    XBOX_DEVICE_TYPE_MEDIA_BOARD = 0x3b,
    XBOX_DEVICE_FLAG_EXCLUSIVE = 0x02,
    XBOX_DEVICE_FLAG_HAS_NAME = 0x08,
    XBOX_DEVICE_FLAG_INITIALIZING = 0x10,
    XBOX_IO_STACK_LOCATION_SIZE = 0x18,
    XBOX_DEVICE_OBJECT_SIZE = 0x48,
    XBOX_IRP_SIZE = 0x68,
    XBOX_IRP_CURRENT_STACK_LOCATION_OFFSET = 0x5c,
    XBOX_DEVICE_EXTENSION_OFFSET = 0x18,
    XBOX_DEVICE_TYPE_OFFSET = 0x1c,
    XBOX_DEVICE_STACK_SIZE_OFFSET = 0x1e,
    XBOX_DEVICE_FLAGS_OFFSET = 0x14,
    XBOX_DEVICE_EXTENSION_BASE_SIZE = XBOX_DEVICE_OBJECT_SIZE
};

enum {
    XBOX_IRP_FLAG_SYNCHRONOUS_API = 0x00000004,
    XBOX_IRP_FLAG_READ_OPERATION = 0x00000010,
    XBOX_IRP_FLAG_WRITE_OPERATION = 0x00000020,
    XBOX_SL_PENDING_RETURNED = 0x01,
    XBOX_MAJOR_READ = 0x02,
    XBOX_MAJOR_WRITE = 0x03,
    XBOX_MAJOR_DEVICE_CONTROL = 0x0a,
    XBOX_MAJOR_INTERNAL_DEVICE_CONTROL = 0x0b
};

/* Xbox packet layout: a four-byte header followed by the complete 20-byte
 * parameter payload. The regression test records the title-binary byte
 * evidence for the observed parameter offsets; a Cxbx record-layout probe
 * places Parameters at +8 and is not used as the guest-layout authority. */
typedef struct XBOX_GUEST_IO_STACK_LOCATION {
    uint8_t major_function;
    uint8_t minor_function;
    uint8_t flags;
    uint8_t control;
    uint32_t parameters[5];
} XBOX_GUEST_IO_STACK_LOCATION;

typedef struct XBOX_GUEST_DRIVER_OBJECT {
    uint32_t driver_start_io;
    uint32_t driver_delete_device;
    uint32_t driver_dismount_volume;
    uint32_t major_function[14];
} XBOX_GUEST_DRIVER_OBJECT;

typedef struct XBOX_GUEST_IO_STATUS_BLOCK {
    uint32_t status;
    uint32_t information;
} XBOX_GUEST_IO_STATUS_BLOCK;

/* Xbox IRP, with the x86 ABI's 8-byte LARGE_INTEGER alignment preserved at
 * Overlay. In particular, the reserved word before Overlay is not optional. */
typedef struct XBOX_GUEST_IRP {
    uint16_t type;                    /* 0x00 */
    uint16_t size;                    /* 0x02 */
    uint32_t flags;                   /* 0x04 */
    uint32_t thread_list_entry[2];    /* 0x08 */
    XBOX_GUEST_IO_STATUS_BLOCK io_status; /* 0x10 */
    int8_t stack_count;               /* 0x18 */
    int8_t current_location;          /* 0x19 */
    uint8_t pending_returned;         /* 0x1a */
    uint8_t cancel;                   /* 0x1b */
    uint32_t user_iosb;               /* 0x1c */
    uint32_t user_event;              /* 0x20 */
    uint32_t alignment_pad;           /* 0x24 */
    uint32_t overlay[2];              /* 0x28; union includes LARGE_INTEGER */
    uint32_t user_buffer;             /* 0x30 */
    uint32_t segment_array;           /* 0x34 */
    uint32_t locked_buffer_length;    /* 0x38 */
    union {
        struct {
            uint32_t device_queue_list[2]; /* 0x3c */
            uint32_t device_queue_sort_key;/* 0x44 */
            uint8_t device_queue_inserted; /* 0x48 */
            uint8_t device_queue_pad[3];
        } device_queue_entry;
        uint32_t driver_context[5]; /* Xbox has five entries */
    } tail_overlay;                  /* 0x3c */
    uint32_t thread;                  /* 0x50 */
    uint32_t list_entry[2];           /* 0x54 */
    uint32_t current_stack_location;  /* 0x5c */
    uint32_t original_file_object;    /* 0x60 */
    uint32_t xbox_alignment_pad;      /* 0x64; sizeof(IRP) rounds to 8 */
} XBOX_GUEST_IRP;

typedef struct XBOX_GUEST_DEVICE_QUEUE {
    uint16_t type;
    uint8_t size;
    uint8_t busy;
    uint32_t device_list_head[2];
} XBOX_GUEST_DEVICE_QUEUE;

typedef struct XBOX_GUEST_DISPATCHER_HEADER {
    uint8_t type;
    uint8_t absolute;
    uint8_t size;
    uint8_t inserted;
    int32_t signal_state;
    uint32_t wait_list_head[2];
} XBOX_GUEST_DISPATCHER_HEADER;

typedef struct XBOX_GUEST_DEVICE_OBJECT {
    uint16_t type;                    /* 0x00 */
    uint16_t size;                    /* 0x02 */
    int32_t reference_count;          /* 0x04 */
    uint32_t driver_object;           /* 0x08 */
    uint32_t mounted_or_self_device;  /* 0x0c */
    uint32_t current_irp;             /* 0x10 */
    uint32_t flags;                   /* 0x14 */
    uint32_t device_extension;        /* 0x18 */
    uint8_t device_type;              /* 0x1c */
    uint8_t start_io_flags;           /* 0x1d */
    int8_t stack_size;                /* 0x1e */
    uint8_t delete_pending;           /* 0x1f */
    uint32_t sector_size;             /* 0x20 */
    uint32_t alignment_requirement;   /* 0x24 */
    XBOX_GUEST_DEVICE_QUEUE device_queue; /* 0x28, 0x0c bytes */
    XBOX_GUEST_DISPATCHER_HEADER device_lock; /* 0x34, 0x10 bytes */
    uint32_t start_io_key;            /* 0x44 */
} XBOX_GUEST_DEVICE_OBJECT;

#if defined(__cplusplus)
#define XBOX_IO_STATIC_ASSERT(condition, message) static_assert(condition, message)
#else
#define XBOX_IO_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
#endif

XBOX_IO_STATIC_ASSERT(sizeof(XBOX_GUEST_IO_STATUS_BLOCK) == 8, "Xbox IOSB size");
XBOX_IO_STATIC_ASSERT(offsetof(XBOX_GUEST_IRP, io_status) == 0x10, "Xbox IRP IOSB offset");
XBOX_IO_STATIC_ASSERT(offsetof(XBOX_GUEST_IRP, overlay) == 0x28, "Xbox IRP overlay alignment");
XBOX_IO_STATIC_ASSERT(offsetof(XBOX_GUEST_IRP, user_buffer) == 0x30, "Xbox IRP buffer offset");
XBOX_IO_STATIC_ASSERT(offsetof(XBOX_GUEST_IRP, tail_overlay) == 0x3c, "Xbox IRP tail offset");
XBOX_IO_STATIC_ASSERT(offsetof(XBOX_GUEST_IRP, current_stack_location) ==
                      XBOX_IRP_CURRENT_STACK_LOCATION_OFFSET, "Xbox IRP current stack offset");
XBOX_IO_STATIC_ASSERT(sizeof(XBOX_GUEST_IRP) == XBOX_IRP_SIZE, "Xbox IRP size/alignment");
XBOX_IO_STATIC_ASSERT(sizeof(XBOX_GUEST_DEVICE_QUEUE) == 0x0c, "Xbox device queue size");
XBOX_IO_STATIC_ASSERT(sizeof(XBOX_GUEST_DISPATCHER_HEADER) == 0x10,
                      "Xbox dispatcher header size");
XBOX_IO_STATIC_ASSERT(offsetof(XBOX_GUEST_DISPATCHER_HEADER, signal_state) == 4,
                      "Xbox dispatcher SignalState offset");
XBOX_IO_STATIC_ASSERT(offsetof(XBOX_GUEST_DISPATCHER_HEADER, wait_list_head) == 8,
                      "Xbox dispatcher wait-list offset");
XBOX_IO_STATIC_ASSERT(offsetof(XBOX_GUEST_DEVICE_OBJECT, device_queue) == 0x28,
                      "Xbox device queue offset");
XBOX_IO_STATIC_ASSERT(offsetof(XBOX_GUEST_DEVICE_OBJECT, device_lock) == 0x34,
                      "Xbox device lock offset");
XBOX_IO_STATIC_ASSERT(offsetof(XBOX_GUEST_DEVICE_OBJECT, start_io_key) == 0x44,
                      "Xbox device key offset");
XBOX_IO_STATIC_ASSERT(sizeof(XBOX_GUEST_DEVICE_OBJECT) == XBOX_DEVICE_OBJECT_SIZE,
                      "Xbox device object size");
XBOX_IO_STATIC_ASSERT(sizeof(XBOX_GUEST_IO_STACK_LOCATION) == 0x18,
                      "Xbox stack location size");
XBOX_IO_STATIC_ASSERT(offsetof(XBOX_GUEST_IO_STACK_LOCATION, parameters) == 4,
                      "Xbox stack parameters offset");
XBOX_IO_STATIC_ASSERT(offsetof(XBOX_GUEST_IO_STACK_LOCATION, parameters) + 0 * sizeof(uint32_t) == 4,
                      "Xbox Read/Write Length offset");
XBOX_IO_STATIC_ASSERT(offsetof(XBOX_GUEST_IO_STACK_LOCATION, parameters) + 1 * sizeof(uint32_t) == 8,
                      "Xbox Read/Write Buffer offset");
XBOX_IO_STATIC_ASSERT(offsetof(XBOX_GUEST_IO_STACK_LOCATION, parameters) + 2 * sizeof(uint32_t) == 12,
                      "Xbox Read/Write ByteOffset offset");
XBOX_IO_STATIC_ASSERT(offsetof(XBOX_GUEST_IO_STACK_LOCATION, parameters) + 2 * sizeof(uint32_t) == 12,
                      "Xbox DeviceIoControl input buffer offset");
XBOX_IO_STATIC_ASSERT(offsetof(XBOX_GUEST_IO_STACK_LOCATION, parameters) + 3 * sizeof(uint32_t) == 16,
                      "Xbox DeviceIoControl IoControlCode offset");
XBOX_IO_STATIC_ASSERT(sizeof(XBOX_GUEST_DRIVER_OBJECT) == 0x44,
                      "Xbox driver object dispatch table size");

#undef XBOX_IO_STATIC_ASSERT
#endif
