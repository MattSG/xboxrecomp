/* A real host sharing conflict must survive the Xbox error conversion. */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel_test_host.h"

typedef void (*guest_fn)(void);
extern guest_fn recomp_lookup_kernel(uint32_t);
extern RECOMP_TLS uint32_t g_eax, g_esp;
extern ptrdiff_t g_xbox_mem_offset;
extern uint32_t xbox_LastFileError(void);
void *recomp_lookup(ULONG address) { (void)address; abort(); }
void *recomp_lookup_manual(ULONG address) { (void)address; abort(); }

static volatile LONG observer_calls;
static HANDLE observer_root;
static int observer_path_matched;
static int observer_bad;
static void opened_observer(const xbox_host_char *path)
{
    InterlockedIncrement(&observer_calls);
#ifdef _WIN32
    observer_path_matched = wcsstr(path, L"sharing.bin") != NULL;
#else
    observer_path_matched = strstr(path, "sharing.bin") != NULL;
#endif
    /* A nested failed host-side open must not replace the observed result. */
    XBOX_ANSI_STRING name={0}; XBOX_OBJECT_ATTRIBUTES oa={0}; XBOX_IO_STATUS_BLOCK ios={0};
    HANDLE h=NULL;
    name.Buffer="observer-absent.bin"; name.Length=(USHORT)strlen(name.Buffer); name.MaximumLength=name.Length+1;
    oa.RootDirectory=observer_root; oa.ObjectName=&name;
    NTSTATUS status=xbox_NtCreateFile(&h,XBOX_GENERIC_READ,&oa,&ios,NULL,0,1,XBOX_FILE_OPEN,XBOX_FILE_NON_DIRECTORY_FILE);
    if (status!=STATUS_OBJECT_NAME_NOT_FOUND || h) observer_bad=1;
    SetLastError(12345);
}

typedef struct MissingOpen {
    HANDLE root, ready, release;
    NTSTATUS status, ios_status;
    ULONG_PTR information;
    uint32_t error_before, error_after;
} MissingOpen;

static DWORD WINAPI missing_open_worker(void *parameter)
{
    MissingOpen *test = parameter;
    XBOX_ANSI_STRING name = {0};
    XBOX_OBJECT_ATTRIBUTES oa = {0};
    XBOX_IO_STATUS_BLOCK ios = {0};
    HANDLE opened = NULL;
    name.Buffer = "absent.bin";
    name.Length = (USHORT)strlen(name.Buffer);
    name.MaximumLength = name.Length + 1;
    oa.RootDirectory = test->root;
    oa.ObjectName = &name;
    test->status = xbox_NtCreateFile(&opened, XBOX_GENERIC_READ, &oa, &ios,
        NULL, 0x80, 1, XBOX_FILE_OPEN, XBOX_FILE_NON_DIRECTORY_FILE);
    test->ios_status = ios.Status;
    test->information = ios.Information;
    test->error_before = xbox_LastFileError();
    SetEvent(test->ready);
    if (WaitForSingleObject(test->release, 5000) != WAIT_OBJECT_0) return 2;
    test->error_after = xbox_LastFileError();
    if (opened) xbox_NtClose(opened);
    return 0;
}

static int malformed_name_contract(guest_fn open_guest, uint8_t *memory)
{
    uint32_t *sp = (uint32_t *)(memory + 0x20000);
    FILE *capture = NULL;
    int saved_stderr, ok;
    char output[4096] = {0};
    if (!open_guest || tmpfile_s(&capture) || !capture) return 0;
    saved_stderr = _dup(_fileno(stderr));
    if (saved_stderr < 0) { fclose(capture); return 0; }
    fflush(stderr);
    if (_dup2(_fileno(capture), _fileno(stderr))) {
        _close(saved_stderr); fclose(capture); return 0;
    }
    memset(sp, 0, 40);
    memset(memory + 0x40000, 0, 0x110);
    *(uint32_t *)(memory + 0x40004) = 0x40100; /* Empty ANSI_STRING.Buffer. */
    *(uint32_t *)(memory + 0x40500) = 0xA5A5A5A5;
    sp[0] = 0xBEEF0001; sp[1] = 0x40500; sp[2] = XBOX_GENERIC_READ;
    sp[3] = 0x40000; sp[4] = 0x40600;
    sp[6] = 0x80; sp[7] = 1; sp[8] = XBOX_FILE_OPEN; sp[9] = XBOX_FILE_NON_DIRECTORY_FILE;
    g_esp = 0x20000;
    open_guest();
    fflush(stderr);
    _dup2(saved_stderr, _fileno(stderr));
    _close(saved_stderr);
    rewind(capture);
    fread(output, 1, sizeof output - 1, capture);
    fclose(capture);
    ok = g_eax == STATUS_OBJECT_PATH_NOT_FOUND && g_esp == 0x20028 &&
         *(uint32_t *)(memory + 0x40600) == STATUS_OBJECT_PATH_NOT_FOUND &&
         *(uint32_t *)(memory + 0x40604) == 0 &&
         *(uint32_t *)(memory + 0x40500) == 0xA5A5A5A5 &&
         strstr(output, "win32 err=0") && !strstr(output, "ERROR_SHARING_VIOLATION");
    if (!ok) fprintf(stderr, "FAIL: malformed guest name inherited an unrelated host open error:\n%s", output);
    return ok;
}

/* Default/cleared policy preserves exclusive reads; opt-in broadens only
 * read-only opens and must never give writers shared access. */
static int shared_read_contract(XBOX_OBJECT_ATTRIBUTES *oa, int shared)
{
    HANDLE first = NULL, second = NULL, writer = NULL;
    XBOX_IO_STATUS_BLOCK ios = {0};
    NTSTATUS status;
    int ok = 0;
    status = xbox_NtCreateFile(&first, XBOX_GENERIC_READ, oa, &ios,
        NULL, 0x80, 0, XBOX_FILE_OPEN, XBOX_FILE_NON_DIRECTORY_FILE);
    if (status != STATUS_SUCCESS || !first) goto done;
    status = xbox_NtCreateFile(&second, XBOX_GENERIC_READ, oa, &ios,
        NULL, 0x80, 0, XBOX_FILE_OPEN, XBOX_FILE_NON_DIRECTORY_FILE);
    if (!shared) {
        ok = status == STATUS_SHARING_VIOLATION && ios.Status == status && !second;
        goto done;
    }
    if (status != STATUS_SUCCESS || !second) goto done;
    status = xbox_NtCreateFile(&writer, XBOX_GENERIC_WRITE, oa, &ios,
        NULL, 0x80, 1, XBOX_FILE_OPEN, XBOX_FILE_NON_DIRECTORY_FILE);
    if (status != STATUS_SHARING_VIOLATION || ios.Status != status || writer ||
        xbox_LastFileError() != ERROR_SHARING_VIOLATION) goto done;
    xbox_NtClose(second); second = NULL;
    xbox_NtClose(first); first = NULL;
    status = xbox_NtCreateFile(&writer, XBOX_GENERIC_WRITE, oa, &ios,
        NULL, 0x80, 0, XBOX_FILE_OPEN, XBOX_FILE_NON_DIRECTORY_FILE);
    if (status != STATUS_SUCCESS || !writer) goto done;
    status = xbox_NtCreateFile(&first, XBOX_GENERIC_READ, oa, &ios,
        NULL, 0x80, 0, XBOX_FILE_OPEN, XBOX_FILE_NON_DIRECTORY_FILE);
    ok = status == STATUS_SHARING_VIOLATION && ios.Status == status && !first &&
         xbox_LastFileError() == ERROR_SHARING_VIOLATION;
done:
    if (first) xbox_NtClose(first);
    if (second) xbox_NtClose(second);
    if (writer) xbox_NtClose(writer);
    if (!ok) fprintf(stderr, "FAIL: system-link shared reads or exclusive writes changed\n");
    return ok;
}

int main(int argc,char **argv)
{
    /* This fixture owns fixed raw scratch addresses below the export page. */
    if (!xbox_ConfigureKernelData(0x00740000u)) return 90;

    char dir[MAX_PATH] = {0}, path[MAX_PATH] = {0};
    HANDLE exclusive = INVALID_HANDLE_VALUE, root = INVALID_HANDLE_VALUE, opened = NULL;
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK ios;
    NTSTATUS status;
    uint8_t *memory = NULL;
    guest_fn convert;
    int result = 1, directory_created = 0;
    uint32_t *sp;
    MissingOpen missing = {0};
    HANDLE worker = NULL;
    int diagnostics = argc > 1 && !strcmp(argv[1],"diagnostics"), saved_stderr = -1;
    FILE *captured = NULL;
    if (diagnostics) {
        _putenv_s("RECOMP_FILE_DIAGNOSTICS","1");
        captured = tmpfile(); saved_stderr = _dup(_fileno(stderr));
        if (!captured || saved_stderr < 0 || _dup2(_fileno(captured),_fileno(stderr))) {
            if (captured) fclose(captured);
            if (saved_stderr >= 0) _close(saved_stderr);
            return 2;
        }
    }


    /* Narrow paths: the host helpers create the unique directory on either
     * platform, and CreateFileA is the same share-mode call as CreateFileW. */
    if (!xt_make_temp_dir(dir, sizeof dir))
        goto done;
    directory_created = 1;
    if (xt_join(path, sizeof path, dir, "sharing.bin") < 0)
        goto done;
    exclusive = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    root = CreateFileA(dir, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (exclusive == INVALID_HANDLE_VALUE || root == INVALID_HANDLE_VALUE)
        goto done;
    observer_root=root;
    xbox_SetFileOpenObserver(opened_observer);
    memset(&name, 0, sizeof name);
    name.Buffer = "sharing.bin";
    name.Length = (USHORT)strlen(name.Buffer);
    name.MaximumLength = name.Length + 1;
    memset(&oa, 0, sizeof oa);
    oa.RootDirectory = root;
    oa.ObjectName = &name;
    memset(&ios, 0xA5, sizeof ios);
    status = xbox_NtCreateFile(&opened,
        XBOX_GENERIC_READ | XBOX_SYNCHRONIZE | XBOX_FILE_READ_ATTRIBUTES,
        &oa, &ios, NULL, 0x80, 1, XBOX_FILE_OPEN,
        XBOX_FILE_SYNCHRONOUS_IO_NONALERT | XBOX_FILE_NON_DIRECTORY_FILE);
    if (status != STATUS_SHARING_VIOLATION || ios.Status != status ||
        ios.Information != 0 || opened != NULL ||
        xbox_RtlNtStatusToDosError(status) != ERROR_SHARING_VIOLATION) {
        fprintf(stderr, "FAIL: sharing conflict status=%08X ios=%08X information=%llu handle=%p\n",
                (unsigned)status, (unsigned)ios.Status,
                (unsigned long long)ios.Information, opened);
        goto done;
    }

    if (observer_calls) { fprintf(stderr,"FAIL: observer ran for a failed open\n"); goto done; }
    /* Another streaming worker can miss an optional search candidate between
     * the host open and the bridge's diagnostic. Each thread must retain its
     * own error; the missing open must still fail with its exact status. */
    missing.root = root;
    missing.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    missing.release = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!missing.ready || !missing.release) goto done;
    worker = CreateThread(NULL, 0, missing_open_worker, &missing, 0, NULL);
    if (!worker || WaitForSingleObject(missing.ready, 5000) != WAIT_OBJECT_0) goto done;
    if (xbox_LastFileError() != ERROR_SHARING_VIOLATION ||
        missing.status != STATUS_OBJECT_NAME_NOT_FOUND || missing.ios_status != missing.status ||
        missing.information || missing.error_before != ERROR_FILE_NOT_FOUND) {
        fprintf(stderr, "FAIL: concurrent open mixed error provenance: main=%u worker=%u status=%08X\n",
                xbox_LastFileError(), missing.error_before, (unsigned)missing.status);
        goto done;
    }

    /* Translated games use the bridge's independent status mapping. */
    memory = VirtualAlloc(NULL, 16 * 1024 * 1024,
                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!memory)
        goto done;
    g_xbox_mem_offset = (ptrdiff_t)memory;
    *(uint32_t *)(memory + 0x10000) = 0x80000000u | 301;
    *(uint32_t *)(memory + 0x10004) = 0x80000000u | 190;
    xbox_kernel_set_thunk_address(0x10000, 2);
    xbox_kernel_bridge_init();
    convert = recomp_lookup_kernel(*(uint32_t *)(memory + 0x10000));
    if (!convert)
        goto done;
    g_esp = 0x20000;
    sp = (uint32_t *)(memory + g_esp);
    sp[0] = 0xBEEF0001;
    sp[1] = (uint32_t)status;
    convert();
    if (g_eax != ERROR_SHARING_VIOLATION || g_esp != 0x20008) {
        fprintf(stderr, "FAIL: bridge lost sharing error: eax=%08X esp=%08X\n", g_eax, g_esp);
        goto done;
    }
    if (!malformed_name_contract(recomp_lookup_kernel(*(uint32_t *)(memory + 0x10004)), memory))
        goto done;

    /* Sharing was enforced, and the same request can succeed after release. */
    CloseHandle(exclusive);
    exclusive = INVALID_HANDLE_VALUE;
    status = xbox_NtCreateFile(&opened,
        XBOX_GENERIC_READ | XBOX_SYNCHRONIZE | XBOX_FILE_READ_ATTRIBUTES,
        &oa, &ios, NULL, 0x80, 1, XBOX_FILE_OPEN,
        XBOX_FILE_SYNCHRONOUS_IO_NONALERT | XBOX_FILE_NON_DIRECTORY_FILE);
    if (status != STATUS_SUCCESS || ios.Status != STATUS_SUCCESS || !opened) {
        fprintf(stderr, "FAIL: unchanged open failed after host lock closed: %08X\n", (unsigned)status);
        goto done;
    }
    if (xbox_LastFileError() != 0) {
        fprintf(stderr, "FAIL: successful open retained a stale failure reason\n");
        goto done;
    }
    if (observer_calls!=1 || observer_bad || !observer_path_matched || GetLastError()==12345) {
        fprintf(stderr,"FAIL: successful-open observer or caller error preservation\n"); goto done;
    }
    xbox_SetFileOpenObserver(NULL);
    SetEvent(missing.release);
    if (WaitForSingleObject(worker, 5000) != WAIT_OBJECT_0 ||
        missing.error_after != ERROR_FILE_NOT_FOUND || xbox_LastFileError() != 0) {
        fprintf(stderr, "FAIL: worker lost its error after the other thread opened successfully\n");
        goto done;
    }
    /* Empty-file streaming completion remains EOF, not successful fake data. */
    {
        unsigned char bytes[512];
        LARGE_INTEGER offset = {0};
        memset(bytes, 0xA5, sizeof bytes);
        status = xbox_NtReadFile(opened, NULL, NULL, NULL, &ios, bytes, sizeof bytes, &offset);
        if (status != STATUS_END_OF_FILE || ios.Status != status || ios.Information != 0 || bytes[0] != 0xA5) {
            fprintf(stderr, "FAIL: EOF changed status, count or destination bytes\n");
            goto done;
        }
    }
    xbox_NtClose(opened); opened = NULL;
    if (!shared_read_contract(&oa, 0)) goto done;
    xbox_SetReadOnlyFileSharing(TRUE);
    if (!shared_read_contract(&oa, 1)) goto done;
    xbox_SetReadOnlyFileSharing(FALSE);
    if (!shared_read_contract(&oa, 0)) goto done;
    puts("PASS: system-link reads share; writes and host locks stay exclusive; concurrent errors, absence and EOF retain native statuses");
    result = 0;

done:
    xbox_SetReadOnlyFileSharing(FALSE);
    xbox_SetFileOpenObserver(NULL);
    if (worker) {
        SetEvent(missing.release);
        WaitForSingleObject(worker, 5000);
        CloseHandle(worker);
    }
    if (missing.ready) CloseHandle(missing.ready);
    if (missing.release) CloseHandle(missing.release);
    if (opened && opened != INVALID_HANDLE_VALUE) xbox_NtClose(opened);
    if (root != INVALID_HANDLE_VALUE) CloseHandle(root);
    if (exclusive != INVALID_HANDLE_VALUE) CloseHandle(exclusive);
    if (memory) VirtualFree(memory, 0, MEM_RELEASE);
    if (*path) xt_delete_file(path);
    if (directory_created) xt_remove_dir(dir);
    if (diagnostics) {
        char output[65536] = {0};
        fflush(stderr); _dup2(saved_stderr,_fileno(stderr)); _close(saved_stderr);
        rewind(captured); fread(output,1,sizeof output-1,captured); fclose(captured);
        if (!strstr(output,"[FILE_DIAG] sharing failure") ||
            !strstr(output,"sharing.bin") || !strstr(output,"guest_same_file handle=")) {
            fprintf(stderr,"FAIL: generic sharing diagnostics omitted synthetic file/owner evidence\n");
            result = 1;
        }
    }
    return result;
}
