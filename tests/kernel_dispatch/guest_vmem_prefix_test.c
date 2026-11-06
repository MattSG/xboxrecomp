/* Real host reservation blockers: retain a usable prefix, never replace an
 * unavailable fixed guest address with an unrelated heap allocation. */
#include "xbox_memory_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include "../../src/kernel/guest_vmem.c"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "VMA prefix line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

int main(void)
{
    const uint32_t span = 3u * 1024u * 1024u, prefix = 2u * 1024u * 1024u;
    const uint32_t lo = GUEST_VMEM_TOP - span;
    uint8_t *host = VirtualAlloc(NULL, span, MEM_RESERVE, PAGE_NOACCESS);
    uint32_t base, size, status, info[7];
    uint32_t *tail, *head;
    CHECK(host && VirtualFree(host, 0, MEM_RELEASE));
    tail = VirtualAlloc(host + prefix, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(tail == (void *)(host + prefix));
    *tail = 0x11223344;
#ifdef _WIN32
    _putenv_s("RECOMP_EXT_VMA", "1");
#else
    setenv("RECOMP_EXT_VMA", "1", 1);
#endif
    CHECK(guest_vmem_init((ptrdiff_t)host - lo, lo - 65536u, lo, 0));
    CHECK(s_top == lo + prefix);
    base = lo; size = 4096;
    CHECK(guest_vmem_allocate(&base, &size, 0x3000, PAGE_READWRITE, &status) && !status);
    *(uint32_t *)host = 0xABCDEF01;
    CHECK(*(uint32_t *)host == 0xABCDEF01 && *tail == 0x11223344);
    base = lo + prefix; size = 4096;
    CHECK(guest_vmem_allocate(&base, &size, 0x2000, PAGE_READWRITE, &status) && status == STATUS_CONFLICTING_ADDRESSES);
    CHECK(base == lo + prefix && guest_vmem_query(base, info) && info[4] == 0x2000);
    guest_vmem_shutdown();
    CHECK(*tail == 0x11223344);

    head = VirtualAlloc(host, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(head == (void *)host);
    *head = 0x55667788;
    CHECK(!guest_vmem_init((ptrdiff_t)host - lo, lo - 65536u, lo, 0));
    base = lo; size = 4096;
    CHECK(guest_vmem_allocate(&base, &size, 0x2000, PAGE_READWRITE, &status) && status == STATUS_CONFLICTING_ADDRESSES);
    CHECK(base == lo && guest_vmem_query(base, info) && info[4] == 0x2000);
    CHECK(guest_vmem_free(&base, &size, 0x8000, &status) && status != 0);
    guest_vmem_shutdown();
    CHECK(*head == 0x55667788 && *tail == 0x11223344);
    CHECK(VirtualFree(head, 0, MEM_RELEASE) && VirtualFree(tail, 0, MEM_RELEASE));
    puts("PASS: partial prefix, blocked fixed requests, truthful query and foreign mapping ownership");
    return 0;
}
