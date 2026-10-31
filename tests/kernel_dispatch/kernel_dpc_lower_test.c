/* Queued completions must precede object destruction after KfLowerIrql. */
#ifndef KERNEL_BRIDGE_SOURCE
#define KERNEL_BRIDGE_SOURCE "../../src/kernel/kernel_bridge.c"
#endif
#define xbox_HeapAlloc test_dpc_heap_alloc
#define xbox_HeapFree test_dpc_heap_free
#include KERNEL_BRIDGE_SOURCE
#undef xbox_HeapAlloc
#undef xbox_HeapFree
#include <fenv.h>
#include <xmmintrin.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "DPC lowering check failed at line %d: %s\n", __LINE__, #c); \
    ExitProcess(1); \
} } while (0)

enum { DPC = 0x20000, HEAD = 0x22000, VOICE = 0x24000,
       ROUTINE = 0x26000, LIVE_VTABLE = 0x28000, DEAD_VTABLE = 0x28004,
       RETURN_PC = 0x2A000 };
static uint8_t *memory;
static int callbacks, max_depth;
static RECOMP_TLS int callback_depth;
static const char *mode;
static RECOMP_TLS DWORD lowering_thread;
static LONG fallback_allocations, fallback_frees;
static uint32_t outer_callback_stack;
static uint32_t callback_stack;
static unsigned char caller_stack[256];

/* Keep the focused harness's memory arena independent of heap initialization;
 * production still allocates from xbox_HeapAlloc on worker-pool exhaustion. */
uint32_t test_dpc_heap_alloc(uint32_t size, uint32_t alignment)
{
    CHECK(size == XBOX_WORKER_STACK_SIZE && alignment == 4096);
    LONG index = InterlockedIncrement(&fallback_allocations) - 1;
    return 0xC00000 + (uint32_t)index * XBOX_WORKER_STACK_SIZE;
}
void test_dpc_heap_free(uint32_t address)
{
    CHECK(address >= 0xC00000 && address < 0x1000000);
    InterlockedIncrement(&fallback_frees);
}
static DWORD WINAPI parallel_lower(LPVOID unused);

/* Avoid depending on declarations introduced by the repair, so the same
 * regression compiles against a saved before-source. */
extern RECOMP_TLS uint32_t g_ebp;
extern RECOMP_TLS int g_df, g_fp_top, g_fp_cmp;
extern RECOMP_TLS uint16_t g_fp_control_word, g_fp_cc;
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS RecompMmx g_mm0, g_mm1, g_mm2, g_mm3;
extern RECOMP_TLS RecompMmx g_mm4, g_mm5, g_mm6, g_mm7;
extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;

typedef struct {
    uint32_t regs[10];
    RecompMmx mm[8];
    RecompXmm xmm[8];
    double fp[8];
    int df, fp_top, fp_cmp, host_rounding;
    uint16_t control, cc;
    unsigned mxcsr;
} State;

static void snapshot(State *s)
{
    memset(s, 0, sizeof(*s));
    s->regs[0] = g_ecx; s->regs[1] = g_edx; s->regs[2] = g_esp;
    s->regs[3] = g_ebx; s->regs[4] = g_esi; s->regs[5] = g_edi;
    s->regs[6] = g_ebp; s->regs[7] = g_seh_ebp;
    s->regs[8] = g_fs_base; s->regs[9] = g_xbox_kernel_caller;
    s->mm[0] = g_mm0; s->mm[1] = g_mm1; s->mm[2] = g_mm2; s->mm[3] = g_mm3;
    s->mm[4] = g_mm4; s->mm[5] = g_mm5; s->mm[6] = g_mm6; s->mm[7] = g_mm7;
    s->xmm[0] = g_xmm0; s->xmm[1] = g_xmm1; s->xmm[2] = g_xmm2; s->xmm[3] = g_xmm3;
    s->xmm[4] = g_xmm4; s->xmm[5] = g_xmm5; s->xmm[6] = g_xmm6; s->xmm[7] = g_xmm7;
    memcpy(s->fp, g_fp_stack, sizeof(s->fp));
    s->df = g_df; s->fp_top = g_fp_top; s->fp_cmp = g_fp_cmp;
    s->control = g_fp_control_word; s->cc = g_fp_cc;
    s->host_rounding = fegetround(); s->mxcsr = _mm_getcsr();
}

static void queue(void)
{
    uint32_t saved_esp = g_esp;
    g_esp -= 12;
    BRIDGE_MEM32(g_esp) = DPC;
    BRIDGE_MEM32(g_esp + 4) = 0x1234;
    BRIDGE_MEM32(g_esp + 8) = 0x5678;
    bridge_KeInsertQueueDpc();
    CHECK(g_eax == 1);
    g_esp = saved_esp;
}

static void clobber_state(void)
{
    g_eax = g_ecx = g_edx = g_ebx = g_esi = g_edi = 0xDEADBEEF;
    g_ebp = 0xDDDDDDDD; g_seh_ebp = 0xEEEEEEEE;
    g_xbox_kernel_caller = 0xBADCA11;
    memset(g_fp_stack, 0xBA, sizeof(g_fp_stack));
    g_fp_top = 7; g_fp_cmp = -1; g_fp_control_word = 0x77F; g_fp_cc = 0x100;
    g_mm0.q = g_mm1.q = g_mm2.q = g_mm3.q = 0xBAADF00DDEADBEEFull;
    g_mm4 = g_mm5 = g_mm6 = g_mm7 = g_mm0;
    g_xmm0.q[0] = 0xBBBBBBBBBBBBBBBBull; g_xmm0.q[1] = 0xCCCCCCCCCCCCCCCCull;
    g_xmm1 = g_xmm2 = g_xmm3 = g_xmm4 = g_xmm5 = g_xmm6 = g_xmm7 = g_xmm0;
    g_df = 1;
    CHECK(fesetround(FE_TOWARDZERO) == 0);
    _mm_setcsr(_mm_getcsr() ^ 0x8040u); /* FTZ and DAZ must also survive. */
}

static void guest_dpc(void)
{
    CHECK(GetCurrentThreadId() == lowering_thread);
    CHECK(BRIDGE_MEM8(g_fs_base + XBOX_KPCR_IRQL_OFFSET) == 2);
    CHECK(BRIDGE_MEM32(g_esp) == 0);
    CHECK(BRIDGE_MEM32(g_esp + 4) == DPC);
    CHECK(BRIDGE_MEM32(g_esp + 8) == VOICE);
    CHECK(BRIDGE_MEM32(g_esp + 12) == 0x1234);
    CHECK(BRIDGE_MEM32(g_esp + 16) == 0x5678);
    CHECK(BRIDGE_MEM8(DPC + 2) == 0);
    if (!strcmp(mode, "stack") || !strcmp(mode, "exhausted")) {
        /* Match the observed USB alias: the request lies 0x54 bytes below
         * the lowering caller's ESP, and a callback pushes a return PC there. */
        uint32_t saved_esp = g_esp;
        for (unsigned i = 0; i < 48; i++) {
            g_esp -= 4;
            BRIDGE_MEM32(g_esp) = RETURN_PC;
        }
        g_esp = saved_esp;
        if (BRIDGE_MEM32(0x80000 - 0x54 + 8) != 0x5A5A5A5A)
            fprintf(stderr, "USB-shaped request callback overwritten: %08X\n",
                    BRIDGE_MEM32(0x80000 - 0x54 + 8));
        CHECK(!memcmp(memory + 0x80000 - sizeof(caller_stack),
                      caller_stack, sizeof(caller_stack)));
        if (!strcmp(mode, "stack")) {
            if (callback_stack) CHECK(callback_stack == g_esp);
            callback_stack = g_esp;
        }
    }
    callbacks++;
    callback_depth++;
    if (callback_depth > max_depth) max_depth = callback_depth;

    if (!strcmp(mode, "context") || !strcmp(mode, "active")) {
        CHECK(BRIDGE_MEM8(g_fs_base + XBOX_KPCR_DPC_ACTIVE_OFFSET) == 1);
        bridge_KeIsExecutingDpc();
        CHECK(g_eax == 1);
    }
    if (!strcmp(mode, "lifetime")) {
        /* Match DirectSound's intrusive +0x54 completion link and the vtable
         * transition at the end of its base destructor. */
        uint32_t entry = BRIDGE_MEM32(HEAD);
        CHECK(entry == VOICE + 0x54);
        CHECK(BRIDGE_MEM32(entry - 0x54) == LIVE_VTABLE);
        BRIDGE_MEM32(HEAD) = BRIDGE_MEM32(HEAD + 4) = HEAD;
        BRIDGE_MEM32(entry) = BRIDGE_MEM32(entry + 4) = entry;
    } else if (!strcmp(mode, "context")) {
        CHECK(g_df == 0);
        clobber_state();
    } else if (!strcmp(mode, "concurrent") && callbacks == 1) {
        outer_callback_stack = g_esp;
        BRIDGE_MEM32(g_esp - 64) = 0x12345678;
        xbox_KfLowerIrql(0);
        HANDLE thread = CreateThread(NULL, 0, parallel_lower, NULL, 0, NULL);
        CHECK(thread && WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
        CloseHandle(thread);
        xbox_KfRaiseIrql(2);
        CHECK(BRIDGE_MEM32(g_esp - 64) == 0x12345678);
    } else if (!strcmp(mode, "concurrent")) {
        CHECK(g_esp != outer_callback_stack);
        BRIDGE_MEM32(g_esp - 64) = 0xBAADF00D;
    } else if (!strcmp(mode, "nested") && callbacks == 1) {
        queue();
        g_ecx = 0;
        bridge_KfLowerIrql();
        CHECK(callbacks == 1); /* Never invoke a DPC from inside a DPC. */
        xbox_KfRaiseIrql(2);
    }
    callback_depth--;
    g_esp += 20;
}

recomp_func_t recomp_lookup(uint32_t address)
{
    return address == ROUTINE ? guest_dpc : NULL;
}
recomp_func_t recomp_lookup_manual(uint32_t address)
{
    (void)address;
    return NULL;
}

static void prepare(void)
{
    memset(memory + DPC, 0, 32);
    BRIDGE_MEM16(DPC) = 0x13;
    BRIDGE_MEM32(DPC + 12) = ROUTINE;
    BRIDGE_MEM32(DPC + 16) = VOICE;
    BRIDGE_MEM32(VOICE) = LIVE_VTABLE;
    BRIDGE_MEM32(HEAD) = BRIDGE_MEM32(HEAD + 4) = VOICE + 0x54;
    BRIDGE_MEM32(VOICE + 0x54) = BRIDGE_MEM32(VOICE + 0x58) = HEAD;
    queue();
}

static DWORD WINAPI parallel_lower(LPVOID unused)
{
    (void)unused;
    g_fs_base = 0x11000; g_esp = 0x90000;
    lowering_thread = GetCurrentThreadId();
    xbox_KfRaiseIrql(2); prepare();
    g_ecx = 0; bridge_KfLowerIrql();
    CHECK(g_esp == 0x90000);
    return 0;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    mode = argv[1];
    memory = VirtualAlloc(NULL, 16 * 1024 * 1024,
                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(memory);
    xbox_worker_stack_reset(0x400000); /* owned slices within this private arena */
    g_xbox_mem_offset = (ptrdiff_t)memory;
    g_fs_base = 0x10000;
    g_esp = 0x80000;
    lowering_thread = GetCurrentThreadId();
    int occupied[XBOX_WORKER_STACK_COUNT];
    if (!strcmp(mode, "exhausted") || !strcmp(mode, "concurrent")) {
        for (int i = 0; i < XBOX_WORKER_STACK_COUNT; i++) {
            occupied[i] = xbox_worker_stack_alloc();
            CHECK(occupied[i] >= 0);
        }
        CHECK(xbox_worker_stack_alloc() < 0);
    }
    xbox_KfRaiseIrql(16);
    prepare();
    if (!strcmp(mode, "stack") || !strcmp(mode, "exhausted")) {
        memset(caller_stack, 0x5A, sizeof(caller_stack));
        memcpy(memory + 0x80000 - sizeof(caller_stack), caller_stack, sizeof(caller_stack));
    }

    if (!strcmp(mode, "boundary")) {
        g_ecx = 2;
        bridge_KfLowerIrql();
        CHECK(callbacks == 0 && BRIDGE_MEM8(DPC + 2) == 1);
    }
    if (!strcmp(mode, "context")) {
        const int modes[] = { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO };
        for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
            if (i) { xbox_KfRaiseIrql(2); prepare(); }
            g_ecx = 0; g_edx = 0x22222222; g_ebx = 0x33333333;
            g_esi = 0x44444444; g_edi = 0x55555555;
            g_ebp = 0x66666666; g_seh_ebp = 0x77777777; g_xbox_kernel_caller = 0x88888888;
            memset(g_fp_stack, 0x12, sizeof(g_fp_stack));
            g_fp_top = 3; g_fp_cmp = 1; g_fp_control_word = 0xB7F; g_fp_cc = 0x4000;
            RecompMmx *mm[] = { &g_mm0, &g_mm1, &g_mm2, &g_mm3, &g_mm4, &g_mm5, &g_mm6, &g_mm7 };
            RecompXmm *xmm[] = { &g_xmm0, &g_xmm1, &g_xmm2, &g_xmm3, &g_xmm4, &g_xmm5, &g_xmm6, &g_xmm7 };
            for (unsigned r = 0; r < 8; r++) {
                mm[r]->q = 0x1234567800000000ull + r;
                xmm[r]->q[0] = 0x9988776600000000ull + r;
                xmm[r]->q[1] = 0xAABBCCDD00000000ull + r;
            }
            g_df = 1;
            CHECK(fesetround(modes[i]) == 0);
            _mm_setcsr((_mm_getcsr() & ~0x8040u) | ((i & 1) ? 0x8040u : 0));
            State before, after;
            snapshot(&before);
            bridge_KfLowerIrql();
            snapshot(&after);
            CHECK(callbacks == (int)i + 1);
            CHECK(g_eax == 0 && !memcmp(&before, &after, sizeof(before)));
        }
    } else {
        g_ecx = 0;
        bridge_KfLowerIrql();
        CHECK(callbacks == ((!strcmp(mode, "nested") || !strcmp(mode, "concurrent")) ? 2 : 1));
    }
    if (!strcmp(mode, "stack") || !strcmp(mode, "exhausted")) {
        CHECK(!memcmp(memory + 0x80000 - sizeof(caller_stack), caller_stack, sizeof(caller_stack)));
        CHECK(g_esp == 0x80000);
        if (!strcmp(mode, "exhausted")) {
            CHECK(fallback_allocations == 1);
            xbox_KfRaiseIrql(2); prepare();
            memcpy(memory + 0x80000 - sizeof(caller_stack), caller_stack, sizeof(caller_stack));
            g_ecx = 0; bridge_KfLowerIrql();
            CHECK(callbacks == 2 && fallback_allocations == 2 && fallback_frees == 2);
            for (int i = 0; i < XBOX_WORKER_STACK_COUNT; i++) xbox_worker_stack_free(occupied[i]);
        }
        /* Every lease must be released after a synchronous drain. */
        for (int i = 0; i < XBOX_WORKER_STACK_COUNT; i++) {
            occupied[i] = xbox_worker_stack_alloc(); CHECK(occupied[i] >= 0);
        }
        for (int i = 0; i < XBOX_WORKER_STACK_COUNT; i++) xbox_worker_stack_free(occupied[i]);
    }
    if (!strcmp(mode, "concurrent")) {
        CHECK(fallback_allocations == 2 && fallback_frees == 2);
        for (int i = 0; i < XBOX_WORKER_STACK_COUNT; i++) xbox_worker_stack_free(occupied[i]);
    }
    CHECK(max_depth == 1 && g_esp == 0x80000);
    CHECK(BRIDGE_MEM8(g_fs_base + XBOX_KPCR_IRQL_OFFSET) == 0);
    CHECK(BRIDGE_MEM8(g_fs_base + XBOX_KPCR_DPC_ACTIVE_OFFSET) == 0);
    CHECK(BRIDGE_MEM8(DPC + 2) == 0);
    if (!strcmp(mode, "lifetime")) {
        BRIDGE_MEM32(VOICE) = DEAD_VTABLE;
        CHECK(BRIDGE_MEM32(HEAD) == HEAD);
    }
    int completed = callbacks;
    kernel_drain_dpcs(&(int){0});
    CHECK(callbacks == completed); /* Worker fallback cannot run it twice. */
    bridge_KeIsExecutingDpc();
    CHECK(g_eax == 0);
    CHECK(xbox_IrqlRaisedCount() == 0);
    xbox_worker_stack_reset(0);
    CHECK(VirtualFree(memory, 0, MEM_RELEASE));
    printf("PASS: DPC lower %s\n", mode);
    return 0;
}
