# MCPX APU support

The shared APU contains VP voice processing, guest-uploaded GP/EP DSP execution,
MMIO dispatch, a host PCM mixer and output backends. These are partial hardware
and compatibility implementations, not a claim of full audio fidelity.
See [source provenance and licenses](dsp/PROVENANCE.md).

## Interfaces

- [apu.h](apu.h): initialization/shutdown, offset-based MMIO, software mixer.
- [apu_regs.h](apu_regs.h): register constants and masks.
- [apu_state.h](apu_state.h): internal VP/GP/EP state.
- [apu_mmio_hook.c](apu_mmio_hook.c): selected x86-64 trapped access forms.
- [apu_xaudio2.h](apu_xaudio2.h): native output submission contract.
- [dsp/dsp.h](dsp/dsp.h): interpreter/frame state interfaces.

A port supplies physical guest RAM to mcpx_apu_init_standalone and checks for
NULL before using the result. Shut down the owned APU before releasing its RAM.
MMIO addresses passed to mcpx_apu_mmio_read/write are offsets from the device
base, not host pointers. The exception hook supports selected instruction forms;
unsupported instructions must not be assumed to execute correctly. See the
actual decoder and tests/apu_mmio for the exercised cases.

The software mixer is a separate PCM path that bypasses hardware VP processing.
Check voice allocation and pointer results before configuring a voice. PCM storage
must remain valid while the mixer uses it; stop/free the voice before releasing
that storage. The monitor test tone also bypasses hardware voices and does not
verify guest voice or DSP processing.

## Output and diagnostics

apu_xaudio2.c uses XAudio2 on Windows and an SDL3 audio stream elsewhere.
Output is interleaved signed 16-bit stereo at 48 kHz. Submission accepts
1..xa2_get_buffer_size() stereo frames: 1 means accepted, 0 means queue full,
and -1 means invalid input or backend failure. Callers handle output fallback.
Compatibility SDL/resampler declarations in apu_shim.h are separate from this
native output implementation and do not establish resampling/output fidelity.

RECOMP_AUDIO_MUTE controls initial output gain. RECOMP_XA2_PCM_DUMP optionally
writes accepted source PCM, excluding underrun padding; it does not record the
physical device output. RECOMP_APU_TRACE logs bounded trapped register traffic
without additional device reads. tests/xaudio2 covers native queue contracts with
synthetic XAudio2 operations on Windows or SDL's dummy audio driver on Linux.
Those checks do not establish physical endpoint timing, full MCPX scheduling,
DSP instruction conformance or arbitrary-game audio correctness.
