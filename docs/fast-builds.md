# Fast generated-function builds

XboxRecomp already emits generated functions in configurable chunks via
`tools.recomp --split`; each chunk is a separate translation unit, so changing
one chunk does not recompile the others. The consumer template now uses
`CONFIGURE_DEPENDS`, so newly generated chunks are picked up after regeneration
without hand-editing CMake.

Measure a consumer's real clean and incremental paths with:

```powershell
python -m tools.fastbuild_benchmark --clean "cmake --build tools/builds/mm3 --config Debug --clean-first" --incremental "cmake --build tools/builds/mm3 --config Debug"
```

The benchmark is intentionally command-based because only the consumer knows
its generator, compiler, and build directory. No linker override or fragile
patch library is introduced.

Baseline native library timing (MSVC 19.50, Visual Studio 18 generator,
parallel 2, this branch, 2026-08-25): Release first build after Debug had
already populated the configure tree: 2.45 s; unchanged Release rebuild:
0.43 s. These are library timings, not a generated MM3 function rebuild.
