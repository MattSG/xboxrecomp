/* Host waiting only: guest GET/NOP/stop values remain the completion rules. */
#ifndef XBOX_NV2A_NOTIFY_WAIT_H
#define XBOX_NV2A_NOTIFY_WAIT_H

#include "platform/xbox_winnt.h"
#include <stdint.h>

typedef struct {
    volatile LONG generation;
#ifdef _WIN32
    BOOL (WINAPI *wait_address)(volatile VOID *, PVOID, SIZE_T, DWORD);
    VOID (WINAPI *wake_all)(PVOID);
#endif
} XBOX_NV2A_WAKE;

static void xbox_nv2a_wake_init(XBOX_NV2A_WAKE *wake)
{
    wake->generation = 0;
#ifdef _WIN32
    /* Resolve together so older hosts retain their existing yield fallback.
     * No new library dependency is required by applications using the runtime. */
    HMODULE module = GetModuleHandleW(L"kernelbase.dll");
    if (!module) module = GetModuleHandleW(L"kernel32.dll");
    wake->wait_address = module ? (BOOL (WINAPI *)(volatile VOID *, PVOID, SIZE_T, DWORD))
        GetProcAddress(module, "WaitOnAddress") : NULL;
    wake->wake_all = module ? (VOID (WINAPI *)(PVOID))
        GetProcAddress(module, "WakeByAddressAll") : NULL;
    if (!wake->wake_all) wake->wait_address = NULL;
#endif
}

static LONG xbox_nv2a_wake_snapshot(XBOX_NV2A_WAKE *wake)
{
    return InterlockedCompareExchange(&wake->generation, 0, 0);
}

static void xbox_nv2a_wake_publish(XBOX_NV2A_WAKE *wake)
{
    /* Publish after the corresponding guest-visible completion stores. */
    InterlockedIncrement(&wake->generation);
#ifdef _WIN32
    if (wake->wait_address) wake->wake_all((PVOID)&wake->generation);
#endif
}

static BOOL xbox_nv2a_wake_wait(XBOX_NV2A_WAKE *wake, LONG observed)
{
#ifdef _WIN32
    if (wake->wait_address)
        return wake->wait_address(&wake->generation, &observed, sizeof observed, 1);
#else
    (void)wake; (void)observed;
#endif
    SwitchToThread();
    return TRUE;
}

static int xbox_nv2a_notify_pending(volatile uint32_t *get,
    volatile uint32_t *put, volatile LONG *notifications, LONG notified,
    volatile LONG *stop)
{
    return *get != *put && *notifications == notified &&
           !InterlockedCompareExchange(stop, 0, 0);
}

static void xbox_nv2a_notify_wait(XBOX_NV2A_WAKE *wake,
    volatile uint32_t *get, volatile uint32_t *put,
    volatile LONG *notifications, LONG notified, volatile LONG *stop)
{
    unsigned spins = 0;
    while (xbox_nv2a_notify_pending(get, put, notifications, notified, stop)) {
#ifdef _WIN32
        /* Short completions avoid a host wait. Prolonged waits, including
         * vblank pacing in the executor, release the guest thread's core. */
        if (++spins & 63) {
            YieldProcessor();
            continue;
        }
#else
        (void)spins;
#endif
        LONG observed = xbox_nv2a_wake_snapshot(wake);
        /* Snapshot BEFORE this predicate check. A completion between the
         * check and WaitOnAddress changes generation, so waiting on the old
         * value returns immediately even if its wake already happened. */
        if (!xbox_nv2a_notify_pending(get, put, notifications, notified, stop))
            break;
        /* The one-millisecond fallback also notices guest-origin register
         * writes that do not pass through the executor's publishers. */
        xbox_nv2a_wake_wait(wake, observed);
    }
}

/* Exact unsigned distance test from the guest's direct fence polling loop.
 * In particular, a completed value ahead of submitted wraps the distance;
 * treating it as a signed number would change the guest's branch. */
static int xbox_nv2a_fence_pending(volatile uint32_t *completed,
    uint32_t submitted, uint32_t allowed_distance)
{
    return allowed_distance < (uint32_t)(submitted - *completed);
}

static void xbox_nv2a_fence_wait(XBOX_NV2A_WAKE *wake,
    volatile uint32_t *completed, uint32_t submitted, uint32_t allowed_distance,
    volatile LONG *stop)
{
    unsigned spins = 0;
    while (xbox_nv2a_fence_pending(completed, submitted, allowed_distance) &&
           !InterlockedCompareExchange(stop, 0, 0)) {
#ifdef _WIN32
        if (++spins & 63) {
            YieldProcessor();
            continue;
        }
#else
        (void)spins;
#endif
        LONG observed = xbox_nv2a_wake_snapshot(wake);
        if (!xbox_nv2a_fence_pending(completed, submitted, allowed_distance) ||
            InterlockedCompareExchange(stop, 0, 0))
            break;
        xbox_nv2a_wake_wait(wake, observed);
    }
}

#endif
