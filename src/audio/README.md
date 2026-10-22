# xbox_dsound: partial DirectSound host adapter

The buffer adapter delegates 16-bit PCM playback to the shared APU software
mixer. The APU chooses the host output backend. These host interfaces are not
proof of complete Xbox SDK ABI or DirectSound feature coverage.

## Implemented buffer paths

- `CreateSoundBuffer` allocates optional storage and a mixer voice. Its default
  format is 16-bit stereo at 44.1 kHz; a supplied format sets channels/rate.
- `SetBufferData` copies replacement PCM, stops playback and resets the cursor.
  Passing no data clears the buffer.
- `Lock` exposes one linear region of allocated storage; `Unlock` is a no-op.
  Storage filled this way is attached to the same mixer as `SetBufferData`.
- `Play`/`Stop` control playback and looping. Position/status methods report the
  software mixer's byte cursor and playing/looping flags.
- Volume and frequency setters update mixer gain and sample rate.
- Releasing the last buffer reference stops/frees its voice and storage.

See [dsound_xbox.h](dsound_xbox.h) for declarations and
[dsound_device.c](dsound_device.c) for the implementation.

## Partial methods

Streaming is not implemented: `CreateSoundStream` returns no stream object.
Play-region, listener/spatial, cone, mixbin and deferred-setting methods are
compatibility no-ops. `Lock` does not implement circular wraparound. The buffer
mixer path described above handles 16-bit PCM, not arbitrary wave formats.

## Focused regression

With GCC available, run from the toolkit root:

```sh
python tests/audio_mixer/run.py
```

The test compiles actual mixer code and the DirectSound adapter. Synthetic
mono/stereo samples check both storage-creation paths, cursor/status, looping,
stop/resume, replacement/clearing, release and long-buffer positions. The fixture
stubs APU wakeups/locking and uses no audio device; it does not establish host
output, concurrency, spatial audio or game-audio fidelity.
