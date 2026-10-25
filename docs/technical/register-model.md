# Guest register and call model

Lifted x86 functions exchange guest state through shared runtime declarations
and simulated guest memory. Use the generated port header based on
[recomp_types.h](../../templates/runtime/recomp_types.h); hand-written overrides
must use the same declarations and calling contract.

## Per-thread state

Integer registers, x87 state, direction state and FS base are thread-local.
`g_eax`, `g_ecx`, `g_edx`, `g_esp`, `g_ebx`, `g_esi` and `g_edi` are shared
across lifted calls on the current thread. This preserves implicit register
arguments. Generated guest instructions implement register saves/restores;
C's host calling convention does not implement the guest ABI.

The translator also uses a local `ebp`, initialized to zero. Selected frameless
functions inherit `g_ebp`; frame bridges and SEH use shared `g_ebp`/`g_seh_ebp`
state where emitted. Local EBP alone does not preserve every cross-function
frame dependency. Inspect the translated call path before writing an override.
See [EBP initialization checks](../../tools/recomp/test_translator_ebp_init.py)
and [frame bridge checks](../../tools/recomp/test_lifter_frame_bridge.py).

The template's generated-code aliases expose guest register names without
requiring host code to adopt those macros. Do not redeclare guest state as
ordinary process globals: concurrent guest threads require independent state.

## Guest memory and stack

`g_esp` contains a guest virtual address. `MEM*` helpers translate through the
runtime memory offset; a guest address is not a host pointer. Stack placement
and capacity come from the active runtime layout and port configuration, not
a title's fixed address. See [memory integration](memory-layout.md).

`PUSH32` evaluates its value before decrementing the supplied stack pointer;
`POP32` reads the current word before advancing it. An override must preserve
argument order, return-address slots and the caller/callee cleanup convention
of its guest call site.

## Calls and returns

Lifted entry points use `void(void)`: arguments live in guest registers and
memory, and return values are written to guest register state. Generated calls
push the actual following guest instruction address. Guest code can inspect
that slot even when native C calls provide the control flow. A dummy return
address changes observable guest behavior.

Direct calls and indirect dispatch have distinct generated helpers. Indirect
targets that depend on ESP must be captured before pushing the return slot.
Do not add cleanup by guessing an argument count: inspect emitted caller and
callee instructions. See [return-address checks](../../tools/recomp/test_call_retaddr.py)
and the frame bridge checks above.

## Flags and floating point

The lifter combines emitted comparisons with tracked arithmetic/control state;
there is no blanket rule that flags can be discarded across instructions or
calls. [CPU helpers](../../include/recomp_cpu.h) implement width-aware flag
calculation and PUSHFD/POPFD images. Direction state controls string traversal.
The runtime template contains x87 stack/control/status and rounding helpers;
these are guest state, independent of the host's floating-point mode.

Sub-register helpers extract or replace the appropriate byte/word while
preserving the remaining bits. Overrides must respect operand width, signedness
and flag consumers rather than replacing guest arithmetic with an unchecked
host expression.

These declarations and focused checks describe the current implementation.
They do not certify complete x86, SEH, nonlocal transfer or concurrent runtime
fidelity. Captured title addresses, disassembly and performance evidence belong
in the application repository.
