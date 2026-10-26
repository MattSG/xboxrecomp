# MCPX DSP source provenance

Upstream: https://github.com/xemu-project/xemu

Pinned commit: `478b4f496102379c7eaa7f3ec10e714a703c4300`.
Original directory: `hw/xbox/mcpx/apu/dsp/`.

## Incorporated source

| Local files | Upstream license |
| --- | --- |
| `interp/dsp_cpu.c`, `interp/dsp_cpu.h`, `interp/dsp_cpu_regs.h` | GPL-2.0-or-later |
| `interp/dsp_emu.c.inc`, `interp/dsp_dis.c.inc`, `debug.h` | GPL-2.0-or-later |
| `dsp_dma.c`, `dsp_dma.h`, `dsp_dma_regs.h` | LGPL-2.0-or-later |
| `dsp.c` (adapted from upstream `dsp.c`/`dsp_c.c`) | GPL-2.0-or-later |
| `../apu_dsp.c` (adapted from upstream `gp_ep.c`) | LGPL-2.0-or-later |

Original copyright and license headers are retained. The interpreter credits
espes, Matt Borgerson, the ARAnyM developers and Thomas Huth's Hatari adaptation.
The original upstream license texts are included as `COPYING.GPL-2` and
`COPYING.LGPL-2.1`; the latter is the upstream repository's supplied LGPL text.
Per-file headers specify the license versions applicable to each file.

These files are not covered solely by xboxrecomp's top-level MIT license.
The GPL interpreter's redistribution terms must be accounted for when shipping
a linked binary. No JIT, QEMU machine/device runtime, or full APU was imported.

## Adaptation boundaries

`dsp_port.h` supplies standard C host support and explicit little-endian access
in place of QEMU host includes. QEMU tracing is disabled. The local interpreter
contains ISA corrections beyond the pinned source; upstream identity is not a
claim of unchanged instruction behavior or full conformance.

Local corrections include short-I/O bank selection and bit-number decoding,
relative bit branches, long-offset memory moves, immediate logical operations,
SA arithmetic and stack accounting. Review the implementation and executable
regressions under `tests/dsp` for the supported cases and their assertions.

DMA advances descriptors at scheduled instruction boundaries after START and
completes on transfer EOL. DMA_CONTROL reads do not advance completion.
Descriptor staging storage is local rather than an upstream static allocation.
Instruction scheduling, peripheral timing and arbitrary guest audio fidelity
require independent validation. Frame replay with adapted local DMA/peripherals
is not independent full-xemu APU equivalence.
