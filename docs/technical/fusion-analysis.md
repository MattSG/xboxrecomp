# Reference analysis tools

The `tools.fusion` helpers inspect caller-supplied Xbox BC modules and matching
guest executables. They provide metadata and manual comparison aids, not a
replacement runtime or proof that a translation is correct. Keep captured
modules, guest assets and title-derived signature databases in the port.

## Inputs and dependencies

Run commands from the toolkit root. These optional tools use `capstone`,
`pefile` and `numpy`; their regression suite also uses `pytest`. Supply the guest
XBE corresponding to the reference module's build. A different retail, updated
or packaged build can have different addresses and function bodies.

## Module metadata and address maps

```sh
python -m tools.fusion.module path/to/module.dll
```

`FusionModule` reads PE version resources, exports and the exported version-1
`PrecompiledSymbolTable`. The reader expects four leading u32 fields (string
blob reference, version, blob size, record count), with 12-byte records at
+0x18 containing guest start, byte size and name offset. This describes the
implemented reader, not every private compiler format.

`map_pairs()` scans `.rdata` for a long monotonic guest/host pair run;
`map_entry_points()` uses the same detector and `host_of()` looks up an exact
guest value. The large-run confidence threshold excludes small tables. The
scan does not establish exact table bounds or authoritative function boundaries.

## Read an SVOD package

```sh
python -m tools.fusion.svod verify path/to/package
python -m tools.fusion.svod ls path/to/package
python -m tools.fusion.svod extract path/to/package default.xbe donor.xbe
```

The reader accepts the package root, its `Content/Game` directory or its
`DefaultPackage.data` directory. It removes the supported interleaved hash-block
layout and derives a sector base by checking the root directory and referenced
XBE header together. Library callers can supply an explicit base.

Hash verification samples the block stride; it does not authenticate the whole
hash tree. XBE output checks are plausibility diagnostics rather than universal
validity rules. Inspect extracted sections and bytes before using an executable
as a reference; a plausible header alone is insufficient.

## Compare reference metadata and code

```sh
python -m tools.fusion.coverage_oracle path/to/module.dll functions.json
python -m tools.fusion.lifter_diff ms path/to/module.dll symbol_or_0xaddress
python -m tools.fusion.lifter_diff diff path/to/module.dll generated_dir symbol_or_0xaddress
```

The coverage command compares named reference starts with detector output for
the same guest build. Missing starts inside detected functions are candidates
for inspecting merged boundaries. Counts cover the supplied named references,
not all guest functions or gameplay acceptance.

The current listing formatter associates guest eax..ebp with the corresponding
host registers, guest esp with r14, and the guest RAM base with r15. It uses
that analyzed model to display memory operands in guest terms, not to prove
that other compiler versions use it or that similar offsets imply equivalent
semantics.

## Build and match signature candidates

```sh
python -m tools.fusion.signatures build path/to/module.dll donor.xbe signatures.json
python -m tools.fusion.signatures match signatures.json target.xbe functions.json names.json
python -m tools.fusion.signatures merge combined.json first.json second.json
```

Signature bytes come from the donor guest XBE and names from module metadata.
Masking uses the supplied image's section span; selected address operands are
wildcards while opcode/structure bytes remain fixed. Equal length and masked
prefix matches produce candidate names, not semantic equivalence. Ambiguous
names are reported separately. `--only-unnamed` limits matching output to
placeholder-named functions. Regenerate saved databases after masker changes.

## Runtime feedback and checks

For this toolkit's runtime target collection and seed workflow, use
[Indirect Call Dispatch](indirect-calls.md). Generic lifter/runtime fixes belong
in the toolkit; guest overrides, observations and policy remain port-owned.

```sh
python -m pytest tools/fusion -q
```

The suite uses synthetic instruction bytes, maps and directory/XBE fixtures.
It does not establish correctness against captured modules, complete package
support, cross-title naming precision or runnable gameplay.
