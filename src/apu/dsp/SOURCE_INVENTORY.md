# DSP source origins

Pinned xemu commit: `478b4f496102379c7eaa7f3ec10e714a703c4300`.
Paths below are relative to src/apu. Local files contain adaptations; they are
not byte-identical copies. See [PROVENANCE.md](PROVENANCE.md) and per-file headers.

| Local source | Pinned upstream source | Header license |
|---|---|---|
| `dsp/interp/dsp_cpu.c` | `hw/xbox/mcpx/apu/dsp/interp/dsp_cpu.c` | GPL-2.0-or-later |
| `dsp/interp/dsp_cpu.h` | `hw/xbox/mcpx/apu/dsp/interp/dsp_cpu.h` | GPL-2.0-or-later |
| `dsp/interp/dsp_cpu_regs.h` | `hw/xbox/mcpx/apu/dsp/interp/dsp_cpu_regs.h` | GPL-2.0-or-later |
| `dsp/interp/dsp_emu.c.inc` | `hw/xbox/mcpx/apu/dsp/interp/dsp_emu.c.inc` | GPL-2.0-or-later |
| `dsp/interp/dsp_dis.c.inc` | `hw/xbox/mcpx/apu/dsp/interp/dsp_dis.c.inc` | GPL-2.0-or-later |
| `dsp/debug.h` | `hw/xbox/mcpx/apu/dsp/debug.h` | GPL-2.0-or-later |
| `dsp/dsp_dma.c` | `hw/xbox/mcpx/apu/dsp/dsp_dma.c` | LGPL-2.0-or-later |
| `dsp/dsp_dma.h` | `hw/xbox/mcpx/apu/dsp/dsp_dma.h` | LGPL-2.0-or-later |
| `dsp/dsp_dma_regs.h` | `hw/xbox/mcpx/apu/dsp/dsp_dma_regs.h` | LGPL-2.0-or-later |
| `dsp/dsp.c` | `hw/xbox/mcpx/apu/dsp/dsp.c` | GPL-2.0-or-later |
| `apu_dsp.c` | `hw/xbox/mcpx/apu/dsp/gp_ep.c` | LGPL-2.0-or-later |

`dsp/dsp.c` also adapts upstream `hw/xbox/mcpx/apu/dsp/dsp_c.c`.
`dsp.h` and `dsp_port.h` provide local state/host interfaces with incorporated
GPL/LGPL declarations. Preserve source headers and the supplied license texts.
The supplied COPYING.GPL-2 and COPYING.LGPL-2.1 are upstream COPYING/COPYING.LIB;
per-file headers determine applicable versions. This mapping is not a
redistribution-package license audit.
