/*
 * kernel_file.c - Xbox File I/O
 *
 * Implements the Nt*File kernel functions. All Xbox device paths are
 * translated through kernel_path.c before use.
 *
 * The Xbox kernel uses NT-style file I/O with ANSI strings in
 * OBJECT_ATTRIBUTES (unlike Windows NT, which uses Unicode).
 *
 * Two backends:
 *   _WIN32  -> Win32 CreateFileW / ReadFile / FindFirstFileW ...
 *   POSIX   -> open / read / write / stat / opendir ...
 * The Xbox semantics (disposition mapping, IO_STATUS_BLOCK, info classes)
 * are identical on both; only the host syscalls differ.
 */

#define _GNU_SOURCE   /* FNM_CASEFOLD */
#include "kernel.h"
#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>

#ifndef STATUS_BUFFER_TOO_SMALL
#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xC0000023L)
#endif
#ifndef STATUS_INFO_LENGTH_MISMATCH
#define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004L)
#endif

static INIT_ONCE s_dir_cs_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK initialize_dir_cs(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    (void)once;
    (void)context;
    InitializeCriticalSection((LPCRITICAL_SECTION)parameter);
    return TRUE;
}

static void ensure_dir_cs(CRITICAL_SECTION *cs)
{
    InitOnceExecuteOnce(&s_dir_cs_once, initialize_dir_cs, cs, NULL);
}

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <fnmatch.h>
#endif

/* Object handles and unsupported-code notes, defined in kernel_io.c. Declared
 * here, not in kernel.h, because only this file and kernel_bridge.c use them. */
extern NTSTATUS xbox_OpenSymbolicLink(const char* name, HANDLE* out);
extern const char* xbox_SymbolicLinkHandleTarget(HANDLE h);
extern int xbox_CloseObjectHandle(HANDLE h);
extern void xbox_NoteUnsupportedIoctl(const char* what, ULONG code);
static void xbox_dir_context_close(HANDLE FileHandle);

/* Get the ANSI path from OBJECT_ATTRIBUTES (platform-independent). */
static const char* get_xbox_path(PXBOX_OBJECT_ATTRIBUTES ObjectAttributes)
{
    if (!ObjectAttributes || !ObjectAttributes->ObjectName ||
        !ObjectAttributes->ObjectName->Buffer)
        return NULL;
    return ObjectAttributes->ObjectName->Buffer;
}

/* Xbox volume geometry.
 *
 * FATX uses 16 KB clusters: 512-byte sectors, 32 sectors per cluster. That is
 * not cosmetic. A title's CRT startup asks for FileFsSizeInformation and
 * multiplies SectorsPerAllocationUnit by BytesPerSector, then *requires* the
 * product to equal the cluster size it was built for. Half-Life 2 checks for
 * 0x4000 and returns STATUS_DEVICE_NOT_READY (0xC000014F) otherwise, which
 * aborts CRT init before main ever runs -- the process then exits cleanly,
 * which reads as a title that did nothing rather than one that failed.
 *
 * Reporting the host's PC-typical 4 KB cluster (512 x 8) fails that check. */
#define XBOX_BYTES_PER_SECTOR       512u
#define XBOX_SECTORS_PER_CLUSTER    32u      /* 512 * 32 = 16384 */

/* ======================================================================== */
/* Shared by both backends. kernel_bridge.c calls xbox_LastFileError()
 * unconditionally, so defining it only under _WIN32 leaves every POSIX link
 * with an undefined symbol -- invisible until something links an executable,
 * because a static archive never resolves its own references.
 *
 * The POSIX backend does not populate this yet, so it reads 0 there; that is a
 * missing detail rather than a missing symbol, and it is visible here. */
/* A bridge reads this after its host open returns. Streaming workers can open
 * simultaneously, so another thread must not replace that call's reason. */
static XBOX_THREAD_LOCAL uint32_t g_xbox_last_file_error;

static void (*s_file_open_observer)(const xbox_host_char *);
void xbox_SetFileOpenObserver(void (*observer)(const xbox_host_char *path))
{
    s_file_open_observer = observer;
}
static void file_open_notify(const xbox_host_char *path)
{
    if (s_file_open_observer) {
        DWORD error = GetLastError();
        uint32_t file_error = g_xbox_last_file_error;
        s_file_open_observer(path);
        g_xbox_last_file_error = file_error;
        SetLastError(error);
    }
}

uint32_t xbox_LastFileError(void)
{
    return g_xbox_last_file_error;
}

#if defined(_WIN32)
/* ====================  Win32 backend  =================================== */
/* ======================================================================== */

static INIT_ONCE s_file_hitch_once = INIT_ONCE_STATIC_INIT;
static volatile LONG s_file_hitch_ready;
static int s_file_hitch_enabled;
static LARGE_INTEGER s_file_hitch_frequency;

/* Opt-in evidence for failed shared-file opens. The ledger does not change
 * the effective access or sharing policy: it records opens, never retries or
 * closes an owner's handle. It is diagnostic only, not handle ownership. */
typedef struct XboxFileDiagOpen {
    struct XboxFileDiagOpen *next;
    HANDLE handle;
    DWORD thread;
    ULONG access, share, disposition, options;
    WCHAR path[MAX_PATH];
} XboxFileDiagOpen;

static INIT_ONCE s_file_diag_once = INIT_ONCE_STATIC_INIT;
static volatile LONG s_file_diag_ready;
static int s_file_diag_enabled;
static SRWLOCK s_file_diag_lock = SRWLOCK_INIT;
static XboxFileDiagOpen *s_file_diag_opens;

static BOOL CALLBACK file_diag_initialize(PINIT_ONCE once, PVOID parameter,
                                         PVOID *context)
{
    const char *setting = getenv("RECOMP_FILE_DIAGNOSTICS");
    (void)once; (void)parameter; (void)context;
    s_file_diag_enabled = setting && *setting && strcmp(setting, "0") != 0;
    InterlockedExchange(&s_file_diag_ready, 1);
    return TRUE;
}

static void file_diag_init(void)
{
    if (!s_file_diag_ready)
        InitOnceExecuteOnce(&s_file_diag_once, file_diag_initialize, NULL, NULL);
}

static int file_diag_path(const WCHAR *path, WCHAR normalized[MAX_PATH])
{
    WCHAR unc[MAX_PATH];
    DWORD length;
    if (!_wcsnicmp(path, L"\\\\?\\UNC\\", 8)) {
        if (swprintf_s(unc, MAX_PATH, L"\\\\%s", path + 8) < 0) return 0;
        path = unc;
    } else if (!_wcsnicmp(path, L"\\\\?\\", 4)) {
        path += 4;
    }
    length = GetFullPathNameW(path, MAX_PATH, normalized, NULL);
    return length && length < MAX_PATH;
}

static void file_diag_open(HANDLE handle, const WCHAR *path, ULONG access,
                           ULONG share, ULONG disposition, ULONG options)
{
    XboxFileDiagOpen *entry;
    WCHAR normalized[MAX_PATH];
    DWORD error = GetLastError();
    file_diag_init();
    if (!s_file_diag_enabled || !file_diag_path(path, normalized)) goto done;
    entry = malloc(sizeof *entry);
    if (!entry) {
        fprintf(stderr, "[FILE_DIAG] file open could not be recorded handle=%p\n", handle);
        goto done;
    }
    entry->handle = handle;
    entry->thread = GetCurrentThreadId();
    entry->access = access; entry->share = share;
    entry->disposition = disposition; entry->options = options;
    wcscpy_s(entry->path, MAX_PATH, normalized);
    AcquireSRWLockExclusive(&s_file_diag_lock);
    entry->next = s_file_diag_opens;
    s_file_diag_opens = entry;
    ReleaseSRWLockExclusive(&s_file_diag_lock);
done:
    SetLastError(error);
}

static BOOL file_diag_close_native(HANDLE handle)
{
    XboxFileDiagOpen **link, *entry = NULL;
    DWORD error;
    BOOL closed;
    if (!s_file_diag_enabled) return CloseHandle(handle);
    /* Serialize the close with ledger publication: once CloseHandle returns,
     * another thread may already get this same native handle value. Removing
     * by value after releasing the lock could erase that newer open's record. */
    AcquireSRWLockExclusive(&s_file_diag_lock);
    closed = CloseHandle(handle);
    error = GetLastError();
    if (closed) {
        for (link = &s_file_diag_opens; *link; link = &(*link)->next) {
            if ((*link)->handle != handle) continue;
            entry = *link;
            *link = entry->next;
            break;
        }
    }
    ReleaseSRWLockExclusive(&s_file_diag_lock);
    free(entry);
    SetLastError(error);
    return closed;
}

/* SystemExtendedHandleInformation is used only on the failed-open path.
 * Duplicate current-process handles long enough to query their disk paths,
 * then close the duplicates. No foreign-process handles are opened, and no
 * original handle is altered. A handle can disappear concurrently; such a
 * failed duplicate is simply omitted rather than treated as an owner. */
typedef struct XboxFileDiagHandleEntry {
    PVOID object;
    ULONG_PTR process_id, handle;
    ULONG access;
    USHORT backtrace, type;
    ULONG attributes, reserved;
} XboxFileDiagHandleEntry;
typedef struct XboxFileDiagHandles {
    ULONG_PTR count, reserved;
    XboxFileDiagHandleEntry entries[1];
} XboxFileDiagHandles;
typedef LONG (NTAPI *XboxFileDiagQuerySystem)(ULONG, PVOID, ULONG, PULONG);

static void file_diag_host_handles(const WCHAR *failed_path)
{
    XboxFileDiagQuerySystem query = (XboxFileDiagQuerySystem)GetProcAddress(
        GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
    XboxFileDiagHandles *snapshot = NULL;
    ULONG capacity = 1024u * 1024u, needed = 0;
    LONG status;
    unsigned owners = 0;
    if (!query) goto unavailable;
    for (;;) {
        snapshot = malloc(capacity);
        if (!snapshot) goto unavailable;
        status = query(64u, snapshot, capacity, &needed);
        if (status >= 0) break;
        free(snapshot); snapshot = NULL;
        if ((ULONG)status != 0xC0000004u || capacity >= 32u * 1024u * 1024u)
            goto unavailable;
        capacity *= 2;
        if (needed > capacity) capacity = needed;
        if (capacity > 32u * 1024u * 1024u) goto unavailable;
    }
    if (snapshot->count > (capacity - offsetof(XboxFileDiagHandles, entries)) /
                           sizeof snapshot->entries[0]) goto unavailable;
    for (ULONG_PTR i = 0; i < snapshot->count; ++i) {
        const XboxFileDiagHandleEntry *item = &snapshot->entries[i];
        HANDLE duplicate;
        WCHAR path[MAX_PATH], normalized[MAX_PATH];
        DWORD length;
        int tracked = 0;
        if (item->process_id != GetCurrentProcessId()) continue;
        if (!DuplicateHandle(GetCurrentProcess(), (HANDLE)item->handle,
                GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) continue;
        if (GetFileType(duplicate) != FILE_TYPE_DISK) {
            CloseHandle(duplicate);
            continue;
        }
        length = GetFinalPathNameByHandleW(duplicate, path, MAX_PATH,
                                         FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        CloseHandle(duplicate);
        if (!length || length >= MAX_PATH || !file_diag_path(path, normalized) ||
            _wcsicmp(normalized, failed_path)) continue;
        AcquireSRWLockShared(&s_file_diag_lock);
        for (XboxFileDiagOpen *entry = s_file_diag_opens; entry; entry = entry->next)
            if (entry->handle == (HANDLE)item->handle &&
                !_wcsicmp(entry->path, failed_path)) { tracked = 1; break; }
        ReleaseSRWLockShared(&s_file_diag_lock);
        fprintf(stderr, "[FILE_DIAG] host_same_file handle=%p granted=0x%08lX guest_tracked=%d\n",
                (HANDLE)item->handle, (unsigned long)item->access, tracked);
        ++owners;
    }
    fprintf(stderr, "[FILE_DIAG] current_process_same_file_handles=%u; external or transient ownership is not determined\n",
            owners);
    free(snapshot);
    return;
unavailable:
    free(snapshot);
    fprintf(stderr, "[FILE_DIAG] current-process handle inventory unavailable; lock ownership unresolved\n");
}

static void file_diag_failure(const WCHAR *path, DWORD error)
{
    WCHAR normalized[MAX_PATH];
    unsigned owners = 0;
    file_diag_init();
    if (!s_file_diag_enabled || error != ERROR_SHARING_VIOLATION ||
        !file_diag_path(path, normalized))
        goto done;
    fprintf(stderr, "[FILE_DIAG] sharing failure pid=%lu thread=%lu path=%ls\n",
            (unsigned long)GetCurrentProcessId(), (unsigned long)GetCurrentThreadId(), normalized);
    AcquireSRWLockShared(&s_file_diag_lock);
    for (XboxFileDiagOpen *entry = s_file_diag_opens; entry; entry = entry->next) {
        if (_wcsicmp(entry->path, normalized)) continue;
        fprintf(stderr, "[FILE_DIAG] guest_same_file handle=%p open_thread=%lu access=0x%08X share=0x%X disposition=%u options=0x%X\n",
                entry->handle, (unsigned long)entry->thread, (unsigned)entry->access,
                (unsigned)entry->share, (unsigned)entry->disposition, (unsigned)entry->options);
        ++owners;
    }
    ReleaseSRWLockShared(&s_file_diag_lock);
    fprintf(stderr, "[FILE_DIAG] active_guest_same_file_handles=%u\n", owners);
    file_diag_host_handles(normalized);
    fflush(stderr);
done:
    SetLastError(error);
}

static BOOL CALLBACK file_hitch_init(PINIT_ONCE once, PVOID parameter,
                                     PVOID *context)
{
    const char *setting = getenv("RECOMP_HITCH_TRACE");
    (void)once; (void)parameter; (void)context;
    s_file_hitch_enabled = setting && *setting && strcmp(setting, "0") != 0;
    if (s_file_hitch_enabled)
        QueryPerformanceFrequency(&s_file_hitch_frequency);
    InterlockedExchange(&s_file_hitch_ready, 1);
    return TRUE;
}

static int64_t file_hitch_begin(void)
{
    LARGE_INTEGER now;
    if (!s_file_hitch_ready)
        InitOnceExecuteOnce(&s_file_hitch_once, file_hitch_init, NULL, NULL);
    if (!s_file_hitch_enabled) return 0;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

static void file_hitch_end(int64_t started, HANDLE handle, ULONG length,
                           PLARGE_INTEGER offset, DWORD bytes, BOOL result,
                           DWORD error)
{
    LARGE_INTEGER ended;
    double ms, duration;
    if (!started) return;
    QueryPerformanceCounter(&ended);
    ms = 1000.0 / (double)s_file_hitch_frequency.QuadPart;
    duration = (ended.QuadPart - started) * ms;
    if (duration >= 8.0)
        fprintf(stderr,
            "[HITCH] phase=file_read qpc_ms=%.3f start_qpc_ms=%.3f "
            "duration_ms=%.3f thread=%lu handle=%p length=%u offset=%lld "
            "bytes=%lu error=%lu\n",
            ended.QuadPart * ms, started * ms, duration,
            (unsigned long)GetCurrentThreadId(), handle, (unsigned)length,
            offset ? (long long)offset->QuadPart : -1LL,
            (unsigned long)bytes, (unsigned long)(result ? ERROR_SUCCESS : error));
    /* Timing and diagnostics must not replace the read's last-error value. */
    SetLastError(error);
}

/* Convert Xbox create disposition to Win32 */
static DWORD xbox_disposition_to_win32(ULONG Disposition)
{
    switch (Disposition) {
        case XBOX_FILE_SUPERSEDE:    return CREATE_ALWAYS;
        case XBOX_FILE_OPEN:         return OPEN_EXISTING;
        case XBOX_FILE_CREATE:       return CREATE_NEW;
        case XBOX_FILE_OPEN_IF:      return OPEN_ALWAYS;
        case XBOX_FILE_OVERWRITE:    return TRUNCATE_EXISTING;
        case XBOX_FILE_OVERWRITE_IF: return CREATE_ALWAYS;
        default:                     return OPEN_EXISTING;
    }
}

/* Convert Xbox access mask to Win32 */
static DWORD xbox_access_to_win32(ACCESS_MASK Access)
{
    DWORD result = 0;
    if (Access & XBOX_GENERIC_READ)           result |= GENERIC_READ;
    if (Access & XBOX_GENERIC_WRITE)          result |= GENERIC_WRITE;
    if (Access & XBOX_GENERIC_ALL)            result |= GENERIC_ALL;
    if (Access & XBOX_FILE_READ_DATA)         result |= FILE_READ_DATA;
    if (Access & XBOX_FILE_WRITE_DATA)        result |= FILE_WRITE_DATA;
    if (Access & XBOX_FILE_APPEND_DATA)       result |= FILE_APPEND_DATA;
    if (Access & XBOX_FILE_READ_ATTRIBUTES)   result |= FILE_READ_ATTRIBUTES;
    if (Access & XBOX_FILE_WRITE_ATTRIBUTES)  result |= FILE_WRITE_ATTRIBUTES;
    if (Access & XBOX_SYNCHRONIZE)            result |= SYNCHRONIZE;
    if (Access & XBOX_DELETE)                  result |= DELETE;
    if (result == 0 || result == SYNCHRONIZE)
        result |= GENERIC_READ;
    return result;
}

/* Convert Xbox share access to Win32 */
static DWORD xbox_share_to_win32(ULONG Share)
{
    DWORD result = 0;
    if (Share & 0x01) result |= FILE_SHARE_READ;
    if (Share & 0x02) result |= FILE_SHARE_WRITE;
    if (Share & 0x04) result |= FILE_SHARE_DELETE;
    return result;
}

/* Translate an Xbox OBJECT_ATTRIBUTES path to a Win32 wide path */
static BOOL translate_obj_path(PXBOX_OBJECT_ATTRIBUTES ObjectAttributes,
                               WCHAR* win_path, DWORD buf_size)
{
    const char* xbox_path = get_xbox_path(ObjectAttributes);
    if (!xbox_path)
        return FALSE;
    if (ObjectAttributes->RootDirectory && xbox_path[0] != '\\' &&
        !(xbox_path[0] && xbox_path[1] == ':')) {
        /* XDeleteSaveGame opens each child relative to its save directory. */
        DWORD used = GetFinalPathNameByHandleW(ObjectAttributes->RootDirectory,
            win_path, buf_size, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!used || used >= buf_size || used + 1 >= buf_size) return FALSE;
        if (win_path[used - 1] != L'\\') win_path[used++] = L'\\';
        int count = MultiByteToWideChar(CP_ACP, 0, xbox_path, -1,
            win_path + used, (int)(buf_size - used));
        return count != 0;
    }
    return xbox_translate_path(xbox_path, win_path, buf_size);
}

NTSTATUS __stdcall xbox_NtCreateFile(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
    ULONG CreateDisposition, ULONG CreateOptions)
{
    WCHAR win_path[MAX_PATH];
    HANDLE h;
    DWORD flags_and_attrs = FILE_ATTRIBUTE_NORMAL;
    (void)AllocationSize;
    g_xbox_last_file_error = 0;

    if (!FileHandle || !ObjectAttributes)
        return STATUS_INVALID_PARAMETER;

    if (!translate_obj_path(ObjectAttributes, win_path, MAX_PATH)) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_FILE, "NtCreateFile: path translation failed");
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }

    /* A partition device opened as a directory.
     *
     * The path layer maps \Device\Harddisk0\PartitionN to a PartitionN.img
     * backing file, but a title asking for free space opens the partition with
     * FILE_DIRECTORY_FILE | FILE_OPEN_FOR_FREE_SPACE_QUERY -- it wants the
     * volume, not the bytes. Opening a regular file as a directory fails, and
     * the title reads STATUS_OBJECT_PATH_NOT_FOUND as "no such volume".
     *
     * Half-Life 2's CRT probes partition0 this way during startup and treats
     * the failure as fatal. Redirecting to the directory that holds the image
     * gives a handle that is valid for exactly what the caller is going to do
     * with it, which is NtQueryVolumeInformationFile. */
    if ((CreateOptions & XBOX_FILE_DIRECTORY_FILE) &&
        GetFileAttributesW(win_path) != INVALID_FILE_ATTRIBUTES &&
        !(GetFileAttributesW(win_path) & FILE_ATTRIBUTE_DIRECTORY)) {
        WCHAR *slash = wcsrchr(win_path, L'\\');
        if (slash && slash != win_path) {
            *slash = 0;
            xbox_log(XBOX_LOG_INFO, XBOX_LOG_FILE,
                     "NtCreateFile: directory open of a device image, "
                     "using its containing directory instead");
        }
    }

    /* The Xbox has one process, so a title's exclusive share mode only ever
     * meant "not within this title". On the host a second copy of the game
     * (system-link testing) reads the same disc files: read-only opens share
     * reads, or the first copy locks the second out of D:. */
    DWORD share = xbox_share_to_win32(ShareAccess);
    if (!(xbox_access_to_win32(DesiredAccess) & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA |
          FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES | DELETE)))
        share |= FILE_SHARE_READ;

    if (CreateOptions & XBOX_FILE_DIRECTORY_FILE) {
        if (CreateDisposition == XBOX_FILE_CREATE || CreateDisposition == XBOX_FILE_OPEN_IF)
            CreateDirectoryW(win_path, NULL);
        h = CreateFileW(win_path, xbox_access_to_win32(DesiredAccess),
            share, NULL, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, NULL);
    } else {
        if (CreateOptions & XBOX_FILE_NO_INTERMEDIATE_BUFFERING)
            flags_and_attrs |= FILE_FLAG_NO_BUFFERING;
        if (FileAttributes & XBOX_FILE_ATTRIBUTE_READONLY)
            flags_and_attrs |= FILE_ATTRIBUTE_READONLY;
        h = CreateFileW(win_path, xbox_access_to_win32(DesiredAccess),
            share, NULL,
            xbox_disposition_to_win32(CreateDisposition), flags_and_attrs, NULL);
    }

    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        NTSTATUS status;
        file_diag_failure(win_path, err);
        const char *result = (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
            ? "NOT_FOUND" : "FAILED";
        fprintf(stderr, "[NtCreateFile] %s path=%ls err=%lu access=0x%08X share=0x%X disposition=%u options=0x%X\n",
                result, win_path, (unsigned long)err, (unsigned)DesiredAccess,
                (unsigned)ShareAccess, (unsigned)CreateDisposition,
                (unsigned)CreateOptions);
        /* Kept for the caller's trace. An NTSTATUS says "it did not open";
         * only the Win32 error distinguishes a title probing for a file that
         * is genuinely absent from one it cannot open because this runtime
         * already holds it open with a share mode the second open forbids --
         * and those need opposite responses. */
        g_xbox_last_file_error = (uint32_t)err;
        /* A warning, not a compiled-out trace: a failed open is how a title
         * silently decides a volume or asset is missing, and in a Release
         * build that decision was invisible. */
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                 "NtCreateFile %s: %S (err=%u)", result, win_path, err);
        switch (err) {
            case ERROR_FILE_NOT_FOUND: status = STATUS_OBJECT_NAME_NOT_FOUND; break;
            case ERROR_PATH_NOT_FOUND: status = STATUS_OBJECT_PATH_NOT_FOUND; break;
            case ERROR_ACCESS_DENIED:  status = STATUS_ACCESS_DENIED; break;
            case ERROR_ALREADY_EXISTS: status = STATUS_OBJECT_NAME_COLLISION; break;
            /* A short-lived host file lock is not a missing asset or a
             * generic I/O failure. Preserve it through the guest's error
             * conversion without retrying or altering existing handles. */
            case ERROR_SHARING_VIOLATION: status = STATUS_SHARING_VIOLATION; break;
            default:                   status = STATUS_UNSUCCESSFUL; break;
        }
        if (IoStatusBlock) {
            IoStatusBlock->Status = status;
            IoStatusBlock->Information = 0;
        }
        return status;
    }

    file_diag_open(h, win_path, DesiredAccess, ShareAccess,
                   CreateDisposition, CreateOptions);
    *FileHandle = h;
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = (CreateDisposition == XBOX_FILE_CREATE) ? 2 : 1;
    }
    XBOX_TRACE(XBOX_LOG_FILE, "NtCreateFile: %S -> handle=%p", win_path, h);
    file_open_notify(win_path);
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtReadFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
    PLARGE_INTEGER ByteOffset)
{
    DWORD bytes_read = 0;
    BOOL result;
    OVERLAPPED ov;
    DWORD error;
    int64_t hitch_started;
    (void)ApcRoutine; (void)ApcContext;

    if (!IoStatusBlock)
        return STATUS_INVALID_PARAMETER;

    hitch_started = file_hitch_begin();

    if (ByteOffset && ByteOffset->QuadPart >= 0) {
        memset(&ov, 0, sizeof(ov));
        ov.Offset = ByteOffset->LowPart;
        ov.OffsetHigh = ByteOffset->HighPart;
        result = ReadFile(FileHandle, Buffer, Length, &bytes_read, &ov);
    } else {
        result = ReadFile(FileHandle, Buffer, Length, &bytes_read, NULL);
    }
    error = GetLastError();
    file_hitch_end(hitch_started, FileHandle, Length, ByteOffset, bytes_read,
                   result, error);

    if (result || error == ERROR_HANDLE_EOF) {
        IoStatusBlock->Information = bytes_read;
        if (bytes_read == 0 && Length > 0) {
            IoStatusBlock->Status = STATUS_END_OF_FILE;
            return STATUS_END_OF_FILE;
        }
        IoStatusBlock->Status = STATUS_SUCCESS;
        if (Event) SetEvent(Event);
        return STATUS_SUCCESS;
    }

    {
        fprintf(stderr, "[NtReadFile] FAILED handle=%p len=%u err=%lu\n",
                FileHandle, Length, (unsigned long)error);
        XBOX_TRACE(XBOX_LOG_FILE, "NtReadFile(handle=%p, len=%u) failed err=%u",
                   FileHandle, Length, error);
    }
    IoStatusBlock->Status = STATUS_UNSUCCESSFUL;
    IoStatusBlock->Information = 0;
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS __stdcall xbox_NtWriteFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
    PLARGE_INTEGER ByteOffset)
{
    DWORD bytes_written = 0;
    BOOL result;
    OVERLAPPED ov;
    (void)ApcRoutine; (void)ApcContext;

    if (!IoStatusBlock)
        return STATUS_INVALID_PARAMETER;

    if (ByteOffset && ByteOffset->QuadPart >= 0) {
        memset(&ov, 0, sizeof(ov));
        ov.Offset = ByteOffset->LowPart;
        ov.OffsetHigh = ByteOffset->HighPart;
        result = WriteFile(FileHandle, Buffer, Length, &bytes_written, &ov);
    } else {
        result = WriteFile(FileHandle, Buffer, Length, &bytes_written, NULL);
    }

    if (result) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = bytes_written;
        if (Event) SetEvent(Event);
        return STATUS_SUCCESS;
    }

    XBOX_TRACE(XBOX_LOG_FILE, "NtWriteFile(handle=%p, len=%u) failed err=%u",
               FileHandle, Length, GetLastError());
    IoStatusBlock->Status = STATUS_UNSUCCESSFUL;
    IoStatusBlock->Information = 0;
    return STATUS_UNSUCCESSFUL;
}


NTSTATUS __stdcall xbox_NtClose(HANDLE Handle)
{
    XBOX_TRACE(XBOX_LOG_FILE, "NtClose(handle=%p)", Handle);
    if (xbox_CloseObjectHandle(Handle))
        return STATUS_SUCCESS;  /* object handle: a link or directory, nothing to close */
    if (Handle && Handle != INVALID_HANDLE_VALUE) {
        xbox_dir_context_close(Handle);
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_HANDLE;
}

NTSTATUS __stdcall xbox_NtDeleteFile(PXBOX_OBJECT_ATTRIBUTES ObjectAttributes)
{
    WCHAR win_path[MAX_PATH];
    if (!translate_obj_path(ObjectAttributes, win_path, MAX_PATH))
        return STATUS_OBJECT_PATH_NOT_FOUND;
    XBOX_TRACE(XBOX_LOG_FILE, "NtDeleteFile: %S", win_path);
    if (DeleteFileW(win_path))    return STATUS_SUCCESS;
    if (RemoveDirectoryW(win_path)) return STATUS_SUCCESS;
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

NTSTATUS __stdcall xbox_NtQueryInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation, ULONG Length, XBOX_FILE_INFORMATION_CLASS FileInformationClass)
{
    (void)Length;
    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;

    switch (FileInformationClass) {
        case XboxFileBasicInformation: {
            PXBOX_FILE_BASIC_INFORMATION info = (PXBOX_FILE_BASIC_INFORMATION)FileInformation;
            BY_HANDLE_FILE_INFORMATION fi;
            if (!GetFileInformationByHandle(FileHandle, &fi))
                return STATUS_UNSUCCESSFUL;
            info->CreationTime.LowPart    = fi.ftCreationTime.dwLowDateTime;
            info->CreationTime.HighPart   = fi.ftCreationTime.dwHighDateTime;
            info->LastAccessTime.LowPart  = fi.ftLastAccessTime.dwLowDateTime;
            info->LastAccessTime.HighPart = fi.ftLastAccessTime.dwHighDateTime;
            info->LastWriteTime.LowPart   = fi.ftLastWriteTime.dwLowDateTime;
            info->LastWriteTime.HighPart  = fi.ftLastWriteTime.dwHighDateTime;
            info->ChangeTime = info->LastWriteTime;
            info->FileAttributes = fi.dwFileAttributes;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_BASIC_INFORMATION);
            return STATUS_SUCCESS;
        }
        case XboxFileStandardInformation: {
            PXBOX_FILE_STANDARD_INFORMATION info = (PXBOX_FILE_STANDARD_INFORMATION)FileInformation;
            BY_HANDLE_FILE_INFORMATION fi;
            if (!GetFileInformationByHandle(FileHandle, &fi))
                return STATUS_UNSUCCESSFUL;
            info->AllocationSize.QuadPart = ((LONGLONG)fi.nFileSizeHigh << 32) | fi.nFileSizeLow;
            info->AllocationSize.QuadPart = (info->AllocationSize.QuadPart + 4095) & ~4095LL;
            info->EndOfFile.QuadPart = ((LONGLONG)fi.nFileSizeHigh << 32) | fi.nFileSizeLow;
            info->NumberOfLinks = fi.nNumberOfLinks;
            info->DeletePending = FALSE;
            info->Directory = (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? TRUE : FALSE;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_STANDARD_INFORMATION);
            return STATUS_SUCCESS;
        }
        case XboxFilePositionInformation: {
            PXBOX_FILE_POSITION_INFORMATION info = (PXBOX_FILE_POSITION_INFORMATION)FileInformation;
            LARGE_INTEGER pos, zero;
            zero.QuadPart = 0;
            if (!SetFilePointerEx(FileHandle, zero, &pos, FILE_CURRENT))
                return STATUS_UNSUCCESSFUL;
            info->CurrentByteOffset = pos;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_POSITION_INFORMATION);
            return STATUS_SUCCESS;
        }
        case XboxFileNetworkOpenInformation: {
            PXBOX_FILE_NETWORK_OPEN_INFORMATION info = (PXBOX_FILE_NETWORK_OPEN_INFORMATION)FileInformation;
            BY_HANDLE_FILE_INFORMATION fi;
            if (!GetFileInformationByHandle(FileHandle, &fi))
                return STATUS_UNSUCCESSFUL;
            info->CreationTime.LowPart    = fi.ftCreationTime.dwLowDateTime;
            info->CreationTime.HighPart   = fi.ftCreationTime.dwHighDateTime;
            info->LastAccessTime.LowPart  = fi.ftLastAccessTime.dwLowDateTime;
            info->LastAccessTime.HighPart = fi.ftLastAccessTime.dwHighDateTime;
            info->LastWriteTime.LowPart   = fi.ftLastWriteTime.dwLowDateTime;
            info->LastWriteTime.HighPart  = fi.ftLastWriteTime.dwHighDateTime;
            info->ChangeTime = info->LastWriteTime;
            info->EndOfFile.QuadPart = ((LONGLONG)fi.nFileSizeHigh << 32) | fi.nFileSizeLow;
            info->AllocationSize.QuadPart = (info->EndOfFile.QuadPart + 4095) & ~4095LL;
            info->FileAttributes = fi.dwFileAttributes;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_NETWORK_OPEN_INFORMATION);
            return STATUS_SUCCESS;
        }
        default:
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                "NtQueryInformationFile: unhandled class %d", FileInformationClass);
            return STATUS_NOT_IMPLEMENTED;
    }
}

NTSTATUS __stdcall xbox_NtSetInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation, ULONG Length, XBOX_FILE_INFORMATION_CLASS FileInformationClass)
{
    (void)Length;
    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;

    switch (FileInformationClass) {
        case XboxFilePositionInformation: {
            PXBOX_FILE_POSITION_INFORMATION info = (PXBOX_FILE_POSITION_INFORMATION)FileInformation;
            if (!SetFilePointerEx(FileHandle, info->CurrentByteOffset, NULL, FILE_BEGIN)) {
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                    "SetFilePosition failed: offset=%lld err=%u",
                    (long long)info->CurrentByteOffset.QuadPart, GetLastError());
                return STATUS_UNSUCCESSFUL;
            }
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileEndOfFileInformation: {
            PXBOX_FILE_END_OF_FILE_INFORMATION info = (PXBOX_FILE_END_OF_FILE_INFORMATION)FileInformation;
            LARGE_INTEGER cur, zero = {0};
            SetFilePointerEx(FileHandle, zero, &cur, FILE_CURRENT);
            SetFilePointerEx(FileHandle, info->EndOfFile, NULL, FILE_BEGIN);
            if (!SetEndOfFile(FileHandle)) {
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                    "SetEndOfFile failed: size=%lld err=%u",
                    (long long)info->EndOfFile.QuadPart, GetLastError());
                SetFilePointerEx(FileHandle, cur, NULL, FILE_BEGIN);
                return STATUS_UNSUCCESSFUL;
            }
            if (cur.QuadPart <= info->EndOfFile.QuadPart)
                SetFilePointerEx(FileHandle, cur, NULL, FILE_BEGIN);
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileAllocationInformation: {
            /* Reserve space for a file. Halo's save path calls this before
             * writing, and an unimplemented class here returned
             * STATUS_NOT_IMPLEMENTED, which the title turned into DOS error
             * 317 and reported as "couldn't open or create saved game file".
             *
             * Same payload shape as EndOfFile: one LARGE_INTEGER. Windows
             * FileAllocationInfo is the direct equivalent; if the filesystem
             * declines it, fall back to setting the size, since the caller
             * only needs the space to exist. */
            PXBOX_FILE_END_OF_FILE_INFORMATION info =
                (PXBOX_FILE_END_OF_FILE_INFORMATION)FileInformation;
            FILE_ALLOCATION_INFO fai;
            fai.AllocationSize = info->EndOfFile;
            if (!SetFileInformationByHandle(FileHandle, FileAllocationInfo,
                                            &fai, sizeof(fai))) {
                LARGE_INTEGER cur, zero = {0};
                SetFilePointerEx(FileHandle, zero, &cur, FILE_CURRENT);
                SetFilePointerEx(FileHandle, info->EndOfFile, NULL, FILE_BEGIN);
                SetEndOfFile(FileHandle);
                SetFilePointerEx(FileHandle, cur, NULL, FILE_BEGIN);
            }
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileDispositionInformation: {
            PXBOX_FILE_DISPOSITION_INFORMATION info = (PXBOX_FILE_DISPOSITION_INFORMATION)FileInformation;
            FILE_DISPOSITION_INFO fdi;
            fdi.DeleteFile = info->DeleteFile;
            if (!SetFileInformationByHandle(FileHandle, FileDispositionInfo, &fdi, sizeof(fdi)))
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                         "SetFileDispositionInfo failed: err=%u", GetLastError());
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileBasicInformation: {
            PXBOX_FILE_BASIC_INFORMATION info = (PXBOX_FILE_BASIC_INFORMATION)FileInformation;
            FILETIME ct, at, wt;
            ct.dwLowDateTime = info->CreationTime.LowPart;
            ct.dwHighDateTime = info->CreationTime.HighPart;
            at.dwLowDateTime = info->LastAccessTime.LowPart;
            at.dwHighDateTime = info->LastAccessTime.HighPart;
            wt.dwLowDateTime = info->LastWriteTime.LowPart;
            wt.dwHighDateTime = info->LastWriteTime.HighPart;
            SetFileTime(FileHandle, &ct, &at, &wt);
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        default:
            /* stderr, not xbox_log: WARN is filtered out by default, and an
             * unimplemented info class is exactly the kind of silent gap that
             * surfaces far away. Halo's save path hits one, gets
             * STATUS_NOT_IMPLEMENTED, converts it to DOS error 317 and asserts
             * "couldn't open or create saved game file". */
            fprintf(stderr, "  [FILE] NtSetInformationFile: unhandled class %d\n",
                    (int)FileInformationClass);
            fflush(stderr);
            return STATUS_NOT_IMPLEMENTED;
    }
}

NTSTATUS __stdcall xbox_NtQueryVolumeInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FsInformation, ULONG Length, XBOX_FS_INFORMATION_CLASS FsInformationClass)
{
    (void)FileHandle; (void)Length;
    if (!IoStatusBlock || !FsInformation)
        return STATUS_INVALID_PARAMETER;

    switch (FsInformationClass) {
        case XboxFileFsSizeInformation: {
            PXBOX_FILE_FS_SIZE_INFORMATION info = (PXBOX_FILE_FS_SIZE_INFORMATION)FsInformation;
            ULARGE_INTEGER free_bytes, total_bytes, total_free;
            if (GetDiskFreeSpaceExW(NULL, &free_bytes, &total_bytes, &total_free)) {
                info->BytesPerSector = XBOX_BYTES_PER_SECTOR;
                info->SectorsPerAllocationUnit = XBOX_SECTORS_PER_CLUSTER;
                ULONGLONG cs = (ULONGLONG)info->BytesPerSector * info->SectorsPerAllocationUnit;
                info->TotalAllocationUnits.QuadPart = total_bytes.QuadPart / cs;
                info->AvailableAllocationUnits.QuadPart = free_bytes.QuadPart / cs;
            } else {
                info->BytesPerSector = XBOX_BYTES_PER_SECTOR;
                info->SectorsPerAllocationUnit = XBOX_SECTORS_PER_CLUSTER;
                info->TotalAllocationUnits.QuadPart = 1048576;
                info->AvailableAllocationUnits.QuadPart = 524288;
            }
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_FS_SIZE_INFORMATION);
            return STATUS_SUCCESS;
        }
        default:
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                "NtQueryVolumeInformationFile: unhandled class %d", FsInformationClass);
            return STATUS_NOT_IMPLEMENTED;
    }
}

NTSTATUS __stdcall xbox_NtFlushBuffersFile(HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock)
{
    FlushFileBuffers(FileHandle);
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = 0;
    }
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtQueryFullAttributesFile(
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes,
    PXBOX_FILE_NETWORK_OPEN_INFORMATION FileInformation)
{
    WCHAR win_path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fad;

    if (!FileInformation)
        return STATUS_INVALID_PARAMETER;
    if (!translate_obj_path(ObjectAttributes, win_path, MAX_PATH))
        return STATUS_OBJECT_PATH_NOT_FOUND;

    if (!GetFileAttributesExW(win_path, GetFileExInfoStandard, &fad)) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
            return STATUS_OBJECT_NAME_NOT_FOUND;
        return STATUS_UNSUCCESSFUL;
    }

    FileInformation->CreationTime.LowPart    = fad.ftCreationTime.dwLowDateTime;
    FileInformation->CreationTime.HighPart   = fad.ftCreationTime.dwHighDateTime;
    FileInformation->LastAccessTime.LowPart  = fad.ftLastAccessTime.dwLowDateTime;
    FileInformation->LastAccessTime.HighPart = fad.ftLastAccessTime.dwHighDateTime;
    FileInformation->LastWriteTime.LowPart   = fad.ftLastWriteTime.dwLowDateTime;
    FileInformation->LastWriteTime.HighPart  = fad.ftLastWriteTime.dwHighDateTime;
    FileInformation->ChangeTime = FileInformation->LastWriteTime;
    FileInformation->EndOfFile.QuadPart = ((LONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    FileInformation->AllocationSize.QuadPart = (FileInformation->EndOfFile.QuadPart + 4095) & ~4095LL;
    FileInformation->FileAttributes = fad.dwFileAttributes;
    return STATUS_SUCCESS;
}

#define MAX_DIR_CONTEXTS 64
typedef struct {
    HANDLE file_handle;
    HANDLE find_handle;
    BOOL   first_done;
    BOOL   initial_query;
    BOOL   pending;
    BOOL   exhausted;
    WCHAR  pattern[MAX_PATH];
    WIN32_FIND_DATAW find_data;
} DIR_CONTEXT;

static DIR_CONTEXT s_dir_contexts[MAX_DIR_CONTEXTS];
static CRITICAL_SECTION s_dir_cs;

static DIR_CONTEXT* find_or_create_dir_context(HANDLE FileHandle, BOOL create)
{
    ensure_dir_cs(&s_dir_cs);
    EnterCriticalSection(&s_dir_cs);
    for (int i = 0; i < MAX_DIR_CONTEXTS; i++) {
        if (s_dir_contexts[i].file_handle == FileHandle) {
            return &s_dir_contexts[i];
        }
    }
    if (!create) { LeaveCriticalSection(&s_dir_cs); return NULL; }
    for (int i = 0; i < MAX_DIR_CONTEXTS; i++) {
        if (s_dir_contexts[i].file_handle == NULL) {
            s_dir_contexts[i].file_handle = FileHandle;
            s_dir_contexts[i].first_done = FALSE;
            s_dir_contexts[i].initial_query = TRUE;
            s_dir_contexts[i].pending = FALSE;
            s_dir_contexts[i].exhausted = FALSE;
            return &s_dir_contexts[i];
        }
    }
    LeaveCriticalSection(&s_dir_cs);
    return NULL;
}

/* A closed directory handle takes its enumeration with it. Host handle values
 * are reused, and a search abandoned part-way (a title that stops at the first
 * match) otherwise carried on under the next directory opened at the same
 * value: its first answer was the old search's next entry. */
static void xbox_dir_context_close(HANDLE FileHandle)
{
    ensure_dir_cs(&s_dir_cs);
    EnterCriticalSection(&s_dir_cs);
    for (int i = 0; i < MAX_DIR_CONTEXTS; i++) {
        if (s_dir_contexts[i].file_handle == FileHandle) {
            if (s_dir_contexts[i].find_handle &&
                s_dir_contexts[i].find_handle != INVALID_HANDLE_VALUE)
                FindClose(s_dir_contexts[i].find_handle);
            s_dir_contexts[i].find_handle = NULL;
            s_dir_contexts[i].file_handle = NULL;
            s_dir_contexts[i].first_done = FALSE;
            s_dir_contexts[i].initial_query = FALSE;
            s_dir_contexts[i].pending = FALSE;
            s_dir_contexts[i].exhausted = FALSE;
        }
    }
    file_diag_close_native(FileHandle);
    LeaveCriticalSection(&s_dir_cs);
}

NTSTATUS __stdcall xbox_NtQueryDirectoryFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length,
    XBOX_FILE_INFORMATION_CLASS FileInformationClass,
    PXBOX_ANSI_STRING FileName, BOOLEAN RestartScan)
{
    DIR_CONTEXT* ctx;
    PXBOX_FILE_DIRECTORY_INFORMATION entry;
    (void)Event; (void)ApcRoutine; (void)ApcContext;

    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;
    IoStatusBlock->Information = 0;
    if (FileInformationClass != XboxFileDirectoryInformation) {
        IoStatusBlock->Status = STATUS_INVALID_INFO_CLASS;
        return STATUS_INVALID_INFO_CLASS;
    }

    ULONG header_size = (ULONG)offsetof(XBOX_FILE_DIRECTORY_INFORMATION, FileName);
    if (Length < sizeof(XBOX_FILE_DIRECTORY_INFORMATION)) {
        IoStatusBlock->Status = STATUS_INFO_LENGTH_MISMATCH;
        return STATUS_INFO_LENGTH_MISMATCH;
    }

    ctx = find_or_create_dir_context(FileHandle, TRUE);
    if (!ctx) {
        IoStatusBlock->Status = STATUS_INSUFFICIENT_RESOURCES;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (ctx->exhausted && !RestartScan) {
        IoStatusBlock->Status = STATUS_NO_MORE_FILES;
        LeaveCriticalSection(&s_dir_cs);
        return STATUS_NO_MORE_FILES;
    }

    if (RestartScan || !ctx->first_done) {
        if (!ctx->first_done) {
            if (FileName && FileName->Buffer && FileName->Length > 0) {
                if (FileName->Length >= MAX_PATH) {
                    IoStatusBlock->Status = STATUS_INVALID_PARAMETER;
                    LeaveCriticalSection(&s_dir_cs);
                    return STATUS_INVALID_PARAMETER;
                }
                int pattern_chars = MultiByteToWideChar(CP_ACP, 0, FileName->Buffer,
                    FileName->Length, ctx->pattern, MAX_PATH - 1);
                if (pattern_chars <= 0 || pattern_chars >= MAX_PATH) {
                    IoStatusBlock->Status = STATUS_INVALID_PARAMETER;
                    LeaveCriticalSection(&s_dir_cs);
                    return STATUS_INVALID_PARAMETER;
                }
                ctx->pattern[pattern_chars] = L'\0';
            } else {
                ctx->pattern[0] = L'\0';
            }
        }
        if (ctx->find_handle && ctx->find_handle != INVALID_HANDLE_VALUE) {
            FindClose(ctx->find_handle);
            ctx->find_handle = NULL;
        }
        WCHAR search_path[MAX_PATH];
        WCHAR dir_path[MAX_PATH];
        DWORD path_len = GetFinalPathNameByHandleW(FileHandle, dir_path, MAX_PATH,
                                                   FILE_NAME_NORMALIZED);
        if (path_len == 0 || path_len >= MAX_PATH) {
            IoStatusBlock->Status = STATUS_UNSUCCESSFUL;
            LeaveCriticalSection(&s_dir_cs);
            return STATUS_UNSUCCESSFUL;
        }
        WCHAR* clean_path = dir_path;
        if (wcsncmp(clean_path, L"\\\\?\\", 4) == 0)
            clean_path += 4;
        if (ctx->pattern[0]) {
            swprintf_s(search_path, MAX_PATH, L"%s\\%s", clean_path, ctx->pattern);
        } else {
            swprintf_s(search_path, MAX_PATH, L"%s\\*", clean_path);
        }
        ctx->find_handle = FindFirstFileW(search_path, &ctx->find_data);
        ctx->first_done = TRUE;
        if (ctx->find_handle == INVALID_HANDLE_VALUE) {
            ctx->find_handle = NULL;
            ctx->first_done = TRUE;
            ctx->exhausted = TRUE;
            IoStatusBlock->Status = STATUS_NO_MORE_FILES;
            LeaveCriticalSection(&s_dir_cs);
            return STATUS_NO_MORE_FILES;
        }
        ctx->first_done = TRUE;
        ctx->initial_query = TRUE;
        ctx->pending = TRUE;
        ctx->exhausted = FALSE;
    } else if (!ctx->pending) {
        if (!FindNextFileW(ctx->find_handle, &ctx->find_data)) {
            FindClose(ctx->find_handle);
            ctx->find_handle = NULL;
            ctx->first_done = TRUE;
            ctx->exhausted = TRUE;
            IoStatusBlock->Status = STATUS_NO_MORE_FILES;
            LeaveCriticalSection(&s_dir_cs);
            return STATUS_NO_MORE_FILES;
        }
        ctx->pending = TRUE;
    }

    /* FATX enumeration never exposes the host's dot directories. */
    while (!wcscmp(ctx->find_data.cFileName, L".") ||
           !wcscmp(ctx->find_data.cFileName, L"..")) {
        if (!FindNextFileW(ctx->find_handle, &ctx->find_data)) {
            FindClose(ctx->find_handle);
            ctx->find_handle = NULL;
            ctx->first_done = TRUE;
            ctx->exhausted = TRUE;
            ctx->pending = FALSE;
            IoStatusBlock->Status = STATUS_NO_MORE_FILES;
            LeaveCriticalSection(&s_dir_cs);
            return STATUS_NO_MORE_FILES;
        }
        ctx->pending = TRUE;
    }

    char filename_ansi[MAX_PATH];
    int name_len = WideCharToMultiByte(CP_ACP, 0, ctx->find_data.cFileName, -1,
                                       filename_ansi, MAX_PATH, NULL, NULL);
    if (name_len <= 0) {
        IoStatusBlock->Status = STATUS_UNSUCCESSFUL;
        LeaveCriticalSection(&s_dir_cs);
        return STATUS_UNSUCCESSFUL;
    }
    name_len--;

    ULONG available_name = Length - header_size;
    ULONG copied_name = (ULONG)name_len < available_name ? (ULONG)name_len : available_name;
    if (copied_name < (ULONG)name_len && !ctx->initial_query) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = 0;
        LeaveCriticalSection(&s_dir_cs);
        return STATUS_SUCCESS;
    }
    entry = (PXBOX_FILE_DIRECTORY_INFORMATION)FileInformation;
    memset(entry, 0, header_size);
    entry->NextEntryOffset = 0;
    entry->FileIndex = 0;
    entry->CreationTime.LowPart   = ctx->find_data.ftCreationTime.dwLowDateTime;
    entry->CreationTime.HighPart  = ctx->find_data.ftCreationTime.dwHighDateTime;
    entry->LastAccessTime.LowPart = ctx->find_data.ftLastAccessTime.dwLowDateTime;
    entry->LastAccessTime.HighPart = ctx->find_data.ftLastAccessTime.dwHighDateTime;
    entry->LastWriteTime.LowPart  = ctx->find_data.ftLastWriteTime.dwLowDateTime;
    entry->LastWriteTime.HighPart = ctx->find_data.ftLastWriteTime.dwHighDateTime;
    entry->ChangeTime = entry->LastWriteTime;
    entry->EndOfFile.QuadPart = ((LONGLONG)ctx->find_data.nFileSizeHigh << 32) | ctx->find_data.nFileSizeLow;
    entry->AllocationSize.QuadPart = (entry->EndOfFile.QuadPart + 4095) & ~4095LL;
    entry->FileAttributes = ctx->find_data.dwFileAttributes;
    entry->FileNameLength = name_len;
    if (copied_name)
        memcpy(entry->FileName, filename_ansi, copied_name);
    IoStatusBlock->Information = header_size + copied_name;
    if (copied_name < (ULONG)name_len) {
        ctx->initial_query = FALSE;
        IoStatusBlock->Status = STATUS_BUFFER_OVERFLOW;
        LeaveCriticalSection(&s_dir_cs);
        return STATUS_BUFFER_OVERFLOW;
    }
    ctx->pending = FALSE;
    ctx->initial_query = FALSE;
    IoStatusBlock->Status = STATUS_SUCCESS;
    LeaveCriticalSection(&s_dir_cs);
    return STATUS_SUCCESS;
}

/* ======================================================================== */
#else /* !_WIN32 */
/* ====================  POSIX backend  =================================== */
/* ======================================================================== */

/* Convert Xbox access mask + disposition to POSIX open() flags. */
static int posix_open_flags(ACCESS_MASK access, ULONG disposition)
{
    int wantWrite = (access & (XBOX_GENERIC_WRITE | XBOX_GENERIC_ALL |
                               XBOX_FILE_WRITE_DATA | XBOX_FILE_APPEND_DATA)) != 0;
    int rw = wantWrite ? O_RDWR : O_RDONLY;
    int extra;

    switch (disposition) {
        case XBOX_FILE_SUPERSEDE:    extra = O_CREAT | O_TRUNC; break;
        case XBOX_FILE_OPEN:         extra = 0;                 break;
        case XBOX_FILE_CREATE:       extra = O_CREAT | O_EXCL;  break;
        case XBOX_FILE_OPEN_IF:      extra = O_CREAT;           break;
        case XBOX_FILE_OVERWRITE:    extra = O_TRUNC;           break;
        case XBOX_FILE_OVERWRITE_IF: extra = O_CREAT | O_TRUNC; break;
        default:                     extra = 0;                 break;
    }
    /* O_TRUNC / O_CREAT imply write intent */
    if ((extra & (O_TRUNC | O_CREAT)) && rw == O_RDONLY)
        rw = O_RDWR;
    if (access & XBOX_FILE_APPEND_DATA)
        extra |= O_APPEND;
    return rw | extra;
}

static void unix_to_filetime(time_t sec, long nsec, LARGE_INTEGER* out)
{
    /* 100-ns ticks since 1601-01-01 */
    ULONGLONG t = 116444736000000000ULL
                + (ULONGLONG)sec * 10000000ULL
                + (ULONGLONG)nsec / 100ULL;
    out->LowPart  = (DWORD)(t & 0xFFFFFFFFULL);
    out->HighPart = (LONG)(t >> 32);
}

static ULONG mode_to_xbox_attrs(mode_t m)
{
    ULONG a = 0;
    if (S_ISDIR(m))      a |= XBOX_FILE_ATTRIBUTE_DIRECTORY;
    if (!(m & S_IWUSR))  a |= XBOX_FILE_ATTRIBUTE_READONLY;
    if (a == 0)          a = XBOX_FILE_ATTRIBUTE_NORMAL;
    return a;
}

static NTSTATUS errno_to_status(int e)
{
    switch (e) {
        case ENOENT:  return STATUS_OBJECT_NAME_NOT_FOUND;
        case ENOTDIR: return STATUS_OBJECT_PATH_NOT_FOUND;
        case EACCES:
        case EPERM:   return STATUS_ACCESS_DENIED;
        case EEXIST:  return STATUS_OBJECT_NAME_COLLISION;
        case ENOMEM:  return STATUS_NO_MEMORY;
        default:      return STATUS_UNSUCCESSFUL;
    }
}

/* Win32 error for a failed host open, as xbox_LastFileError reports it. */
static DWORD errno_to_win32(int e)
{
    switch (e) {
        case ENOENT:  return ERROR_FILE_NOT_FOUND;
        case ENOTDIR: return ERROR_PATH_NOT_FOUND;
        case EACCES:
        case EPERM:   return ERROR_ACCESS_DENIED;
        case EEXIST:  return ERROR_ALREADY_EXISTS;
        default:      return ERROR_GEN_FAILURE;
    }
}

/* Xbox access mask to the Win32 bits the share rules read. Mirrors
 * xbox_access_to_win32 in the Win32 backend. */
static DWORD posix_access_to_win32(ACCESS_MASK Access)
{
    DWORD result = 0;
    if (Access & XBOX_GENERIC_READ)          result |= GENERIC_READ;
    if (Access & XBOX_GENERIC_WRITE)         result |= GENERIC_WRITE;
    if (Access & XBOX_GENERIC_ALL)           result |= GENERIC_ALL;
    if (Access & XBOX_FILE_READ_DATA)        result |= FILE_READ_DATA;
    if (Access & XBOX_FILE_WRITE_DATA)       result |= FILE_WRITE_DATA;
    if (Access & XBOX_FILE_APPEND_DATA)      result |= FILE_APPEND_DATA;
    if (Access & XBOX_FILE_READ_ATTRIBUTES)  result |= FILE_READ_ATTRIBUTES;
    if (Access & XBOX_FILE_WRITE_ATTRIBUTES) result |= FILE_WRITE_ATTRIBUTES;
    if (Access & XBOX_SYNCHRONIZE)           result |= SYNCHRONIZE;
    if (Access & XBOX_DELETE)                result |= DELETE;
    if (result == 0 || result == SYNCHRONIZE)
        result |= GENERIC_READ;
    return result;
}

/* Xbox share bits (FILE_SHARE_READ/WRITE/DELETE) are the Win32 values. */
static DWORD posix_share_to_win32(ULONG Share)
{
    return (DWORD)(Share & (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE));
}

/* Host path for OBJECT_ATTRIBUTES. A relative name resolves under its
 * RootDirectory handle's host path, as the Win32 backend does with
 * GetFinalPathNameByHandle; everything else goes through the path table. */
static BOOL posix_translate_obj_path(PXBOX_OBJECT_ATTRIBUTES ObjectAttributes,
                                     char* host_path, DWORD buf_size)
{
    const char* xbox_path = get_xbox_path(ObjectAttributes);
    const char* root;
    size_t used;

    if (!xbox_path)
        return FALSE;
    if (!ObjectAttributes->RootDirectory || xbox_path[0] == '\\' ||
        (xbox_path[0] && xbox_path[1] == ':'))
        return xbox_translate_path(xbox_path, host_path, buf_size);

    root = w32_handle_path(ObjectAttributes->RootDirectory);
    if (!root)
        return FALSE;
    used = (size_t)snprintf(host_path, buf_size, "%s", root);
    if (used + 2 >= buf_size)
        return FALSE;
    if (xbox_path[0]) {
        host_path[used++] = '/';
        for (; *xbox_path && used + 1 < buf_size; xbox_path++)
            host_path[used++] = *xbox_path == '\\' ? '/' : *xbox_path;
    }
    host_path[used] = '\0';
    return *xbox_path == '\0';   /* FALSE if the name did not fit */
}

NTSTATUS __stdcall xbox_NtCreateFile(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
    ULONG CreateDisposition, ULONG CreateOptions)
{
    char host_path[MAX_PATH];
    DWORD win_access, share;
    int truncate = 0;
    (void)AllocationSize; (void)FileAttributes;
    g_xbox_last_file_error = 0;

    if (!FileHandle || !ObjectAttributes)
        return STATUS_INVALID_PARAMETER;

    if (!posix_translate_obj_path(ObjectAttributes, host_path, MAX_PATH)) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_FILE, "NtCreateFile: path translation failed");
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }

    int fd;
    struct stat st;
    /* A partition device opened as a directory (a free-space query) gets the
     * directory that holds its image, as in the Win32 backend. */
    if ((CreateOptions & XBOX_FILE_DIRECTORY_FILE) && stat(host_path, &st) == 0 &&
        !S_ISDIR(st.st_mode)) {
        char *slash = strrchr(host_path, '/');
        if (slash && slash != host_path)
            *slash = 0;
    }
    /* Win32 share rules, as in the Win32 backend: the caller's ShareAccess, and
     * read sharing for read-only opens (system-link testing shares read-only
     * disc files between two copies of the game). */
    win_access = posix_access_to_win32(DesiredAccess);
    share = posix_share_to_win32(ShareAccess);
    if (!(win_access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA |
          FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES | DELETE)))
        share |= FILE_SHARE_READ;

    if (CreateOptions & XBOX_FILE_DIRECTORY_FILE) {
        if (CreateDisposition == XBOX_FILE_CREATE || CreateDisposition == XBOX_FILE_OPEN_IF)
            mkdir(host_path, 0755);   /* EEXIST is fine */
        fd = open(host_path, O_RDONLY | O_DIRECTORY);
    } else {
        /* O_TRUNC is left to w32_open_handle_shared, so a refused open
         * cannot empty a file another open still holds. */
        int flags = posix_open_flags(DesiredAccess, CreateDisposition);
        truncate = (flags & O_TRUNC) != 0;
        fd = open(host_path, flags & ~O_TRUNC, 0644);
    }

    HANDLE handle = INVALID_HANDLE_VALUE;
    int e = 0;
    DWORD err = ERROR_SUCCESS;
    if (fd < 0) {
        e = errno;
        err = errno_to_win32(e);
    } else {
        handle = w32_open_handle_shared(fd, host_path, win_access, share, truncate);
        if (handle == INVALID_HANDLE_VALUE) {
            err = GetLastError();
            close(fd);
        }
    }
    if (handle == INVALID_HANDLE_VALUE) {
        NTSTATUS status = err == ERROR_SHARING_VIOLATION ? STATUS_SHARING_VIOLATION
                        : fd < 0 ? errno_to_status(e) : STATUS_UNSUCCESSFUL;
        const char *result = status == STATUS_SHARING_VIOLATION ? "SHARING_VIOLATION"
                           : e == ENOENT || e == ENOTDIR ? "NOT_FOUND" : "FAILED";
        /* Logged in release builds too: a failed open is how a title decides
         * a volume or asset is missing. The Win32 reason is kept for the
         * bridge, which turns a sharing conflict into the title's error. */
        fprintf(stderr, "[NtCreateFile] %s path=%s errno=%d err=%lu access=0x%08X disposition=%u options=0x%X\n",
                result, host_path, e, (unsigned long)err,
                (unsigned)DesiredAccess, (unsigned)CreateDisposition, (unsigned)CreateOptions);
        g_xbox_last_file_error = (uint32_t)err;
        if (IoStatusBlock) {
            IoStatusBlock->Status = status;
            IoStatusBlock->Information = 0;
        }
        return status;
    }

    *FileHandle = handle;
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = (CreateDisposition == XBOX_FILE_CREATE) ? 2 : 1;
    }
    XBOX_TRACE(XBOX_LOG_FILE, "NtCreateFile: %s -> handle=%p", host_path, *FileHandle);
    file_open_notify(host_path);
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtReadFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
    PLARGE_INTEGER ByteOffset)
{
    (void)ApcRoutine; (void)ApcContext;
    if (!IoStatusBlock)
        return STATUS_INVALID_PARAMETER;

    int fd = w32_handle_fd(FileHandle);
    if (fd < 0) {
        IoStatusBlock->Status = STATUS_INVALID_HANDLE;
        return STATUS_INVALID_HANDLE;
    }

    if (ByteOffset && ByteOffset->QuadPart >= 0)
        lseek(fd, (off_t)ByteOffset->QuadPart, SEEK_SET);

    ssize_t n = read(fd, Buffer, Length);
    if (n < 0) {
        XBOX_TRACE(XBOX_LOG_FILE, "NtReadFile(handle=%p) errno=%d", FileHandle, errno);
        IoStatusBlock->Status = STATUS_UNSUCCESSFUL;
        IoStatusBlock->Information = 0;
        return STATUS_UNSUCCESSFUL;
    }

    IoStatusBlock->Information = (ULONG_PTR)n;
    if (n == 0 && Length > 0) {
        IoStatusBlock->Status = STATUS_END_OF_FILE;
        return STATUS_END_OF_FILE;
    }
    IoStatusBlock->Status = STATUS_SUCCESS;
    if (Event) SetEvent(Event);
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtWriteFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
    PLARGE_INTEGER ByteOffset)
{
    (void)ApcRoutine; (void)ApcContext;
    if (!IoStatusBlock)
        return STATUS_INVALID_PARAMETER;

    int fd = w32_handle_fd(FileHandle);
    if (fd < 0) {
        IoStatusBlock->Status = STATUS_INVALID_HANDLE;
        return STATUS_INVALID_HANDLE;
    }

    if (ByteOffset && ByteOffset->QuadPart >= 0)
        lseek(fd, (off_t)ByteOffset->QuadPart, SEEK_SET);

    ssize_t n = write(fd, Buffer, Length);
    if (n < 0) {
        XBOX_TRACE(XBOX_LOG_FILE, "NtWriteFile(handle=%p) errno=%d", FileHandle, errno);
        IoStatusBlock->Status = STATUS_UNSUCCESSFUL;
        IoStatusBlock->Information = 0;
        return STATUS_UNSUCCESSFUL;
    }

    IoStatusBlock->Status = STATUS_SUCCESS;
    IoStatusBlock->Information = (ULONG_PTR)n;
    if (Event) SetEvent(Event);
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtClose(HANDLE Handle)
{
    XBOX_TRACE(XBOX_LOG_FILE, "NtClose(handle=%p)", Handle);
    if (xbox_CloseObjectHandle(Handle))
        return STATUS_SUCCESS;  /* object handle: a link or directory, nothing to close */
    if (Handle && Handle != INVALID_HANDLE_VALUE) {
        xbox_dir_context_close(Handle);
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_HANDLE;
}

NTSTATUS __stdcall xbox_NtDeleteFile(PXBOX_OBJECT_ATTRIBUTES ObjectAttributes)
{
    char host_path[MAX_PATH];
    if (!posix_translate_obj_path(ObjectAttributes, host_path, MAX_PATH))
        return STATUS_OBJECT_PATH_NOT_FOUND;
    XBOX_TRACE(XBOX_LOG_FILE, "NtDeleteFile: %s", host_path);
    if (unlink(host_path) == 0) return STATUS_SUCCESS;
    if (rmdir(host_path)  == 0) return STATUS_SUCCESS;
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

NTSTATUS __stdcall xbox_NtQueryInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation, ULONG Length, XBOX_FILE_INFORMATION_CLASS FileInformationClass)
{
    (void)Length;
    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;

    int fd = w32_handle_fd(FileHandle);
    if (fd < 0)
        return STATUS_INVALID_HANDLE;

    struct stat st;
    if (FileInformationClass != XboxFilePositionInformation) {
        if (fstat(fd, &st) != 0)
            return STATUS_UNSUCCESSFUL;
    }

    switch (FileInformationClass) {
        case XboxFileBasicInformation: {
            PXBOX_FILE_BASIC_INFORMATION info = (PXBOX_FILE_BASIC_INFORMATION)FileInformation;
            unix_to_filetime(st.st_ctime, 0, &info->CreationTime);
            unix_to_filetime(st.st_atime, 0, &info->LastAccessTime);
            unix_to_filetime(st.st_mtime, 0, &info->LastWriteTime);
            info->ChangeTime = info->LastWriteTime;
            info->FileAttributes = mode_to_xbox_attrs(st.st_mode);
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_BASIC_INFORMATION);
            return STATUS_SUCCESS;
        }
        case XboxFileStandardInformation: {
            PXBOX_FILE_STANDARD_INFORMATION info = (PXBOX_FILE_STANDARD_INFORMATION)FileInformation;
            info->EndOfFile.QuadPart = st.st_size;
            info->AllocationSize.QuadPart = (st.st_size + 4095) & ~4095LL;
            info->NumberOfLinks = (ULONG)st.st_nlink;
            info->DeletePending = FALSE;
            info->Directory = S_ISDIR(st.st_mode) ? TRUE : FALSE;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_STANDARD_INFORMATION);
            return STATUS_SUCCESS;
        }
        case XboxFilePositionInformation: {
            PXBOX_FILE_POSITION_INFORMATION info = (PXBOX_FILE_POSITION_INFORMATION)FileInformation;
            off_t pos = lseek(fd, 0, SEEK_CUR);
            if (pos < 0) return STATUS_UNSUCCESSFUL;
            info->CurrentByteOffset.QuadPart = pos;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_POSITION_INFORMATION);
            return STATUS_SUCCESS;
        }
        case XboxFileNetworkOpenInformation: {
            PXBOX_FILE_NETWORK_OPEN_INFORMATION info = (PXBOX_FILE_NETWORK_OPEN_INFORMATION)FileInformation;
            unix_to_filetime(st.st_ctime, 0, &info->CreationTime);
            unix_to_filetime(st.st_atime, 0, &info->LastAccessTime);
            unix_to_filetime(st.st_mtime, 0, &info->LastWriteTime);
            info->ChangeTime = info->LastWriteTime;
            info->EndOfFile.QuadPart = st.st_size;
            info->AllocationSize.QuadPart = (st.st_size + 4095) & ~4095LL;
            info->FileAttributes = mode_to_xbox_attrs(st.st_mode);
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_NETWORK_OPEN_INFORMATION);
            return STATUS_SUCCESS;
        }
        default:
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                "NtQueryInformationFile: unhandled class %d", FileInformationClass);
            return STATUS_NOT_IMPLEMENTED;
    }
}

NTSTATUS __stdcall xbox_NtSetInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation, ULONG Length, XBOX_FILE_INFORMATION_CLASS FileInformationClass)
{
    (void)Length;
    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;

    int fd = w32_handle_fd(FileHandle);
    if (fd < 0)
        return STATUS_INVALID_HANDLE;

    switch (FileInformationClass) {
        case XboxFilePositionInformation: {
            PXBOX_FILE_POSITION_INFORMATION info = (PXBOX_FILE_POSITION_INFORMATION)FileInformation;
            if (lseek(fd, (off_t)info->CurrentByteOffset.QuadPart, SEEK_SET) < 0)
                return STATUS_UNSUCCESSFUL;
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileEndOfFileInformation: {
            PXBOX_FILE_END_OF_FILE_INFORMATION info = (PXBOX_FILE_END_OF_FILE_INFORMATION)FileInformation;
            if (ftruncate(fd, (off_t)info->EndOfFile.QuadPart) != 0)
                return STATUS_UNSUCCESSFUL;
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileDispositionInformation: {
            PXBOX_FILE_DISPOSITION_INFORMATION info = (PXBOX_FILE_DISPOSITION_INFORMATION)FileInformation;
            /* POSIX: unlinking an open file removes it on last close -- this
             * matches NT "delete on close" semantics exactly. */
            if (info->DeleteFile) {
                const char* p = w32_handle_path(FileHandle);
                if (p) unlink(p);
            }
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileBasicInformation:
            /* Setting file times is non-essential for the game; accept it. */
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        default:
            /* stderr, not xbox_log: WARN is filtered out by default, and an
             * unimplemented info class is exactly the kind of silent gap that
             * surfaces far away. Halo's save path hits one, gets
             * STATUS_NOT_IMPLEMENTED, converts it to DOS error 317 and asserts
             * "couldn't open or create saved game file". */
            fprintf(stderr, "  [FILE] NtSetInformationFile: unhandled class %d\n",
                    (int)FileInformationClass);
            fflush(stderr);
            return STATUS_NOT_IMPLEMENTED;
    }
}

NTSTATUS __stdcall xbox_NtQueryVolumeInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FsInformation, ULONG Length, XBOX_FS_INFORMATION_CLASS FsInformationClass)
{
    (void)Length;
    if (!IoStatusBlock || !FsInformation)
        return STATUS_INVALID_PARAMETER;

    switch (FsInformationClass) {
        case XboxFileFsSizeInformation: {
            PXBOX_FILE_FS_SIZE_INFORMATION info = (PXBOX_FILE_FS_SIZE_INFORMATION)FsInformation;
            struct statvfs vfs;
            int fd = w32_handle_fd(FileHandle);
            /* Xbox geometry, not the host's -- see the note on
             * XBOX_SECTORS_PER_CLUSTER above. */
            info->BytesPerSector = XBOX_BYTES_PER_SECTOR;
            info->SectorsPerAllocationUnit = XBOX_SECTORS_PER_CLUSTER;
            if (fd >= 0 && fstatvfs(fd, &vfs) == 0) {
                ULONGLONG cs = (ULONGLONG)info->BytesPerSector * info->SectorsPerAllocationUnit;
                ULONGLONG total = (ULONGLONG)vfs.f_blocks * vfs.f_frsize;
                ULONGLONG avail = (ULONGLONG)vfs.f_bavail * vfs.f_frsize;
                info->TotalAllocationUnits.QuadPart = total / cs;
                info->AvailableAllocationUnits.QuadPart = avail / cs;
            } else {
                info->TotalAllocationUnits.QuadPart = 1048576;
                info->AvailableAllocationUnits.QuadPart = 524288;
            }
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_FS_SIZE_INFORMATION);
            return STATUS_SUCCESS;
        }
        default:
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                "NtQueryVolumeInformationFile: unhandled class %d", FsInformationClass);
            return STATUS_NOT_IMPLEMENTED;
    }
}

NTSTATUS __stdcall xbox_NtFlushBuffersFile(HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock)
{
    int fd = w32_handle_fd(FileHandle);
    if (fd >= 0) fsync(fd);
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = 0;
    }
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtQueryFullAttributesFile(
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes,
    PXBOX_FILE_NETWORK_OPEN_INFORMATION FileInformation)
{
    char host_path[MAX_PATH];
    struct stat st;

    if (!FileInformation)
        return STATUS_INVALID_PARAMETER;
    const char* xbox_path = get_xbox_path(ObjectAttributes);
    if (!xbox_path || !xbox_translate_path(xbox_path, host_path, MAX_PATH))
        return STATUS_OBJECT_PATH_NOT_FOUND;
    if (stat(host_path, &st) != 0)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    unix_to_filetime(st.st_ctime, 0, &FileInformation->CreationTime);
    unix_to_filetime(st.st_atime, 0, &FileInformation->LastAccessTime);
    unix_to_filetime(st.st_mtime, 0, &FileInformation->LastWriteTime);
    FileInformation->ChangeTime = FileInformation->LastWriteTime;
    FileInformation->EndOfFile.QuadPart = st.st_size;
    FileInformation->AllocationSize.QuadPart = (st.st_size + 4095) & ~4095LL;
    FileInformation->FileAttributes = mode_to_xbox_attrs(st.st_mode);
    return STATUS_SUCCESS;
}

/* Directory enumeration state, keyed by the directory's Nt handle. */
#define MAX_DIR_CONTEXTS 64
typedef struct {
    HANDLE handle;
    DIR*   dir;
    char   pattern[MAX_PATH];
    BOOL   initial_query;
    BOOL   pending;
    BOOL   exhausted;
    BOOL   pattern_set;
    char   pending_name[MAX_PATH];
    struct stat pending_stat;
} DIR_CONTEXT;

static DIR_CONTEXT s_dir_contexts[MAX_DIR_CONTEXTS];
static CRITICAL_SECTION s_dir_cs;

static void xbox_dir_context_close(HANDLE FileHandle)
{
    ensure_dir_cs(&s_dir_cs);
    EnterCriticalSection(&s_dir_cs);
    for (int i = 0; i < MAX_DIR_CONTEXTS; i++) {
        DIR_CONTEXT *ctx = &s_dir_contexts[i];
        if (ctx->handle != FileHandle)
            continue;
        if (ctx->dir) {
            closedir(ctx->dir);
            ctx->dir = NULL;
        }
        ctx->handle = NULL;
        ctx->initial_query = FALSE;
        ctx->pending = FALSE;
        ctx->exhausted = FALSE;
        ctx->pattern_set = FALSE;
        ctx->pattern[0] = '\0';
    }
    CloseHandle(FileHandle);
    LeaveCriticalSection(&s_dir_cs);
}

/* Closing a directory handle ends its search, as the Win32 backend does. An
 * open DIR* would otherwise hold a context slot, and a recycled handle value
 * would resume the old enumeration. */
void xbox_dir_context_drop(HANDLE FileHandle)
{
    if (!s_dir_cs_init) return;
    EnterCriticalSection(&s_dir_cs);
    for (int i = 0; i < MAX_DIR_CONTEXTS; i++) {
        if (s_dir_contexts[i].handle == FileHandle && FileHandle) {
            if (s_dir_contexts[i].dir) closedir(s_dir_contexts[i].dir);
            s_dir_contexts[i].dir = NULL;
            s_dir_contexts[i].handle = NULL;
        }
    }
    LeaveCriticalSection(&s_dir_cs);
}

NTSTATUS __stdcall xbox_NtQueryDirectoryFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length,
    XBOX_FILE_INFORMATION_CLASS FileInformationClass,
    PXBOX_ANSI_STRING FileName, BOOLEAN RestartScan)
{
    (void)Event; (void)ApcRoutine; (void)ApcContext;
    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;
    IoStatusBlock->Information = 0;
    if (FileInformationClass != XboxFileDirectoryInformation) {
        IoStatusBlock->Status = STATUS_INVALID_INFO_CLASS;
        return STATUS_INVALID_INFO_CLASS;
    }

    ULONG header_size = (ULONG)offsetof(XBOX_FILE_DIRECTORY_INFORMATION, FileName);
    if (Length < sizeof(XBOX_FILE_DIRECTORY_INFORMATION)) {
        IoStatusBlock->Status = STATUS_INFO_LENGTH_MISMATCH;
        return STATUS_INFO_LENGTH_MISMATCH;
    }

    ensure_dir_cs(&s_dir_cs);
    EnterCriticalSection(&s_dir_cs);

    /* Locate or create the per-handle enumeration context. */
    DIR_CONTEXT* ctx = NULL;
    for (int i = 0; i < MAX_DIR_CONTEXTS; i++)
        if (s_dir_contexts[i].handle == FileHandle) { ctx = &s_dir_contexts[i]; break; }
    if (!ctx) {
        for (int i = 0; i < MAX_DIR_CONTEXTS; i++)
            if (s_dir_contexts[i].handle == NULL) { ctx = &s_dir_contexts[i]; break; }
        if (!ctx) {
            IoStatusBlock->Status = STATUS_INSUFFICIENT_RESOURCES;
            LeaveCriticalSection(&s_dir_cs);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        ctx->handle = FileHandle;
        ctx->dir = NULL;
        ctx->initial_query = TRUE;
        ctx->pending = FALSE;
        ctx->exhausted = FALSE;
        ctx->pattern_set = FALSE;
    }

    if (ctx->exhausted && !RestartScan) {
        IoStatusBlock->Status = STATUS_NO_MORE_FILES;
        LeaveCriticalSection(&s_dir_cs);
        return STATUS_NO_MORE_FILES;
    }
    if (RestartScan || (!ctx->exhausted && ctx->dir == NULL)) {
        if (!ctx->pattern_set) {
            if (FileName && FileName->Buffer && FileName->Length >= sizeof(ctx->pattern)) {
                IoStatusBlock->Status = STATUS_INVALID_PARAMETER;
                LeaveCriticalSection(&s_dir_cs);
                return STATUS_INVALID_PARAMETER;
            }
            if (FileName && FileName->Buffer && FileName->Length > 0) {
                USHORT n = FileName->Length;
                memcpy(ctx->pattern, FileName->Buffer, n);
                ctx->pattern[n] = '\0';
            } else {
                strcpy(ctx->pattern, "*");
            }
            ctx->pattern_set = TRUE;
        }
        if (ctx->dir) { closedir(ctx->dir); ctx->dir = NULL; }
        const char* dpath = w32_handle_path(FileHandle);
        if (!dpath) { LeaveCriticalSection(&s_dir_cs); return STATUS_UNSUCCESSFUL; }
        ctx->dir = opendir(dpath);
        if (!ctx->dir) {
            ctx->handle = NULL;
            LeaveCriticalSection(&s_dir_cs);
            IoStatusBlock->Status = STATUS_NO_MORE_FILES;
            return STATUS_NO_MORE_FILES;
        }
        ctx->initial_query = TRUE;
        ctx->pending = FALSE;
        ctx->exhausted = FALSE;
    }

    /* Advance to the next entry matching the search pattern. */
    if (!ctx->pending) {
        struct dirent* de;
        const char* dpath = w32_handle_path(FileHandle);
        if (!dpath) {
            LeaveCriticalSection(&s_dir_cs);
            IoStatusBlock->Status = STATUS_UNSUCCESSFUL;
            return STATUS_UNSUCCESSFUL;
        }
        errno = 0;
        for (;;) {
            de = readdir(ctx->dir);
            if (!de) {
                int read_error = errno;
                closedir(ctx->dir);
                ctx->dir = NULL;
                ctx->pending = FALSE;
                ctx->exhausted = TRUE;
                LeaveCriticalSection(&s_dir_cs);
                IoStatusBlock->Status = read_error ? errno_to_status(read_error) : STATUS_NO_MORE_FILES;
                return IoStatusBlock->Status;
            }
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                continue;
            if (fnmatch(ctx->pattern, de->d_name, FNM_CASEFOLD) == 0)
                break;
        }
        size_t name_length = strlen(de->d_name);
        if (name_length >= sizeof(ctx->pending_name)) {
            LeaveCriticalSection(&s_dir_cs);
            IoStatusBlock->Status = STATUS_UNSUCCESSFUL;
            return STATUS_UNSUCCESSFUL;
        }
        memcpy(ctx->pending_name, de->d_name, name_length + 1);
        if (fstatat(dirfd(ctx->dir), ctx->pending_name, &ctx->pending_stat, 0) != 0) {
            NTSTATUS status = errno_to_status(errno);
            ctx->pending = FALSE;
            IoStatusBlock->Status = status;
            LeaveCriticalSection(&s_dir_cs);
            return status;
        }
        ctx->pending = TRUE;
    }

    size_t name_len = strlen(ctx->pending_name);
    struct stat st = ctx->pending_stat;
    char name[MAX_PATH];
    memcpy(name, ctx->pending_name, name_len + 1);

    ULONG copied_name = (ULONG)name_len < Length - header_size
        ? (ULONG)name_len : Length - header_size;
    if (copied_name < name_len && !ctx->initial_query) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = 0;
        LeaveCriticalSection(&s_dir_cs);
        return STATUS_SUCCESS;
    }
    PXBOX_FILE_DIRECTORY_INFORMATION entry = (PXBOX_FILE_DIRECTORY_INFORMATION)FileInformation;
    memset(entry, 0, header_size + copied_name);
    entry->NextEntryOffset = 0;
    entry->FileIndex = 0;
    unix_to_filetime(st.st_ctime, 0, &entry->CreationTime);
    unix_to_filetime(st.st_atime, 0, &entry->LastAccessTime);
    unix_to_filetime(st.st_mtime, 0, &entry->LastWriteTime);
    entry->ChangeTime = entry->LastWriteTime;
    entry->EndOfFile.QuadPart = st.st_size;
    entry->AllocationSize.QuadPart = (st.st_size + 4095) & ~4095LL;
    entry->FileAttributes = mode_to_xbox_attrs(st.st_mode);
    entry->FileNameLength = (ULONG)name_len;

    if (copied_name)
        memcpy(entry->FileName, name, copied_name);
    IoStatusBlock->Information = header_size + copied_name;
    if (copied_name < name_len) {
        ctx->initial_query = FALSE;
        IoStatusBlock->Status = STATUS_BUFFER_OVERFLOW;
        LeaveCriticalSection(&s_dir_cs);
        return STATUS_BUFFER_OVERFLOW;
    }
    ctx->pending = FALSE;
    ctx->initial_query = FALSE;
    IoStatusBlock->Status = STATUS_SUCCESS;
    LeaveCriticalSection(&s_dir_cs);
    return STATUS_SUCCESS;
}

#endif /* _WIN32 */

/* ======================================================================== */
/* ====================  Platform-independent  ============================ */
/* ======================================================================== */

NTSTATUS __stdcall xbox_NtOpenFile(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    ULONG ShareAccess, ULONG OpenOptions)
{
    /* NtOpenFile is NtCreateFile with FILE_OPEN disposition */
    return xbox_NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes,
        IoStatusBlock, NULL, 0, ShareAccess, XBOX_FILE_OPEN, OpenOptions);
}

NTSTATUS __stdcall xbox_IoCreateFile(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
    ULONG Disposition, ULONG CreateOptions, ULONG Options)
{
    (void)Options;
    return xbox_NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
        AllocationSize, FileAttributes, ShareAccess, Disposition, CreateOptions);
}

NTSTATUS __stdcall xbox_NtFsControlFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, ULONG FsControlCode,
    PVOID InputBuffer, ULONG InputBufferLength,
    PVOID OutputBuffer, ULONG OutputBufferLength)
{
    (void)FileHandle; (void)Event; (void)ApcRoutine; (void)ApcContext;
    (void)InputBuffer; (void)InputBufferLength; (void)OutputBuffer; (void)OutputBufferLength;
    xbox_NoteUnsupportedIoctl("NtFsControlFile", FsControlCode);
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_NOT_IMPLEMENTED;
        IoStatusBlock->Information = 0;
    }
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS __stdcall xbox_NtDeviceIoControlFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, ULONG IoControlCode,
    PVOID InputBuffer, ULONG InputBufferLength,
    PVOID OutputBuffer, ULONG OutputBufferLength)
{
    (void)FileHandle; (void)Event; (void)ApcRoutine; (void)ApcContext;
    (void)InputBuffer; (void)InputBufferLength; (void)OutputBuffer; (void)OutputBufferLength;
    xbox_NoteUnsupportedIoctl("NtDeviceIoControlFile", IoControlCode);
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_NOT_IMPLEMENTED;
        IoStatusBlock->Information = 0;
    }
    return STATUS_NOT_IMPLEMENTED;
}

/* The link's name comes from the OBJECT_ATTRIBUTES. Path translation (kernel_path.c)
 * maps the drive letters; this only answers which link a handle names. */
NTSTATUS __stdcall xbox_NtOpenSymbolicLinkObject(
    PHANDLE LinkHandle, PXBOX_OBJECT_ATTRIBUTES ObjectAttributes)
{
    char name[128];
    PXBOX_ANSI_STRING s;

    if (!LinkHandle || !ObjectAttributes || !(s = ObjectAttributes->ObjectName) ||
        !s->Buffer || s->Length >= sizeof(name))
        return STATUS_INVALID_PARAMETER;
    memcpy(name, s->Buffer, s->Length);
    name[s->Length] = '\0';
    return xbox_OpenSymbolicLink(name, LinkHandle);
}

NTSTATUS __stdcall xbox_NtQuerySymbolicLinkObject(
    HANDLE LinkHandle, PXBOX_ANSI_STRING LinkTarget, PULONG ReturnedLength)
{
    const char* target = xbox_SymbolicLinkHandleTarget(LinkHandle);
    USHORT len;

    if (!target)
        return STATUS_INVALID_HANDLE;
    len = (USHORT)strlen(target);
    if (ReturnedLength)
        *ReturnedLength = (ULONG)len;
    if (!LinkTarget || !LinkTarget->Buffer || len >= LinkTarget->MaximumLength)
        return (NTSTATUS)0xC0000023L; /* STATUS_BUFFER_TOO_SMALL */
    memcpy(LinkTarget->Buffer, target, len + 1);
    LinkTarget->Length = len;
    return STATUS_SUCCESS;
}
