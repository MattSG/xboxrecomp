# Xbox D3D8 compatibility layer

The D3D8 object interface is declared in [d3d8_xbox.h](d3d8_xbox.h).
Its Windows implementation uses D3D11; the POSIX implementation uses OpenGL
through SDL3 and libepoxy. The NV2A push-buffer executor and its presentation
backends have separate interfaces under src/kernel and src/video.

## Implementation map

- [d3d8_device.c](d3d8_device.c): Windows device and draw entrypoints.
- [d3d8_resources.c](d3d8_resources.c): resource wrappers and format conversion.
- [d3d8_states.c](d3d8_states.c): render/sampler state translation.
- [d3d8_shaders.c](d3d8_shaders.c): fixed-function shader/input layout support.
- [d3d8_combiners.c](d3d8_combiners.c): register-combiner translation.
- [d3d8_vsh.c](d3d8_vsh.c): vertex-program translation and caching.
- [d3d8_gamma.c](d3d8_gamma.c): presentation gamma transform.
- [d3d8_gl.c](d3d8_gl.c): POSIX object-interface backend.
- [d3d8_swizzle.h](d3d8_swizzle.h): CPU layout and compressed-block helpers.
- [d3d8_fvf.h](d3d8_fvf.h): FVF position-field classification.

Create an interface with xbox_Direct3DCreate8, check the returned pointer, then
call its CreateDevice with initialized presentation parameters and a suitable
host window. Check HRESULTs and output pointers before using resources.
Follow each interface's ownership/refcount and lock/unlock rules. The port owns
its guest call adaptation, window lifetime and application presentation policy.

Format names, enums and mappings are implementation declarations, not proof
of complete hardware format equivalence. Some conversions are approximate.
A successful resource allocation or shader compile alone does not establish
correct sampling, state preservation or presentation. Inspect the actual
conversion/backend and relevant tests for a particular format or shader path.

## Focused checks

The standalone tests/d3d8_smoke CMake project uses production sources. On
Windows it includes CPU format/layout checks and D3D11 checks for gamma,
bound states, alpha/palette sampling and vertex-program handling. Some use D3D11's
software device. The POSIX test needs a working GL context/display.

Run from the toolkit checkout:

```sh
cmake -S tests/d3d8_smoke -B build/d3d8-checks
cmake --build build/d3d8-checks --config Release
ctest --test-dir build/d3d8-checks -C Release --output-on-failure
```

These are scoped regressions. They do not prove complete guest ABI translation,
all format/shader combinations, parity between backends, or playable gameplay.
