# DSP56300 stack architectural references

These references guide the interpreter; they are not a hardware-conformance
claim. Tests live in `tests/dsp/dsp_cpu_test.c`.

## Established architectural rules

- OMR SEN bit 20 enables extension; XYS bit 16 selects X (clear) or Y (set).
- OMR WRP bit 19 becomes sticky when hardware-to-memory copying begins.
- EOV bit 18 sets on a push requested with SP equal to SZ; EUN bit 17 sets on a pull requested with SP zero. Each flag's clear-to-set transition raises the nonmaskable stack-error exception. Both clear on hardware reset or an explicit OMR MOVE.
- With SEN enabled, SP is a 24-bit logical counter. Its low four bits still select the physical top. Bits 4/5 are counter bits, not non-extended SE/UF flags.
- SC is a separate five-bit count of hardware entries. Hardware reset clears it; implicit stack operations and explicit MOVEC reference it.
- SZ is not initialized by hardware reset. Software initializes it before enabling extension. SZ is 15 plus half the extension buffer size in 24-bit words.
- EP is a 24-bit pointer into extension data memory.

Sources: 5-6/5-7 (PDF 86/87), 5-18 through 5-20 (PDF 98-100).

## Transfer thresholds and ordering

The general description says a push is always into the hardware stack, followed by moving least-recently-used entries to memory when full. A pull takes the physical top, followed by loading older entries from memory when empty. Transfers contain one or two 48-bit entries (two or four 24-bit words).

The timing appendix gives more precise thresholds: SC=14 is declared full; an additional push activates extension. SC=2 is declared empty; an additional pop activates extension. Do not implement a naive SC=16/SC=0 trigger.

Sources: 5-17 (PDF 97), A-11 (PDF 435). A self-consistent round-trip test alone cannot prove transfer ordering or timing.

## Valid-program restrictions

While SEN is enabled, MOVE to EP and BCHG/BSET/BCLR on EP are prohibited. MOVE to SC with a value greater than 15 is prohibited. Subroutine call instructions must not occupy the stack-error vector while extension is enabled. Documented SSH read/write pipeline hazards require two NOPs before affected following stack reads.

Sources: A-22/A-23 (PDF 446/447).

## Transfer ordering and validation limits

NXP APR20 section 3.3 describes the SC=14 spill and SC=2 refill thresholds,
the SP>SC refill guard, and two-word EP movement. Its capacity formula differs
from the later family manual; distinguish those definitions when reviewing faults.
Motorola patent EP0720087A1 Figures 3/4 describes LOW/HIGH spill and reverse
HIGH/LOW refill. These are architectural/design sources, not executed hardware
results. Pipeline batching, timing and mode-transition fidelity require separate
validation. Pinned xemu lacks extension and cannot validate those paths.

- DSP56300FM Rev. 5: https://www.nxp.com/docs/en/reference-manual/DSP56300FM.pdf
- APR20 section 3.3: https://www.nxp.com/docs/en/application-note/APR20.pdf
- EP0720087A1 Figures 3/4: https://patents.google.com/patent/EP0720087A1/en

Capture format is declared by DSP_FRAME_CAPTURE_VERSION in dsp.h. Do not assume
old states have compatible architectural stack accounting merely because their
binary layout matches. Use the capture/replay version checks.
