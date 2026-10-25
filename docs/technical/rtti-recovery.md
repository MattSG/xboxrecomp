# MSVC RTTI recovery

`tools.rtti` recovers supported 32-bit MSVC type descriptors, complete-object
locators, hierarchy records and vtables from a caller-supplied XBE. Class names
and method addresses are analysis proposals, not proof of function boundaries,
original member names or callable ABI compatibility.

## Run

From the toolkit directory:

```powershell
python -m tools.rtti path/to/input.xbe --out build/input/rtti.json --seeds build/input/seeds.json --names build/input/names.json
```

`--out` defaults to rtti.json; seeds and names are optional. `--verbose` reports
additional counts. If no supported type descriptors are found, the command
returns successfully without writing these files. That does not establish the
absence of every possible RTTI/compiler layout.

Use proposed seeds with the matching input image:

```powershell
python -m tools.disasm path/to/input.xbe --seed-functions build/input/seeds.json -o build/input/disasm
```

Review seeds and names before applying them. Shared implementations, adjustor
thunks, malformed records and inferred disassembly boundaries limit attribution.

## Implementation boundaries

The [recovery implementation](../../tools/rtti/rtti.py) reads sections from the
input header. Executable flags identify code ranges; conventional code/SDK
section names remain a compatibility fallback for missing flags. Vtable scans
currently use .rdata and .data. File-backed extents are clipped to available
bytes, and 32-bit reads must fit completely inside one such section.
This is not a complete image/RTTI validator.

Type descriptors use supported decorated-name patterns. Locator references
identify candidate vtables, whose slots must point into recognized code ranges.
The longest offset-zero table supplies the primary-table summary. Hierarchy
records are interpreted in preorder; this is not full multiple-inheritance layout
modelling or proof that the declaring class of each slot has been recovered.

`owning_class` proposes an owner only when one recovered candidate is an ancestor
of all holding classes (or the method appears in one class). Other methods are
omitted from the names map. Compiler folding and incomplete hierarchy recovery
can still make a proposed owner wrong. Confidence and counts are not runtime
acceptance evidence.

## Python API and checks

```python
from tools.rtti import recover, seeds, methods_by_class, names
result = recover("path/to/input.xbe")
proposed_starts = seeds(result)
holding_classes = methods_by_class(result)
proposed_names = names(result)
```

Synthetic checks cover executable sections with nonstandard names, legacy-name
fallback, raw-section boundaries and truncated input reads:

```powershell
python -m unittest tools.rtti.test_rtti -v
```

Keep per-title RTTI output, expected counts and captured class/address reports in
the consuming project. Synthetic checks do not establish whole-title correctness.
