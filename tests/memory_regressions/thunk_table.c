/* Synthetic XBE import-table selection, bounds and exact patch ownership. */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void *recomp_lookup(ULONG address) { (void)address; return NULL; }
void *recomp_lookup_manual(ULONG address) { (void)address; return NULL; }
static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "thunk line %d: %s\n", __LINE__, #c); failures++; } } while (0)
static void put(uint8_t *bytes, unsigned offset, uint32_t value)
{ memcpy(bytes + offset, &value, 4); }
static uint32_t *word(uint32_t va)
{ return (uint32_t *)((uintptr_t)xbox_GetMemoryOffset() + va); }
int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "high-retail";
    int high = !strcmp(mode, "high-retail") || !strcmp(mode, "high-debug");
    int invalid = !strcmp(mode,"partial") || !strcmp(mode,"unterminated") ||
                  !strcmp(mode,"bad-ordinal") || !strcmp(mode,"bad-marker");
    uint32_t data_va = high ? 0x04010000u : 0x30000u;
    uint32_t table_va = data_va + 64, actual_base, actual_count;
    uint32_t expected = 2;
    uint8_t *xbe = calloc(1, 0x3000);
    if (!xbe) return 2;
    memcpy(xbe, "XBEH", 4);
    put(xbe,0x104,0x10000); put(xbe,0x108,0x1000);
    put(xbe,0x11c,2); put(xbe,0x120,0x10200); put(xbe,0x130,4096);
    put(xbe,0x200,4); put(xbe,0x204,0x20000); put(xbe,0x208,4096);
    put(xbe,0x20c,0x1000); put(xbe,0x210,4096);
    put(xbe,0x238,2); put(xbe,0x23c,data_va); put(xbe,0x240,4096);
    put(xbe,0x244,0x2000); put(xbe,0x248,4096);
    put(xbe,0x203c,0x80000067); /* data outside the selected table */
    put(xbe,0x2040,0x80000067); put(xbe,0x2044,0x80000097);
    if (!strcmp(mode,"empty")) { put(xbe,0x2040,0); expected=0; }
    if (!strcmp(mode,"none") || !strcmp(mode,"encoded-none")) expected=0;
    if (!strcmp(mode,"partial")) table_va=0x08000000;
    if (!strcmp(mode,"bad-ordinal")) put(xbe,0x2040,0x80000000u);
    if (!strcmp(mode,"remapped")) put(xbe,0x2040,0x800001ff);
    if (!strcmp(mode,"bad-marker")) put(xbe,0x2040,103);
    if (!strcmp(mode,"unterminated") || !strcmp(mode,"maximum")) {
        for (unsigned i=0;i<XBOX_KERNEL_THUNK_TABLE_SIZE+1;i++) put(xbe,0x2040+i*4,0x80000067);
        if (!strcmp(mode,"maximum")) { put(xbe,0x2040+XBOX_KERNEL_THUNK_TABLE_SIZE*4,0); expected=XBOX_KERNEL_THUNK_TABLE_SIZE; }
    }
    if (!strcmp(mode,"sparse")) { put(xbe,0x2044,0); put(xbe,0x2048,0x80000097); expected=1; }
    put(xbe,0x158,!strcmp(mode,"none") ? 0 : table_va ^
        (!strcmp(mode,"high-debug") ? 0xEFB1F152u : 0x5B6D40B6u));
    if (!strcmp(mode,"encoded-none")) put(xbe,0x158,0x5B6D40B6u);
    xbox_SetTotalRam(XBOX_DEVKIT_RAM);
    xbox_kernel_set_thunk_address(0x60000,3); /* previous image metadata */
    BOOL loaded = xbox_MemoryLayoutInit(xbe,0x3000);
    CHECK(loaded == !invalid);
    xbox_kernel_get_thunk_address(&actual_base,&actual_count);
    if (invalid) {
        CHECK(!xbox_GetMemoryBase() && !actual_base && !actual_count);
        goto done;
    }
    if (!loaded) goto done;
    CHECK(actual_base == (expected ? table_va : 0) && actual_count == expected);
    if (!strcmp(mode,"sparse")) {
        /* A port may explicitly select a bounded span containing NULL gaps. */
        xbox_kernel_set_thunk_address(table_va,3);
        xbox_kernel_init();
        CHECK(xbox_kernel_thunk_table[0] && xbox_kernel_thunk_table[2]);
        xbox_kernel_bridge_init();
        CHECK(*word(table_va) == 0xFE000000 && !*word(table_va+4) &&
              *word(table_va+8) == 0xFE000008);
        CHECK(*word(table_va-4) == 0x80000067);
        xbox_kernel_shutdown();
    }
    if (!strcmp(mode,"remapped")) {
        unsigned short map[512] = {0};
        extern void (*recomp_lookup_kernel(uint32_t))(void);
        extern RECOMP_TLS uint32_t g_eax, g_esp;
        map[511] = 103; /* noncanonical source ordinal -> KeGetCurrentIrql */
        xbox_kernel_set_ordinal_remap(map,512);
        xbox_kernel_bridge_init();
        void (*call)(void) = recomp_lookup_kernel(*word(table_va));
        uint32_t sp = XBOX_STACK_TOP;
        *word(sp) = 0;
        g_esp=sp; g_eax=0xdeadbeef;
        CHECK(call != NULL);
        if (call) { call(); CHECK(g_eax == 0 && g_esp == sp+4); }
        xbox_kernel_set_ordinal_remap(NULL,0);
    }
    xbox_MemoryLayoutShutdown();
    xbox_kernel_get_thunk_address(&actual_base,&actual_count);
    CHECK(!actual_base && !actual_count);
    CHECK(xbox_MemoryLayoutInit(xbe,0x3000));
    if (xbox_GetMemoryBase()) {
        xbox_kernel_set_thunk_address(0,0);
        xbox_kernel_get_thunk_address(&actual_base,&actual_count);
        CHECK(!actual_base && !actual_count);
        xbox_kernel_set_thunk_address(table_va,XBOX_KERNEL_THUNK_TABLE_SIZE+1);
        xbox_kernel_get_thunk_address(&actual_base,&actual_count);
        CHECK(!actual_base && !actual_count);
        const uint32_t bad[][2] = {
            {table_va+1,1}, {0x07fffffc,2}, {0xfffffffc,2},
            {0x0000fffc,1}, {table_va,0}
        };
        for (unsigned i=0;i<sizeof bad/sizeof bad[0];++i) {
            xbox_kernel_set_thunk_address(table_va,1);
            xbox_kernel_set_thunk_address(bad[i][0],bad[i][1]);
            xbox_kernel_get_thunk_address(&actual_base,&actual_count);
            CHECK(!actual_base && !actual_count);
        }
    }
done:
    if (xbox_GetMemoryBase()) xbox_MemoryLayoutShutdown();
    free(xbe);
    if (!failures) printf("PASS thunk %s\n",mode);
    return failures ? 1 : 0;
}
