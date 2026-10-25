# IDA naming helpers

Export recovered functions and symbols from an IDA database, then use the
shared [name merger](../ghidra_naming/merge_names.py). The exporter writes
`functions.json` and `symbols.json`; it does not validate recovered names or ABI.

Prepare a flat image with
`python tools/ghidra_naming/extract_for_ghidra.py input.xbe --out-dir build/ida`.
Import `xbe_flat.bin` as x86 32-bit at the `base_address` in `sections.json`.
The database addresses must match the input XBE addresses used by the recompiler.

Run `export_xbe_names.py` inside IDA through File > Script file, or headlessly:

```text
ida -A -S"tools/ida_naming/export_xbe_names.py build/ida-export --image-base <base_address> --exit" database.i64
```

The script waits for auto-analysis. `--image-base` optionally checks the
caller-supplied expected base before writing; omitting it trusts the database.
`--exit` closes IDA after a headless run. Export defaults beside the script.

Merge and inspect proposals before applying them to a matching function database:

```text
python tools/ghidra_naming/merge_names.py --export-dir build/ida-export --out build/ida-names.json
```

`--apply` writes matching entries in the supplied `--functions-json` file and
keeps an initial `.bak`. Multiple passes overwrite matching names in pass order.
Regenerate affected recompilation sources after accepting names.

The synthetic exporter check mocks IDA APIs. It does not prove IDA-version
compatibility, analysis quality, loader correctness or game fidelity. Keep
captured naming statistics and title exports in the consuming project.
