# Source-file attribution from debug strings

`tools.debug_symbols` proposes source-file attribution from source-path strings
referenced by functions. It consumes caller-supplied `functions.json` and
`strings.json`; it needs no title-specific paths, addresses or external database.
The input must retain suitable strings and references for this to be useful.

## Usage

```powershell
python -m tools.debug_symbols --functions build/input/functions.json --strings build/input/strings.json --output build/input/source_names.json
```

Inspect the result before applying it to the matching function database:

```powershell
python -m tools.debug_symbols --functions build/input/functions.json --strings build/input/strings.json --apply
```

| Option | Behavior |
|---|---|
| `--output FILE` / `-o FILE` | Write summary, names and attribution maps |
| `--apply` | Update input function metadata; create a one-time .bak |
| `--include-headers` | Include header-path strings as candidates |
| `--no-interpolate` | Use direct reference votes only |
| `--verbose` / `-v` | Report progress and counts |

At least --output or --apply is required. Application renames only `sub_...`
placeholders, retaining existing names. It also adds source-file provenance to
attributed functions. Backups are one-time copies, not a versioned history.

## Mechanisms and limits

The [recovery code](recover.py) recognizes path-shaped strings, excluding bare
filenames and RCS banners with trailing revision text. Function ranges map
references to owners. Multiple candidate paths use a majority vote with a
preference against .h/.hpp paths on ties. This does not prove that a string is an assert
operand or that all code in a function originated in that file.

Optional interpolation proposes the same file for unlabeled functions bracketed
by agreeing endpoints in the same section. Section checks limit this inference;
they do not prove original object-file boundaries or linker ordering. Direct
votes override interpolated proposals.

Output names combine a sanitized filename stem with the input function address.
The JSON contains `summary`, `names` and `attribution`; each attribution includes
`source_file` and `origin` (`direct` or `interpolated`). Coverage counts describe
this heuristic pass, not validated symbols, ABI compatibility or working code.

## Checks

```powershell
python tools/debug_symbols/test_recover.py
```

Synthetic checks cover path extraction, prefix stripping, C identifier names,
function ownership, section boundaries, interpolation and preservation of
existing names during application. Captured title results belong to the port.
