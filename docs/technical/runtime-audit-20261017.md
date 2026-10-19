# XboxRecomp implementation audit, 2026-10-17

Source audit of the fork's kernel, compatibility layer, D3D8, NV2A, input,
audio and network paths. Findings identify reachable exported/registered
interfaces; they do not claim MM3 exercised every path. Fresh game logs and
gameplay acceptance belong to the consuming game's audit.

## Corrected in this pass

- `MmMapIoSpace` native and guest bridge paths now share owned, independent
  section views of physical contiguous RAM, NV2A/MCPX registers and flash.
  Device apertures retain their canonical state; aliases preserve existing
  PVIDEO/APU/USB/NIC traps and AC97 reset behavior. Unmapping requires the exact
  issued address/extent and never frees canonical memory or another view.
  Protection queries/changes preserve independent per-page rights and traps.
  Windows Release and Linux Release mapping tests pass; Windows also exercises
  actual NV2A model state and repeated AC97 single-step writes. Tests check
  read-only faults, unrelated-address canaries, overlapping aliases, per-page
  changes, guest stdcall dispatch, RAM alias buffer access checks, MMIO alias
  rejection, invalid ranges/protections and concurrent map/shutdown. Existing
  Windows memory regressions pass all seven modes; the existing Linux memory
  layout test passes, including teardown/reinitialization. Native physical
  address checks reject uninitialized and post-shutdown contiguous pointers.
  Current explicit limits: 128 live mappings within the 128 MB alias arena;
  supported protection bases are no-access, read-only and read/write, with
  noncached/write-combine policy flags. Executable, COW and guard requests are
  rejected, as are unmodeled physical address ranges. Linux device-model
  functionality remains limited by the separately listed missing host handlers.
  `MmGetPhysicalAddress` translates owned aliases, the contiguous window and
  ordinary guest RAM into a coherent runtime-virtualized physical namespace.
  Low-RAM IDs start at the next 64 MB boundary after the mapped guest VA range;
  they map back to the same guest RAM bytes, while physical IDs below 64 MB
  resolve to the independent contiguous arena, including backed offsets the
  allocator has not handed out. OHCI, NV2A DMA and APU paths use the shared
  low-RAM resolver while preserving their established allocation/image rules;
  the APU still enforces its 28-bit bus limit. NV2A method parameters retain
  their 32-bit guest value, but wider hardware DMA-field conformance is not
  established.
  This namespace is an internal backing contract, not a claim that the IDs
  reproduce Xbox hardware page numbering. Mapping regressions cover range
  bounds, low-RAM round trips, identical-offset backing independence, an OHCI
  descriptor read/write, bridge buffer protections, concurrent shutdown and
  existing device aliases.
- Non-volatile OS settings now persist a checksummed 96-byte HLE user block
  under the configured save root. Short buffers, unsupported settings, corrupt
  storage and failed writes return errors. Shared reader/exclusive writer locks
  coordinate processes around atomic replacement. Windows and Linux tests cover
  persistence, isolated roots, two writers and a concurrent 5,000-query reader.
  This does not implement encrypted/factory EEPROM storage.
- GPU replay checks all snapshot allocations before copying, releases partial
  storage on failure and permits retry. Real-executor fault injection covers
  all six failed allocations followed by successful save/restore.
- `src/kernel/kernel_crypto.c`: removed unconditional signature acceptance.
  RSA1 little-endian raw RSA now uses the existing `XcModExp`; key length and
  public encryption work. Verification checks the signature range, SHA1 digest,
  complete PKCS1 padding and supported digest encodings. The actual argument
  order is signature, public key, digest (the old parameter names were wrong).
- `src/kernel/kernel_crypto.c`: two-key Triple DES now schedules K1,K2,K1
  from the Xbox's 16-byte key, eliminating a read past its end and correcting
  encryption. Independent OpenSSL ECB/CBC known answers, encryption/decryption,
  feedback and in-place CBC tests pass. A 16-byte key directly before an
  inaccessible page makes the old third-key read fail on Windows and Linux.
- `tests/kernel_crypto`: executable checks include the 2048-bit retail
  signature/digest vector that passed original Xbox hardware, incorrect hash,
  signature and modulus, missing pointers, malformed padding, equal-to-modulus
  signature, invalid key headers, output padding and canary, and all three Xbox
  digest encodings. MSVC Release and Linux GCC Release CTest passed.
  Public RSA also matches the original-hardware 2048-bit Habibi encryption
  vector and an independent Python `pow` result for a consistent 1024-bit blob.
  The hardware suite's additional inconsistent bitlen=1024/keylen=264 case
  produces undocumented behavior; this implementation rejects that geometry
  rather than claiming conformance. Private RSA's CRT blob offsets remain
  unverified: the hardware suite itself says its fixture lacks CRT parameters
  and does not decrypt correctly on hardware. No offsets were guessed.

Primary ABI/vector references:
[Xbox kernel hardware tests](https://github.com/Cxbx-Reloaded/xbox_kernel_test_suite/blob/master/src/tests/xc/suite/PK.c),
[XboxDev key format and padding](https://github.com/XboxDev/xbedump/blob/master/xboxlib.c).
The two-key DES contract is also documented in
[Cxbx's kernel implementation](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded/blob/master/src/core/kernel/exports/EmuKrnlXc.cpp).
The test embeds data; it does not download dependencies at runtime.

## 2026-10-18 execution updates

### 2026-10-19 continuation

- NT file bridges now validate the complete mandatory eight-byte guest IOSB
  before backend calls or output mutation. Shared create/open handling covers
  NtCreateFile, NtOpenFile and IoCreateFile; read/write, information queries,
  directory queries, flush and control requests preflight their IOSBs too.
  NtReadFile/NtWriteFile validate optional eight-byte offsets before reading
  either half. NtUserIoApcDispatcher validates its IOSB before reading it.
  Invalid spans return STATUS_ACCESS_VIOLATION with normal stdcall cleanup.
  The focused fixture rejects null, read-only and split-page pointers; its
  previous-source control fails while independent Windows/Linux builds pass.
  This closes these IOSB/offset accesses, not all guest pointer boundaries:
  object attributes, output handles and several control payloads remain open.

- Guest I/O builders now allocate initialized packets and populate read/write
  and IOCTL arguments instead of returning null packets with successful IOSBs.
  Dispatch enters guest driver callbacks through modeled x86 call frames;
  completion publishes the guest's eight-byte IOSB and signals its event.
  Pending requests retain ownership until completion. Synchronous builder
  packets are released on completion; explicit free removes their ownership
  record. Asynchronous builder and allocated packets remain caller-owned.
  Device queues implement ordered insertion, FIFO/keyed removal and StartIo
  callbacks. Validation rejects duplicate, malformed and cyclic links before
  queue mutation. The post-completion/pre-StartNext gap accepts a new packet
  without dereferencing an already-freed active IRP. Xbox argument counts are
  corrected for ordinals 60, 81, 82 and 83. Independent MSVC Release and
  GCC 13.3 Release thunk-dispatch tests pass, including nested completion,
  event consumption, pending ownership, queue ordering, malformed links and
  more than 256 simultaneous synchronous builder packets.
  This is guest packet/dispatch support, not native host-pointer driver I/O.
  Native unsupported operations return failure rather than fabricated success.
  Synchronous helper ordinals 84/85, IoMarkIrpMustComplete semantics and
  abandoned thread-owned request rundown remain open; no full storage-driver
  acceptance is claimed.
- Stack payload offsets follow original driver instructions retained in the
  consuming project's `build/audit-continuation/reference`: read Length `+4`,
  Buffer `+8`, ByteOffset `+0xc/+0x10`, IOCTL code `+0x10`. The pinned Cxbx
  declaration compiled for x86 places its union at `+8`, contradicting both
  its comments and those driver accesses. Its host record is not copied as
  the guest payload. Raw-byte assertions cover the observed packet fields.
  Static XBE instructions establish ABI expectations, not live reachability
  or original-hardware conformance.

- POSIX thread cleanup now has one finalizer for normal return, `ExitThread`
  and deferred cancellation. It runs timer/APC rundown before publishing
  terminal state and releases the worker reference exactly once. Cancellation
  cleanup unlocks mutexes reacquired by condition waits; normal
  `SleepConditionVariableCS` returns with its critical section held. Native
  pthreads start detached. Focused tests cover exit codes, timer/APC owners,
  suspended cancellation, duplicate handles, immediate close and condition
  wait lock ownership. Warning-clean and UBSan gates pass 3/3; an independent
  GCC 13.3 ASan+UBSan build passes 3/3 on the final source. GCC 11 ASan still
  fails during cancellation unwind with an internal sanitizer CHECK; a
  standalone nested-cleanup reproduction passes, so the cause remains open.
  This is deferred cancellation: CPU-bound guest code cannot be stopped
  promptly until it reaches a cancellation point. It is not full Win32
  `TerminateThread` conformance.
- A native Windows/POSIX probe confirms that POSIX `VirtualQuery` ignores
  page protection and reservation state. A read-only page is reported
  read/write, and reserve-only storage is reported committed. The probe
  passes on Windows and fails on POSIX. Canonical guest-buffer checks also
  currently accept low RAM and contiguous ranges without page-rights checks.
  The subsequent memory repair tracks per-page reservation, commitment and
  protection, returns actual old protections and query extents, and makes
  Linux decommit discard data before zero-filled recommit. Release validates
  exact ownership before unmapping; failed registration rolls a mapping back.
  Mapped read/write/copy access and protection are checked. The POSIX-only
  reservation-carve helper retains stable ownership IDs across fragments;
  layout shutdown releases those fragments without touching mappings created
  in carved holes. The contiguous view uses its matching unmap operation.
  Canonical bridge buffers now share a complete host-page rights walk with
  guest event validation, as well as the existing guest range/alias checks.
  Independent GCC 13 ASan+UBSan platform-memory tests pass; kernel-memory and
  Linux/Windows mapping tests pass. Linking the prior bridge against the same
  mapping fixture fails four canonical rights assertions. The same native
  query probe now passes on both hosts. Existing POSIX layout integration also
  passes. A stale physical-address fixture was corrected only after a clean
  baseline reproduced its four failures; initialized physical backing remains
  covered by the mapping integration test. macOS decommit explicitly reports
  unsupported; it does not claim Linux discard semantics. The registry remains
  bounded at 512 mappings, with explicit allocation failure at capacity.
- The consuming game's owned saved-Cruise pause/resume scenario passed.
  Independently inspected captures show the pause menu and resumed driving;
  pre/post three-second motion samples measured 26.80 and 44.01 units.
  No in-pause movement measurement was obtained. Evidence is under
  `build/audit-continuation/pause-cruise-resume/run-mwbm2quv` in the consuming
  project; this does not validate subsequent runtime edits.

### Earlier validated slices

- IRP allocation and initialization now use fixed-width guest layouts checked
  by compiling pinned nxdk `14d5ee97e73347c973f1f57b68b79ec08c9e77f2`
  headers for `i386-pc-win32`: IRP `0x68`, current stack pointer `0x5c`,
  device `0x48`, device stack count `0x1e`. Ordinal 59 consumes one argument.
  Complete device headers precede extensions; malformed names and invalid
  initialization ranges fail without corrupting adjacent memory. Windows and
  Linux Release dispatcher tests pass, covering initialized fields, stack
  cleanup, free accounting, invalid packet preservation and device queue/lock
  fields. This is allocation/initialization support; builders, guest driver
  callbacks and completion still need actual implementation.
- Directory enumeration now bounds fixed fields and partial names, retains
  an overflowing entry for retry, distinguishes initial overflow from later
  too-small queries, captures the mask once and retains EOF until restart.
  Queries and close share enumeration ownership under one initialized lock.
  POSIX metadata errors propagate instead of reporting fabricated zero data.
  Windows native directory and renderer controls pass 6/6; the Linux directory
  integration test passes 1/1. Minimum size `0x48` and filename offset `0x40`
  match the compiled nxdk layout; Windows native probes also checked partial
  retry behavior. Full original-Xbox filesystem conformance remains unproven.
- POSIX waitable timers now repeat and deliver APCs on the arming thread in
  alertable waits. Relative deadlines use monotonic time; cancellation retains
  an expired signal and removes pending APCs. Native Windows probes confirmed
  callback delivery after last-handle close. Tests cover queue saturation,
  coalescing, normal armer exit, rearm to an earlier deadline, zero-time
  alertable waits and extreme relative values. Three registered tests pass
  independently under ASan/UBSan. Cooperative servicing has bounded polling
  latency; power resume reports `ERROR_NOT_SUPPORTED`, and forced
  `TerminateThread` timer-owner rundown remains open.
- The final consuming Windows build passed extended fresh-profile driving
  (72.797 s) and saved-profile Cruise with draw-every-frame plus uncapped
  pacing (56.328 s, 95.05 m player motion). Captures were inspected. Start
  presses did not visibly open pause UI, so pause/resume remains unverified.
  These runs do not prove POSIX gameplay, DSP/audio fidelity or general
  render-queue ownership. Logs/captures and the independently repeated pinned
  nxdk compiler probe are retained under the consuming project's
  `build/audit-continuation`.

- The two I/O builder bridges now marshal native IOSBs into the guest's two
  32-bit fields using the existing helper. Directly casting guest memory to
  the 16-byte host structure previously overwrote memory after the guest's
  8-byte IOSB. Actual dispatcher tests pass on MSVC and Linux Release; linking
  the old bridge into the same test fails both field and adjacent-canary
  checks for both builders. Null IOSBs and stdcall cleanup are covered. The
  stale ordinal-173 fixture now expects rejection of an uninitialized alias,
  consistent with the existing mapping contract. This fixes memory corruption,
  not the missing IRP/driver semantics. The shared guest IOSB writer still
  needs full-span write validation and an explicit caller error policy.

- POSIX wait-all now locks all participating objects in stable address order,
  checks readiness, then acquires them together. Failed waits preserve event,
  semaphore, mutex and auto-reset timer state. Single waits share the same
  readiness/acquisition helpers; infinite waits no longer copy an uninitialized
  deadline. The standalone `tests/platform_wait_all` regression fails against
  the old source at its event-preservation assertion. Ubuntu Release and
  ASan/UBSan CTest pass; independent WSL `mm3-gpu-test` Release CTest passed ten
  consecutive runs. Delayed acquisition, opposite lock orders and wait-any
  index ordering are included. Periodic timers, APC behavior, abandoned mutexes
  and pseudo-handle semantics are outside this correction.

- The physical-address mapping gates pass on Windows and Linux. Fresh main
  runtime driving completed in 54.584 s and 62.7096 s with normal gameplay
  captures. The SDL runtime also completed a 53.0497 s driving run after the
  mapping and sampler-lifetime fixes, with a normal capture; its visible
  launcher smoke passed at 760x720 and closed normally. These runs establish
  playable-path integration for the tested build and scenario, not complete
  physical-address or device conformance.
- The SDL sampler-cache selector regression passes on Windows and Linux. The
  Linux test compiles with `-Wall -Wextra -Werror`; the `--legacy` control
  fails because it evicts a sampler pinned by the active draw. The test models
  the mid-setup flush and subsequent bind; the fresh SDL driving run supplies
  the integration evidence for the actual renderer path.
- Pushbuffer replay now captures the full frame-start GPU state on the
  executor thread, loads it for replay, then restores the in-progress next
  frame. Real-executor regressions pass for inherited clear color, next-frame
  restoration, actual `NV097_FLIP_STALL` boundaries, activation after a frame
  starts, disable/skip/re-enable, timeout during vertex capture, and recording
  allocation failures. A deterministic clock test covers an even tick and
  the 250 ms cutoff. These tests establish the state and rejection contracts;
  they do not establish general visual interpolation correctness. The main
  runtime's old-generator smooth+uncapped run passed at 63.0004 s with three
  normal captures. The replay-fixed refined candidate also passed smooth+uncapped
  in main (`run-e46_z31k`, 3 normal captures) and SDL (`run-3y069j9u`, 3 normal
  captures showing normal geometry and driving/AI motion). These bounded runs
  are native gameplay evidence for the tested scenario, not broad interpolation
  correctness. Logs and captures are archived at
  `C:\Users\Matt\Documents\MM3-Recovery-20261017\Audit-evidence-20261018`.
- Refined CFG generator commit `34330f4ae0d2c7dcea1d1173c202f18620bd3291` is
  now the MM3 generator pin. It passed the replay-fixed main and SDL
  smooth+uncapped comparisons. Earlier one-off distortion remains
  unexplained; repeat candidate and old-generator controls were visually
  normal. The candidate generated 26 dead-branch guards and leaves 345
  unresolved functions, with none removed from baseline. This bounded evidence
  does not establish interpolation correctness across all scenes.

## Remaining implementation gaps

| Priority | Source | Evidence / correct implementation | Regression needed |
|---|---|---|---|
| High | `src/kernel/kernel_memory.c`, `xbox_MmGetPhysicalAddress` | Runtime physical IDs are now coherent across ordinary RAM, contiguous storage, OHCI, NV2A DMA, APU reads and `MmMapIoSpace`. The IDs intentionally virtualize the physical namespace to avoid collisions between separate host backing sections; hardware-accurate Xbox page numbering and DMA masks above the supported consumer width remain open. | Compare representative physical-page numbering, bus-visible RAM and DMA field widths with hardware or an authoritative emulator trace before claiming Xbox physical-address conformance. |
| High | `src/kernel/kernel_crypto.c`, `xbox_XcPKDecPrivate` | Returns failure for every key. Implement the Xbox private blob layout and actual private RSA operation; do not substitute unconditional success. | Independent private/public roundtrip and invalid blob tests; real hardware vector where available. |
| High | `src/kernel/kernel_crypto.c`, `xbox_XcUpdateCrypto` | Discards both vectors; titles cannot install crypto implementations or retrieve originals. Requires guest-function dispatch in the bridge, not calling guest addresses as host pointers. | Install one replacement, verify only that function changes, retrieve originals, null entries preserve current replacements. |
| High | `src/kernel/kernel_io.c:149-303`, `src/kernel/kernel_bridge.c:5305-5416` | Completion, synchronous I/O and driver dispatch exports return success without doing I/O or completing an IRP. Builds are not a substitute for guest driver behavior. | Real IRP layouts, queued dispatch/completion and events, transfer counts/status, invalid devices. Trace actual callers before choosing device implementations. |
| Medium | `src/platform/win32_compat.c`, timer/thread lifecycle | Periodic timers and alertable APC delivery are implemented and tested. Forced `TerminateThread` still uses cancellation without timer-owner rundown; power resume is unsupported. | Forced owner cancellation while armed/queued, without leaks or stale callbacks; normal-exit tests do not prove this path. |
| Medium | `src/platform/win32_compat.c`, host waits | Wait-all signal loss is corrected and covered by `tests/platform_wait_all`. Abandoned mutexes and pseudo-process/thread handles still need separate semantic investigation. | Compare owner-thread exit and current-process/current-thread waits against the host API; the focused acquisition test does not establish those behaviors. |
| Medium | `src/platform/win32_compat.c:1948-1964` | POSIX exception helpers are inert; `RaiseException` logs then returns. Guest RtlUnwind has a separate bridge implementation, so do not confuse these two paths. | Host shim noncontinuable dispatch/unwind tests and guest bridge exception tests independently. |
| Medium | `src/input/xinput_device.c:533-551` | POSIX controller discovery runs only at initialization. Late attach/replacement cannot acquire a usable slot; rumble discards SDL failure. | Mock controller attach/disconnect/reconnect and failed rumble propagation. |
| Medium | `src/d3d/d3d8_device.c:406-429` | GetDeviceCaps/GetDisplayMode/GetCreationParameters return success without output; Reset does nothing. COM vtable routes to these implementations. | Fill outputs with poison, call public interfaces and validate real data; resize/reset then render and read back. |
| Medium | `src/nv2a/nv2a_pgraph_d3d11.c:614-633` | Legacy PGRAPH translator acknowledges ignored transform, vertex, texture, combiner and flip methods. `nv2a_core.c:556` returns early for acknowledged methods. Separate from the primary pushbuffer executor. | Run commands changing actual rendering state and inspect pixels/fence behavior; establish whether consuming target links this legacy path. |
| Medium | `src/net/net_backend.c:611-615`, `src/net/nvnet.c:507-511` | POSIX networking has no backend/NIC implementation. Windows NAT handles UDP only (`net_backend.c:421`), dropping TCP/ICMP. Missing protocols are documented, not implemented. | ARP/DHCP/DNS and bidirectional UDP on POSIX; TCP handshake/data and ICMP response when implemented. |
| Medium | `src/apu/apu_xaudio2.c:16`, `src/apu/apu_core.c:332` | Main's POSIX APU has no active host audio backend. Windows-only XAudio2 returns inactive; waveOut compatibility fallback is also inert. SDL branch may supply a different backend; verify branch independently. | Submit known nonzero PCM through actual POSIX output backend and capture produced samples; DSP arithmetic tests alone cannot prove audible output. |
| Medium | `src/kernel/kernel_hal.c:576-603`, `:658-675` | PCI reads return zero and writes disappear; software interrupt request/clear and interrupt disable are no-ops. Some bridge interrupt paths implement their own machinery, so trace each path before replacing it. | PCI device identity/state and register writes; request/clear interrupt with delivery at correct IRQL. |

## Diagnostics and test boundaries

- `src/nv2a/nv2a_state.h:33` defines an empty `NV2A_UNIMPLEMENTED`, but the
  indexed source search found no call sites; deleting this macro alone fixes
  no observed behavior. Disabled verbose logging is not inherently a failure.
- `src/nv2a/nv2a_core.c:666-682` stub MMIO blocks return zero/discard writes
  with debug-only logging. Their registration table names the missing blocks;
  investigate accesses from logs before implementing individual hardware units.
- `src/kernel/kernel_io.c` unsupported-control-code logging retains first
  occurrences and exit totals under a lock. This is useful bounded diagnostics,
  not a blanket silencer. A genuinely unsupported FATX operation may correctly
  return an error rather than inventing success.
- Resident section bookkeeping, host shutdown on return-to-dashboard, and
  page-lock no-ops for permanently resident user memory can be valid host
  replacements. Their comments alone do not establish a missing feature.
- `tests/kernel_bridge/test_main.c` exercises only a small explicit set of
  ordinals; bridge-routing tests prove routes and stack sizes, not semantic
  implementation of all 371 exports.
- `tests/kernel_audio_setting/test_main.c` verifies only the supported audio
  read. It cannot detect dropped writes, short buffers or unknown indexes.
- `tests/d3d8_smoke/test_main.c` intentionally stubs device accessors and checks
  format conversion. It does not verify device getter/reset behavior.
- `tests/audio_mixer/run.py` executes actual mixer code with assertions enabled
  using its fixed GCC command. Its lack of `NDEBUG` is deliberate; do not claim
  the checks disappear in its documented execution path.

Coverage remains incomplete: shader correctness, all unsupported NV2A blocks,
complete NT object/file classes, DSP instruction/timing semantics and live
multi-console network behavior need dedicated execution evidence. This audit
does not claim all implementation gaps are resolved.
