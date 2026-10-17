/* Timer state must be published before callbacks can rearm or retire it. */
#define WIN32_LEAN_AND_MEAN
#include "platform/xbox_winnt.h"
static int expire_on_reset;
static LONGLONG system_now = 133000000000000000LL;
static ULONGLONG tick_now = 100000;
static LONGLONG audio_deadline;
static unsigned completed_audio_work;
static VOID __stdcall test_query_time(PLARGE_INTEGER out) { out->QuadPart = system_now; }
static ULONGLONG WINAPI test_tick(void) { return tick_now; }
#define xbox_KeQuerySystemTime test_query_time
#define GetTickCount64 test_tick
static void kernel_fire_due_timer(int i, long long now);
static BOOL WINAPI watched_reset(HANDLE event)
{
    if (expire_on_reset) {
        expire_on_reset = 0;
        kernel_fire_due_timer(0, 0x7FFFFFFFFFFFFFFFLL);
    }
    return ResetEvent(event);
}
#define ResetEvent watched_reset
#ifndef KERNEL_BRIDGE_SOURCE
#define KERNEL_BRIDGE_SOURCE "../../src/kernel/kernel_bridge.c"
#endif
#include KERNEL_BRIDGE_SOURCE
#undef ResetEvent
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "timer lifecycle line %d: %s\n", __LINE__, #c); ExitProcess(1); } } while (0)
enum { TIMER = 0x20000, DPC = 0x22000, ROUTINE = 0x26000 };
static uint8_t *memory;
static HANDLE timer_event;
static unsigned callbacks;
static const char *mode;
static void guest_dpc(void)
{
    CHECK(BRIDGE_MEM8(g_fs_base + 0x24) == 2);
    CHECK(BRIDGE_MEM32(g_esp + 4) == DPC);
    callbacks++;
    if (!strcmp(mode, "order")) {
        CHECK(BRIDGE_MEM32(TIMER + 4) == 1);
        CHECK(WaitForSingleObject(timer_event, 0) == WAIT_OBJECT_0);
    } else if (!strcmp(mode, "rearm")) {
        kernel_set_timer(TIMER, -1000000000LL, 0, DPC);
        CHECK(!g_eax);
    } else if (!strcmp(mode, "retire")) {
        memset(memory + TIMER, 0xA5, 40);
    } else if (!strcmp(mode, "audio_deadline")) {
        /* MM3 sub_0027B554 -> sub_0027B389: deferred audio work that is
         * still in the future rearms KeSetTimer at that absolute time. */
        if (system_now < audio_deadline) kernel_set_timer(TIMER, audio_deadline, 0, DPC);
        else completed_audio_work++;
    }
    g_esp += 20;
}
recomp_func_t recomp_lookup(uint32_t address) { return address == ROUTINE ? guest_dpc : NULL; }
recomp_func_t recomp_lookup_manual(uint32_t address) { (void)address; return NULL; }
int main(int argc, char **argv)
{
    CHECK(argc == 2); mode = argv[1];
    memory = VirtualAlloc(NULL, 16 * 1024 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(memory); g_xbox_mem_offset = (ptrdiff_t)memory; g_fs_base = 0x10000; g_esp = 0x80000;
    InitializeCriticalSection(&g_timer_lock); g_timer_started = 1;
    BRIDGE_MEM32(g_esp) = TIMER; BRIDGE_MEM32(g_esp + 4) = 0;
    bridge_KeInitializeTimerEx(); timer_event = ke_shadow_lookup(TIMER); CHECK(timer_event);
    BRIDGE_MEM32(DPC + 12) = ROUTINE;
    if (!strcmp(mode, "audio_deadline")) {
        unsigned step;
        audio_deadline = system_now + 20000000;
        kernel_set_timer(TIMER, audio_deadline, 0, DPC);
        for (step = 0; step < 200; step++) {
            tick_now += 10; system_now += 100000;
            kernel_fire_due_timer(0, (long long)tick_now);
        }
        printf("audio timer: callbacks=%u completed=%u over 200 polling ticks\n", callbacks, completed_audio_work);
        CHECK(callbacks == 1 && completed_audio_work == 1);
        ke_shadow_remove(TIMER); VirtualFree(memory, 0, MEM_RELEASE);
        return 0;
    }
    if (!strcmp(mode, "absolute") || !strcmp(mode, "expired") || !strcmp(mode, "relative")) {
        long long due = !strcmp(mode, "absolute") ? system_now + 20000000LL :
                        !strcmp(mode, "expired") ? system_now - 1 : -15001;
        long long expected = !strcmp(mode, "absolute") ? 102000 :
                             !strcmp(mode, "expired") ? 100000 : 100002;
        kernel_set_timer(TIMER, due, 0, DPC);
        CHECK(g_timers[0].due_ms == expected);
        kernel_fire_due_timer(0, expected - 1); CHECK(!callbacks);
        kernel_fire_due_timer(0, expected); CHECK(callbacks == 1);
        ke_shadow_remove(TIMER); VirtualFree(memory, 0, MEM_RELEASE);
        printf("PASS timer %s: deadline honored without repeated early audio DPCs\n", mode);
        return 0;
    }
    if (!strcmp(mode, "publication")) expire_on_reset = 1;
    kernel_set_timer(TIMER, 0, !strcmp(mode, "periodic") ? 10 : 0,
                     !strcmp(mode, "publication") ? 0 : DPC);
    CHECK(!g_eax);
    if (!strcmp(mode, "publication")) {
        /* If expiration beat reset, its signal must not disappear. */
        CHECK(g_timers[0].timer_va == TIMER || WaitForSingleObject(timer_event, 0) == WAIT_OBJECT_0);
    } else {
        long long now = g_timers[0].due_ms;
        kernel_fire_due_timer(0, now);
        CHECK(callbacks == 1 && g_esp == 0x80000);
        if (!strcmp(mode, "rearm")) {
            CHECK(g_timers[0].timer_va == TIMER && g_timers[0].due_ms > now);
            CHECK(BRIDGE_MEM32(TIMER + 4) == 0 && WaitForSingleObject(timer_event, 0) == WAIT_TIMEOUT);
            kernel_fire_due_timer(0, now); CHECK(callbacks == 1);
            kernel_fire_due_timer(0, g_timers[0].due_ms); CHECK(callbacks == 2);
        } else if (!strcmp(mode, "retire")) {
            unsigned i; for (i = 0; i < 40; i++) CHECK(memory[TIMER + i] == 0xA5);
        } else if (!strcmp(mode, "periodic")) {
            CHECK(g_timers[0].timer_va == TIMER && g_timers[0].due_ms == now + 10);
            kernel_fire_due_timer(0, now + 9); CHECK(callbacks == 1);
            kernel_fire_due_timer(0, now + 10); CHECK(callbacks == 2);
            CHECK(xbox_kernel_cancel_timer(TIMER));
            kernel_fire_due_timer(0, now + 100); CHECK(callbacks == 2);
        } else { CHECK(!g_timers[0].timer_va); }
    }
    CHECK(BRIDGE_MEM8(g_fs_base + 0x24) == 0);
    ke_shadow_remove(TIMER); VirtualFree(memory, 0, MEM_RELEASE);
    printf("PASS timer %s: state/event ordering, callback lifetime and scheduling\n", mode);
    return 0;
}
