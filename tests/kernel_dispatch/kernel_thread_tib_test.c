/* Production KPCR/TLS allocation: DPC-active storage must not alias TLS. */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef MEMORY_LAYOUT_SOURCE
#define MEMORY_LAYOUT_SOURCE "../../src/kernel/xbox_memory_layout.c"
#endif
#include MEMORY_LAYOUT_SOURCE
void *recomp_lookup(ULONG address) { (void)address; abort(); }
void *recomp_lookup_manual(ULONG address) { (void)address; abort(); }
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "thread KPCR line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static uint8_t *bytes(uint32_t va) { return (uint8_t *)((uintptr_t)g_memory_offset + va); }
static uint32_t *word(uint32_t va) { return (uint32_t *)bytes(va); }
int main(int argc, char **argv)
{
    uint8_t *arena = VirtualAlloc(NULL, 32 * 1024 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    uint32_t tib, other, block, context, thread_data;
    unsigned i;
    CHECK(arena && argc == 2);
    g_memory_offset = (ptrdiff_t)arena;
    InitializeCriticalSection(&g_allocator_lock); g_allocator_lock_initialized = 1;
    g_tls_template_va = 0x10000; g_tls_total = 64;
    for (i = 0; i < g_tls_total; i++) bytes(g_tls_template_va)[i] = (uint8_t)(0x60 + i);
    memset(bytes(XBOX_TIB_MAIN), 0xCC, 0x280);
    *word(XBOX_TIB_MAIN + 0x08) = 0x780000;
    *word(XBOX_TIB_MAIN + 0x20) = 0x761000;
    bytes(XBOX_TIB_MAIN)[0x24] = 2; /* creator happens to be in a DPC */
    *word(XBOX_TIB_MAIN + 0x58) = 1;
    tib = xbox_AllocThreadTib(); other = xbox_AllocThreadTib();
    CHECK(tib && other && tib != other);
    context = *word(tib + 0x28); block = *word(context + 0x28); thread_data = *word(block);
    CHECK(*word(tib) == 0xFFFFFFFFu && *word(tib + 0x18) == tib);
    CHECK(*word(tib + 0x04) == block + g_tls_total && *word(tib + 0x08) == 0x780000);
    CHECK(*word(tib + 0x20) == 0x761000);
    CHECK(thread_data == block + g_tls_total && context == thread_data + g_tls_thread_size);
    CHECK(!memcmp(bytes(block + 4), bytes(g_tls_template_va + 4), g_tls_total - 4));
    CHECK(context != *word(other + 0x28));
    if (!strcmp(argv[1], "state")) {
        CHECK(bytes(tib)[0x24] == 0 && *word(tib + 0x58) == 0);
        CHECK(*word(tib + 0x1C) == tib);
        CHECK(bytes(XBOX_TIB_MAIN)[0x24] == 2 && *word(XBOX_TIB_MAIN + 0x58) == 1);
        *word(tib + 0x58) = 1;
        CHECK(*word(other + 0x58) == 0);
    } else {
        uint8_t snapshot[64 + 64 + 0x200];
        CHECK(sizeof snapshot == g_tls_total + g_tls_thread_size + 0x200);
        memcpy(snapshot, bytes(block), sizeof snapshot);
        *word(tib + 0x58) = 1;
        CHECK(!memcmp(snapshot, bytes(block), sizeof snapshot));
        *word(tib + 0x58) = 0;
        CHECK(!memcmp(snapshot, bytes(block), sizeof snapshot));
        CHECK(block >= tib + 0x280); /* KPCR 0x28 prefix + 0x258 KPRCB. */
    }
    xbox_HeapFree(tib); xbox_HeapFree(other);
    CHECK(VirtualFree(arena, 0, MEM_RELEASE));
    printf("PASS thread KPCR %s: independent execution state and nonoverlapping TLS/bootstrap storage\n", argv[1]);
    return 0;
}
