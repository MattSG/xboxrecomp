# Ghidra naming helpers

These tools prepare a caller-supplied XBE for raw import, export Ghidra's
analysis, and merge proposed names with a recompiler function database.
Recovered names and boundaries need validation; counts are not title acceptance.

## Prepare and import

The [image builder](extract_for_ghidra.py) places sections relative to the XBE
header's base address, zero-fills gaps/virtual tails, and writes `xbe_flat.bin`
and `sections.json`. The [runner](run_ghidra.sh) reads `base_address` from that
metadata for the headless import. It must match the builder's origin; a fixed
reference-image base can shift every imported address.

From the toolkit directory in Git Bash, with Python's `py -3` launcher and a
Windows Ghidra installation providing `support/analyzeHeadless.bat`:

```bash
GHIDRA_HOME=/path/to/ghidra XBE=/path/to/input.xbe tools/ghidra_naming/run_ghidra.sh
```

The runner can also search `GHIDRA_ROOT` for an installation. Its work/project
and export directories are under this tool directory. Keep separate inputs and
analysis paired; existing projects and exports are caller-owned state.

Optional decompilation uses `run_ghidra.sh decompile 4000` (or `all`). For an
existing imported program, `IMPORT=0 ANALYZE=0` selects export without reimport
or reanalysis. Use that only when the project already contains the matching
image; this path does not verify the existing program's load base.

## Export, inspect and merge

Scripts in `ghidra_scripts` include analysis-option setup, function/name export,
function-entry listing and address triage. `ClassifyTargets.java` accepts caller
addresses and reports the containing block/function and instruction/data state.
Those reports describe Ghidra's analysis, which can be wrong.

Use the [merge helper](merge_names.py) with `--help` for its inputs and output
options. Inspect proposed names before applying them to the matching function
database. Regenerate affected recompilation sources after an accepted rename.
Library signatures, optional seeding and decompiler output do not establish
correct boundaries or callable ABI compatibility.

## Verification boundaries

Synthetic tests cover flat-image placement at two header bases and metadata
argument forwarding to a mock headless executable. The mock does not run
Ghidra. These checks do not establish Ghidra-version compatibility, live analysis
quality, full XBE validation or title fidelity. Keep captured title statistics,
addresses, naming results and decompiled output in the consuming project.
