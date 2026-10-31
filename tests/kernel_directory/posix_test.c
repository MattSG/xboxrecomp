#define _GNU_SOURCE
#include "kernel.h"
#include <dirent.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void *recomp_lookup(ULONG address) { (void)address; abort(); }
void *recomp_lookup_manual(ULONG address) { (void)address; abort(); }

#ifndef STATUS_BUFFER_TOO_SMALL
#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xC0000023L)
#endif
#ifndef STATUS_INFO_LENGTH_MISMATCH
#define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004L)
#endif

static int fail(const char *message, NTSTATUS status, ULONG_PTR information)
{
    fprintf(stderr, "FAIL: %s (status=%08X information=%zu)\n",
            message, (unsigned)status, (size_t)information);
    return 0;
}

static int query(HANDLE handle, const char *pattern, BOOLEAN restart,
                 void *buffer, ULONG length, XBOX_IO_STATUS_BLOCK *ios)
{
    XBOX_ANSI_STRING name = {0};
    if (pattern) {
        name.Buffer = (PCHAR)pattern;
        name.Length = (USHORT)strlen(pattern);
        name.MaximumLength = name.Length + 1;
    }
    return xbox_NtQueryDirectoryFile(handle, NULL, NULL, NULL, ios, buffer,
        length, XboxFileDirectoryInformation, pattern ? &name : NULL, restart);
}

static int check_boundary(HANDLE handle, ULONG length)
{
    enum { GUARD = 16, CAPACITY = sizeof(XBOX_FILE_DIRECTORY_INFORMATION) + 64 + GUARD };
    unsigned char bytes[CAPACITY];
    XBOX_IO_STATUS_BLOCK ios = {0};
    memset(bytes, 0xA5, sizeof(bytes));
    NTSTATUS status = (NTSTATUS)query(handle, "alpha.txt", TRUE, bytes, length, &ios);
    if (status != STATUS_INFO_LENGTH_MISMATCH || ios.Status != status || ios.Information != 0)
        return fail("undersized fixed header must fail without data", status, ios.Information);
    for (size_t i = 0; i < sizeof(bytes); i++)
        if (bytes[i] != 0xA5)
            return fail("undersized buffer was modified", status, ios.Information);
    return 1;
}

static int check_record(HANDLE handle, ULONG length, NTSTATUS expected_status,
                        ULONG_PTR expected_information, size_t copied_name, BOOLEAN restart)
{
    enum { CAPACITY = sizeof(XBOX_FILE_DIRECTORY_INFORMATION) + 64 + 16 };
    unsigned char bytes[CAPACITY];
    XBOX_IO_STATUS_BLOCK ios = {0};
    const ULONG header = (ULONG)offsetof(XBOX_FILE_DIRECTORY_INFORMATION, FileName);
    const char name[] = "alpha.txt";
    memset(bytes, 0xA5, sizeof(bytes));
    NTSTATUS status = (NTSTATUS)query(handle, "alpha.txt", restart, bytes, length, &ios);
    XBOX_FILE_DIRECTORY_INFORMATION *entry = (void *)bytes;
    if (status != expected_status || ios.Status != status ||
        ios.Information != expected_information || entry->FileNameLength != sizeof(name) - 1)
        return fail("directory entry status, information, or required name length differs",
                    status, ios.Information);
    if (copied_name && memcmp(entry->FileName, name, copied_name) != 0)
        return fail("partial or complete filename differs", status, ios.Information);
    for (size_t i = header + copied_name; i < sizeof(bytes); i++)
        if (bytes[i] != 0xA5)
            return fail("bytes beyond the reported filename were modified", status, ios.Information);
    return 1;
}

static int check_retry_larger_buffer(HANDLE handle, ULONG length)
{
    unsigned char bytes[sizeof(XBOX_FILE_DIRECTORY_INFORMATION) + 64 + 16];
    XBOX_IO_STATUS_BLOCK ios = {0};
    memset(bytes, 0xA5, sizeof(bytes));
    NTSTATUS status = (NTSTATUS)query(handle, "alpha.txt", FALSE, bytes, length, &ios);
    if (status != STATUS_SUCCESS || ios.Status != status || ios.Information != 0)
        return fail("subsequent short buffer reports success with no structure", status, ios.Information);
    for (size_t i = 0; i < sizeof(bytes); i++)
        if (bytes[i] != 0xA5)
            return fail("subsequent short buffer modified output", status, ios.Information);
    return 1;
}

static int check_long_pattern(HANDLE handle, ULONG length)
{
    char pattern[MAX_PATH];
    unsigned char bytes[sizeof(XBOX_FILE_DIRECTORY_INFORMATION) + 64];
    XBOX_ANSI_STRING name = { .Buffer = pattern, .Length = sizeof(pattern),
                              .MaximumLength = sizeof(pattern) };
    XBOX_IO_STATUS_BLOCK ios = {0};
    memset(pattern, 'a', sizeof(pattern));
    memset(bytes, 0xA5, sizeof(bytes));
    NTSTATUS status = xbox_NtQueryDirectoryFile(handle, NULL, NULL, NULL, &ios,
        bytes, length, XboxFileDirectoryInformation, &name, TRUE);
    if (status != STATUS_INVALID_PARAMETER || ios.Status != status || ios.Information)
        return fail("overlong counted ANSI search mask rejected", status, ios.Information);
    for (size_t i = 0; i < sizeof(bytes); i++)
        if (bytes[i] != 0xA5)
            return fail("overlong search mask modified output", status, ios.Information);
    return 1;
}

static int check_missing_metadata(PXBOX_OBJECT_ATTRIBUTES attributes,
                                  XBOX_IO_STATUS_BLOCK *ios, const char *pattern)
{
    HANDLE handle = NULL;
    NTSTATUS status = xbox_NtCreateFile(&handle, XBOX_GENERIC_READ, attributes,
        ios, NULL, 0, 1, XBOX_FILE_OPEN, XBOX_FILE_DIRECTORY_FILE);
    if (status != STATUS_SUCCESS || !handle)
        return fail("open directory for metadata failure case", status, ios->Information);
    XBOX_ANSI_STRING name = { .Buffer = (PCHAR)pattern,
        .Length = (USHORT)strlen(pattern), .MaximumLength = (USHORT)strlen(pattern) + 1 };
    unsigned char buffer[sizeof(XBOX_FILE_DIRECTORY_INFORMATION) + 32];
    memset(buffer, 0xA5, sizeof(buffer));
    status = xbox_NtQueryDirectoryFile(handle, NULL, NULL, NULL, ios, buffer,
        sizeof(buffer), XboxFileDirectoryInformation, &name, TRUE);
    int ok = status == STATUS_OBJECT_NAME_NOT_FOUND && ios->Status == status &&
        ios->Information == 0;
    if (!ok)
        fail("dangling directory entry metadata failure", status, ios->Information);
    for (size_t i = 0; ok && i < sizeof(buffer); i++)
        if (buffer[i] != 0xA5) {
            ok = fail("metadata failure modified output", status, ios->Information);
            break;
        }
    xbox_NtClose(handle);
    return ok;
}

typedef struct QueryThread {
    HANDLE handle;
    const char *pattern;
    NTSTATUS status;
    char name[64];
    ULONG name_length;
} QueryThread;

static void *query_thread(void *arg)
{
    QueryThread *test = arg;
    unsigned char buffer[sizeof(XBOX_FILE_DIRECTORY_INFORMATION) + 64];
    XBOX_IO_STATUS_BLOCK ios = {0};
    test->status = (NTSTATUS)query(test->handle, test->pattern, TRUE,
        buffer, sizeof(buffer), &ios);
    XBOX_FILE_DIRECTORY_INFORMATION *entry = (void *)buffer;
    if (test->status == STATUS_SUCCESS && ios.Information >= offsetof(XBOX_FILE_DIRECTORY_INFORMATION, FileName)) {
        test->name_length = entry->FileNameLength;
        if (test->name_length < sizeof(test->name))
            memcpy(test->name, entry->FileName, test->name_length);
    }
    return NULL;
}

static int open_fd_count(void)
{
    DIR *dir = opendir("/proc/self/fd");
    if (!dir) return -1;
    int count = 0;
    while (readdir(dir)) count++;
    closedir(dir);
    return count;
}

int main(void)
{
    char root[] = "/tmp/kernel-directory-posix-XXXXXX";
    char path[512];
    if (!mkdtemp(root)) { perror("mkdtemp"); return 2; }
    snprintf(path, sizeof(path), "%s/alpha.txt", root);
    FILE *file = fopen(path, "wb");
    if (!file) { perror("fopen alpha"); return 2; }
    fclose(file);
    snprintf(path, sizeof(path), "%s/bravo.txt", root);
    file = fopen(path, "wb");
    if (!file) { perror("fopen bravo"); return 2; }
    fclose(file);
    char missing_path[512];
    snprintf(missing_path, sizeof(missing_path), "%s/missing-link", root);
    if (symlink("absent-target", missing_path) != 0) { perror("symlink missing"); return 2; }

    xbox_path_init(root, NULL);
    XBOX_ANSI_STRING directory_name = { .Buffer = "D:\\", .Length = 3, .MaximumLength = 4 };
    XBOX_OBJECT_ATTRIBUTES attributes = {0};
    XBOX_IO_STATUS_BLOCK ios = {0};
    attributes.ObjectName = &directory_name;
    HANDLE handle = NULL;
    NTSTATUS status = xbox_NtCreateFile(&handle, XBOX_GENERIC_READ, &attributes,
        &ios, NULL, 0, 1, XBOX_FILE_OPEN, XBOX_FILE_DIRECTORY_FILE);
    if (status != STATUS_SUCCESS || !handle) {
        fprintf(stderr, "FAIL: create directory handle status=%08X\n", (unsigned)status);
        return 2;
    }
    if (!check_missing_metadata(&attributes, &ios, "missing-link")) {
        xbox_NtClose(handle);
        return 1;
    }

    const ULONG header = (ULONG)offsetof(XBOX_FILE_DIRECTORY_INFORMATION, FileName);
    const ULONG minimum = sizeof(XBOX_FILE_DIRECTORY_INFORMATION);
    int ok = check_boundary(handle, 0) && check_boundary(handle, header - 1) &&
        check_boundary(handle, minimum - 1) && check_long_pattern(handle, minimum);
    /* The fixed header plus a truncated name is returned as overflow; the
     * same entry remains available to a larger retry, then enumeration moves on. */
    if (ok) ok = check_record(handle, minimum, STATUS_BUFFER_OVERFLOW, minimum, 8, TRUE);
    if (ok) ok = check_retry_larger_buffer(handle, minimum);
    if (ok) ok = check_record(handle, header + 9, STATUS_SUCCESS, header + 9, 9, FALSE);
    if (ok) {
        unsigned char buffer[header + 64];
        status = query(handle, "alpha.txt", FALSE, buffer, sizeof(buffer), &ios);
        if (status != STATUS_NO_MORE_FILES || ios.Status != status || ios.Information != 0)
            ok = fail("continuation after complete entry", status, ios.Information);
        if (ok && query(handle, "alpha.txt", FALSE, buffer, sizeof(buffer), &ios) != STATUS_NO_MORE_FILES)
            ok = fail("repeated EOF remains exhausted until restart", ios.Status, ios.Information);
    }
    if (ok) ok = check_record(handle, minimum, STATUS_BUFFER_OVERFLOW, minimum, 8, TRUE);
    if (ok) ok = check_record(handle, header + 9, STATUS_SUCCESS, header + 9, 9, FALSE);
    if (ok) {
        QueryThread a = { .handle = handle, .pattern = "alpha.txt" };
        QueryThread b = { .handle = handle, .pattern = "alpha.txt" };
        pthread_t ta, tb;
        if (pthread_create(&ta, NULL, query_thread, &a) || pthread_create(&tb, NULL, query_thread, &b))
            ok = fail("start concurrent query workers", STATUS_UNSUCCESSFUL, 0);
        else {
            pthread_join(ta, NULL);
            pthread_join(tb, NULL);
            if (a.status != STATUS_SUCCESS || b.status != STATUS_SUCCESS ||
                strcmp(a.name, "alpha.txt") != 0 || strcmp(b.name, "alpha.txt") != 0)
                ok = fail("concurrent serialized queries returned wrong names", STATUS_UNSUCCESSFUL, 0);
        }
    }

    if (ok) {
        unsigned char buffer[sizeof(XBOX_FILE_DIRECTORY_INFORMATION) + 64];
        status = query(handle, "bravo.txt", TRUE, buffer, sizeof(buffer), &ios);
        if (status != STATUS_SUCCESS ||
            ((XBOX_FILE_DIRECTORY_INFORMATION *)buffer)->FileNameLength != 9 ||
            memcmp(((XBOX_FILE_DIRECTORY_INFORMATION *)buffer)->FileName, "alpha.txt", 9) ||
            query(handle, "alpha.txt", FALSE, buffer, sizeof(buffer), &ios) != STATUS_NO_MORE_FILES)
            ok = fail("restart retains the first captured search mask", status, ios.Information);
        int before = open_fd_count();
        status = query(handle, "alpha.txt", TRUE, buffer, minimum, &ios);
        if (before < 0 || status != STATUS_BUFFER_OVERFLOW || open_fd_count() != before + 1)
            ok = fail("partial search owns one host directory stream", status, ios.Information);
        int with_search = open_fd_count();
        xbox_NtClose(handle);
        handle = NULL;
        if (ok && (with_search < 0 || open_fd_count() != before - 1))
            ok = fail("NtClose releases the search stream and host handle", STATUS_UNSUCCESSFUL, 0);
    } else {
        xbox_NtClose(handle);
        handle = NULL;
    }

    if (ok) {
        status = xbox_NtCreateFile(&handle, XBOX_GENERIC_READ, &attributes,
            &ios, NULL, 0, 1, XBOX_FILE_OPEN, XBOX_FILE_DIRECTORY_FILE);
        unsigned char buffer[sizeof(XBOX_FILE_DIRECTORY_INFORMATION) + 64];
        if (status != STATUS_SUCCESS || !handle ||
            query(handle, "bravo.txt", FALSE, buffer, sizeof(buffer), &ios) != STATUS_SUCCESS ||
            ((XBOX_FILE_DIRECTORY_INFORMATION *)buffer)->FileNameLength != 9 ||
            memcmp(((XBOX_FILE_DIRECTORY_INFORMATION *)buffer)->FileName, "bravo.txt", 9))
            ok = fail("reopened host handle starts with a fresh search", ios.Status, ios.Information);
    }
    if (handle) xbox_NtClose(handle);

    if (!ok) return 1;
    unlink(path);
    snprintf(path, sizeof(path), "%s/alpha.txt", root); unlink(path);
    unlink(missing_path);
    rmdir(root);
    puts("PASS: POSIX directory query guards short buffers, truncates names safely, retries and serializes concurrent searches");
    return 0;
}
