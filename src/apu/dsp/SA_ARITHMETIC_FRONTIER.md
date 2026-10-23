# SA arithmetic review boundaries

SA changes significant-word packing and arithmetic width. Review multiplication,
accumulation, shifts, rounding, signed minima, immediate alignment and flags at
their shared helpers and instruction callers. Masking operand bytes alone does
not establish correct accumulator layout.

Executable cases live in tests/dsp/dsp_cpu_test.c. Handler coverage, successful
frame replay and absence of NULL decoder entries are not proof of arbitrary
program correctness or hardware timing. Consult the implementation and current
assertions rather than treating a captured caller list as a live inventory.
