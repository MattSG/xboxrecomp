"""
Function boundary detection for the disassembler.

Implements multi-pass function detection with confidence scoring:
1. Known addresses (entry point)
2. Standard prologues (push ebp; mov ebp, esp)
3. CC padding boundaries (CC run after ret)
3b. Packed glue-thunk tables (E8+A3+C3, no inter-function padding)
4. Call targets (destinations of call instructions)
5. Cross-validation and overlap resolution
"""

from dataclasses import dataclass, field
from typing import Dict, List, Optional, Set, Tuple
import struct

from . import config
from .engine import DisasmEngine, Instruction
from .loader import BinaryImage, SectionInfo, DATA_SECTION_NAMES
from .xrefs import XRefTracker, XRefType
from .labels import LabelManager, Label, LabelType


@dataclass
class Function:
    """A detected function with boundaries and metadata."""
    start: int
    end: int           # Address after last instruction
    name: str
    section: str = ""
    confidence: float = 0.0
    detection_method: str = ""

    # Call graph data
    calls_to: List[int] = field(default_factory=list)     # Functions this calls
    called_by: List[int] = field(default_factory=list)     # Functions that call this

    # Instruction stats
    num_instructions: int = 0
    has_prologue: bool = False

    @property
    def size(self) -> int:
        return self.end - self.start

    def to_dict(self) -> dict:
        return {
            "start": f"0x{self.start:08X}",
            "end": f"0x{self.end:08X}",
            "size": self.size,
            "name": self.name,
            "section": self.section,
            "confidence": self.confidence,
            "detection_method": self.detection_method,
            "num_instructions": self.num_instructions,
            "has_prologue": self.has_prologue,
            "calls_to": [f"0x{a:08X}" for a in self.calls_to],
            "called_by": [f"0x{a:08X}" for a in self.called_by],
        }


class FunctionDetector:
    """
    Multi-pass function boundary detector.

    Identifies function start addresses through multiple heuristics,
    then determines function boundaries by following instruction flow
    until the next function or a terminal instruction.
    """

    def __init__(self, engine: DisasmEngine, image: BinaryImage,
                 xrefs: XRefTracker, labels: LabelManager):
        self.engine = engine
        self.image = image
        self.xrefs = xrefs
        self.labels = labels

        # Candidate function starts: address -> (confidence, method)
        self._candidates: Dict[int, Tuple[float, str]] = {}

        # Final function list
        self.functions: Dict[int, Function] = {}

    def detect_all(self, sections: Optional[List[SectionInfo]] = None) -> int:
        """
        Run all detection passes and build the function database.

        Args:
            sections: Sections to analyze. If None, uses all executable sections.

        Returns:
            Number of functions detected.
        """
        if sections is None:
            sections = self.image.get_code_sections()

        # Pass 1: Known addresses
        self._pass_known_addresses()

        # Pass 2: Prologue patterns
        for sec in sections:
            self._pass_prologues(sec)

        # Pass 3: CC padding boundaries
        for sec in sections:
            self._pass_cc_boundaries(sec)

        # Pass 4: Call targets
        self._pass_call_targets(sections)

        # Pass 4a: Switch-dispatch jump tables
        # jmp [index*scale + table] reads its destination from an embedded
        # dword table the linear sweep decodes as code, so the case leaves
        # never become instruction starts. Seed them before function
        # building so each leaf gets its own translation.
        self._pass_indirect_jump_tables(sections)

        # Pass 4b: Packed glue-thunk tables (no padding between leaf functions)
        # Runs after call_target so the CALL destinations are in _candidates.
        for sec in sections:
            self._pass_thunk_tables(sec)

        # Pass 4c: Jump targets (tail calls / intra-function entry points)
        # The lifter treats any jump/cond_jump target outside the current
        # function as an external call and emits a named call to it. If that
        # target is not a registered function start, the recomp generates an
        # empty stub, and the missing body returns garbage eax (MM3: 0x950EC
        # shared body inside sub_000950E2). This pass needs function extents,
        # so it runs iteratively AFTER _build_functions (Pass 5) below.
        # (Implementation in _pass_external_jump_targets.)

        # Pass 4d: Data-table function pointers (.data/.rdata)
        # Functions referenced only through data tables (vtables, callback
        # tables) have zero direct call/jump xrefs and are invisible to every
        # code-scan pass. MM3: 0x00257E12 is stored in the .data callback
        # table at 0x391F30 and never referenced from code. Scan writable
        # data for dwords that point at decoded instruction boundaries in
        # executable sections and register them as function starts.
        self._pass_data_pointer_targets(sections)

        # Pass 4e: Code-immediate function pointers.
        # Function pointers are also stored from code as immediates, e.g.
        # D3DX vtable constructors (`mov [eax+0x28], 0x2680ED`). Those
        # targets have no prologue and no call/jump/data-table xref, so no
        # earlier pass sees them; the runtime falls through to the D3DX safe
        # stub and returns a fake pointer.
        self._pass_code_immediate_targets(sections)

        # Pass 5: Build functions from candidates
        self._build_functions(sections)

        # Pass 5b: External jump targets (iterative).
        # Loop back-edges land INSIDE their own function and must not become
        # function starts; external tail-call targets must. Extents are only
        # known after _build_functions, so scan, rebuild, repeat until the
        # candidate set stabilizes (usually 1-2 rounds).
        for _round in range(4):
            added = self._pass_external_jump_targets(sections)
            if not added:
                break
            self._build_functions(sections)

        # Populate call graph
        self._build_call_graph()

        return len(self.functions)

    def _add_candidate(self, addr: int, confidence: float, method: str) -> None:
        """Add a function start candidate, keeping highest confidence."""
        existing = self._candidates.get(addr)
        if existing is None or confidence > existing[0]:
            self._candidates[addr] = (confidence, method)

    def _pass_known_addresses(self) -> None:
        """Pass 1: Add known function addresses."""
        # Entry point
        self._add_candidate(
            self.image.entry_point,
            config.CONFIDENCE_KNOWN,
            "entry_point"
        )

    def _pass_prologues(self, section: SectionInfo) -> None:
        """
        Pass 2: Scan for standard function prologues.

        Detects:
        - Standard: push ebp (0x55); mov ebp, esp (0x8BEC or 0x89E5)
        - SEH:      push imm8 (0x6A); push imm32 (0x68); call rel32 (0xE8)
        """
        data = self.image.get_section_data(section)
        if not data:
            return

        va_start = section.virtual_addr
        i = 0
        while i < len(data) - 2:
            # Check for push ebp; mov ebp, esp
            if data[i] == 0x55:
                if (i + 2 < len(data) and
                        data[i + 1] == 0x8B and data[i + 2] == 0xEC):
                    addr = va_start + i
                    if addr in self.engine.instructions:
                        self._add_candidate(
                            addr,
                            config.CONFIDENCE_PROLOGUE,
                            "prologue"
                        )
                    i += 3
                    continue
                elif (i + 2 < len(data) and
                      data[i + 1] == 0x89 and data[i + 2] == 0xE5):
                    addr = va_start + i
                    if addr in self.engine.instructions:
                        self._add_candidate(
                            addr,
                            config.CONFIDENCE_PROLOGUE,
                            "prologue_alt"
                        )
                    i += 3
                    continue
            # Check for SEH prologue: push imm8; push imm32; call rel32
            # Pattern: 6A XX 68 XX XX XX XX E8 XX XX XX XX
            if data[i] == 0x6A and i + 10 < len(data):
                if (data[i + 2] == 0x68 and data[i + 7] == 0xE8):
                    addr = va_start + i
                    if addr in self.engine.instructions:
                        self._add_candidate(
                            addr,
                            config.CONFIDENCE_PROLOGUE * 0.9,
                            "seh_prologue"
                        )
                    i += 10
                    continue
            i += 1

    def _pass_cc_boundaries(self, section: SectionInfo) -> None:
        """
        Pass 3: Find function boundaries at CC padding.

        Pattern: ret instruction, followed by one or more 0xCC bytes,
        followed by the start of the next function.
        """
        data = self.image.get_section_data(section)
        if not data:
            return

        va_start = section.virtual_addr
        i = 0

        while i < len(data):
            # Look for CC padding runs
            if data[i] == config.CC_PADDING:
                cc_start = i
                while i < len(data) and data[i] == config.CC_PADDING:
                    i += 1

                cc_run_length = i - cc_start

                if cc_run_length >= config.MIN_CC_RUN and i < len(data):
                    # Check if instruction before CC run was a ret
                    before_addr = va_start + cc_start
                    found_ret = False
                    for check_offset in range(1, 4):
                        check_addr = before_addr - check_offset
                        insn = self.engine.get_instruction(check_addr)
                        if insn and insn.is_ret and insn.end_address == before_addr:
                            found_ret = True
                            break

                    if found_ret:
                        next_addr = va_start + i
                        if next_addr in self.engine.instructions:
                            self._add_candidate(
                                next_addr,
                                config.CONFIDENCE_CC_BOUNDARY,
                                "cc_boundary"
                            )
            else:
                i += 1

    def _pass_thunk_tables(self, section: SectionInfo) -> None:
        """
        Pass 3b: Detect packed glue-thunk tables.

        Pattern: consecutive 11-byte leaf functions with zero padding:
            E8 rel32        CALL  <real_function>
            A3 abs32        MOV   [abs32], EAX
            C3              RET

        These are common in D3DX/XGRPH glue thunks. They lack prologues
        and have no CC padding between entries, so the standard prologue
        and CC-boundary passes miss them.

        Only accepts candidates where:
        - Section is executable
        - Exact 3-instruction sequence: E8 rel32 + A3 abs32 + C3
        - Bytes decode as valid instructions at exact boundaries
        - CALL target is in an executable section with a decoded instruction
        - A3 destination is a writable address in the XBE image
        - Candidate doesn't overlap an existing function
        """
        if not section.executable:
            return

        data = self.image.get_section_data(section)
        if not data or len(data) < 11:
            return

        va_start = section.virtual_addr
        existing_funcs = set(self._candidates.keys())

        i = 0
        while i <= len(data) - 11:
            # Check exact 11-byte thunk pattern
            if (data[i] == 0xE8 and          # CALL rel32
                data[i + 5] == 0xA3 and      # MOV [abs32], EAX
                data[i + 10] == 0xC3):       # RET

                addr = va_start + i

                # (a) Must be at a decoded instruction boundary
                if addr not in self.engine.instructions:
                    i += 1
                    continue

                # (b) All 3 instructions must decode to exact boundaries
                insn0 = self.engine.get_instruction(addr)
                insn1 = self.engine.get_instruction(addr + 5)
                insn2 = self.engine.get_instruction(addr + 10)
                if not (insn0 and insn1 and insn2):
                    i += 1
                    continue
                if not (insn0.end_address == addr + 5 and
                        insn1.end_address == addr + 10 and
                        insn2.end_address == addr + 11):
                    i += 1
                    continue

                # (c) CALL target must be in executable section with
                #     a decoded instruction AND already a known function
                #     candidate from earlier passes (prologue/cc_boundary/
                #     entry_point). This prevents matching thunks whose
                #     CALL targets happen to decode but aren't real functions.
                call_target = insn0.call_target
                if call_target is None:
                    i += 1
                    continue
                target_sec = self.image.get_section_at_va(call_target)
                if not target_sec or not target_sec.executable:
                    i += 1
                    continue
                if call_target not in self.engine.instructions:
                    i += 1
                    continue
                if call_target not in existing_funcs:
                    i += 1
                    continue

                # (d) A3 destination must be writable (in .data or .rdata)
                store_dest = struct.unpack_from('<I', data, i + 6)[0]
                dest_sec = self.image.get_section_at_va(store_dest)
                if not dest_sec or not dest_sec.writable:
                    i += 1
                    continue

                # (e) Must not overlap an existing function
                #     Check that no existing function contains addr
                if addr in existing_funcs:
                    i += 1
                    continue

                self._add_candidate(
                    addr,
                    config.CONFIDENCE_CALL_TARGET * 0.9,
                    "thunk_table"
                )
                i += 11
                continue

            i += 1

    def _pass_call_targets(self, sections: List[SectionInfo]) -> None:
        """
        Pass 4: Add destinations of direct call instructions as function starts.
        """
        call_targets = self.engine.get_call_targets()
        for target in call_targets:
            if target in self.engine.instructions:
                section = self.image.get_section_at_va(target)
                if section and section.executable:
                    self._add_candidate(
                        target,
                        config.CONFIDENCE_CALL_TARGET,
                        "call_target"
                    )
            else:
                self._seed_call_target(target)

    def _seed_call_target(self, target: int) -> None:
        """Recover a call destination the linear sweep misaligned."""
        self._seed_code_target(target, "call_target")

    def _seed_code_target(self, target: int, method: str,
                          stop_at_terminator: bool = False) -> None:
        """Recover a code address the linear sweep misaligned.

        The sweep walks embedded data (jump tables, padding) as code, so a
        code address can land off every decoded boundary and never becomes
        an instruction start (MM3 0x346B30: the sweep consumed its first
        byte as the tail of a data-table instruction; sub_00346C80's switch
        leaves at 0x346C95/0x346C9C/0x346CA3 were consumed by the jump-table
        dwords). A call/jump-table destination is code by construction, so
        decode it from the raw bytes at that address and add the
        instructions for the function detector. stop_at_terminator bounds
        the decode to the first ret/jmp for switch leaves, which sit right
        before the data table the linear sweep already walked as garbage.
        """
        section = self.image.get_section_at_va(target)
        if section is None or not section.executable:
            return
        data = self.image.get_section_data(section)
        if not data:
            return
        off = target - section.virtual_addr
        if off < 0 or off >= len(data):
            return
        insns = list(self.engine._cs.disasm(data[off:off + 4096], target))
        if not insns:
            return
        for cs_insn in insns:
            insn = self.engine._classify_instruction(cs_insn)
            self.engine.instructions[insn.address] = insn
            if stop_at_terminator and (insn.is_ret or insn.is_jump):
                break
        self.engine._sorted_addrs = None
        confidence = (config.CONFIDENCE_CALL_TARGET if method == "call_target"
                      else config.CONFIDENCE_CALL_TARGET * 0.85)
        self._add_candidate(
            target,
            confidence,
            method
        )

    def _pass_indirect_jump_tables(self, sections: List[SectionInfo]) -> None:
        """Seed switch-dispatch case leaves (jmp [index*scale + table]).

        The linear sweep walks the embedded jump table as code, so the case
        leaves after the dispatch never decode to instruction starts. The
        recomp then emits RECOMP_ITAIL for a VA with no generated function
        -> unresolved switch tail that drops the caller's frame (MM3
        sub_00346C80: the runtime fell to safe_stub and leaked 4 esp bytes
        into sub_00341E50). Reading the table dwords recovers the leaves.
        """
        # Snapshot: _seed_jump_table mutates engine.instructions.
        for insn in list(self.engine.instructions.values()):
            if insn.table_ref is None:
                continue
            self._seed_jump_table(insn.table_ref)

    def _read_switch_targets(self, table_va: int) -> List[int]:
        """Absolute dword targets of a switch table, in order.

        Same walk as _seed_jump_table, factored out so the extent walk can
        follow a switch dispatch without seeding candidates.
        """
        section = self.image.get_section_at_va(table_va)
        if section is None:
            return []
        data = self.image.get_section_data(section)
        if not data:
            return []
        off = table_va - section.virtual_addr
        if off < 0 or off + 4 > len(data):
            return []
        targets = []
        for i in range(256):
            o = off + i * 4
            if o + 4 > len(data):
                break
            target = struct.unpack_from('<I', data, o)[0]
            target_sec = self.image.get_section_at_va(target)
            if target_sec is None or not target_sec.executable:
                break
            targets.append(target)
        return targets

    def _seed_jump_table(self, table_va: int) -> None:
        """Read a switch table's absolute dword targets and seed each leaf."""
        section = self.image.get_section_at_va(table_va)
        if section is None:
            return
        data = self.image.get_section_data(section)
        if not data:
            return
        off = table_va - section.virtual_addr
        if off < 0 or off + 4 > len(data):
            return
        for i in range(256):
            o = off + i * 4
            if o + 4 > len(data):
                break
            target = struct.unpack_from('<I', data, o)[0]
            target_sec = self.image.get_section_at_va(target)
            if target_sec is None or not target_sec.executable:
                break
            self._seed_code_target(target, "jump_table",
                                   stop_at_terminator=True)

    def _pass_external_jump_targets(self, sections: List[SectionInfo]) -> int:
        """
        Pass 5b: Register jump targets that leave their containing function.

        Mirrors the lifter's _is_external_target logic: any unconditional
        jump or conditional-jump target OUTSIDE [func.start, func.end) is
        treated as an external call (tail call / conditional tail call) and
        emitted as a named call. Targets INSIDE the same function (loop
        back-edges, branch targets) become `goto` and are NOT entry points.

        Returns the number of new candidates added (0 = stable).
        """
        funcs = list(self.functions.values())
        added = 0
        for func in funcs:
            insns = self.engine.get_instructions_in_range(func.start, func.end)
            for insn in insns:
                target = insn.jump_target
                if target is None:
                    continue
                if func.start <= target < func.end:
                    continue  # internal branch (loop back-edge, if/else)
                if target in self._candidates:
                    continue
                if target not in self.engine.instructions:
                    continue
                section = self.image.get_section_at_va(target)
                if section is None or not section.executable:
                    continue
                self._add_candidate(
                    target,
                    config.CONFIDENCE_CALL_TARGET * 0.85,
                    "jump_target"
                )
                added += 1
        return added

    def _pass_data_pointer_targets(self, sections: List[SectionInfo]) -> None:
        """
        Pass 4d: Register data-table function pointers as function starts.

        Functions reachable only through data tables (vtables, callback
        tables, class registries) have zero direct call/jump xrefs, so no
        code-scan pass can see them. MM3: the init callback table at
        0x391F30 stores 0x00257E12 (a D3DX thunk) which is only ever called
        via ICALL from the table walker — never from code. Without a
        dispatch entry it falls to the D3DX safe stub and returns a fake
        pointer.

        Scan writable data sections for 32-bit values that point at decoded
        instruction boundaries inside executable sections. Each hit becomes
        a function-start candidate. Only data sections with raw bytes are
        scanned (BSS has no table content).
        """
        text_secs = [s for s in sections if s.executable]
        if not text_secs:
            return
        # Valid instruction boundaries in executable sections
        code_addrs = set(self.engine.instructions.keys())
        # Candidate data sections: conventional PE data sections by name.
        # XBE linkers mark .data/.rdata executable, so the executable flag
        # is NOT a reliable discriminator — name is (loader.DATA_SECTION_NAMES
        # is the same list get_code_sections uses to exclude them).
        data_secs = [s for s in self.image.sections
                     if (s.name in DATA_SECTION_NAMES or
                         s.name.upper().endswith("DATA")) and s.raw_size >= 4]
        for sec in data_secs:
            data = self.image.get_section_data(sec)
            if not data:
                continue
            # Only scan the raw portion (BSS tail has no initialized data)
            n = min(len(data), sec.raw_size) - 3
            offsets = range(0, n, 4)
            if sec.name not in DATA_SECTION_NAMES:
                # Library *DATA sections can mix opaque codec state with a
                # dense callback table at the end. Scanning every random-looking
                # dword creates false function starts; accept only a dense tail.
                words = [struct.unpack_from('<I', data, i)[0] for i in offsets]
                dense = set()
                for j in range(len(words) - 7):
                    window = words[j:j + 16]
                    if sum(val in code_addrs for val in window) * 4 >= len(window) * 3:
                        dense.update(k for k, val in enumerate(window, j)
                                     if val in code_addrs)
                offsets = (range(min(dense) * 4, (max(dense) + 1) * 4, 4)
                           if dense else ())
            for i in offsets:
                val = struct.unpack_from('<I', data, i)[0]
                if val not in code_addrs:
                    continue
                target_sec = self.image.get_section_at_va(val)
                if target_sec is None or not target_sec.executable:
                    continue
                if val in self._candidates:
                    continue
                self._add_candidate(
                    val,
                    config.CONFIDENCE_CALL_TARGET * 0.8,
                    "data_pointer"
                )

    def _pass_code_immediate_targets(self, sections: List[SectionInfo]) -> None:
        """Pass 4e: seed function starts from code-loaded function pointers.

        Some functions are reachable only through pointer tables built at
        runtime by `mov [reg+disp], imm32` instructions. Scan every decoded
        code instruction's ``imm_ref`` for values that land on decoded
        instruction boundaries inside code sections and register them as
        function-start candidates.
        """
        code_sections = [s for s in sections if s.executable]
        if not code_sections:
            return
        code_sec_names = {s.name for s in code_sections}
        code_addrs = set(self.engine.instructions.keys())
        for insn in self.engine.instructions.values():
            target = insn.imm_ref
            if target is None or target in self._candidates:
                continue
            if target not in code_addrs:
                continue
            target_sec = self.image.get_section_at_va(target)
            if target_sec is None or target_sec.name not in code_sec_names:
                continue
            self._add_candidate(
                target,
                config.CONFIDENCE_CALL_TARGET * 0.7,
                "code_imm"
            )

    def _build_functions(self, sections: List[SectionInfo]) -> None:
        """
        Pass 5: Build Function objects from candidates.

        Determines function boundaries by finding the extent of each
        function (up to the next function start or unreachable point).

        Intra-entry candidates (jump_target / data_pointer) do NOT bound
        the parent function: a tail-jump or data-table pointer can land in
        the middle of another function's body (shared code body), and the
        parent must keep its full terminator-based extent. Each intra
        candidate still gets its own Function entry with its own extent
        (overlapping ranges are intentional — trap #38: each entry point
        produces its own translation starting at the right offset).
        """
        INTRA_METHODS = {"jump_target", "data_pointer", "jump_table", "code_imm"}

        sorted_starts = sorted(self._candidates.keys())
        if not sorted_starts:
            return

        sec_ranges = {}
        for sec in sections:
            sec_ranges[sec.name] = (sec.virtual_addr,
                                    sec.virtual_addr + sec.virtual_size)

        for idx, start_addr in enumerate(sorted_starts):
            confidence, method = self._candidates[start_addr]

            section = self.image.get_section_at_va(start_addr)
            sec_name = section.name if section else ""

            # Next REAL function boundary: skip intra-entry candidates that
            # live inside this function's body (shared entry points).
            next_func = None
            for j in range(idx + 1, len(sorted_starts)):
                cand = sorted_starts[j]
                cand_method = self._candidates[cand][1]
                if cand_method in INTRA_METHODS:
                    continue
                # seh_prologue (push imm8; push imm32; call rel32) is byte-for-
                # byte the same as a normal two-arg call sequence, so a
                # continuation chunk of the previous function is routinely
                # mis-detected as a function start (MM3 sub_001EC520 ->
                # 0x001EC5D0: the chunk shares the parent's frame, pops the
                # registers the parent pushed, and returns with the parent's
                # ret 0xc). A continuation is entered only by a jump from the
                # parent (or plain fall-through); it has no CALL, data-pointer,
                # or other reference. Such a candidate must not bound the
                # parent: let the parent's CFG walk fall through / jump into it
                # and merge. Any non-jump reference (CALL, data_imm handler
                # table, ...) means a real function start: keep it as a
                # boundary (MM3 sub_00083A6C is an SEH handler reached via a
                # data_imm xref from 0x83B16 and must stay split).
                if (cand_method == "seh_prologue"
                        and all(r.xref_type in (XRefType.JUMP,
                                                XRefType.COND_JUMP)
                                for r in self.xrefs.get_refs_to(cand))):
                    continue
                next_func = cand
                break

            sec_end = None
            if section:
                sec_end = section.virtual_addr + section.virtual_size

            end_addr = self._find_function_end(start_addr, next_func, sec_end)

            insns = self.engine.get_instructions_in_range(start_addr, end_addr)
            num_insns = len(insns)

            if num_insns == 0:
                continue

            first_insn = self.engine.get_instruction(start_addr)
            has_prologue = (first_insn is not None and
                            first_insn.mnemonic == "push" and
                            first_insn.op_str == "ebp")

            label = self.labels.get(start_addr)
            if label:
                name = label.name
            else:
                name = f"sub_{start_addr:08X}"
                self.labels.auto_name_function(
                    start_addr, sec_name, confidence)

            func = Function(
                start=start_addr,
                end=end_addr,
                name=name,
                section=sec_name,
                confidence=confidence,
                detection_method=method,
                num_instructions=num_insns,
                has_prologue=has_prologue,
            )
            self.functions[start_addr] = func

    def _find_function_end(self, start: int, next_func: Optional[int],
                           sec_end: Optional[int]) -> int:
        """Determine where a function ends.

        Walks the function's control flow, following every forward jump
        target inside [start, upper). A mid-function continuation reachable
        only through a conditional branch (shared epilogue / if-else chunk
        after an early ret) is still part of this function: the original
        single-addr walk stopped at the first ret and left those targets
        outside the extent, so pass 5b split one function into fragments
        (MM3 pool allocator: 0x8454C split from its 0x84592 continuation,
        corrupting callee-saved register/arg semantics).
        """
        max_addr = start
        visited = set()

        upper = sec_end if sec_end else start + 0x100000
        if next_func and next_func < upper:
            upper = next_func

        worklist = [start]
        while worklist:
            addr = worklist.pop()
            while addr < upper:
                # Guard against infinite loops when following internal jmp
                # targets (e.g. a jmp back to an earlier block). Since the
                # walk only follows forward jumps inside [start, upper), a
                # revisit means we are in a cycle we already covered.
                if addr in visited:
                    break
                visited.add(addr)

                insn = self.engine.get_instruction(addr)
                if insn is None:
                    break

                end = insn.end_address
                if end > max_addr:
                    max_addr = end

                # A switch dispatch (jmp [index*scale + table]) has no
                # jump_target, so the walk used to stop at it and every block
                # reachable only through the table fell outside the extent.
                # When those blocks hold the epilogue, the function loses its
                # register restores: MM3 sub_0008A368 ended at 0x0008AB43 and
                # dropped "pop ebx" at 0x0008AB7A, so it pushed EBX six times
                # and popped it never. Its caller sub_000888CF then passed a
                # stale loop cursor where a container pointer belonged, and
                # the allocator walked a null bucket table. Same principle as
                # the conditional-branch case below: a block reachable only
                # through the dispatch is still part of this function.
                if insn.table_ref is not None:
                    for t in self._read_switch_targets(insn.table_ref):
                        if start <= t < upper and t not in visited:
                            worklist.append(t)

                if insn.jump_target is not None:
                    target = insn.jump_target
                    if (start <= target < upper and target not in visited):
                        if insn.is_cond_jump:
                            # Conditional branch into the same function's
                            # continuation: walk it like any other block.
                            worklist.append(target)
                        elif insn.is_jump:
                            addr = target
                            continue

                if insn.is_ret:
                    break

                addr = insn.end_address

        return max_addr

    def _build_call_graph(self) -> None:
        """Populate calls_to and called_by for all functions."""
        func_starts = set(self.functions.keys())

        for func in self.functions.values():
            insns = self.engine.get_instructions_in_range(func.start, func.end)
            callees = set()
            for insn in insns:
                if insn.call_target is not None:
                    callees.add(insn.call_target)

            func.calls_to = sorted(callees)

            for callee_addr in callees:
                callee = self.functions.get(callee_addr)
                if callee is not None:
                    callee.called_by.append(func.start)

        for func in self.functions.values():
            func.called_by = sorted(set(func.called_by))

    def get_function_at(self, addr: int) -> Optional[Function]:
        """Get the function containing an address."""
        if addr in self.functions:
            return self.functions[addr]
        for func in self.functions.values():
            if func.start <= addr < func.end:
                return func
        return None

    def get_functions_in_section(self, section_name: str) -> List[Function]:
        """Get all functions in a section, sorted by address."""
        return sorted(
            [f for f in self.functions.values() if f.section == section_name],
            key=lambda f: f.start
        )

    def summary(self) -> dict:
        """Return summary statistics."""
        by_method: Dict[str, int] = {}
        by_section: Dict[str, int] = {}
        total_insns = 0
        with_prologue = 0

        for func in self.functions.values():
            by_method[func.detection_method] = by_method.get(
                func.detection_method, 0) + 1
            by_section[func.section] = by_section.get(func.section, 0) + 1
            total_insns += func.num_instructions
            if func.has_prologue:
                with_prologue += 1

        return {
            "total_functions": len(self.functions),
            "total_instructions_in_functions": total_insns,
            "with_prologue": with_prologue,
            "by_detection_method": by_method,
            "by_section": by_section,
        }
