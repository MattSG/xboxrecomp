# Splitting an XBE for decompilation

`tools.split` emits per-function byte listings and a manifest. It uses the
analysis pipeline without running the recompiler or the guest. Boundaries and
ABI annotations are inferred; retain their methods/confidence and review them.
See [getting started](GETTING_STARTED.md) for the native recompilation workflow.

## Run

From the toolkit directory, with the analysis dependencies installed:

```powershell
python -m tools.xbe_parser path/to/input.xbe --json path/to/input_analysis.json
python -m tools.split path/to/input.xbe --analysis-json path/to/input_analysis.json --out build/split
```

The splitter analyzes the input itself. Use `--abi path/to/abi_functions.json`
for optional signature annotations; the default is the ABI tool's output path.
Keep analysis and ABI files paired with their input image. Use separate output
directories for different images.

| Option | Purpose |
|---|---|
| `--section .text` | Restrict emitted functions; repeat for more sections |
| `--text-only` | Analyze only .text |
| `--extra-sections NAME,OTHER` | Analyze additional sections as code |
| `-n 50` | Limit the number of emitted functions |
| `--abi FILE` | Select optional ABI annotations |

## Synthetic example

The tracked [synthetic XBE generator](../tools/conformance/mkxbe.py) supplies
five arithmetic functions. It contains no captured title code. Parse and split
its generated output using the commands above. The first listing is:

```asm
; sub_00011000
; 0x00011000 - 0x0001100E  (14 bytes, 7 instructions)
; section .text, detected by prologue (confidence 0.95)
;
; The bytes below are the original encoding, emitted as data so
; this assembles byte-identically. The disassembly is the comment
; beside each one -- read that, write the C, delete this file.

sub_00011000:
    db 0x55                                       ; 00011000  push ebp
    db 0x8B, 0xEC                                 ; 00011001  mov ebp, esp
    db 0x8B, 0x45, 0x08                           ; 00011003  mov eax, dword ptr [ebp + 8]
    db 0x03, 0x45, 0x0C                           ; 00011006  add eax, dword ptr [ebp + 0xc]
    db 0x03, 0x45, 0x10                           ; 00011009  add eax, dword ptr [ebp + 0x10]
    db 0x5D                                       ; 0001100C  pop ebp
    db 0xC3                                       ; 0001100D  ret
```

Its arithmetic corresponds to `uint32_t add3(uint32_t a, uint32_t b, uint32_t c)`
returning `a + b + c`. That is a semantic description, not a matching C build.

The corresponding `manifest.json` function record is:

```json
{
  "name": "sub_00011000",
  "address": "0x00011000",
  "end": "0x0001100E",
  "size": 14,
  "section": ".text",
  "instructions": 7,
  "has_prologue": true,
  "detection_method": "prologue",
  "confidence": 0.95,
  "called_by": [],
  "calls_to": [],
  "file": "asm/text/sub_00011000.s"
}
```

The manifest also records function totals, section names and ABI coverage.
Call graph and optional signature fields come from the input analysis.

## Byte preservation and limits

x86 instructions can have multiple encodings. The splitter emits original
bytes as `db` directives and puts decoded instructions in comments, preserving
undecoded data inside the chosen function range. Byte preservation does not
prove that the inferred range is a real function or that an annotation is right.
It also does not reproduce original section layout, object grouping or linking.
Data sections are not split into individual symbols.

For behavioral comparison on suitable functions, use the explicit XBE phase:

```powershell
python -m tools.conformance --xbe path/to/input.xbe
```

This runs selected native and lifted functions on matching inputs. Selection,
quarantine, skips and tested inputs limit the result; it does not prove arbitrary
functions safe or establish whole-title correctness. See
[conformance testing](technical/conformance-testing.md).

For optional naming assistance, see [Ghidra naming](../tools/ghidra_naming/README.md)
and [RTTI recovery](technical/rtti-recovery.md). Keep captured title results
and addresses in the consuming project.
