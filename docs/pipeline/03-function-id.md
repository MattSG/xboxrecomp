# Step 3: Function identification

`tools.func_id` enriches caller-supplied disassembly with heuristic categories,
names and confidence scores. A label is a debugging hint, not proof of function
identity, ABI compatibility or working gameplay.

## Run

From the toolkit directory, after parsing/disassembling the same input image:

```powershell
python -m tools.func_id path/to/input.xbe --functions build/input/functions.json --strings build/input/strings.json --xrefs build/input/xrefs.json --output build/input/identified
```

| Option | Purpose |
|---|---|
| `--functions FILE` | Detected functions JSON |
| `--strings FILE` | Extracted strings JSON |
| `--xrefs FILE` | Cross-reference JSON |
| `--output DIR` / `-o DIR` | Output directory |
| `--game-keywords FILE` | Optional caller-owned keyword policy JSON |
| `--verbose` / `-v` | Detailed progress |

Input/output defaults are in [config.py](../../tools/func_id/config.py).
Use explicit paths to keep multiple images' analysis separate.

## Mechanisms and limits

- CRT identification matches curated opening-byte patterns with optional masks
  and size limits. Patterns can collide or vary across compiler/library builds;
  a match is not a validated signature or callable replacement.
- RenderWare identification parses source-ID paths with a numeric `RW...Active`
  version tag under `//RenderWare/.../rwsdk/`. Direct references seed labels;
  nearby data and code regions expand them heuristically. Accepting a version
  tag does not prove compatibility with that library's ABI or structures.
- SDK section classification uses names and ranges from the input XBE.
  Reference-title addresses never supply default ranges.
- Call graph, proximity, vtable and simple-code-pattern passes add further
  classifications. Each pass's assumptions and coverage need review.
- Application keyword classification is disabled by default. A caller may pass
  `{"space": ["orbit"]}` as JSON to propose `game_space` for matching strings.
  Keywords must be nonempty lowercase strings; policy does not persist across
  calls. These lists belong to the consuming project.

See [implementation orchestration](../../tools/func_id/identify.py) for pass
order and precedence. Confidence scores rank heuristics; they are not calibrated
probabilities or evidence that a function has been executed correctly.

## Output

The directory contains `identified_functions.json`, `rw_modules.json`,
`crt_functions.json` and `summary.json`. The enriched function database is a
list; entries preserve input addresses, sizes, names and sections, and add
category, confidence, method and optional identification fields. Consumers
should retain those provenance fields and validate labels before applying
manual replacements. Output is not a title coverage or runtime acceptance report.
