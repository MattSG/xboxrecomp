/* Canceled DPCs must release queue capacity without disturbing live order. */
#ifndef KERNEL_BRIDGE_SOURCE
#define KERNEL_BRIDGE_SOURCE "../../src/kernel/kernel_bridge.c"
#endif
#include KERNEL_BRIDGE_SOURCE
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "DPC queue line %d: %s\n", __LINE__, #c); ExitProcess(1); } } while (0)
enum { DPC_BASE = 0x20000, ROUTINE = 0x26000 };
static unsigned seen[128], calls;
static void guest_dpc(void)
{
    unsigned id = (BRIDGE_MEM32(g_esp + 4) - DPC_BASE) / 32;
    CHECK(BRIDGE_MEM8(g_fs_base + 0x24) == 2);
    CHECK(BRIDGE_MEM32(g_esp + 12) == id + 100);
    CHECK(BRIDGE_MEM32(g_esp + 16) == id + 200);
    CHECK(!BRIDGE_MEM8(DPC_BASE + id * 32 + 2));
    CHECK(calls < 128); seen[calls++] = id;
    g_esp += 20;
}
recomp_func_t recomp_lookup(uint32_t address) { return address == ROUTINE ? guest_dpc : NULL; }
recomp_func_t recomp_lookup_manual(uint32_t address) { (void)address; return NULL; }
static void insert(unsigned id, unsigned result)
{
    BRIDGE_MEM32(g_esp) = DPC_BASE + id * 32;
    BRIDGE_MEM32(g_esp + 4) = id + 100;
    BRIDGE_MEM32(g_esp + 8) = id + 200;
    bridge_KeInsertQueueDpc(); CHECK(g_eax == result);
}
static void remove_dpc(unsigned id, unsigned result)
{
    BRIDGE_MEM32(g_esp) = DPC_BASE + id * 32;
    bridge_KeRemoveQueueDpc(); CHECK(g_eax == result);
}
int main(int argc, char **argv)
{
    uint8_t *memory = VirtualAlloc(NULL, 16 * 1024 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    unsigned i;
    int saved;
    CHECK(memory && argc == 2);
    g_xbox_mem_offset = (ptrdiff_t)memory; g_fs_base = 0x10000; g_esp = 0x80000;
    for (i = 0; i < 64; i++) BRIDGE_MEM32(DPC_BASE + i * 32 + 12) = ROUTINE;
    saved = xbox_IrqlEnterInterrupt(2);
    if (!strcmp(argv[1], "capacity")) {
        for (i = 0; i < 1000; i++) { insert(0, 1); remove_dpc(0, 1); }
        insert(1, 1);
        xbox_IrqlLeaveInterrupt(saved);
        kernel_drain_dpcs(); CHECK(calls == 1 && seen[0] == 1);
    } else {
        g_dpc_head = g_dpc_tail = XBOX_MAX_PENDING_DPC - 3;
        for (i = 1; i <= 5; i++) insert(i, 1);
        remove_dpc(3, 1); remove_dpc(1, 1); remove_dpc(5, 1);
        remove_dpc(3, 0); insert(6, 1); insert(2, 0);
        xbox_IrqlLeaveInterrupt(saved);
        kernel_drain_dpcs();
        CHECK(calls == 3 && seen[0] == 2 && seen[1] == 4 && seen[2] == 6);
        CHECK(g_dpc_head == g_dpc_tail);
    }
    CHECK(g_esp == 0x80000 && BRIDGE_MEM8(g_fs_base + 0x24) == 0);
    VirtualFree(memory, 0, MEM_RELEASE);
    printf("PASS DPC %s: cancellation releases capacity; live order, arguments and duplicate rejection preserved\n", argv[1]);
    return 0;
}
