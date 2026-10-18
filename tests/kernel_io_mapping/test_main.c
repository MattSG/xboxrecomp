#include "kernel.h"
#include "xbox_memory_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "host/xbox_host.h"
#include "nv2a/nv2a_mmio_hook.h"
#include "nv2a/nv2a_state.h"
#else
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>
#endif
typedef void (*guest_fn)(void);
guest_fn recomp_lookup(uint32_t address) { (void)address; abort(); }
guest_fn recomp_lookup_manual(uint32_t address) { (void)address; abort(); }
extern guest_fn recomp_lookup_kernel(uint32_t);
extern RECOMP_TLS uint32_t g_eax, g_esp;
static int failed;
static volatile long map_shutdown_stop, map_shutdown_calls;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); failed++; } } while (0)
static void *native(uint32_t va) { return (void *)((uintptr_t)va + xbox_GetMemoryOffset()); }
static uint32_t guest(const void *p) { return (uint32_t)((uintptr_t)p - xbox_GetMemoryOffset()); }

static int write_faults(volatile uint32_t *p)
{
#ifdef _WIN32
    __try { *p = 42; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return 1; }
    return 0;
#else
    pid_t child = fork();
    int status;
    if (!child) {
        struct rlimit no_core = {0, 0}; setrlimit(RLIMIT_CORE, &no_core);
        *p = 42; _exit(0);
    }
    if (child < 0 || waitpid(child, &status, 0) != child) return 0;
    return WIFSIGNALED(status);
#endif
}

#ifdef _WIN32
static DWORD WINAPI map_during_shutdown(LPVOID unused)
#else
static void *map_during_shutdown(void *unused)
#endif
{
    (void)unused;
    while (
#ifdef _WIN32
        !InterlockedCompareExchange(&map_shutdown_stop, 0, 0)
#else
        !__sync_fetch_and_add(&map_shutdown_stop, 0)
#endif
    ) {
        void *p = xbox_MapPhysicalMemory(0x12000, 32, PAGE_READWRITE);
#ifdef _WIN32
        InterlockedIncrement(&map_shutdown_calls);
#else
        __sync_add_and_fetch(&map_shutdown_calls, 1);
#endif
        if (p) xbox_UnmapPhysicalMemory(p, 32);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

int main(int argc, char **argv)
{
    FILE *file;
    long length;
    unsigned char *xbe;
    volatile uint32_t *ram, *a, *b, *ro, *device, *foreign, *dpc_rw, *dpc_ro, *mmio_buffer;
    uint32_t physical, protect, scratch, *stack, va;
    void *contig_probe;
    if (argc != 2 || !(file = fopen(argv[1], "rb"))) return 10;
    fseek(file, 0, SEEK_END); length = ftell(file); rewind(file);
    xbe = malloc((size_t)length);
    if (!xbe || fread(xbe, 1, (size_t)length, file) != (size_t)length) return 11;
    fclose(file);
#ifdef _WIN32
    _putenv_s("RECOMP_AC97_READY", "1");
#endif
    if (!xbox_MemoryLayoutInit(xbe, (size_t)length)) return 12;
    free(xbe);
    contig_probe = native(0x80001000u);
    ram = native(0x80011234u);
    foreign = VirtualAlloc(native(0xe8000000u), 0x10000,
                            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(foreign != NULL);
    if (!foreign) return 13;
    *foreign = 0xcafef00d;
    a = xbox_MmMapIoSpace(0x11234, 32, PAGE_READWRITE);
    b = xbox_MmMapIoSpace(0x11234, 32, PAGE_READWRITE);
    ro = xbox_MmMapIoSpace(0x11234, 32, PAGE_READONLY);
    CHECK(a && b && ro && a != b && a != ro);
    CHECK(a != foreign && *foreign == 0xcafef00d);
    if (!a || !b || !ro) return 13;
    *ram = 0x12345678; CHECK(*a == *ram && *b == *ram && *ro == *ram);
    *a = 0x87654321; CHECK(*ram == *a && *b == *a);
    CHECK(xbox_MmGetPhysicalAddress((PVOID)a) == 0x11234);
    CHECK(xbox_MmGetPhysicalAddress((PVOID)(uintptr_t)guest((PVOID)a)) == 0);
    CHECK(xbox_MmGetPhysicalAddress(native(0x11234)) == 0);
    CHECK(xbox_MmQueryAddressProtect((PVOID)ro) == PAGE_READONLY);
    CHECK(write_faults(ro) && *ram == 0x87654321);
    xbox_MmSetAddressProtect((PVOID)a, 4, PAGE_READONLY);
    CHECK(xbox_MmQueryAddressProtect((PVOID)a) == PAGE_READONLY && write_faults(a));
    CHECK(xbox_MmQueryAddressProtect((PVOID)b) == PAGE_READWRITE);
    xbox_MmSetAddressProtect((PVOID)a, 4, PAGE_READWRITE);
    CHECK(xbox_MmQueryAddressProtect((PVOID)a) == PAGE_READWRITE);
    xbox_MmUnmapIoSpace((PVOID)ram, 32); /* not owned: canonical memory survives */
    xbox_MmUnmapIoSpace((PVOID)foreign, 0x10000);
    CHECK(*foreign == 0xcafef00d);
    xbox_MmUnmapIoSpace((PVOID)a, 31); /* wrong extent cannot release a mapping */
    CHECK(*a == *ram);
    xbox_MmUnmapIoSpace((PVOID)a, 32);
    CHECK(!xbox_IoMappingQuery((PVOID)a, NULL, NULL, NULL));
    CHECK(*b == *ram && *ro == *ram);
    device = xbox_MmMapIoSpace(0xfd001234, 32, PAGE_READWRITE | 0x200);
    CHECK(device != NULL);
    if (device) {
        *device = 0x13579bdf;
        CHECK(*(volatile uint32_t *)native(0xfd001234) == *device);
        *(volatile uint32_t *)native(0xfd001234) = 0x2468ace0;
        CHECK(*device == 0x2468ace0);
        CHECK(xbox_IoMappingQuery((PVOID)device, &physical, &protect, NULL));
        CHECK(physical == 0xfd001234 && protect == (PAGE_READWRITE | 0x200));
        xbox_MmUnmapIoSpace((PVOID)device, 32);
    }
    CHECK(!xbox_MmMapIoSpace(0xffffffff, 2, PAGE_READWRITE));
    CHECK(!xbox_MmMapIoSpace(0x03fffff0, 32, PAGE_READWRITE));
    CHECK(!xbox_MmMapIoSpace(0x1234, 0, PAGE_READWRITE));
    CHECK(!xbox_MmMapIoSpace(0x1234, 4, PAGE_EXECUTE_READ));
    CHECK(!xbox_MmMapIoSpace(0x1234, 4, PAGE_READWRITE | 0x100));
    {
        volatile uint32_t *pages = xbox_MmMapIoSpace(0x30000, 8192, PAGE_READWRITE);
        CHECK(pages != NULL);
        if (pages) {
            xbox_MmSetAddressProtect((PVOID)(pages + 1024), 4, PAGE_READONLY);
            CHECK(xbox_MmQueryAddressProtect((PVOID)pages) == PAGE_READWRITE);
            CHECK(xbox_MmQueryAddressProtect((PVOID)(pages + 1024)) == PAGE_READONLY);
            *pages = 0x10203040;
            CHECK(*(volatile uint32_t *)native(0x80030000) == 0x10203040);
            CHECK(write_faults(pages + 1024));
            xbox_MmUnmapIoSpace((PVOID)pages, 8192);
        }
    }

    /* Both public dispatch paths use the same physical mapping owner. */
    scratch = xbox_HeapAlloc(0x4000, 4096);
    CHECK(scratch != 0);
    *(uint32_t *)native(scratch) = 0x800000b1u; /* ordinal177 */
    *(uint32_t *)native(scratch + 4) = 0x800000b7u; /* ordinal183 */
    *(uint32_t *)native(scratch + 8) = 0x800000adu; /* ordinal173 */
    *(uint32_t *)native(scratch + 12) = 0x8000006bu; /* ordinal107 */
    xbox_kernel_set_thunk_address(scratch, 4); xbox_kernel_bridge_init();
    stack = native(scratch + 0x1000);
    stack[0] = 0x12345678; stack[1] = 0x11234; stack[2] = 32; stack[3] = PAGE_READWRITE;
    g_esp = scratch + 0x1000;
    recomp_lookup_kernel(*(uint32_t *)native(scratch))(); va = g_eax;
    CHECK(va && g_esp == scratch + 0x1010);
    CHECK(*(volatile uint32_t *)native(va) == *ram);
    stack[0] = 0x12345678; stack[1] = va; stack[2] = 32;
    g_esp = scratch + 0x1000;
    recomp_lookup_kernel(*(uint32_t *)native(scratch + 4))();
    CHECK(g_esp == scratch + 0x100c && !xbox_IoMappingQuery(native(va), NULL, NULL, NULL));
    stack[0] = 0x12345678; stack[1] = guest((PVOID)b); g_esp = scratch + 0x1000;
    recomp_lookup_kernel(*(uint32_t *)native(scratch + 8))();
    CHECK(g_eax == 0x11234 && g_esp == scratch + 0x1008);
    stack[0] = 0x12345678; stack[1] = 0x00011234; g_esp = scratch + 0x1000;
    recomp_lookup_kernel(*(uint32_t *)native(scratch + 8))();
    CHECK(g_eax == 0 && g_esp == scratch + 0x1008);
    dpc_rw = xbox_MmMapIoSpace(0x30000, 32, PAGE_READWRITE);
    dpc_ro = xbox_MmMapIoSpace(0x31000, 32, PAGE_READONLY);
    CHECK(dpc_rw && dpc_ro);
    if (!dpc_rw || !dpc_ro) return 14;
    stack[0] = 0x12345678; stack[1] = guest((PVOID)dpc_rw);
    stack[2] = 0x12345678; stack[3] = 0xabcdef01;
    g_esp = scratch + 0x1000;
    recomp_lookup_kernel(*(uint32_t *)native(scratch + 12))();
    CHECK(g_esp == scratch + 0x1010);
    CHECK(*(uint16_t *)dpc_rw == 0x13 && *(uint32_t *)((char *)dpc_rw + 12) == 0x12345678);
    stack[0] = 0x12345678; stack[1] = guest((PVOID)dpc_ro);
    stack[2] = 0x12345678; stack[3] = 0xabcdef01;
    g_esp = scratch + 0x1000;
    recomp_lookup_kernel(*(uint32_t *)native(scratch + 12))();
    CHECK(*(uint16_t *)dpc_ro == 0);
    xbox_MmUnmapIoSpace((PVOID)dpc_rw, 32);
    xbox_MmUnmapIoSpace((PVOID)dpc_ro, 32);
    mmio_buffer = xbox_MmMapIoSpace(0xfd001234, 32, PAGE_READWRITE);
    CHECK(mmio_buffer != NULL);
    if (!mmio_buffer) return 15;
    *mmio_buffer = 0xfeedcafe;
    stack[0] = 0x12345678; stack[1] = guest((PVOID)mmio_buffer);
    stack[2] = 0x12345678; stack[3] = 0xabcdef01;
    g_esp = scratch + 0x1000;
    recomp_lookup_kernel(*(uint32_t *)native(scratch + 12))();
    CHECK(*mmio_buffer == 0xfeedcafe);
    xbox_MmUnmapIoSpace((PVOID)mmio_buffer, 32);
#ifdef _WIN32
    /* A trapped alias reaches the real NV2A model, not the aperture's RAM. */
    nv2a_hook_init(xbox_GetMemoryOffset());
    CHECK(xbox_HostHardwareInit());
    device = xbox_MmMapIoSpace(0xfd008900, 4, PAGE_READWRITE);
    CHECK(device != NULL);
    if (device) {
        *device = 0x12345678;
        CHECK(nv2a_mmio_read(nv2a_get_state(), 0x8900, 4) == 0x12345678);
        CHECK(*device == 0x12345678);
        xbox_MmSetAddressProtect((PVOID)device, 4, PAGE_READONLY);
        CHECK(write_faults(device));
        CHECK(xbox_MmQueryAddressProtect((PVOID)device) == PAGE_READONLY);
        xbox_MmSetAddressProtect((PVOID)device, 4, PAGE_READWRITE);
        *device = 0x12345678; /* restoring access must retain the PVIDEO trap */
        CHECK(xbox_IoMappingResolve(guest((PVOID)device), 1, &physical) == 1 && physical == 0xfd008900);
        {
            volatile uint32_t *read_only = xbox_MmMapIoSpace(0xfd008900, 4, PAGE_READONLY);
            CHECK(read_only != NULL);
            if (read_only) {
                CHECK(*read_only == 0x12345678);
                CHECK(write_faults(read_only));
                CHECK(*device == 0x12345678);
                xbox_MmUnmapIoSpace((PVOID)read_only, 4);
            }
        }
        xbox_MmUnmapIoSpace((PVOID)device, 4);
    }
    {
        volatile unsigned char *ac97 = xbox_MmMapIoSpace(0xfec0010b, 1, PAGE_READWRITE);
        CHECK(ac97 != NULL);
        if (ac97) {
            *ac97 = 3; /* Reset clears immediately; run/pause remains set. */
            CHECK(*ac97 == 1 && *(volatile unsigned char *)native(0xfec0010b) == 1);
            *ac97 = 2; CHECK(*ac97 == 0); /* alias page was re-armed */
            xbox_MmUnmapIoSpace((PVOID)ac97, 1);
        }
    }
    xbox_HostHardwareShutdown();
#endif
    xbox_MmUnmapIoSpace((PVOID)b, 32); xbox_MmUnmapIoSpace((PVOID)ro, 32);
    CHECK(*ram == 0x87654321);
    CHECK(*foreign == 0xcafef00d);
    VirtualFree((PVOID)foreign, 0, MEM_RELEASE);
#ifdef _WIN32
    {
        HANDLE thread = CreateThread(NULL, 0, map_during_shutdown, NULL, 0, NULL);
        CHECK(thread != NULL);
        if (thread) {
            while (InterlockedCompareExchange(&map_shutdown_calls, 0, 0) < 100) Sleep(1);
            xbox_MemoryLayoutShutdown();
            InterlockedExchange(&map_shutdown_stop, 1);
            WaitForSingleObject(thread, INFINITE);
            CloseHandle(thread);
        } else xbox_MemoryLayoutShutdown();
    }
#else
    {
        pthread_t thread;
        if (pthread_create(&thread, NULL, map_during_shutdown, NULL) == 0) {
            while (__sync_fetch_and_add(&map_shutdown_calls, 0) < 100) sched_yield();
            xbox_MemoryLayoutShutdown();
            __sync_lock_test_and_set(&map_shutdown_stop, 1);
            pthread_join(thread, NULL);
        } else {
            CHECK(0);
            xbox_MemoryLayoutShutdown();
        }
    }
#endif
    CHECK(xbox_MapPhysicalMemory(0x12000, 32, PAGE_READWRITE) == NULL);
    CHECK(xbox_MmGetPhysicalAddress(contig_probe) == 0);
    xbox_MemoryLayoutShutdown();
    printf("I/O mapping regressions: %d failures\n", failed);
    return failed != 0;
}
