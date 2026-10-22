# Kernel dispatch regressions

No game files are required. The test allocates synthetic guest memory and imports
KeGetCurrentIrql and KeStallExecutionProcessor. It forces a lookup on another
thread between the first lookup and its invocation. A shared dispatch slot
selects the wrong service and pops an extra argument; per-thread storage passes.

The handle test dispatches actual event and file imports across twelve workers
and thirty-two synchronized cohorts. Registrations overlap removals of distinct
objects; every live token and native handle stays unique, each event signals
only its intended object, and each file returns its own payload. Native handle
counts stay level and return after closure. Separate cases cover concurrent
registration of one native handle, a single successful concurrent take, 64-bit
handles, raw and pseudo handles, invalid tags, full-table ownership and reuse.
Only registration and removal hold the table's writer lock; resolution uses a
native atomic pointer load, and no native wait or I/O runs under that lock.

The log tests dispatch 20,001 calls across three threads and wait beyond the
summary interval. They verify that a zero budget stays quiet, a three-call
budget emits exactly three unique trace lines, and `RECOMP_KERNEL_SUMMARY=1`
retains accurate process-wide totals. Normal gameplay does not collect kernel
summary counters or poll the summary clock after its boot trace budget ends.

The sharing test opens an exclusive host file, verifies that the runtime
returns `STATUS_SHARING_VIOLATION` consistently in the return value and I/O
status block, and checks both native and guest-bridge DOS error conversion.
The same open succeeds after the exclusive host handle closes. Two guest
read-only opens with zero share access also succeed together for system link,
while writers remain exclusive and real host sharing denials still propagate.
It also interleaves a missing-file open on a streaming worker with the sharing
conflict and a later successful open, verifying each thread's diagnostic error
stays tied to its own call. A malformed guest name rejected before host I/O
reports no Win32 error rather than inheriting the earlier sharing conflict.
A zero-byte stream read retains `STATUS_END_OF_FILE`,
zero bytes transferred and unchanged destination data.

The critical-section test races lazy initialization across four threads,
checks recursion and mutual exclusion for forty colliding table/cache keys,
and verifies that bridge TryEnter and Leave use the same shadow as native
Enter/Leave. Guest critical-section bytes remain unchanged. It also reports
the cost of independent lock pairs without using timing as a pass condition.
The default test also rejects disabled counter output; a second run with
`RECOMP_CS_COUNTERS=1` verifies the optional contention totals are reported.
`RECOMP_CS_TRACE_CRT` and `RECOMP_CS_WATCH` also enable those accurate counters.

The GPU notification wait test uses the runtime's exact host helper without
starting a renderer. It checks completion before waiting, a completion injected
between the predicate check and native wait, GET/NOP/shutdown wakeups, spurious
wakes, concurrent guest waiters, unavailable-API fallback, and guest register
changes without an executor notification. Host waits are bounded to 1 ms;
only the original guest completion conditions can end the wait.

The direct fence test preserves the title's unsigned subtraction/comparison,
including wrap boundaries and a completed value ahead of the submitted value.
It also checks completion before waiting, a publication in the check-to-wait
interval, wakeups without completion, shutdown, and timeout/API fallback. The
host helper never modifies the guest fence value.

The contiguous-range cache test uses real allocations, frees, splits and
coalescing. Positive thread-local hints retain only bounds proven covered by
the allocation table; misses always check the locked table. Cases include
adjacent allocations, holes, physical aliases, range overflow, cache collisions,
and allocations made by another thread after an earlier miss. Freeing a block
does not erase its permanent physical ownership or invalidate that coverage.

The command-fence test dispatches actual NV097 semaphore methods through the
executor. `xbox_Nv2aCommandFence` configures an existing fence mirror before
guest entry: only the matching DMA context, semaphore offset and executed
release value can complete it. The next guest counter cannot complete work
during startup or an idle tick. Releases retain their targets at execution,
preserve ordering across target changes and unsigned wrap, and wait for prior
readbacks and a fully consumed GET checkpoint. Incomplete parser passes retain
their releases. Other titles keep the generic counter-snapshot mirror behavior.
The native-heap allocation fallback is tested without an early fence, GET,
notification or GPU-readback publication.

The DMA synchronization test exercises the real publication and completion
functions. `xbox_Nv2aDmaPublish` writes the owned PFIFO PUT and PGRAPH busy state
before exposing USER PUT. The first publication adopts the actual USER GET;
later completions advance USER/PFIFO GET to their captured consumed boundary.
A brief shared lock prevents an older completion from clearing a newer PUT's
busy state. Publishing an already completed PUT stays idle, while a software
channel keeps its original store. `xbox_Nv2aDmaWait` waits only for the original
CACHE1 PUT/GET equality and zero PGRAPH status, with the original final guest
reads retained. Tests cover concurrent stores, older completions, raw register
updates, unknown apertures, shutdown and completion before native registration.

Matching NOPs are recorded during execution without waking the guest. Their
exact notification sequence is published after the corresponding consumed GET
and prior readbacks. A held older GET retains its own sequence, so its release
cannot expose a NOP from a newer incomplete pass. Tests assert both GET stores
at notification wake time, a newer live PUT, incomplete-pass deferral and
notification-count wrap. Native notification, fence and DMA waits snapshot the
wake generation before rechecking their predicates; a wake alone never changes
the guest completion rules.

The pushbuffer hitch test feeds increasing/non-increasing packets, a CALL and
RETURN, and a jump through the real decoder with tracing both off and on.
Deterministic clocks verify a slow draw is identified separately from normal
flip pacing, while quiet mode reads no diagnostic clocks. Commands and their
parameters arrive unchanged and in their original order.
It also splits increasing and non-increasing payloads across multiple PUT
updates, including values that resemble CALL/JUMP/RETURN headers. Further
cases cover real ring jumps, called lists and root payloads crossing the
64 MiB physical boundary, discontinuous scans, and empty packets.
Called lists stop at the actual PUT even before a header or within its payload,
and later publication resumes the saved return address. Reserved header bits,
exact RETURN decoding and method-address wrapping follow
[xemu's PFIFO implementation](https://github.com/xemu-project/xemu/blob/master/hw/xbox/nv2a/pfifo.c).
Work-limit tests stop in root and called payloads, report incomplete consumption,
and resume once without duplicating commands, including a larger PUT on retry.

The `nv2a-pb-capture` case checks the opt-in `RECOMP_PB_CAPTURE=<path-prefix>`
diagnostic. On the first parser work-limit hit it writes these version-1
artifacts, then stops recording:

| Suffix | Contents |
| --- | --- |
| `.json` | Parser start/end, saved packet/CALL state, record sizes/counts, ring bounds and individual write-success flags. |
| `.reads.bin` | First 1,048,576 words fetched in that pass, before executor callbacks; each 24-byte record contains six little-endian `uint32_t` fields: `va`, `word`, `method`, `remaining`, `flags`, `return_va`. Flag 1 marks a payload and flag 2 marks an active CALL. |
| `.recent.bin` | Last 256 fetched records in chronological order, using the same schema. |
| `.ram.bin` | The 64 MiB contiguous RAM window at the limit hit; guest address `0x80000000` is byte zero. |
| `.initial-ring.bin` | Configured producer ring copied before the pass, including its boundary JUMP word; `initial_ring_va` and `initial_ring_bytes` locate it. The copy is limited to 4 MiB. |
| `.state-initial.bin`, `.state-final.bin` | `XBOX_NV2A_CAPTURE_STATE` snapshots before execution and at the limit. |
| `.producer.json` | Named initial/final USER/PFIFO pointers, held GET, notification state and registered fence/device fields. |

The state snapshots are zero-initialized, read-only diagnostic structures from
`xbox_memory_layout.h`. Version 2 is 35,320 bytes: an 18-word header followed by
four mirrors, each with 27 metadata words and a `0x2200`-byte raw device block.
The header carries the version and total byte size. Validity flags and
`device_bytes` distinguish unavailable data from a real zero. Registered device
pointers supply the roots; the helper does not hardcode an image global.
Configure `xbox_Nv2aCaptureLayout` before guest execution to decode the port's
channel, PFIFO pointer and inclusive ring bounds. With no layout, these fields
remain unavailable; raw device and hardware state capture still works. Version 2
adds `ring_first`, `ring_last` and `ring_valid` to each mirror. The JSON contains
these generic fields; device-layout-specific cursor/fence decoding belongs in
the port's diagnostic tools. Old version-1 readers must reject the new size/version.
The snapshots and ring copies do not suspend the producer and are not atomic;
the fetched-word ledger is the evidence of what execution actually read.

Capture mode allocates and copies extra memory and can write roughly 89 MiB
at its first limit, so use it for an isolated diagnosis rather than performance
measurements. With capture disabled there are no capture allocations, extra RAM
copies, snapshot calls or file writes. The regression verifies pre-callback
word preservation, lengths/schema and unchanged host LastError, then removes
only its own artifacts.

Timer lifecycle cases cover signal publication before callbacks, rearming,
retired timer storage, periodic cancellation, and absolute/relative deadlines.
The `audio_deadline` case checks a future-deadline rearm loop: one action
must require one callback, rather than running again on every polling tick.
DPC cancellation cases exercise wrapped FIFO order, duplicate rejection, and
1,000 insert/cancel cycles without losing queue capacity.

The lower-IRQL cases require pending completions to run before the lowering
thread resumes object destruction. They check register, x87, MMX, XMM and host
floating-point state preservation, nested callbacks, the DISPATCH boundary,
and DPC-active state. Callback frames use an independent guest stack; a USB
request-shaped sentinel below the interrupted stack pointer must survive.
Stack-pool exhaustion is also exercised. Thread-layout cases verify that the
full KPCR, including `FS+0x58`, cannot alias the title's TLS and that a new
thread starts with clear IRQL and DPC state.

Async-wait trace cases compare real success, zero-timeout and invalid-handle
outcomes through the dispatcher with native HLE outcomes. They verify register,
x87, stack and host LastError preservation; guarded reads from uncommitted
memory; separate ordinary/failure budgets per wait kind; explicit
trace-disable precedence; and unchanged default behavior for title-built
KEVENTs. The inline case explicitly enables that existing option for the test.
The log snapshots 12 raw dwords at each of ESI and EBX with guarded reads;
`esi_read`/`ebx_read` report readability. It does not select or decode a title
request packet from its return PC.

The inline-event caller cases exercise the public startup-only
`xbox_kernel_set_inline_event_callers` API with synthetic setter and
wait return PCs. The global `RECOMP_TITLE_KEVENTS` option stays disabled.
Real native completion workers pass through the kernel dispatch bridge,
including completion before the first wait and completion while it blocks.
A request-shaped sentinel must remain intact until completion. The timeout
case uses a 50 ms timeout and subsequent cancellation wait; a negative
control reproduces the old invalid-handle return and released request storage.

Further cases check notification versus synchronization signal consumption,
unrelated callers at an already shadowed address, ordinary initialized events,
invalid headers and uncommitted/read-only pointers, repeated stack reuse with
event-type changes, full shadow-table behavior, host handle counts, and
concurrent first registration. No title assets or game process are required.

On Windows with Visual Studio:

```
cmake -S tests/kernel_dispatch -B build/kernel-dispatch -A x64
cmake --build build/kernel-dispatch --config Release
ctest --test-dir build/kernel-dispatch -C Release --output-on-failure
```

Build all targets before running CTest so the timer, DPC and thread-layout
regressions are included.


`guest-vmem-prefix` uses real host mapping blockers. A blocker at the end must
leave the free prefix usable; a blocker at the start must make fixed requests
fail without changing their address, report unavailable space as reserved, and
leave foreign mappings intact through shutdown. The fixture links the platform
library directly, including its transitive POSIX math dependency.
