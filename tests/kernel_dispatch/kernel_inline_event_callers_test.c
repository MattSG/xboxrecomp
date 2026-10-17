/* Real event bridges with MM3 XPP's pending-request/wait/cancel contract.
 * No game assets or guest event opt-in are needed. */
#include "../../src/kernel/kernel_bridge.c"
#include "../kernel_test_host.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s status=%08X error=%lu\n", __LINE__, #c, g_eax, GetLastError()); fflush(stderr); exit(1); } } while (0)
#define EVENT_VA 0x22000u
#define REQUEST_VA 0x30000u
#define TIMEOUT_VA 0x26000u
#define SETTER_PC 0x0035C348u
#define WAIT_PC 0x0035D553u
#define CANCEL_WAIT_PC 0x0035D56Cu
#define AUTO_WAIT_PC 0x0035C7F1u
#define UNRELATED_PC 0x00343D57u
#define USB_PENDING 0x40000000u

recomp_func_t recomp_lookup(uint32_t address) { (void)address; abort(); }
recomp_func_t recomp_lookup_manual(uint32_t address) { (void)address; abort(); }
static uint8_t *memory;
static RECOMP_TLS uint32_t test_stack = 0x28000u;
static const uint32_t callers[] = { SETTER_PC, WAIT_PC, CANCEL_WAIT_PC, AUTO_WAIT_PC };

static uint32_t invoke(unsigned ordinal, uint32_t caller, const uint32_t *args, unsigned count)
{
    unsigned slot = ordinal == 145 ? 0 : ordinal == 159 ? 1 : 2;
    uint32_t *stack = (uint32_t *)(memory + test_stack);
    uint32_t saved[6];
    g_esp = test_stack;
    g_ecx = 0x12345678u; g_edx = 0x23456789u; g_ebx = REQUEST_VA;
    g_esi = 0x3456789Au; g_edi = 0x456789ABu; g_ebp = 0x56789ABCu;
    saved[0] = g_ecx; saved[1] = g_edx; saved[2] = g_ebx;
    saved[3] = g_esi; saved[4] = g_edi; saved[5] = g_ebp;
    stack[0] = caller;
    memcpy(stack + 1, args, count * sizeof(*args));
    recomp_func_t fn = recomp_lookup_kernel(KERNEL_VA_BASE + slot * 4);
    CHECK(fn != NULL);
    fn();
    CHECK(g_esp == test_stack + 4 + count * 4);
    CHECK(g_xbox_kernel_caller == caller);
    CHECK(g_ecx == saved[0] && g_edx == saved[1] && g_ebx == saved[2]);
    CHECK(g_esi == saved[3] && g_edi == saved[4] && g_ebp == saved[5]);
    return g_eax;
}

static uint32_t set_event(uint32_t object, uint32_t caller)
{
    uint32_t args[] = { object, 0, 0 };
    return invoke(145, caller, args, 3);
}

static uint32_t wait_event(uint32_t object, uint32_t caller, uint32_t timeout)
{
    uint32_t args[] = { object, 0, 0, 0, timeout };
    return invoke(159, caller, args, 5);
}

static void prepare_event(uint32_t object, unsigned type, unsigned signaled)
{
    /* Only Type and Size identify the event; other header bytes are arbitrary
     * in the real XPP stack-built headers (00048601 / 01040E00). */
    BRIDGE_MEM32(object) = 0x01040E00u | type;
    BRIDGE_MEM32(object + 4) = signaled;
    BRIDGE_MEM32(object + 8) = object + 8;
    BRIDGE_MEM32(object + 12) = object + 8;
}

static void prepare_request(uint32_t request, uint32_t event)
{
    memset(memory + request, 0, 48);
    BRIDGE_MEM32(request) = 0x00004030u;
    BRIDGE_MEM32(request + 4) = USB_PENDING;
    BRIDGE_MEM32(request + 8) = 0x0035C33Au;
    BRIDGE_MEM32(request + 12) = event;
    BRIDGE_MEM32(request + 16) = 0x11223344u;
}

typedef struct {
    HANDLE start, finished;
    uint32_t request, event, stack;
    unsigned delay;
    volatile LONG damaged, completed;
} COMPLETION;

static DWORD WINAPI complete_request(void *argument)
{
    COMPLETION *c = argument;
    test_stack = c->stack;
    CHECK(WaitForSingleObject(c->start, 2000) == WAIT_OBJECT_0);
    if (c->delay) Sleep(c->delay);
    if (BRIDGE_MEM32(c->request) != 0x4030u ||
        BRIDGE_MEM32(c->request + 8) != 0x0035C33Au ||
        BRIDGE_MEM32(c->request + 12) != c->event ||
        BRIDGE_MEM32(c->request + 16) != 0x11223344u) {
        InterlockedExchange(&c->damaged, 1);
    } else {
        BRIDGE_MEM32(c->request + 4) = 0;
        CHECK(set_event(c->event, SETTER_PC) == 0);
        InterlockedExchange(&c->completed, 1);
    }
    SetEvent(c->finished);
    return 0;
}

static HANDLE start_completion(COMPLETION *c, uint32_t request, uint32_t event, unsigned delay, unsigned index)
{
    memset(c, 0, sizeof(*c));
    c->start = CreateEventA(NULL, TRUE, FALSE, NULL);
    c->finished = CreateEventA(NULL, TRUE, FALSE, NULL);
    c->request = request; c->event = event;
    c->stack = 0x29000u + index * 0x1000u; c->delay = delay;
    CHECK(c->start && c->finished);
    HANDLE worker = CreateThread(NULL, 0, complete_request, c, 0, NULL);
    CHECK(worker != NULL);
    return worker;
}

static void finish_completion(COMPLETION *c, HANDLE worker)
{
    CHECK(WaitForSingleObject(c->finished, 2000) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(worker, 2000) == WAIT_OBJECT_0);
    CloseHandle(worker); CloseHandle(c->start); CloseHandle(c->finished);
}

static unsigned shadow_count(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < KE_SHADOW_SIZE; ++i) n += g_ke_shadow[i].in_use != 0;
    return n;
}

static void check_early(void)
{
    for (unsigned type = 0; type < 2; ++type) {
        prepare_event(EVENT_VA, type, 0);
        prepare_request(REQUEST_VA, EVENT_VA);
        COMPLETION completion;
        HANDLE worker = start_completion(&completion, REQUEST_VA, EVENT_VA, 0, 0);
        SetEvent(completion.start);
        finish_completion(&completion, worker); /* callback precedes wait/shadow registration */
        CHECK(completion.completed && !completion.damaged);
        CHECK(BRIDGE_MEM32(EVENT_VA + 4) == 1);
        CHECK(wait_event(EVENT_VA, WAIT_PC, TIMEOUT_VA + 8) == STATUS_SUCCESS);
        CHECK(BRIDGE_MEM32(EVENT_VA + 4) == (type ? 0u : 1u));
        CHECK(wait_event(EVENT_VA, AUTO_WAIT_PC, TIMEOUT_VA + 8) == (type ? STATUS_TIMEOUT : STATUS_SUCCESS));
    }
}

static void check_late(void)
{
    prepare_event(EVENT_VA, 0, 0);
    prepare_request(REQUEST_VA, EVENT_VA);
    COMPLETION completion;
    HANDLE worker = start_completion(&completion, REQUEST_VA, EVENT_VA, 25, 0);
    SetEvent(completion.start);
    CHECK(wait_event(EVENT_VA, WAIT_PC, 0) == STATUS_SUCCESS);
    /* A successful wait cannot release the pending stack request early. */
    CHECK(BRIDGE_MEM32(REQUEST_VA + 4) == 0);
    CHECK(BRIDGE_MEM32(REQUEST_VA + 8) == 0x0035C33Au);
    finish_completion(&completion, worker);
    CHECK(completion.completed && !completion.damaged);
}

static void check_cancel(void)
{
    prepare_event(EVENT_VA, 1, 0);
    prepare_request(REQUEST_VA, EVENT_VA);
    COMPLETION completion;
    HANDLE worker = start_completion(&completion, REQUEST_VA, EVENT_VA, 0, 0);
    /* XPP 35D526 uses a 50ms relative wait. Only timeout branches to request
     * cancellation, followed by the infinite wait at 35D56C. */
    CHECK(wait_event(EVENT_VA, WAIT_PC, TIMEOUT_VA) == STATUS_TIMEOUT);
    CHECK(BRIDGE_MEM32(REQUEST_VA + 4) == USB_PENDING);
    CHECK(BRIDGE_MEM32(EVENT_VA + 4) == 0);
    SetEvent(completion.start); /* cancellation retires the outstanding request */
    CHECK(wait_event(EVENT_VA, CANCEL_WAIT_PC, 0) == STATUS_SUCCESS);
    CHECK(BRIDGE_MEM32(REQUEST_VA + 4) == 0);
    CHECK(BRIDGE_MEM32(EVENT_VA + 4) == 0);
    finish_completion(&completion, worker);
    CHECK(completion.completed && !completion.damaged);
}

static void check_negative(void)
{
    prepare_event(EVENT_VA, 0, 0);
    prepare_request(REQUEST_VA, EVENT_VA);
    CHECK(xbox_kernel_set_inline_event_callers(NULL, 0) == 0);
    COMPLETION completion;
    HANDLE worker = start_completion(&completion, REQUEST_VA, EVENT_VA, 0, 0);
    uint32_t status = wait_event(EVENT_VA, WAIT_PC, TIMEOUT_VA);
    CHECK(status & 0x80000000u); CHECK(status != STATUS_TIMEOUT);
    CHECK(GetLastError() == ERROR_INVALID_HANDLE);
    /* Retail XPP regards this non-timeout result as completion. Reusing its
     * request storage now reproduces the invalid callback seen in the log. */
    BRIDGE_MEM32(REQUEST_VA + 8) = 0x040172F0u;
    SetEvent(completion.start);
    finish_completion(&completion, worker);
    CHECK(completion.damaged && !completion.completed);
    CHECK(shadow_count() == 0);
}

static void check_scope(void)
{
    uint32_t configured[4];
    memcpy(configured, callers, sizeof configured);
    CHECK(xbox_kernel_set_inline_event_callers(configured, 4) == 0);
    memset(configured, 0, sizeof configured); /* list was copied */
    prepare_event(EVENT_VA, 0, 0);
    CHECK(set_event(EVENT_VA, SETTER_PC) == 0);
    CHECK(wait_event(EVENT_VA, WAIT_PC, TIMEOUT_VA + 8) == STATUS_SUCCESS);
    unsigned slots = shadow_count();
    CHECK(wait_event(EVENT_VA, UNRELATED_PC, TIMEOUT_VA + 8) & 0x80000000u);
    CHECK(GetLastError() == ERROR_INVALID_HANDLE);
    BRIDGE_MEM32(EVENT_VA + 4) = 0;
    CHECK(set_event(EVENT_VA, UNRELATED_PC) == 0);
    CHECK(GetLastError() == ERROR_INVALID_HANDLE);
    CHECK(BRIDGE_MEM32(EVENT_VA + 4) == 0 && shadow_count() == slots);
    /* The title's unrelated D3D notification event remains on its old path. */
    prepare_event(EVENT_VA + 0x20, 0, 0);
    CHECK(wait_event(EVENT_VA + 0x20, UNRELATED_PC, TIMEOUT_VA + 8) & 0x80000000u);
    CHECK(shadow_count() == slots);
    CHECK(xbox_kernel_set_inline_event_callers(NULL, 1) == -1);
    CHECK(xbox_kernel_set_inline_event_callers(callers, 17) == -1);
    uint32_t bad[] = {0};
    CHECK(xbox_kernel_set_inline_event_callers(bad, 1) == -1);
    CHECK(set_event(EVENT_VA, SETTER_PC) == 0);
    /* Events created by KeInitializeEvent keep their ordinary shadow behavior
     * even when the caller is outside the inline-event allow-list. */
    uint32_t args[] = { EVENT_VA, 1, 1 };
    invoke(108, UNRELATED_PC, args, 3);
    CHECK(wait_event(EVENT_VA, UNRELATED_PC, TIMEOUT_VA + 8) == STATUS_SUCCESS);
    CHECK(shadow_count() == slots);
}

static void check_invalid(void)
{
    int type = -1;
    uint32_t invalid[] = { 0, 0x1000, EVENT_VA + 1, 0x50000, 0xFFFFFFFC, 0x80000000 };
    g_xbox_kernel_caller = WAIT_PC;
    for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; ++i)
        CHECK(ke_guest_event(invalid[i], &type) == NULL);
    prepare_event(EVENT_VA, 2, 0);
    CHECK(ke_guest_event(EVENT_VA, &type) == NULL);
    BRIDGE_MEM32(EVENT_VA) = 0x00080000;
    CHECK(ke_guest_event(EVENT_VA, &type) == NULL);
    prepare_event(EVENT_VA, 0, 0);
    {
        DWORD protection;
        CHECK(VirtualProtect(memory + EVENT_VA, 0x1000, PAGE_READONLY, &protection));
        CHECK(ke_guest_event(EVENT_VA, &type) == NULL);
        CHECK(set_event(EVENT_VA, SETTER_PC) == 0);
        CHECK(BRIDGE_MEM32(EVENT_VA + 4) == 0);
        CHECK(VirtualProtect(memory + EVENT_VA, 0x1000, protection, &protection));
    }
    /* A syntactically valid header whose last word has no committed backing. */
    BRIDGE_MEM32(0x3FFF4) = 0x00040000;
    BRIDGE_MEM32(0x3FFF8) = 0;
    BRIDGE_MEM32(0x3FFFC) = 0x3FFFC;
    g_xbox_kernel_caller = WAIT_PC;
    CHECK(ke_guest_event(0x3FFF4, &type) == NULL);
    CHECK(shadow_count() == 0);
}

static void check_reuse(void)
{
    DWORD before = 0, after = 0;
#ifdef _WIN32   /* POSIX events are not descriptors, so the count cannot see them */
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &before));
#endif
    for (unsigned i = 0; i < KE_SHADOW_SIZE + 64; ++i) {
        unsigned type = i & 1;
        prepare_event(EVENT_VA, type, 0);
        CHECK(set_event(EVENT_VA, SETTER_PC) == 0);
        CHECK(wait_event(EVENT_VA, WAIT_PC, TIMEOUT_VA + 8) == STATUS_SUCCESS);
        CHECK(BRIDGE_MEM32(EVENT_VA + 4) == (type ? 0u : 1u));
        /* Reinitializing the same type also clears a still-signaled host event. */
        prepare_event(EVENT_VA, type, 0);
        CHECK(wait_event(EVENT_VA, AUTO_WAIT_PC, TIMEOUT_VA + 8) == STATUS_TIMEOUT);
    }
    CHECK(shadow_count() == 1);
#ifdef _WIN32
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &after));
    CHECK(after == before + 1);
#endif
    ke_shadow_remove(EVENT_VA);
#ifdef _WIN32
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &after)); CHECK(after == before);
#endif
}

static void check_capacity(void)
{
    DWORD before, after;
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &before));
    for (unsigned i = 0; i < KE_SHADOW_SIZE; ++i) {
        HANDLE event = CreateEventA(NULL, TRUE, FALSE, NULL);
        CHECK(event != NULL); ke_shadow_insert(0x10000u + i * 16, event);
    }
    CHECK(shadow_count() == KE_SHADOW_SIZE);
    prepare_event(EVENT_VA, 0, 0);
    g_xbox_kernel_caller = WAIT_PC;
    int type;
    CHECK(ke_guest_event(EVENT_VA, &type) == NULL);
    CHECK(wait_event(EVENT_VA, WAIT_PC, TIMEOUT_VA + 8) & 0x80000000u);
    CHECK(shadow_count() == KE_SHADOW_SIZE && BRIDGE_MEM32(EVENT_VA + 4) == 0);
    for (unsigned i = 0; i < KE_SHADOW_SIZE; ++i) ke_shadow_remove(0x10000u + i * 16);
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &after)); CHECK(after == before);
}

static void check_concurrent(void)
{
    COMPLETION completion[4]; HANDLE worker[4];
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t event = EVENT_VA + i * 0x20, request = REQUEST_VA + i * 0x40;
        prepare_event(event, 1, 0); prepare_request(request, event);
        worker[i] = start_completion(&completion[i], request, event, 0, i);
    }
    for (unsigned i = 0; i < 4; ++i) SetEvent(completion[i].start);
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(wait_event(completion[i].event, WAIT_PC, 0) == STATUS_SUCCESS);
        finish_completion(&completion[i], worker[i]);
        CHECK(completion[i].completed && !completion[i].damaged);
        CHECK(BRIDGE_MEM32(completion[i].event + 4) == 0);
    }
    CHECK(shadow_count() == 4);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    _putenv_s("RECOMP_TITLE_KEVENTS", "0"); _putenv_s("RECOMP_ASYNC_WAIT_TRACE", "0");
    _putenv_s("MM3_BUILDING_TRACE", "0"); _putenv_s("RECOMP_KERNEL_LOG_BUDGET", "0");
    _putenv_s("RECOMP_GUEST_LOCK", "0");
    memory = VirtualAlloc(NULL, 0x100000, MEM_RESERVE, PAGE_NOACCESS);
    CHECK(memory && VirtualAlloc(memory, 0x40000, MEM_COMMIT, PAGE_READWRITE) == memory);
    g_xbox_mem_offset = (ptrdiff_t)memory; g_xbox_map_size = 0x100000;
    for (unsigned i = 0; i < 3; ++i) {
        unsigned ordinal = i == 0 ? 145 : i == 1 ? 159 : 108;
        g_slot_ordinals[i] = ordinal; g_slot_bridges[i] = bridge_for_ordinal(ordinal);
        g_slot_arg_bytes[i] = stdcall_args_for_ordinal(ordinal);
    }
    ((LARGE_INTEGER *)(memory + TIMEOUT_VA))->QuadPart = -500000;
    ((LARGE_INTEGER *)(memory + TIMEOUT_VA + 8))->QuadPart = 0;
    CHECK(xbox_kernel_set_inline_event_callers(callers, 4) == 0);
    if (!strcmp(argv[1], "early")) check_early();
    else if (!strcmp(argv[1], "late")) check_late();
    else if (!strcmp(argv[1], "cancel")) check_cancel();
    else if (!strcmp(argv[1], "negative")) check_negative();
    else if (!strcmp(argv[1], "scope")) check_scope();
    else if (!strcmp(argv[1], "invalid")) check_invalid();
    else if (!strcmp(argv[1], "reuse")) check_reuse();
    else if (!strcmp(argv[1], "capacity")) check_capacity();
    else if (!strcmp(argv[1], "concurrent")) check_concurrent();
    else CHECK(0);
    for (unsigned i = 0; i < KE_SHADOW_SIZE; ++i)
        if (g_ke_shadow[i].in_use) ke_shadow_remove(g_ke_shadow[i].guest_va);
    VirtualFree(memory, 0, MEM_RELEASE);
    printf("PASS: inline event callers %s; RECOMP_TITLE_KEVENTS=0\n", argv[1]);
    return 0;
}
