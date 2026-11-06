/* A real exception goes through xbox_CrashReport, which must leave a minidump in
 * CrashDumps beside the executable. The memory layer it reads is stubbed. */
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "guest_state.h"

RECOMP_TLS uint32_t g_eax, g_ecx, g_ebx, g_esp, g_esi, g_edi, g_ebp, g_seh_ebp, g_fs_base;
int xbox_GuestRead(uint32_t va, void *out, size_t size) { (void)va; (void)out; (void)size; return 0; }
size_t xbox_GetMappedSize(void) { return 0; }
ptrdiff_t xbox_GetMemoryOffset(void) { return 0; }
void *xbox_GetMemoryBase(void) { return NULL; }

#include "../../src/kernel/crash_report.c"

#define CHECK(x) do { if (!(x)) { printf("%s:%d: failed: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

int main(void)
{
    wchar_t pattern[MAX_PATH + 32];
    WIN32_FIND_DATAW found;
    HANDLE find;
    DWORD n = GetModuleFileNameW(NULL, pattern, MAX_PATH);
    long long size = 0;
    int dumps = 0;
    while (n && pattern[n - 1] != L'\\') --n;
    pattern[n] = 0;
    wcscat(pattern, L"CrashDumps\\crash-*.dmp");
    /* start clean so a stale dump cannot pass */
    for (find = FindFirstFileW(pattern, &found); find != INVALID_HANDLE_VALUE;) {
        wchar_t path[MAX_PATH + 32];
        wcscpy(path, pattern);
        wcscpy(wcsrchr(path, L'\\') + 1, found.cFileName);
        DeleteFileW(path);
        if (!FindNextFileW(find, &found)) { FindClose(find); break; }
    }
    __try {
        RaiseException(0xE0000001u, 0, 0, NULL);
    } __except (xbox_CrashReport(GetExceptionInformation()), EXCEPTION_EXECUTE_HANDLER) {
    }
    find = FindFirstFileW(pattern, &found);
    CHECK(find != INVALID_HANDLE_VALUE);
    do { ++dumps; size += ((long long)found.nFileSizeHigh << 32) | found.nFileSizeLow; } while (FindNextFileW(find, &found));
    FindClose(find);
    CHECK(dumps == 1);
    CHECK(size > 10000);
    puts("PASS: a crash writes a minidump beside the executable");
    return 0;
}
