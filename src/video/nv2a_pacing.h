/* Display pacing is owned by the render thread. Keep the successful wait's
 * one-millisecond spin tail and late-frame cadence; timer failures must not
 * turn the entire remaining frame into a busy spin. */
#ifndef NV2A_PACING_H
#define NV2A_PACING_H

#include <stdint.h>
#include <limits.h>

static int nv_pacing_deadline(int64_t *next, int64_t now, int64_t frequency, int limit)
{
    int64_t period;
    if (limit <= 0 || frequency <= 0) return 0;
    period = frequency / limit;
    /* A zero period cannot pace. Rebase rather than overflow signed QPC
     * arithmetic; valid timestamps follow the original deadline calculation. */
    if (period <= 0 || now < 0 || *next < 0 || *next > INT64_MAX - period) {
        *next = now;
        return 0;
    }
    *next += period;
    if (!*next || *next <= now) {
        *next = now;
        return 0;
    }
    return 1;
}

#if defined(_WIN32)
#include <windows.h>

/* The wait: a high-resolution timer, then a short spin (Windows). The SDL3
 * renderer waits with SDL_DelayPrecise instead. */
static HANDLE nv_pacing_create_timer(void)
{
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    return timer;
}

#endif /* the pure conversion below is portable; the timer wait is Windows-only */

/* Convert QPC ticks to relative 100 ns units. The ordinary path is exactly
 * the original integer conversion. Binary division handles only products
 * that would overflow it, without changing the deadline or using floats. */
static inline int64_t nv_pacing_timer_units(int64_t ticks, int64_t frequency)
{
    const uint64_t scale = 10000000;
    uint64_t t = (uint64_t)ticks, f = (uint64_t)frequency, units;
    if (ticks <= 0 || frequency <= 0) return 1;
    if (t <= INT64_MAX / scale) {
        units = t * scale / f;
    } else {
        uint64_t whole = t / f, a = t % f, remainder = 0, fraction = 0;
        int bit;
        for (bit = 23; bit >= 0; bit--) {
            fraction *= 2;
            if (remainder >= f - remainder) {
                remainder -= f - remainder;
                fraction++;
            } else remainder *= 2;
            if ((scale >> bit) & 1) {
                if (remainder >= f - a) {
                    remainder -= f - a;
                    fraction++;
                } else remainder += a;
            }
        }
        if (whole > (INT64_MAX - fraction) / scale) return INT64_MAX;
        units = whole * scale + fraction;
    }
    /* Zero would be an absolute date, rather than a future relative wait. */
    return units ? (int64_t)units : 1;
}

#if defined(_WIN32)
static int64_t nv_pacing_qpc(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

static void nv_pacing_wait(HANDLE timer, int64_t frequency, int64_t deadline, int64_t now)
{
    int64_t remain = deadline - now;
    int64_t spin = frequency / 1000;
    if (remain > spin) {
        int waited = 0;
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -nv_pacing_timer_units(remain - spin, frequency);
            if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
                waited = WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0;
        }
        if (!waited) {
            /* Request at most one ms at a time and recheck QPC: Sleep and
             * ordinary timers can wake late on a coarse-resolution system.
             * Failure never makes us spin longer than the original tail. */
            now = nv_pacing_qpc();
            while (now < deadline && deadline - now > spin) {
                Sleep(1);
                now = nv_pacing_qpc();
            }
        }
    }
    while (nv_pacing_qpc() < deadline) YieldProcessor();
}
#endif

#endif
