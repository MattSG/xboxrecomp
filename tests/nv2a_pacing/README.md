# NV2A display pacing regressions

Build from the parent MM3 checkout with its Windows MSVC environment:

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
python scripts/devenv.py --shell
cmake -S tools/xboxrecomp/tests/nv2a_pacing -B conformance_tmp/pacing-test -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build conformance_tmp/pacing-test
ctest --test-dir conformance_tmp/pacing-test --output-on-failure
```

The four cases exercise the production `nv2a_pacing.h` helper without a GPU:

- `cadence`: 60,000 comparisons with the original deadline calculation at six rates, including early/exact/late frames, disabled pacing, and signed-overflow rebasing.
- `failures`: injected high-resolution and ordinary timer creation failures, arm/wait failures, coarse or late wakes, and successful recovery. Every fallback sleep requests one millisecond, the final spin stays within the original one-millisecond tail, and completion cannot precede the deadline.
- `conversion`: 60,000 exact QPC-to-relative-timer comparisons and overflowing-product, saturation, and sub-100-nanosecond cases.
- `native`: actual Windows waits through both the original and new helpers, with matching cadence calculations and no early completion.

Successful timer waits retain the existing one-millisecond tail and deadline cadence. A late frame still completes immediately and starts a new cadence. High-resolution timer creation now falls back to an ordinary timer; unavailable or failed timers use `Sleep(1)` with QPC rechecks instead of spinning for the full remaining frame. Ordinary timers and sleeps can wake late on systems with coarse scheduling resolution. The fallback bounds the requested sleep and spinning work; it cannot guarantee a one-millisecond actual wake latency.

## Rejected shorter tails

An isolated Windows experiment on the Ryzen 9 PRO 7940HS compared three interleaved 120-frame blocks per policy at 60 Hz, both idle and with four normal-priority CPU workers on 16 logical processors. It changed neither process priority nor affinity. Fixed 250/500-microsecond tails and an adaptive 125–500-microsecond tail lost deadline precision, so they were rejected.

A follow-up compared the original tail against an adaptive 125–1000-microsecond tail, initially using one millisecond and then the largest of the last 16 timer wake latenesses plus 75 microseconds. It reduced measured thread cycles by 26.2% at idle and 57.8% under the controlled load. However, idle p99 deadline error rose from 0 to 136.7 microseconds, and errors above 100 microseconds rose from 1 to 7 of 360 frames. Loaded p99 errors were zero for both. This precision tradeoff was rejected; no adaptive pacing ships. Short-block `GetThreadTimes` values showed coarse accounting effects, so cycle reductions are not claimed as exact CPU milliseconds or gameplay performance gains.

## Microsoft API references

- [CreateWaitableTimerExW](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw): high-resolution timers are supported from Windows 10 version 1803; ordinary timers remain available when that creation fails.
- [SetWaitableTimer](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-setwaitabletimer): negative due times specify relative 100-nanosecond units; the helper uses a one-shot timer.
- [WaitForSingleObject](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject): only `WAIT_OBJECT_0` confirms timer completion; other results take the fallback.
- [Sleep](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-sleep): wake accuracy depends on clock resolution and subsequent scheduling.
- [QPC guidance](https://learn.microsoft.com/en-us/windows/win32/sysinfo/acquiring-high-resolution-time-stamps): QPC measures intervals independently of wall-clock changes, with a frequency cached at initialization.
- [QueryThreadCycleTime](https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-querythreadcycletime): thread cycles include user/kernel execution and must not be converted directly to elapsed time.
