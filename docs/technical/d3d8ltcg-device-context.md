# Investigating statically linked D3D8 device contexts

A statically linked graphics library can keep device state in guest globals or
objects rather than call a host COM interface. Its internal layout is an input
to port investigation, not a universal Xbox ABI. Establish it for the exact XBE
and library build before introducing a replacement or hook.

## Recover the contract

1. Locate device creation and the callers of the suspected graphics entry.
   Trace the device pointer's writer and lifetime; distinguish a pointer global
   from inline storage. Check section bounds against the loaded image.
2. Trace each field's readers and writers. Record access widths, allocation
   sizes, ownership and index bounds. An observed nonzero value does not prove
   that a field is a valid surface, callback or buffer pointer.
3. Follow push-buffer allocation, submission, consumption and fence completion
   together. Identify cursor units, wrap rules and the producer/consumer memory
   domains before interpreting a subtraction or wait condition.
4. Recover the guest calling convention from disassembly and all relevant call
   paths: register arguments, stack arguments, return values, callee/caller stack
   cleanup and preserved registers. Counting pushes alone is insufficient:
   pushes may save registers, and parameters may already occupy stack storage.
5. Compare a bounded original execution with the lifted path at matching state
   transitions. Keep image identity, breakpoints, registers and memory evidence
   with the port that supplied them.

## Put the fix at the responsible boundary

A mistranslated instruction or broken guest-call contract belongs in the generic
lifter/runtime with a minimal synthetic regression. A missing GPU command or
completion transition belongs in the shared hardware/renderer implementation.
The port supplies discovered entry points, image-specific field layouts and
application policy through its integration hooks.

Do not use captured device snapshots, fabricated resource pointers, forced
camera flags or a read cursor aliased to the write cursor as a generic device
implementation. Those changes can bypass a wait without executing queued work
or preserving resource lifetime. If used temporarily to investigate a port,
keep them in that port and distinguish the experiment from a rendering fix.

Check allocation/wrap, repeated submission, completion and guest stack effects
with synthetic cases. Then verify loading and moving gameplay separately;
leaving a spin loop or producing one frame is insufficient proof of fidelity.

See [register model](register-model.md),
[function identification](../pipeline/03-function-id.md), and
[xemu debugging](xemu-debugging.md) for the adjacent investigation workflows.
