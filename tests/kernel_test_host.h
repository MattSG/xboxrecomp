/*
 * kernel_test_host.h - host helpers shared by the kernel regression harnesses.
 *
 * Tests are written against the Win32 vocabulary (xbox_winnt.h) and narrow
 * host paths. On Windows this header maps the helpers onto the Win32 calls
 * they replace. On POSIX it supplies the few CRT and Win32 calls the tests use
 * that win32_compat does not implement, using the closest POSIX equivalent.
 */
#ifndef KERNEL_TEST_HOST_H
#define KERNEL_TEST_HOST_H

#include "platform/xbox_winnt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

#include <io.h>

#define XT_SEP "\\"
#define xt_stricmp _stricmp

/* Unique empty directory under the user temp path. */
static inline BOOL xt_make_temp_dir(char *dir, size_t bytes)
{
    char temp[MAX_PATH], name[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, temp) || !GetTempFileNameA(temp, "xht", 0, name))
        return FALSE;
    if (bytes <= strlen(name)) return FALSE;
    strcpy(dir, name);
    return DeleteFileA(dir) && CreateDirectoryA(dir, NULL);
}

static inline BOOL xt_make_dir(const char *dir) { return CreateDirectoryA(dir, NULL); }
static inline BOOL xt_remove_dir(const char *dir) { return RemoveDirectoryA(dir); }
static inline BOOL xt_delete_file(const char *path) { return DeleteFileA(path); }
static inline BOOL xt_path_exists(const char *path)
{
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

#else  /* POSIX */

#include <dirent.h>
#include <ftw.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define XT_SEP "/"
#define xt_stricmp strcasecmp

/* io.h descriptor helpers. _dup2 returns 0 on success; dup2 returns the fd. */
static inline int xt_dup2(int from, int to) { return dup2(from, to) < 0 ? -1 : 0; }
#define _dup(fd)    dup(fd)
#define _dup2(a, b) xt_dup2((a), (b))
#define _close(fd)  close(fd)
#define _fileno(f)  fileno(f)
/* C11 Annex K tmpfile_s, which glibc does not provide. */
static inline int xt_tmpfile_s(FILE **f) { *f = tmpfile(); return *f ? 0 : 1; }
#define tmpfile_s(f) xt_tmpfile_s(f)

#ifndef FILE_LIST_DIRECTORY
#define FILE_LIST_DIRECTORY        0x00000001u
#endif
#ifndef FILE_FLAG_BACKUP_SEMANTICS
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000u
#endif

/* Unique empty directory under $TMPDIR (or /tmp). */
static inline BOOL xt_make_temp_dir(char *dir, size_t bytes)
{
    const char *temp = getenv("TMPDIR");
    if (!temp || !temp[0]) temp = "/tmp";
    if (snprintf(dir, bytes, "%s/xht-XXXXXX", temp) >= (int)bytes) return FALSE;
    return mkdtemp(dir) != NULL;
}

#ifndef INVALID_FILE_ATTRIBUTES
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#endif

static int xt_remove_entry(const char *path, const struct stat *st, int flag,
                           struct FTW *ftw)
{
    (void)st; (void)flag; (void)ftw;
    return remove(path);
}

/* Removes a directory tree the test created. POSIX only; Windows tests keep
 * their explicit cleanup. */
#ifndef FTW_PHYS
#define FTW_PHYS  1   /* glibc values; <ftw.h> hides them under strict modes */
#endif
#ifndef FTW_DEPTH
#define FTW_DEPTH 2
#endif

static inline void xt_remove_tree(const char *dir)
{
    nftw(dir, xt_remove_entry, 16, FTW_DEPTH | FTW_PHYS);
}

static inline BOOL xt_make_dir(const char *dir) { return mkdir(dir, 0755) == 0; }
static inline BOOL xt_remove_dir(const char *dir) { return rmdir(dir) == 0; }
static inline BOOL xt_delete_file(const char *path) { return unlink(path) == 0; }
static inline BOOL xt_path_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* SetEnvironmentVariableA(name, NULL) removes the variable, as on Windows. */
static inline BOOL SetEnvironmentVariableA(LPCSTR name, LPCSTR value)
{
    return value ? setenv(name, value, 1) == 0 : unsetenv(name) == 0;
}

/* win32_compat handles are opaque objects with no inheritance flags. The test
 * only uses this as a presence check; its identity assertions check validity. */
static inline BOOL GetHandleInformation(HANDLE h, DWORD *flags)
{
    if (!h || h == INVALID_HANDLE_VALUE) return FALSE;
    *flags = 0;
    return TRUE;
}

/* Open descriptors of this process, the POSIX analogue of the handle count.
 * Events and other compat objects are not descriptors, so tests that compare
 * counts across them are Windows-only. */
static inline BOOL GetProcessHandleCount(HANDLE process, DWORD *count)
{
    DIR *dir;
    struct dirent *entry;
    DWORD n = 0;
    (void)process;
    dir = opendir("/proc/self/fd");
    if (!dir) return FALSE;
    while ((entry = readdir(dir)) != NULL)
        if (entry->d_name[0] != '.') n++;
    closedir(dir);
    *count = n - 1;   /* the descriptor opened for this listing */
    return TRUE;
}

#endif /* _WIN32 */

/* Joins a directory and a leaf with the host separator. */
static inline int xt_join(char *out, size_t bytes, const char *dir, const char *leaf)
{
    return snprintf(out, bytes, "%s" XT_SEP "%s", dir, leaf);
}

/* Creates a new file holding exactly `bytes` bytes of `data`. */
static inline BOOL xt_write_file(const char *path, const void *data, size_t bytes)
{
    FILE *f = fopen(path, "wb");
    size_t n;
    if (!f) return FALSE;
    n = fwrite(data, 1, bytes, f);
    return fclose(f) == 0 && n == bytes;
}

/* Reads up to `cap` bytes; returns the count, or -1 if the file cannot be read. */
static inline long xt_read_file(const char *path, void *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f) return -1;
    n = fread(buf, 1, cap, f);
    fclose(f);
    return (long)n;
}

#endif /* KERNEL_TEST_HOST_H */
