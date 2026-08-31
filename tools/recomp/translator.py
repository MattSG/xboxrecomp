"""
Function-level x86 → C translator.

For each function:
1. Read raw bytes from XBE
2. Disassemble with Capstone
3. Build basic blocks
4. Lift each block to C statements
5. Generate a complete C function

Produces compilable C code using recomp_types.h macros.
"""

import json
import os

# Import the functions, not the VA constants: configure_from_xbe() rebinds those
# at startup, so a by-value import would freeze the fallback layout.
from .config import va_to_file_offset, is_code_address
from .disasm import Disassembler
from .lifter import Lifter, lift_basic_block, detect_seh_helpers
from .lifter import _base_mnemonic

# XBE section "D3D": Microsoft's statically linked D3D8LTCG library.
# Functions in this range get the host D3D8 HLE dispatch seam at entry.
D3D8_HLE_LO = 0x0033F960
D3D8_HLE_HI = 0x00353128


def _fixup_icall_esp_save(lines):
    """
    Post-process generated C lines to insert _icall_esp save points.

    When RECOMP_ICALL_SAFE is used, we need to save g_esp BEFORE any
    args are pushed so the macro can restore it on lookup failure.

    Scans backwards from each RECOMP_ICALL_SAFE line to find the pushed
    arg dwords, crossing interleaved computations, fall-through labels, and
    direct calls whose `ret N` cleanup consumed part of those pushes.
    Without that, a direct call interleaved between an icall's arg pushes
    and the icall itself leaves the save placed after the remaining pending
    args (observed: sub_001BE953 vtable call leaked 16 guest bytes and
    crashed with eax=0xFF037AC4).

    Stops at control flow (goto/return/if/POP32), esp reassignment, jump
    labels, other icalls, and direct calls with unknown cleanup.
    """
    import re
    call_re = re.compile(r'/\* call 0x[0-9A-Fa-f]{8}(?: ret (\d+))? \*/')
    label_re = re.compile(r'^loc_([0-9A-Fa-f]+):')
    goto_targets = set(re.findall(r'goto (loc_[0-9A-Fa-f]+);', "\n".join(lines)))

    icall_indices = [i for i, line in enumerate(lines)
                     if 'RECOMP_ICALL_SAFE(' in line]
    if not icall_indices:
        return lines  # nothing to do

    # For each ICALL, determine where to insert the save
    insert_before = set()
    for icall_idx in icall_indices:
        first_push_idx = icall_idx
        skip_dwords = 0  # arg dwords consumed by an interleaved direct call
        j = icall_idx - 1
        while j >= 0:
            stripped = lines[j].strip()
            # Skip blank lines
            if not stripped:
                j -= 1
                continue
            # Stop at control flow, stack reassignment, or other icalls.
            if ('goto ' in stripped or
                'RECOMP_ICALL' in stripped or
                'return;' in stripped or
                stripped.startswith('if (') or
                stripped.startswith('POP32(') or
                re.match(r'^esp\s*=', stripped)):
                break
            # Interleaved direct call: skip the arg dwords its `ret N`
            # pops from the pushes before it (the return slot lives on the
            # call's own line). Unknown cleanup cannot be attributed.
            m = call_re.search(stripped)
            if m:
                if m.group(1) is None:
                    break
                skip_dwords += int(m.group(1)) // 4
                j -= 1
                continue
            if stripped.startswith('PUSH32(esp,'):
                if skip_dwords > 0:
                    skip_dwords -= 1
                else:
                    first_push_idx = j
                j -= 1
                continue
            # Stop at jump-target labels; cross fall-through labels and
            # interleaved arg-evaluation computations.
            lm = label_re.match(stripped)
            if lm and f"loc_{lm.group(1)}" in goto_targets:
                break
            j -= 1

        insert_before.add(first_push_idx)

    # Build result with saves inserted
    result = []
    for i, line in enumerate(lines):
        if i in insert_before:
            # Determine indentation from the current line
            indent = line[:len(line) - len(line.lstrip())]
            result.append(f"{indent}{{ uint32_t _icall_esp = g_esp;")
        result.append(line)
        if 'RECOMP_ICALL_SAFE(' in line:
            indent = line[:len(line) - len(line.lstrip())]
            result.append(f"{indent}}}")

    return result


def _fixup_unbalanced_saves(lines, func_addr=None, seh_epilog=None,
                            skip_rebalance=False):
    """
    Balance callee-saved register save/restore in intra-function
    shared-epilogue functions.

    The disassembler registers intra-function jump targets (e.g. 0x84506, a
    fall-through continuation of sub_000844B9) as standalone functions. Those
    functions legitimately pop registers (edi/esi/ebx/ebp) that their own
    prologue never pushed, because in the original binary the enclosing
    function's prologue did the pushes. When lifted as a standalone C
    function the epilogue then over-pops the simulated stack, corrupting
    g_edi/g_ebx/g_ebp and leaking g_esp (observed: pool allocator's g_ebx
    corrupted to 0x1C, free-list walk spins forever).

    Fix: if a function pops more callee-saved registers than it pushes,
    replace the initial register-push sequence with a balanced push of every
    popped register in reverse-pop order, so the epilogue restores exactly
    what the entry saved.

    The __SEH_epilog is the one deliberate exception: it pops edi/esi/ebx
    that the __SEH_prolog (a *different* function) pushed, so within the
    epilog alone the pops outnumber the pushes. Rebalancing it injects
    self-pushes that make the epilog pop its own current registers (a
    rotation) instead of the prolog's saved frame slots, leaking callee-saved
    registers to the caller (observed: the DICE allocator leaking g_esi =
    pool base 0x02780000 into the vector grow, corrupting the copy args and
    causing a runaway copy loop).
    """
    if skip_rebalance:
        return lines
    if seh_epilog is not None and func_addr == seh_epilog:
        return lines
    import re
    CALLEE = ("edi", "esi", "ebx", "ebp")
    push_re = re.compile(r'^\s*PUSH32\(esp, (edi|esi|ebx|ebp)\);')
    pop_re = re.compile(r'^\s*POP32\(esp, (edi|esi|ebx|ebp)\);')

    pop_order = []
    for line in lines:
        m = pop_re.match(line)
        if m:
            pop_order.append(m.group(1))
    if not pop_order:
        return lines

    # Whole-function pop counts are only meaningful for a single epilogue
    # pop block. Pops spread across multiple sites are exclusive exit paths
    # (e.g. sub_0002B087: 1 push ebx at entry, then 1 pop ebx on each of two
    # exits); naive pop>push would inject phantom entry pushes that leak g_esp.
    # ebp is excluded: its only pop is the leave ("esp = ebp; POP ebp") that
    # every normal epilogue ends with, not an exclusive-exit signal.
    pop_run_re = re.compile(r'^\s*POP32\(esp, (edi|esi|ebx)\);')
    seen_pop = False
    run_ended = False
    for line in lines:
        if pop_run_re.match(line):
            if run_ended:
                return lines
            seen_pop = True
        elif seen_pop and line.strip() and not line.strip().startswith(
                ("loc_", "/*", "*", "//")):
            run_ended = True

    # Arg-cleanup epilogues: when the first pop immediately follows a call,
    # the pops are stdcall arg cleanup that loads the call's args back into
    # edi/esi/ebx (e.g. sub_0008726E: push [ebp-4]/ecx/eax; call sub_00086ED2;
    # pop edi/esi/ebx). Rebalancing such a function injects self-pushes that
    # rotate the pop targets (edi/esi/ebx receive the arg slots instead of the
    # saved frame slots), exactly like the __SEH_epilog bug. Only skip when a
    # call is immediately above the first pop (the shared-epilogue restore
    # pattern has no such call).
    first_pop_idx = None
    for i, line in enumerate(lines):
        if pop_re.match(line):
            first_pop_idx = i
            break
    if first_pop_idx is not None and first_pop_idx > 0:
        prev_idx = first_pop_idx - 1
        while prev_idx > 0:
            _s = lines[prev_idx].strip()
            if (re.match(r'^loc_[0-9A-Fa-f]+:', _s) or not _s
                    or _s.startswith('/*')):
                prev_idx -= 1
                continue
            break
        prev = lines[prev_idx]
        if re.search(r'sub_[0-9A-Fa-f]+\(\);', prev) or 'RECOMP_ICALL' in prev:
            return lines

    push_count = {}
    for line in lines:
        m = push_re.match(line)
        if m:
            push_count[m.group(1)] = push_count.get(m.group(1), 0) + 1
    pop_count = {}
    for r in pop_order:
        pop_count[r] = pop_count.get(r, 0) + 1

    # Only fix functions with a net over-pop (the shared-epilogue pattern).
    # The leave's "esp = ebp; POP ebp" restores the caller's frame, so ebp is
    # never a genuine over-pop by itself: a normal function that balances its
    # edi/esi/ebx pushes with matching pops (e.g. sub_00023213) would otherwise
    # be rebalanced purely because of the leave, injecting self-pushes that
    # shift every subsequent call's args on the simulated stack.
    if not any(pop_count.get(r, 0) > push_count.get(r, 0) for r in ("edi", "esi", "ebx")):
        return lines

    # Locate the entry label and the initial consecutive register pushes.
    label_idx = None
    for i, line in enumerate(lines):
        if re.match(r'^loc_[0-9A-Fa-f]+:', line):
            label_idx = i
            break
    if label_idx is None:
        return lines

    init_pushes = []
    j = label_idx + 1
    while j < len(lines):
        m = push_re.match(lines[j])
        if m:
            init_pushes.append((j, m.group(1)))
            j += 1
        else:
            break

    # Balanced push block: every popped register, in reverse-pop order.
    needed = [f"    PUSH32(esp, {r});" for r in reversed(pop_order)]

    for idx, _ in reversed(init_pushes):
        del lines[idx]
    # Re-locate the entry label after the deletions, then insert the block.
    new_label_idx = None
    for i, line in enumerate(lines):
        if re.match(r'^loc_[0-9A-Fa-f]+:', line):
            new_label_idx = i
            break
    if new_label_idx is None:
        return lines
    lines[new_label_idx + 1:new_label_idx + 1] = needed
    return lines


class FunctionTranslator:
    """Translates individual x86 functions to C source code."""

    def __init__(self, xbe_data, func_db, label_db=None, classification_db=None,
                 abi_db=None, seh_prolog=None, seh_epilog=None):
        """
        xbe_data: bytes - raw XBE file contents
        func_db: dict - addr → function info from functions.json
        label_db: dict - addr → name from labels.json
        classification_db: dict - addr → classification from identified_functions.json
        abi_db: dict - addr → ABI info from abi_functions.json
        seh_prolog/seh_epilog: override the detected SEH helper addresses
        """
        self.xbe_data = xbe_data
        self.func_db = func_db
        self.label_db = label_db or {}
        self.classification_db = classification_db or {}
        self.abi_db = abi_db or {}
        self.disasm = Disassembler()
        self.lifter = Lifter(func_db=func_db, label_db=label_db, abi_db=abi_db,
                             xbe_data=xbe_data, seh_prolog=seh_prolog,
                             seh_epilog=seh_epilog)

    def _read_func_bytes(self, start_va, end_va):
        """Read raw bytes for a function from the XBE."""
        offset = va_to_file_offset(start_va)
        if offset is None:
            return None
        size = end_va - start_va
        if offset + size > len(self.xbe_data):
            return None
        return self.xbe_data[offset:offset + size]

    def _determine_calling_convention(self, func_info):
        """Guess calling convention from function properties."""
        name = func_info.get("name", "")
        # thiscall methods have ecx = this
        if "thiscall" in name or func_info.get("calling_convention") == "thiscall":
            return "thiscall"
        return "cdecl"

    def _func_has_prologue(self, instructions):
        """Check if function starts with push ebp; mov ebp, esp."""
        if len(instructions) < 2:
            return False
        return (instructions[0].mnemonic == "push" and
                instructions[0].op_str == "ebp" and
                instructions[1].mnemonic == "mov" and
                instructions[1].op_str == "ebp, esp")

    def translate_function(self, func_addr, func_info):
        """
        Translate a single function to C code.
        Returns a string of C source code, or None on failure.
        """
        start = func_addr
        end = func_info.get("end")
        if not end:
            end = start + func_info.get("size", 0)
        if end <= start:
            return None

        name = func_info.get("name", f"sub_{start:08X}")
        size = end - start

        # Read bytes from XBE
        raw_bytes = self._read_func_bytes(start, end)
        if not raw_bytes:
            return None

        # Set function bounds for the lifter
        self.lifter.func_start = start
        self.lifter.func_end = end

        # Disassemble
        instructions = self.disasm.disassemble_function(raw_bytes, start, end)
        if not instructions:
            return None

        # Collect switch table targets as extra block leaders
        switch_leaders = set()
        for insn in instructions:
            if insn.mnemonic == "jmp" and not insn.jump_target and insn.operands:
                targets = self.lifter._analyze_switch_table(insn.operands)
                for t in targets:
                    if start <= t < end:
                        switch_leaders.add(t)

        # Build basic blocks
        blocks = self.disasm.build_basic_blocks(
            instructions, start, end,
            extra_leaders=switch_leaders if switch_leaders else None)
        if not blocks:
            return None

        # Get classification and ABI info
        cls_info = self.classification_db.get(start, {})
        category = cls_info.get("category", "unknown")
        module = cls_info.get("module", "")
        source_file = cls_info.get("source_file", "")
        abi_info = self.abi_db.get(start, {})

        # ABI-derived info (kept for comments)
        cc = abi_info.get("calling_convention", "cdecl")
        num_params = abi_info.get("estimated_params", 0)
        return_hint = abi_info.get("return_hint", "int_or_void")
        frame_type = abi_info.get("frame_type", "fpo_leaf")
        stack_frame_size = abi_info.get("stack_frame_size", 0)

        # Determine which registers are used
        used_regs = self._find_used_registers(instructions)
        used_xmm = self._find_used_xmm(instructions)
        # The lifter needs this when lowering setjmp calls: the longjmp
        # return path must re-load the caller's local ebp from g_seh_ebp.
        self.lifter.uses_ebp = "ebp" in used_regs
        has_prologue = self._func_has_prologue(instructions)
        has_fpu = any(insn.mnemonic.startswith("f") for insn in instructions)

        # Volatile registers (eax, ecx, edx, esp) are globals - don't declare
        # them as locals. The RECOMP_GENERATED_CODE #define maps register names
        # to the global variables via preprocessor macros.
        volatile_regs = {"eax", "ecx", "edx", "esp"}

        # Ensure ebp tracked if function uses 'leave' (implicit ebp)
        if any(insn.mnemonic == "leave" for insn in instructions):
            used_regs.add("ebp")

        # Ensure ebp tracked if function has tail jumps (lifter emits
        # g_seh_ebp = ebp before external jmp, indirect jmp, and conditional
        # jumps to external targets).
        has_tail_jump = any(
            (insn.mnemonic == "jmp" and (
                (insn.jump_target and not (start <= insn.jump_target < end))
                or not insn.jump_target  # indirect jmp
            ))
            or (insn.is_cond_jump and insn.jump_target
                and not (start <= insn.jump_target < end))
            for insn in instructions
        )
        if has_tail_jump:
            used_regs.add("ebp")

        # Ensure ebp tracked if function calls __SEH_prolog or __SEH_epilog
        # (lifter emits ebp = g_seh_ebp readback after these calls).
        # Use the per-title detected helpers (self.SEH_PROLOG/SEH_EPILOG),
        # NOT the hardcoded fallback set — those addresses are stale for
        # MM3 (real helpers: 0x00094FC0 prolog / 0x00094FFB epilog) and
        # intra-function entry points that start at an SEH-epilog call
        # would miss the ebp declaration and fail to compile.
        SEH_FUNCS = {
            self.lifter.SEH_PROLOG, self.lifter.SEH_EPILOG,
            # keep the legacy constants as a fallback for titles where the
            # detector does not run (single-function translation)
            0x00244784, 0x002447BF,
        }
        if any(insn.call_target in SEH_FUNCS for insn in instructions):
            used_regs.add("ebp")

        # Ensure ebp tracked if this function IS an SEH helper or alternate
        # prolog variant (lifter emits g_seh_ebp = ebp at the ret bridge).
        if start in (self.lifter.SEH_PROLOG, self.lifter.SEH_EPILOG,
                     0x00097AA4, 0x0009504E):
            used_regs.add("ebp")

        # Build call targets list
        call_targets = set()
        for insn in instructions:
            if insn.call_target and is_code_address(insn.call_target):
                call_targets.add(insn.call_target)

        # All translated functions are void(void).
        # Arguments pass via the global simulated stack (push instructions).
        # Return values pass via g_eax (the global eax register).
        ret_type = "void"
        param_str = "void"

        # Generate C code
        lines = []

        # Header comment
        lines.append(f"/**")
        lines.append(f" * {name}")
        lines.append(f" * Original: 0x{start:08X} - 0x{end:08X} ({size} bytes, {len(instructions)} insns)")
        if category != "unknown":
            lines.append(f" * Category: {category}")
        if source_file:
            lines.append(f" * Source: {source_file}")
        lines.append(f" * CC: {cc}, {num_params} params, returns {return_hint}")
        if frame_type == "ebp_frame":
            lines.append(f" * Frame: EBP-based ({stack_frame_size} bytes locals)")
        else:
            lines.append(f" * Frame: {frame_type}")
        lines.append(f" */")

        # Function signature
        lines.append(f"{ret_type} {name}({param_str})")
        lines.append(f"{{")
        if start in (0x001EC708, 0x001E7F1B):
            lines.append("    uint32_t _saved_ebx = ebx;")
        if start == 0x0003B493:
            lines.append("    recomp_snapshot_3b493_entry();")
        if start == 0x0003B4B4:
            lines.append("    recomp_snapshot_3b4b4_entry();")

        # ebp is the only callee-saved register declared as a local.
        # ebx, esi, edi are global via #define macros (g_ebx, g_esi, g_edi)
        # and must NOT be declared locally, otherwise the local shadows
        # the global and cross-function register passing breaks.
        # Volatile registers (eax, ecx, edx, esp) are also global via macros.
        reg_decls = []
        if "ebp" in used_regs:
            reg_decls.append("ebp")
        if reg_decls:
            lines.append(f"    uint32_t {', '.join(reg_decls)};")

        # Add _flags variable if function has conditional instructions
        has_conditionals = any(
            insn.is_cond_jump or insn.mnemonic.startswith("set")
            or insn.mnemonic.startswith("cmov")
            for insn in instructions)
        if has_conditionals:
            lines.append(f"    int _flags = 0; /* fallback flag var */")

        # Add _cf for carry-dependent instructions (sbb, adc) and for every
        # instruction that produces or clears the carry flag (cmp, test,
        # add/sub, and/or/xor, neg, shifts, rotates). Consumers must read a
        # value that producers store; declaring it whenever either appears
        # keeps the generated C valid for both cases.
        has_carry = any(_base_mnemonic(insn.mnemonic) in (
                "sbb", "adc", "neg", "cmp", "test", "add", "sub",
                "and", "or", "xor", "shl", "sal", "shr", "sar",
                "rol", "ror", "rcl", "rcr", "cmpxchg", "xadd")
                or ("cmps" in insn.mnemonic) or ("scas" in insn.mnemonic)
                        for insn in instructions)
        if has_carry:
            lines.append(f"    int _cf = 0; /* carry flag */")

        # repe cmpsb / repne scasb set ZF; the lifter stores it in _cmps_zf.
        has_cmps = any("cmps" in insn.mnemonic or "scas" in insn.mnemonic
                       for insn in instructions)
        if has_cmps:
            lines.append(f"    int _cmps_zf = 0; /* string-compare ZF */")

        # cmpxchg/xadd publish ZF explicitly: for cmpxchg the failure path
        # makes the accumulator equal the destination, so the flag cannot be
        # recovered from the operands after the fact.
        has_cmpx = any(_base_mnemonic(insn.mnemonic) in ("cmpxchg", "xadd")
                       for insn in instructions)
        if has_cmpx:
            lines.append(f"    int _cmpx_zf = 0; /* cmpxchg/xadd ZF */")

        # Add _fpu_cmp for FPU compare instructions (both old and new style)
        has_fpu_cmp = any(insn.mnemonic in ("fcompi", "fcomip", "fucomi",
                                             "fucompi", "fucomip", "fcomi",
                                             "fcom", "fcomp", "fcompp",
                                             "fucom", "fucomp", "fucompp")
                          for insn in instructions)
        if has_fpu_cmp:
            lines.append(f"    int _fpu_cmp = 0; /* FPU compare result: -1/0/1 */")

        # SSE/MMX register declarations
        if used_xmm:
            xmm_regs = sorted([r for r in used_xmm if r.startswith("xmm")])
            mmx_regs = sorted([r for r in used_xmm if r.startswith("mm")
                               and not r.startswith("xmm")])
            if xmm_regs:
                # float[4], not a single float: the packed forms need all
                # four lanes, and with only one they lifted to nothing.
                decl = ", ".join(f"{r}[4]" for r in xmm_regs)
                lines.append(f"    float {decl};")
            if mmx_regs:
                lines.append(f"    uint64_t {', '.join(mmx_regs)};")

        # FPU stack is shared across translated calls. x87 ST0 survives a
        # normal CALL, so a fresh local stack per C function loses the
        # caller's operand (notably the MM3 float-to-int helper).
        if has_fpu:
            lines.append(f"    #define fp_push(v) (g_fp_stack[--g_fp_top & 7] = (v))")
            lines.append(f"    #define fp_pop() (g_fp_top++)")
            lines.append(f"    #define fp_popp() (fp_pop())")
            lines.append(f"    #define fp_top() g_fp_stack[g_fp_top & 7]")
            lines.append(f"    #define fp_st1() g_fp_stack[(g_fp_top + 1) & 7]")

        # For fpo_leaf functions that use ebp: initialize from g_seh_ebp.
        # In x86, these functions inherit EBP from their caller (typically
        # via a tail jump that shares the caller's frame). In our C translation,
        # ebp is a local variable that would start uninitialized, causing
        # crashes when the function reads MEM32(ebp + offset). The g_seh_ebp
        # global bridges ebp across function boundaries.
        #
        # The same init is REQUIRED for functions with a classic
        # "push ebp; mov ebp, esp" prologue: the first instruction pushes
        # EBP (the caller's frame) onto the stack to save it. In C, that
        # PUSH32(esp, ebp) reads the local ebp, which is uninitialized,
        # pushing a garbage "saved frame" that later propagates into the
        # simulated frame chain and g_seh_ebp via tail jumps and SEH
        # epilogs (observed: g_seh_ebp = 0x????1038 native-heap fragments,
        # then AV when a callee dereferences the poisoned frame). Initializing
        # ebp from g_seh_ebp before the push makes the saved-frame slot hold
        # the true caller frame, exactly like real x86.
        if "ebp" in used_regs:
            lines.append(f"    ebp = g_seh_ebp; /* bridge caller frame from SEH global */")

        # Every translated function is a safe guest-thread execution boundary.
        # Host producers only publish work; this call is where the owning guest
        # thread may accept an IRQ or dispatch a queued DPC.
        lines.append("    recomp_guest_boundary();")
        # Records the frame's callee-saved registers; recomp_guest_exit
        # compares them at every return. No-op unless MM3_CHECK_CALLEE_SAVED.
        lines.append(f"    recomp_guest_enter(0x{func_addr:08X});")
        lines.append(f"")

        # D3D8LTCG high-level-emulation seam.
        #
        # The XBE section "D3D" holds Microsoft's statically linked
        # D3D8LTCG library, not game code. Functions there can be served by
        # the host D3D8 layer instead of executed as translated x86. Emit a
        # dispatch check at entry for every function in that range: when the
        # host handles the call it has already applied the guest-visible
        # effects (registers, stack cleanup), so the translated body is
        # skipped; when it declines, execution falls through unchanged. This
        # is inert until the runtime opts a specific function in.
        #
        # The global is tested inline first, so a run with HLE off pays only
        # a load and a not-taken branch per call.
        if D3D8_HLE_LO <= start < D3D8_HLE_HI:
            lines.append(
                f"    if (g_recomp_hle_on && recomp_hle_dispatch(0x{start:08X})) return;")
            lines.append(f"")
        if start in (0x001EC520, 0x001EC6EE, 0x001EC7F7, 0x001E73AF, 0x001E7627, 0x001E77F3,
                     0x001BF1D4, 0x001BCE30,
                     0x00344A20, 0x00342B00, 0x001EC8E6, 0x001F373E,
                     0x00083BE1, 0x00083B04, 0x00083C55,
                     0x00093B9D, 0x00093C45, 0x00093C7D, 0x00097AFC):
            lines.append(f"    recomp_trace_sched_entry(0x{start:08X});")
            lines.append(f"")
        if start == 0x00083D32:
            lines.append("    recomp_trace_83d32_entry(MEM32(esp + 0x0C), (uint32_t)esp);")
            lines.append(f"")
        if start == 0x00083D49:
            lines.append("    recomp_trace_83d49_read(MEM32(0x0046A154u));")
            lines.append(f"")
        if start == 0x000871C8:
            lines.append("    recomp_trace_871c8_write(MEM32(0x0046A154u));")
            lines.append(f"")
        if start in (0x00096738, 0x00096825, 0x00096874, 0x0016FEF0, 0x00170EC0, 0x00170ED1):
            lines.append(f"    recomp_trace_83d32_caller(0x{start:08X}, (uint32_t)esp);")
            lines.append(f"")
        if start == 0x00096825:
            lines.append("    recomp_trace_96825_entry((uint32_t)esp);")
            lines.append(f"")
        if start == 0x001BF1D4:
            lines.append("    recomp_trace_1bf1d4(0);")
            lines.append(f"")
        if start == 0x001BCE30:
            lines.append("    recomp_trace_bce30(0);")
            lines.append(f"")
        if start == 0x001BCBC0:
            lines.append("    recomp_trace_bcbcc0(0);")
            lines.append(f"")
        if start == 0x001BA085:
            lines.append("    recomp_trace_ba085(ebp);")
            lines.append(f"")
        if start in (0x0027B8C0, 0x0027B742):
            lines.append(f"    recomp_trace_sched_entry(0x{start:08X});")
            lines.append(f"")
        if start in (0x00343E60, 0x00343BD0):
            lines.append(f"    recomp_trace_pump_entry(0x{start:08X});")
            lines.append(f"")
        if start in (0x001EC7F7, 0x001EC6EE,
                     0x00125950, 0x00125966, 0x00125A6C,
                     0x00213CCB, 0x001DD0B8,
                     0x001E687A, 0x001E693F,
                     0x00086ED2, 0x00080136,
                     0x0002B83A, 0x0002B8A1,
                     0x001DB6E8):
            lines.append(f"    recomp_trace_frame_callback(0x{start:08X});")
            lines.append(f"")
        if start == 0x001F443D:
            lines.extend([
                '    if (getenv("MM3_TRACE_1F443D") && g_icall_count >= 93800ULL) {',
                '        fprintf(stderr, "[1F443D-ENTRY] ic=%llu eax=%08X ebx=%08X ecx=%08X edi=%08X esi=%08X esp=%08X a8=%08X\\n",',
                '            (unsigned long long)g_icall_count, g_eax, g_ebx, g_ecx, g_edi, g_esi, g_esp,',
                '            MEM32(g_esp + 4));',
                '    }',
                '',
            ])
        # Generate code for each basic block
        # Create a set of addresses that need labels
        label_addrs = set()
        for bb in blocks:
            for succ in bb.successors:
                label_addrs.add(succ)
        # Also add any jump targets within the function
        for insn in instructions:
            if insn.jump_target and start <= insn.jump_target < end:
                label_addrs.add(insn.jump_target)
        # Add switch table targets (indirect jmp with intra-function table)
        for insn in instructions:
            if insn.mnemonic == "jmp" and not insn.jump_target and insn.operands:
                switch_targets = self.lifter._analyze_switch_table(insn.operands)
                for t in switch_targets:
                    label_addrs.add(t)

        flag_state = None
        snap_counter = [0]  # function-wide flag-snapshot temp name counter
        for bb in blocks:
            # Emit label if this block is a branch target
            if bb.start in label_addrs or bb.start == start:
                # The trailing ';' is load-bearing: C requires a statement after
                # a label, and a block whose instructions all emit comments only
                # (a lone `cmp`, which just sets flags for the next jcc) would
                # otherwise produce `loc_X:` immediately before `}` and fail to
                # compile. The null statement costs nothing and is always valid.
                lines.append(f"loc_{bb.start:08X}: ;")

            # Propagate flag state from previous block (fallthrough path).
            # This handles patterns like: test eax,eax / ja X / jb Y
            # where jb uses the same flags as ja from the preceding block.
            stmts, flag_state = lift_basic_block(
                self.lifter, bb, flag_state=flag_state,
                snap_counter=snap_counter, fpu_cmp_available=has_fpu_cmp)
            for stmt in stmts:
                lines.append(f"    {stmt}")
                if (start == 0x001BCBC0 and
                        stmt.strip() == "ebp = esp + -112;"):
                    lines.append("    recomp_trace_bcbcc0_state(ebp);")
                # MM3-only diagnostic: expose the two intermediate values in
                # the conversion loop without editing generated C by hand.
                if (start == 0x001E793E and
                        stmt.strip() == "ebx = MEM32(edi + 8);"):
                    lines.append("    recomp_trace_1e793e_load(edi, ebx, eax, ecx, edx);")
                if (start == 0x001E793E and
                        stmt.strip().startswith("ebx = ebx + edx * 4;")):
                    lines.append("    recomp_trace_1e793e_scaled(edi, ebx, eax, ecx, edx);")
                if (start == 0x001E793E and
                        "RECOMP_ICALL_SAFE(MEM32(edi + 0x18)" in stmt):
                    lines.insert(len(lines) - 1,
                                 "    recomp_trace_1e793e_callback(MEM32(edi + 0x18), edx, MEM32(ebp + -4));")
                if (start == 0x001E7AF4 and
                        stmt.strip() == "MEM32(ebp + -4) = eax;"):
                    lines.append("    recomp_trace_1e7af4_tile(edi, esi, ebp);")
                if (start == 0x00093B9D and
                        "sub_00097AFC();" in stmt):
                    lines.append("    recomp_trace_97afc_result((uint32_t)eax, (uint32_t)esp);")

            lines.append(f"")

        if start in (0x001EC708, 0x001E7F1B):
            lines = [line.replace("return;", "ebx = _saved_ebx; return;")
                     for line in lines]

        # Insert _icall_esp save points before RECOMP_ICALL_SAFE arg pushes.
        # The pattern is: optional PUSH32 args, then PUSH32(esp, 0); RECOMP_ICALL_SAFE(...).
        # We insert "uint32_t _icall_esp = g_esp;" before the first arg push.
        lines = _fixup_icall_esp_save(lines)

        # Balance callee-saved register saves in shared-epilogue functions
        # (see _fixup_unbalanced_saves). Must run after the ICALL fixup so the
        # register-push scan sees the final prologue layout. The SEH epilog is
        # excluded: its pops consume the SEH prolog's pushes (a different
        # function), so rebalancing it would rotate callee-saved registers.
        # A jump_target seed is a mid-function continuation (e.g. a longjmp
        # resume point). Its parent prologue owns the callee-saved pushes, so
        # injecting a fresh push block here shifts the inherited stack frame
        # and corrupts the restore slots.
        lines = _fixup_unbalanced_saves(
            lines, func_addr=start, seh_epilog=self.lifter.SEH_EPILOG,
            skip_rebalance=(func_info.get("detection_method") == "jump_target"))

        # Validate: comment out goto targets that reference missing labels
        # (dead code after unconditional jumps may reference non-existent labels)
        import re
        defined_labels = set()
        goto_lines = []
        for idx, line in enumerate(lines):
            lbl_match = re.match(r'^(loc_[0-9A-Fa-f]+):', line)
            if lbl_match:
                defined_labels.add(lbl_match.group(1))
            goto_match = re.search(r'goto (loc_[0-9A-Fa-f]+);', line)
            if goto_match:
                goto_lines.append((idx, goto_match.group(1)))
        for idx, target in goto_lines:
            if target not in defined_labels:
                lines[idx] = lines[idx].replace(
                    f"goto {target};",
                    f"(void)0; /* goto {target} - dead code, label not in function */")

        # Ensure labels at end of function have a statement after them.
        # In C, a label must be followed by a statement; a comment alone is not
        # enough.  Walk backwards from the end and if the last real content is a
        # label (with only blank lines / comments after it), insert "(void)0;".
        _last_label_idx = None
        _has_stmt_after = False
        for _ri in range(len(lines) - 1, -1, -1):
            _s = lines[_ri].strip()
            if not _s:
                continue
            if _s.startswith("/*") and _s.endswith("*/"):
                continue
            if re.match(r'^loc_[0-9A-Fa-f]+:', _s):
                _last_label_idx = _ri
                break
            _has_stmt_after = True
            break
        if _last_label_idx is not None and not _has_stmt_after:
            lines.insert(_last_label_idx + 1, "    (void)0;")

        # 0x00348120 is an original mid-function DPC entry added to the
        # generated metadata. Its shared tail owns the ret 0x10, but the
        # recovered entry has no terminal block in the generated CFG.
        if start == 0x00348120 and not any("return;" in line for line in lines):
            lines.append("    esp += 20; return; /* recovered ret 0x10 */")

        # Narrow, opt-in evidence for the simple asset-table accessor. Keep
        # this generator-owned so generated output is never hand-edited.
        if start == 0x001A3E8F:
            for _i, _line in enumerate(lines):
                if "esp += 8; return;" in _line:
                    lines.insert(_i, "    recomp_trace_asset_result(0x001A3E8F, (uint32_t)eax, (uint32_t)esp);")
                    break

        # Undefine FPU macros
        if has_fpu:
            lines.append(f"    #undef fp_push")
            lines.append(f"    #undef fp_pop")
            lines.append(f"    #undef fp_popp")
            lines.append(f"    #undef fp_top")
            lines.append(f"    #undef fp_st1")

        lines.append(f"}}")
        lines.append(f"")

        return "\n".join(lines)

    def _find_used_registers(self, instructions):
        """Find which 32-bit registers are referenced by any instruction."""
        regs = set()
        reg_map = {
            "eax": "eax", "ax": "eax", "al": "eax", "ah": "eax",
            "ebx": "ebx", "bx": "ebx", "bl": "ebx", "bh": "ebx",
            "ecx": "ecx", "cx": "ecx", "cl": "ecx", "ch": "ecx",
            "edx": "edx", "dx": "edx", "dl": "edx", "dh": "edx",
            "esi": "esi", "si": "esi",
            "edi": "edi", "di": "edi",
            "ebp": "ebp", "bp": "ebp",
            "esp": "esp", "sp": "esp",
        }
        for insn in instructions:
            for op in insn.operands:
                if op.type == "reg" and op.reg in reg_map:
                    regs.add(reg_map[op.reg])
                elif op.type == "mem":
                    if op.mem_base and op.mem_base in reg_map:
                        regs.add(reg_map[op.mem_base])
                    if op.mem_index and op.mem_index in reg_map:
                        regs.add(reg_map[op.mem_index])
        return regs

    def _find_used_xmm(self, instructions):
        """Find which XMM and MMX registers are used."""
        regs = set()
        for insn in instructions:
            for op in insn.operands:
                if op.type == "reg" and op.reg:
                    if op.reg.startswith("xmm") or op.reg.startswith("mm"):
                        regs.add(op.reg)
        return regs


class BatchTranslator:
    """Translates multiple functions and writes C source files."""

    def __init__(self, xbe_path, func_json_path, labels_json_path=None,
                 identified_json_path=None, abi_json_path=None,
                 output_dir=None, seh_prolog=None, seh_epilog=None):
        self.xbe_path = xbe_path
        self.output_dir = output_dir or os.path.join(
            os.path.dirname(__file__), "output")

        # Load XBE
        with open(xbe_path, "rb") as f:
            self.xbe_data = f.read()

        # Load function database
        with open(func_json_path, "r") as f:
            func_list = json.load(f)

        self.func_db = {}
        for func in func_list:
            addr = int(func["start"], 16)
            func["_addr"] = addr
            if "end" in func:
                func["end"] = int(func["end"], 16)
            self.func_db[addr] = func

        # Load labels
        self.label_db = {}
        if labels_json_path and os.path.exists(labels_json_path):
            with open(labels_json_path, "r") as f:
                labels = json.load(f)
            for lbl in labels:
                addr = int(lbl["address"], 16)
                self.label_db[addr] = lbl["name"]

        # Load classifications
        self.classification_db = {}
        if identified_json_path and os.path.exists(identified_json_path):
            with open(identified_json_path, "r") as f:
                identified = json.load(f)
            for entry in identified:
                addr = int(entry["start"], 16)
                self.classification_db[addr] = entry

        # Load ABI data
        self.abi_db = {}
        if abi_json_path and os.path.exists(abi_json_path):
            with open(abi_json_path, "r") as f:
                abi_list = json.load(f)
            for entry in abi_list:
                addr = int(entry["address"], 16)
                self.abi_db[addr] = entry

        # Detect the SEH helpers once here rather than per-Lifter, so the
        # result can be reported and overridden from the command line.
        if seh_prolog is None or seh_epilog is None:
            found_prolog, found_epilog = detect_seh_helpers(
                self.func_db, self.xbe_data, verbose=True)
            seh_prolog = seh_prolog if seh_prolog is not None else found_prolog
            seh_epilog = seh_epilog if seh_epilog is not None else found_epilog
        self.seh_prolog = seh_prolog
        self.seh_epilog = seh_epilog

        # Create translator
        self.translator = FunctionTranslator(
            self.xbe_data, self.func_db, self.label_db,
            self.classification_db, self.abi_db,
            seh_prolog=seh_prolog, seh_epilog=seh_epilog)

    def get_functions_by_category(self, categories=None, exclude_categories=None):
        """
        Get function addresses filtered by category.
        Returns list of (addr, func_info) tuples.
        """
        result = []
        for addr, func_info in sorted(self.func_db.items()):
            cls_info = self.classification_db.get(addr, {})
            cat = cls_info.get("category", "unknown")

            if categories and cat not in categories:
                continue
            if exclude_categories and cat in exclude_categories:
                continue

            result.append((addr, func_info))
        return result

    def _make_declaration(self, addr, name):
        """Generate a function declaration string.
        All translated functions are void(void) - args pass via stack,
        return values via g_eax."""
        return f"void {name}(void)"

    def translate_single(self, addr):
        """Translate a single function by address. Returns C code string."""
        func_info = self.func_db.get(addr)
        if not func_info:
            return None
        return self.translator.translate_function(addr, func_info)

    def translate_batch(self, func_list, output_file=None, max_funcs=None,
                        verbose=False):
        """
        Translate a batch of functions.

        func_list: list of (addr, func_info) tuples
        output_file: path to write combined C output
        max_funcs: limit number of functions
        verbose: print progress

        Returns dict with statistics.
        """
        os.makedirs(self.output_dir, exist_ok=True)

        if max_funcs:
            func_list = func_list[:max_funcs]

        stats = {
            "total": len(func_list),
            "translated": 0,
            "failed": 0,
            "total_lines": 0,
            "total_insns": 0,
        }

        c_chunks = []
        c_chunks.append("/**")
        c_chunks.append(" * Burnout 3: Takedown - Mechanically Translated Game Code")
        c_chunks.append(f" * Generated by tools/recomp from original Xbox x86 code.")
        c_chunks.append(f" * Functions: {len(func_list)}")
        c_chunks.append(" */")
        c_chunks.append("")
        c_chunks.append('#define RECOMP_GENERATED_CODE')
        c_chunks.append('#include "recomp_types.h"')
        c_chunks.append('#include <math.h>')
        c_chunks.append('#include <intrin.h>')
        c_chunks.append('#include <windows.h>')
        c_chunks.append("")
        c_chunks.append("/* Forward declarations */")

        # Forward declarations
        for addr, func_info in func_list:
            name = func_info.get("name", f"sub_{addr:08X}")
            decl = self._make_declaration(addr, name)
            c_chunks.append(f"{decl};")
        c_chunks.append("")
        c_chunks.append("/* ═══════════════════════════════════════════════════ */")
        c_chunks.append("")

        # Translate each function
        for i, (addr, func_info) in enumerate(func_list):
            name = func_info.get("name", f"sub_{addr:08X}")
            if verbose and (i % 100 == 0 or i == len(func_list) - 1):
                print(f"  [{i+1}/{len(func_list)}] Translating {name} at 0x{addr:08X}...")

            code = self.translator.translate_function(addr, func_info)
            if code:
                c_chunks.append(code)
                stats["translated"] += 1
                stats["total_lines"] += code.count("\n")

                # Count instructions
                num_insns = func_info.get("num_instructions", 0)
                stats["total_insns"] += num_insns
            else:
                c_chunks.append(f"/* FAILED to translate {name} at 0x{addr:08X} */")
                c_chunks.append(f"void {name}(void) {{ /* translation failed */ }}")
                c_chunks.append("")
                stats["failed"] += 1

        # Write output
        if output_file is None:
            output_file = os.path.join(self.output_dir, "recompiled.c")

        output_text = "\n".join(c_chunks)
        with open(output_file, "w", encoding="utf-8") as f:
            f.write(output_text)

        stats["output_file"] = output_file
        stats["output_size"] = len(output_text)

        return stats

    def translate_by_category(self, categories, output_prefix=None,
                              max_per_file=500, verbose=False):
        """
        Translate functions grouped by category, one file per category.
        Returns dict with per-category stats.
        """
        os.makedirs(self.output_dir, exist_ok=True)
        all_stats = {}

        for cat in categories:
            funcs = self.get_functions_by_category(categories={cat})
            if not funcs:
                continue

            prefix = output_prefix or cat
            out_file = os.path.join(self.output_dir, f"{prefix}.c")

            if verbose:
                print(f"\nCategory: {cat} ({len(funcs)} functions)")

            stats = self.translate_batch(
                funcs, output_file=out_file,
                max_funcs=max_per_file, verbose=verbose)
            all_stats[cat] = stats

        return all_stats

    def translate_batch_split(self, func_list, output_dir, chunk_size=1000,
                              header_name="recomp_funcs.h",
                              prefix="recomp", verbose=False,
                              max_chunk_bytes=None):
        """
        Translate functions into multiple .c files + a shared header.

        Generates:
          output_dir/recomp_funcs.h       - forward declarations for all functions
          output_dir/recomp_0000.c        - chunk 0
          output_dir/recomp_0001.c        - chunk 1
          ...
          output_dir/recomp_dispatch.c    - address -> function pointer table

        Returns dict with stats and list of generated files.
        """
        import sys

        os.makedirs(output_dir, exist_ok=True)

        # Translate all functions first, collecting results
        translations = []
        stats = {
            "total": len(func_list),
            "translated": 0,
            "failed": 0,
            "total_lines": 0,
        }

        for i, (addr, func_info) in enumerate(func_list):
            name = func_info.get("name", f"sub_{addr:08X}")
            if verbose and (i % 500 == 0 or i == len(func_list) - 1):
                print(f"  [{i+1}/{len(func_list)}] Translating {name}...",
                      file=sys.stderr)

            code = self.translator.translate_function(addr, func_info)
            if code:
                translations.append((addr, name, code))
                stats["translated"] += 1
                stats["total_lines"] += code.count("\n")
            else:
                # Stub for failed translations
                stub = f"/* FAILED: {name} at 0x{addr:08X} */\n"
                stub += f"void {name}(void) {{ /* translation failed */ }}\n"
                translations.append((addr, name, stub))
                stats["failed"] += 1

        # A call target can be absent from the selected roots while still being
        # a complete function in the disassembly database. Translate that
        # function before falling back to a stub; an empty body silently leaks
        # the guest fake-return slot and corrupts the caller's stack.
        defined = {name for _, name, _ in translations}
        for addr, name in sorted(self.translator.lifter.referenced_calls.items()):
            if name in defined or addr not in self.translator.func_db:
                continue
            info = self.translator.func_db[addr]
            code = self.translator.translate_function(addr, info)
            if code:
                translations.append((addr, name, code))
                defined.add(name)
                stats["translated"] += 1
                stats["total_lines"] += code.count("\n")

        # Any address called but never defined needs a stub, or the link fails.
        # These are almost all mid-function entry points the function detector
        # did not split out: a call lands a few bytes inside (or just past) a
        # function it already found. Emitting an empty stub keeps the build
        # linking; hitting one at runtime is a silent no-op, so they are
        # reported and written to their own file rather than hidden among the
        # translated chunks.
        defined = {name for _, name, _ in translations}
        unresolved = {
            addr: name
            for addr, name in self.translator.lifter.referenced_calls.items()
            if name not in defined
        }
        stats["unresolved_stubs"] = len(unresolved)

        # Generate header with all forward declarations
        header_path = os.path.join(output_dir, header_name)
        header_lines = [
            "/**",
            " * Burnout 3: Takedown - Recompiled Function Declarations",
            f" * {stats['translated']} functions, auto-generated by tools/recomp",
            " */",
            "",
            "#ifndef RECOMP_FUNCS_H",
            "#define RECOMP_FUNCS_H",
            "",
            '#include "recomp_types.h"',
            "",
        ]
        for addr, name, _ in translations:
            decl = self._make_declaration(addr, name)
            header_lines.append(f"{decl};")

        if unresolved:
            header_lines.append("")
            header_lines.append("/* Unresolved call targets (stubbed) */")
            for addr in sorted(unresolved):
                header_lines.append(f"void {unresolved[addr]}(void);")

        header_lines.extend(["", "#endif /* RECOMP_FUNCS_H */", ""])

        with open(header_path, "w", encoding="utf-8") as f:
            f.write("\n".join(header_lines))

        # Split translations into chunks and write .c files
        import glob
        generated_files = [header_path]
        chunks = []
        chunk = []
        chunk_bytes = 0
        for item in translations:
            item_bytes = len(item[2].encode("utf-8"))
            if chunk and (len(chunk) >= chunk_size or
                          (max_chunk_bytes and
                           chunk_bytes + item_bytes > max_chunk_bytes)):
                chunks.append(chunk)
                chunk = []
                chunk_bytes = 0
            chunk.append(item)
            chunk_bytes += item_bytes
        if chunk:
            chunks.append(chunk)

        for ci, chunk in enumerate(chunks):
            c_path = os.path.join(output_dir, f"{prefix}_{ci:04d}.c")
            c_lines = [
                "/**",
                f" * Burnout 3 - Recompiled code chunk {ci}",
                f" * Functions: {len(chunk)} "
                f"(0x{chunk[0][0]:08X} - 0x{chunk[-1][0]:08X})",
                " */",
                "",
                "#define RECOMP_GENERATED_CODE",
                f'#include "{header_name}"',
                '#include <math.h>',
                '#include <intrin.h>',
                '#include <windows.h>',
                "",
            ]
            for addr, name, code in chunk:
                c_lines.append(code)

            with open(c_path, "w", encoding="utf-8") as f:
                f.write("\n".join(c_lines))
            generated_files.append(c_path)

            if verbose:
                print(f"  Wrote {c_path} ({len(chunk)} functions)",
                      file=sys.stderr)

        # Remove stale chunk files from previous generations (the function
        # count can shrink when disassembly merges mid-function fragments).
        for stale in glob.glob(os.path.join(output_dir, f"{prefix}_*.c")):
            if stale not in generated_files:
                os.remove(stale)

        # Emit the stub bodies for call targets with no definition.
        if unresolved:
            stub_path = os.path.join(output_dir, f"{prefix}_stubs_unresolved.c")
            stub_lines = [
                "/**",
                " * Unresolved call target stubs",
                f" * {len(unresolved)} addresses called by translated code but not",
                " * detected as functions - typically mid-function entry points.",
                " * Auto-generated by tools/recomp.",
                " */",
                "",
                "#define RECOMP_GENERATED_CODE",
                f'#include "{header_name}"',
                "",
            ]
            for addr in sorted(unresolved):
                stub_lines.append(
                    f"void {unresolved[addr]}(void) {{ "
                    f"/* 0x{addr:08X}: not detected. Pop the return address so "
                    f"an unresolved call does not leak guest stack; the callee's "
                    f"own argument bytes are unknown and still leak. */ "
                    f"esp += 4; }}"
                )
            stub_lines.append("")

            with open(stub_path, "w", encoding="utf-8") as f:
                f.write("\n".join(stub_lines))
            generated_files.append(stub_path)

            if verbose:
                print(f"  Wrote {stub_path} ({len(unresolved)} stubs)",
                      file=sys.stderr)

        # Generate dispatch table
        dispatch_path = os.path.join(output_dir, f"{prefix}_dispatch.c")
        self._write_dispatch_table(translations, dispatch_path, header_name)
        generated_files.append(dispatch_path)

        stats["files"] = generated_files
        stats["num_chunks"] = len(chunks)
        stats["chunk_size"] = chunk_size
        return stats

    def _write_dispatch_table(self, translations, output_path, header_name):
        """
        Generate a dispatch table mapping Xbox VA -> function pointer.

        Uses a sorted array + binary search for O(log n) lookup.
        """
        lines = [
            "/**",
            " * Burnout 3 - Recompiled Function Dispatch Table",
            f" * Maps {len(translations)} Xbox VAs to translated function pointers.",
            " * Auto-generated by tools/recomp",
            " */",
            "",
            "#define RECOMP_DISPATCH_H",
            f'#include "{header_name}"',
            '#include <stddef.h>',
            "",
            "/* Generic function pointer type */",
            "typedef void (*recomp_func_t)(void);",
            "",
            "typedef struct {",
            "    uint32_t xbox_va;",
            "    recomp_func_t func;",
            "} recomp_entry_t;",
            "",
            f"static const recomp_entry_t g_recomp_table[] = {{",
        ]

        for addr, name, _ in translations:
            lines.append(f"    {{ 0x{addr:08X}u, (recomp_func_t){name} }},")

        lines.extend([
            "};",
            "",
            f"static const size_t g_recomp_table_size = "
            f"{len(translations)};",
            "",
            "/* Look up a function by Xbox VA.",
            " *",
            " * The binary search below is about 15 random accesses into a",
            " * table of tens of thousands of entries, and this is the single",
            " * hottest thing in the program: on the loading path 40% of all",
            " * guest function entries arrive here, because every indirect",
            " * call goes through it. A direct-mapped cache in front turns the",
            " * repeat case into one compare. Indirect-call targets repeat",
            " * heavily - the archive decoder calls the same few writers per",
            " * byte - so the hit rate is what matters, not the size.",
            " *",
            " * The table is immutable once built, so no invalidation is",
            " * needed. A zero slot cannot alias: VA 0 is not a function and",
            " * its cached NULL is the same answer the search would give. */",
            "#define RECOMP_LOOKUP_CACHE_BITS 13",
            "#define RECOMP_LOOKUP_CACHE_SIZE (1u << RECOMP_LOOKUP_CACHE_BITS)",
            "static uint32_t       g_lookup_cache_va[RECOMP_LOOKUP_CACHE_SIZE];",
            "static recomp_func_t  g_lookup_cache_fn[RECOMP_LOOKUP_CACHE_SIZE];",
            "",
            "recomp_func_t recomp_lookup(uint32_t xbox_va)",
            "{",
            "    uint32_t slot = (xbox_va >> 2) & (RECOMP_LOOKUP_CACHE_SIZE - 1u);",
            "    if (g_lookup_cache_va[slot] == xbox_va)",
            "        return g_lookup_cache_fn[slot];",
            "",
            "    size_t lo = 0, hi = g_recomp_table_size;",
            "    while (lo < hi) {",
            "        size_t mid = lo + (hi - lo) / 2;",
            "        if (g_recomp_table[mid].xbox_va < xbox_va)",
            "            lo = mid + 1;",
            "        else if (g_recomp_table[mid].xbox_va > xbox_va)",
            "            hi = mid;",
            "        else {",
            "            g_lookup_cache_va[slot] = xbox_va;",
            "            g_lookup_cache_fn[slot] = g_recomp_table[mid].func;",
            "            return g_recomp_table[mid].func;",
            "        }",
            "    }",
            "    return NULL;",
            "}",
            "",
            "/* Get the number of registered functions */",
            "size_t recomp_get_count(void)",
            "{",
            "    return g_recomp_table_size;",
            "}",
            "",
            "/* Call all registered functions (for bulk testing) */",
            "size_t recomp_call_all(void)",
            "{",
            "    size_t i;",
            "    for (i = 0; i < g_recomp_table_size; i++) {",
            "        g_recomp_table[i].func();",
            "    }",
            "    return g_recomp_table_size;",
            "}",
            "",
        ])

        with open(output_path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines))
