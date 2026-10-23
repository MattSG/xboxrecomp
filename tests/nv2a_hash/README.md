# Exact texture hash execution

The production header preserves the original four-lane 64-bit hash, including
seeds, length, byte order, wrapping multiplication, shifts, scalar tail and
final combination. Supported x86 Windows CPUs execute the same four independent
lanes together with 256-bit native integer multiplication for inputs at least
1024 bytes long. Short inputs, including 216-byte shader keys, retain the scalar
path. Texture validation still reads all supplied bytes at the same frequency.

Three CPU regressions use the actual production header and an independent copy
of the original scalar implementation:

- `exact`: all lengths 0 through 8191 at all 64 source alignments, seven larger
  sizes up to 8 MiB at all alignments, plus all-zero and all-one patterns.
- `pages`: all lengths 0 through 8192 ending immediately before an inaccessible
  page, a full protected 64 KiB allocation, and unaligned starts. Every 32-byte
  chunk and scalar tail stays inside the requested input.
- `features`: each missing CPUID or XCR0 prerequisite forces the real dispatch
  to the scalar path. XSAVE/OSXSAVE/AVX are checked before XGETBV; unsupported
  dispatch is cached, and short keys perform no feature queries. The real CPU
  selection is checked too.

The exact and page cases cover 566,882 inputs. On a supported CPU each compares
both the dispatcher and native helper against the original scalar result.

From the game repository after loading `python scripts/devenv.py --shell`:

```powershell
cmake -S tools/xboxrecomp/tests/nv2a_hash -B conformance_tmp/stutter-hash-shipping-test -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build conformance_tmp/stutter-hash-shipping-test
ctest --test-dir conformance_tmp/stutter-hash-shipping-test --output-on-failure
./conformance_tmp/stutter-hash-shipping-test/nv2a_hash.exe benchmark
```

The optional benchmark compiles the production header at thresholds 1024, 2048
and 4096, including cached feature dispatch and the noinline native helper. It
alternates five implementations over nine samples, changes a source byte and
consumes every returned hash. It reports per-call and whole-sample medians;
timing is not a CTest assertion. RelWithDebInfo uses the shipping renderer's
`/Zi /O2 /Ob1 /DNDEBUG -std:c11 -MD`, with no global `/arch` option.

On the Ryzen 9 PRO 7940HS, the real CPU/OS guard selected the native path. The
shipping-flags run selected 1024 bytes: its 1 KiB median was 47.46 ns versus
52.30 ns for the original scalar implementation. At 4 KiB, 64 KiB, 1 MiB and
8 MiB, production medians were respectively 163.60 ns, 2483.01 ns, 39.60 us and
343.88 us versus 192.47 ns, 2944.34 ns, 48.94 us and 406.74 us. These isolated
hash gains are about 9% at 1 KiB and 15-19% at the larger sizes; they do not
establish a whole-game frame-rate or hitch improvement. The scalar 216-byte
case measured 31.65 ns versus 30.98 ns, reflecting the small size branch.

The feature guard requires AVX2, AVX512F/DQ/VL, XSAVE, OSXSAVE, AVX and XCR0
state bits `0xE6`. A noinline function contains the feature instructions; the
rest of the translation unit keeps its ordinary compiler ISA. See the
[Intel instruction guide](https://www.intel.com/content/www/us/en/docs/intrinsics-guide/index.html),
[Intel ISA extension reference](https://www.intel.com/content/dam/develop/external/us/en/documents/319433-024-697869.pdf)
and Microsoft's [CPUID intrinsic documentation](https://learn.microsoft.com/en-us/cpp/intrinsics/cpuid-cpuidex?view=msvc-170)
for the instruction features and runtime detection contract. Unsupported CPUs,
OS state or non-MSVC targets retain the original scalar computation.
