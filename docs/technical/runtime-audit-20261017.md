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
| Medium | `src/platform/win32_compat.c:980-1013` | POSIX `SetWaitableTimer` ignores period and completion APC but returns success. | Two observed expirations, cancellation/rearm, manual/auto reset, alertable callback execution. |
| Medium | `src/platform/win32_compat.c:550-557` | POSIX wait-all consumes auto-reset/semaphore/mutex signals sequentially before knowing every object is ready. Timeouts lose signals. | Set only first auto-reset event: wait-all times out and first event remains set; then atomic all-object acquisition succeeds. |
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
