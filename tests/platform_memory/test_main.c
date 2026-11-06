#include "win32_compat.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>

#ifndef MEM_MAPPED
#define MEM_MAPPED 0x00040000u
#endif

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-54s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static int query(LPCVOID address, MEMORY_BASIC_INFORMATION *info)
{
    memset(info, 0, sizeof(*info));
    return VirtualQuery(address, info, sizeof(*info)) == sizeof(*info);
}

static int read_faults(const volatile unsigned char *address)
{
    pid_t child = fork();
    if (child == 0) {
        struct rlimit no_core = {0, 0};
        setrlimit(RLIMIT_CORE, &no_core);
        signal(SIGSEGV, SIG_DFL);
        signal(SIGBUS, SIG_DFL);
        volatile unsigned char value = *address;
        (void)value;
        _exit(0);
    }
    if (child < 0) return 0;
    int status = 0;
    if (waitpid(child, &status, 0) != child) return 0;
    return WIFSIGNALED(status);
}

static int write_faults(volatile unsigned char *address)
{
    pid_t child = fork();
    if (child == 0) {
        struct rlimit no_core = {0, 0};
        setrlimit(RLIMIT_CORE, &no_core);
        signal(SIGSEGV, SIG_DFL);
        signal(SIGBUS, SIG_DFL);
        *address = 0x7F;
        _exit(0);
    }
    if (child < 0) return 0;
    int status = 0;
    if (waitpid(child, &status, 0) != child) return 0;
    return WIFSIGNALED(status);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    SIZE_T size = page * 2;
    unsigned char *p = VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_READWRITE);
    check(p != NULL, "reserve-only VirtualAlloc succeeds");
    if (!p) return 1;
    check(((uintptr_t)p % (64 * 1024)) == 0,
          "new reservations use Win32 allocation-granularity alignment");

    MEMORY_BASIC_INFORMATION info;
    check(query(p, &info) && info.AllocationBase == p &&
          info.AllocationProtect == PAGE_READWRITE && info.State == MEM_RESERVE &&
          info.Protect == 0 && info.RegionSize == size,
          "reserved pages report MEM_RESERVE and the allocation protection");
    check(read_faults(p), "reserve-only pages stay inaccessible despite allocation protection");

    check(VirtualAlloc(p, page, MEM_COMMIT, PAGE_READWRITE) == p,
          "commit of an owned reserved page succeeds");
    check(query(p + 1, &info) && info.BaseAddress == p && info.State == MEM_COMMIT &&
          info.Protect == PAGE_READWRITE && info.RegionSize == page,
          "unaligned query resolves the containing page and split region");
    check(query(p + page, &info) && info.State == MEM_RESERVE &&
          info.RegionSize == page,
          "uncommitted neighbor remains reserved");

    p[0] = 0xA5;
    DWORD old = 0;
    check(VirtualProtect(p + 1, 1, PAGE_READONLY, &old) && old == PAGE_READWRITE,
          "unaligned VirtualProtect rounds to the page and returns its old protection");
    check(query(p, &info) && info.Protect == PAGE_READONLY,
          "VirtualQuery reports updated page protection");
    check(!VirtualProtect(p, size, PAGE_READWRITE, &old),
          "VirtualProtect rejects a range containing reserved pages");
    check(query(p, &info) && info.Protect == PAGE_READONLY,
          "failed VirtualProtect leaves prior page protection unchanged");

#if defined(__APPLE__)
    check(!VirtualFree(p, page, MEM_DECOMMIT),
          "Darwin rejects decommit when zero-on-recommit cannot be guaranteed");
    check(query(p, &info) && info.State == MEM_COMMIT && info.Protect == PAGE_READONLY,
          "unsupported Darwin decommit leaves the mapping unchanged");
#else
    check(VirtualFree(p, page, MEM_DECOMMIT), "decommit succeeds");
    check(query(p, &info) && info.State == MEM_RESERVE,
          "decommitted page reports MEM_RESERVE");
    check(VirtualAlloc(p, page, MEM_COMMIT, PAGE_READWRITE) == p && p[0] == 0,
          "recommit succeeds with zero-filled contents");
#endif

    check(VirtualAlloc(p + page, page, MEM_COMMIT, PAGE_READWRITE) == p + page,
          "commit of remaining reserved page succeeds");
    check(query(p, &info) && info.State == MEM_COMMIT && info.RegionSize == size,
          "adjacent matching committed pages coalesce into one query region");
    check(query(p + page, &info) && info.BaseAddress == p + page &&
          info.RegionSize == page,
          "VirtualQuery reports only the forward run from the queried page");

    check(!VirtualFree(p, size, MEM_RELEASE),
          "MEM_RELEASE rejects nonzero size");
    check(query(p, &info) && info.State == MEM_COMMIT,
          "invalid release leaves allocation registered and committed");
    check(VirtualAlloc((char *)p + 1, 1, MEM_COMMIT, PAGE_READWRITE) == p,
          "commit rounds an unaligned address down to its page");
    check(VirtualFree(p, 0, MEM_RELEASE), "zero-size MEM_RELEASE frees whole allocation");
    check(query(p, &info) && info.State == MEM_FREE,
          "released allocation reports MEM_FREE");
    unsigned char *implicit = VirtualAlloc(NULL, page, MEM_COMMIT, PAGE_READWRITE);
    check(implicit != NULL && query(implicit, &info) && info.State == MEM_COMMIT,
          "NULL-address MEM_COMMIT reserves and commits a new range");
    if (implicit) VirtualFree(implicit, 0, MEM_RELEASE);
    check(VirtualAlloc(NULL, page, MEM_RESERVE, PAGE_READWRITE | PAGE_GUARD) == NULL,
          "unsupported page modifiers are rejected");

    check(CreateFileMappingA(NULL, NULL, PAGE_READWRITE, 0, (DWORD)page, NULL) == NULL,
          "file mapping rejects NULL instead of fabricating pagefile backing");
    HANDLE mapping = CreateFileMappingA((HANDLE)(LONG_PTR)-1, NULL, PAGE_READWRITE, 0,
                                         (DWORD)page, NULL);
    check(mapping != NULL, "file mapping object creation succeeds");
    if (mapping) {
        unsigned char *view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, page);
        check(view != NULL, "read-only mapped view creation succeeds");
        if (view) {
            check(query(view, &info) && info.Type == MEM_MAPPED &&
                  info.Protect == PAGE_READONLY && info.State == MEM_COMMIT,
                  "mapped view reports mapped type and access protection");
            check(write_faults(view), "read-only mapped view rejects writes");
            check(UnmapViewOfFile(view), "mapped view unmaps");
        }
        unsigned char *copy = MapViewOfFile(mapping, FILE_MAP_COPY, 0, 0, page);
        check(copy != NULL, "copy-on-write mapped view creation succeeds");
        if (copy) {
            check(query(copy, &info) && info.Protect == PAGE_WRITECOPY &&
                  info.Type == MEM_MAPPED, "copy view reports PAGE_WRITECOPY");
            copy[0] = 0x5A;
            check(UnmapViewOfFile(copy), "copy-on-write view unmaps");
            unsigned char *shared = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, page);
            check(shared != NULL && shared[0] == 0,
                  "copy-on-write view changes do not reach shared mapping");
            if (shared) UnmapViewOfFile(shared);
        }
        CloseHandle(mapping);
    }
    mapping = CreateFileMappingA((HANDLE)(LONG_PTR)-1, NULL, PAGE_READONLY, 0,
                                 (DWORD)page, NULL);
    check(mapping != NULL && MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, page) == NULL,
          "mapping access cannot exceed creation protection");
    if (mapping) CloseHandle(mapping);

    void *allocations[513] = {0};
    for (size_t i = 0; i < 513; i++)
        allocations[i] = VirtualAlloc(NULL, page, MEM_RESERVE, PAGE_NOACCESS);
    check(allocations[511] != NULL && allocations[512] == NULL,
          "registry-full allocation fails instead of silently escaping tracking");
    mapping = CreateFileMappingA((HANDLE)(LONG_PTR)-1, NULL, PAGE_READWRITE, 0,
                                 (DWORD)page, NULL);
    check(mapping != NULL && MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, page) == NULL,
          "registry-full mapped view fails instead of escaping tracking");
    if (mapping) CloseHandle(mapping);
    for (size_t i = 0; i < 512; i++)
        if (allocations[i]) VirtualFree(allocations[i], 0, MEM_RELEASE);
    mapping = CreateFileMappingA((HANDLE)(LONG_PTR)-1, NULL, PAGE_READWRITE, 0,
                                 (DWORD)page, NULL);
    if (mapping) {
        void *view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, page);
        check(view != NULL, "mapping slot is reusable after full-registry rollback");
        if (view) UnmapViewOfFile(view);
        CloseHandle(mapping);
    } else {
        check(0, "mapping slot is reusable after full-registry rollback");
    }

    const size_t granularity = 64 * 1024;
    unsigned char *span = VirtualAlloc(NULL, 3 * granularity, MEM_RESERVE,
                                       PAGE_NOACCESS);
    uint64_t span_id = w32_reservation_id(span);
    check(span != NULL && span_id != 0, "reserved span has a stable allocation ID");
    if (span && span_id) {
        unsigned char *hole = span + granularity;
        check(w32_release_reserved_region(hole, granularity),
              "POSIX carve releases one reserved middle region");
        check(query(span, &info) && info.State == MEM_RESERVE &&
              query(span + 2 * granularity, &info) && info.State == MEM_RESERVE,
              "carve retains tracked prefix and suffix reservations");
        mapping = CreateFileMappingA((HANDLE)(LONG_PTR)-1, NULL, PAGE_READWRITE,
                                     0, (DWORD)page, NULL);
        void *view = mapping ? MapViewOfFileEx(mapping, FILE_MAP_READ, 0, 0,
                                                page, hole) : NULL;
        check(view == hole, "mapped view can occupy an explicitly carved hole");
        check(w32_release_reservation(span_id),
              "allocation-ID cleanup releases remaining reservation fragments");
        check(view && query(view, &info) && info.State == MEM_COMMIT &&
              info.Type == MEM_MAPPED,
              "reservation cleanup leaves a separately mapped hole untouched");
        if (view) UnmapViewOfFile(view);
        if (mapping) CloseHandle(mapping);
    }

    void *full[512] = {0};
    full[0] = VirtualAlloc(NULL, 3 * page, MEM_RESERVE, PAGE_NOACCESS);
    uint64_t full_id = w32_reservation_id(full[0]);
    for (size_t i = 1; i < 512; i++)
        full[i] = VirtualAlloc(NULL, page, MEM_RESERVE, PAGE_NOACCESS);
    int all_registered = full[0] && full_id != 0;
    for (size_t i = 1; i < 512; i++) all_registered &= full[i] != NULL;
    check(all_registered, "carve failure fixture fills the registry exactly");
    if (all_registered) {
        check(!w32_release_reserved_region((char *)full[0] + page, page),
              "split carve fails when registry has no fragment slot");
        check(query(full[0], &info) && info.State == MEM_RESERVE &&
              info.RegionSize == 3 * page,
              "failed full-registry carve leaves original reservation intact");
    }
    if (full_id) w32_release_reservation(full_id);
    for (size_t i = 1; i < 512; i++)
        if (full[i]) VirtualFree(full[i], 0, MEM_RELEASE);

    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
