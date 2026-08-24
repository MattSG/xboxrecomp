# MM3 consumer validation

Read-only validation on 2026-08-25 used the existing MM3 XBE and disassembly
metadata outside this repository:

```powershell
$env:PYTHONPATH = 'C:\Users\Matt\Source\repos\xboxrecomp'
& 'F:\repos\midtown-madness-3-recomp\tools\builds\python-venv\Scripts\python.exe' `
  -m tools.provenance.xbe `
  --xbe 'F:\repos\midtown-madness-3-recomp\game_files\default.xbe' `
  --functions 'F:\repos\midtown-madness-3-recomp\m4tmp\disasm2437\functions.json' `
  --func 0x001E77F3
```

The report recovered direct calls from `0x001E77F3`, including
`0x001E7627`, `0x001E6C50`, `0x001F3065`, `0x001F33CF`, `0x001F373E`, and
`0x0020F7EB`. Indirect-control sites were reported as `UNKNOWN` when the
available static state could not prove their target. This is the intended
conservative answer and gives the next debugging target without committing
MM3 XBE bytes or state.
