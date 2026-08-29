"""
x86 → C instruction lifter.

Translates individual x86 instructions (and common multi-instruction
patterns like cmp+jcc) into C statements using the recomp_types.h macros.

Register model:
  - eax, ebx, ecx, edx, esi, edi, ebp: uint32_t locals
  - esp: uint32_t local (stack pointer)
  - FPU: double fp_stack[8] with fp_top index

Memory model:
  - MEM8/MEM16/MEM32 macros for memory access at flat addresses
  - Xbox data sections mapped at original VAs
"""

import struct

from .disasm import Disassembler, Instruction, Operand
from .config import is_code_address, is_data_address, va_to_file_offset


# ── Operand formatting ──────────────────────────────────────

def _fmt_reg(name, size=4):
    """Format a register name as a C expression."""
    if not name:
        return "0"

    # Segment registers → constants
    if name in ("fs", "gs", "cs", "ds", "es", "ss"):
        return f"0 /* seg:{name} */"

    # Map sub-registers to expressions on 32-bit locals
    SUB_REGS = {
        "al": "LO8(eax)", "ah": "HI8(eax)", "ax": "LO16(eax)",
        "bl": "LO8(ebx)", "bh": "HI8(ebx)", "bx": "LO16(ebx)",
        "cl": "LO8(ecx)", "ch": "HI8(ecx)", "cx": "LO16(ecx)",
        "dl": "LO8(edx)", "dh": "HI8(edx)", "dx": "LO16(edx)",
        "si": "LO16(esi)", "di": "LO16(edi)",
        "bp": "LO16(ebp)", "sp": "LO16(esp)",
    }
    if name in SUB_REGS:
        return SUB_REGS[name]
    return name


# Sub-register -> width in bytes, for masking immediates in cmp/test so the
# flag math runs at the operand width (see _normalize_cmp_operands).
REG_WIDTH = {
    "eax": 4, "ebx": 4, "ecx": 4, "edx": 4, "esi": 4, "edi": 4,
    "ebp": 4, "esp": 4,
    "ax": 2, "bx": 2, "cx": 2, "dx": 2, "si": 2, "di": 2, "bp": 2, "sp": 2,
    "al": 1, "bl": 1, "cl": 1, "dl": 1, "ah": 1, "bh": 1, "ch": 1, "dh": 1,
}


def _normalize_cmp_operands(ops):
    """Mask 32-bit immediates down to a sub-32-bit register operand's width.

    Capstone sign-extends e.g. cmp si,-1 to imm=0xFFFFFFFF, but x86 flag
    math runs at the register width (16 bits). Without masking, conditions
    like CMP_NE(LO16(si), 0xFFFFFFFFu) are always true, so 8/16-bit compare
    guards never take the equal path (MM3 sub_20F77D crash trace). No-op for
    full-width registers and non-imm pairs. Returns a new operand list.
    """
    if len(ops) >= 2 and ops[0].type == "reg" and ops[1].type == "imm":
        w = REG_WIDTH.get(ops[0].reg)
        if w and w < 4:
            mask = (1 << (8 * w)) - 1
            return [ops[0], Operand(type="imm", imm=ops[1].imm & mask)] + list(ops[2:])
    return list(ops)


def _flag_operand_size(op):
    """Byte width of a flag-setter operand (1/2/4)."""
    if op.type == "reg":
        return getattr(op, "size", 0) or REG_WIDTH.get(op.reg) or 4
    if op.type == "mem":
        return op.mem_size or 4
    return 4


def _signed_cast(expr, size):
    """Cast expr to the signed width of the flag-setter operands."""
    if size == 1:
        return f"(int8_t)({expr})"
    if size == 2:
        return f"(int16_t)({expr})"
    return f"(int32_t)({expr})"


def _fmt_set_reg(name, value_expr):
    """Format assignment to a register, handling sub-register writes."""
    # Segment registers → no-op
    if name in ("fs", "gs", "cs", "ds", "es", "ss"):
        return f"/* mov {name}, {value_expr} - segment register */;"

    SET_MAP = {
        "al": f"SET_LO8(eax, {value_expr})",
        "ah": f"SET_HI8(eax, {value_expr})",
        "ax": f"SET_LO16(eax, {value_expr})",
        "bl": f"SET_LO8(ebx, {value_expr})",
        "bh": f"SET_HI8(ebx, {value_expr})",
        "bx": f"SET_LO16(ebx, {value_expr})",
        "cl": f"SET_LO8(ecx, {value_expr})",
        "ch": f"SET_HI8(ecx, {value_expr})",
        "cx": f"SET_LO16(ecx, {value_expr})",
        "dl": f"SET_LO8(edx, {value_expr})",
        "dh": f"SET_HI8(edx, {value_expr})",
        "dx": f"SET_LO16(edx, {value_expr})",
        "si": f"SET_LO16(esi, {value_expr})",
        "di": f"SET_LO16(edi, {value_expr})",
        "bp": f"SET_LO16(ebp, {value_expr})",
        "sp": f"SET_LO16(esp, {value_expr})",
    }
    if name in SET_MAP:
        return SET_MAP[name] + ";"
    return f"{name} = {value_expr};"


def _fmt_imm(val):
    """Format an immediate value as a C hex literal."""
    if val == 0:
        return "0"
    if val <= 9:
        return str(val)
    if val > 0x7FFFFFFF:
        return f"0x{val:08X}u"
    return f"0x{val:X}"


def _mem_accessor(size):
    """Return the MEM macro name for a given operand size."""
    return {1: "MEM8", 2: "MEM16", 4: "MEM32", 8: "MEM64"}.get(size, "MEM32")


def _smem_accessor(size):
    """Return the signed MEM macro for a given operand size."""
    return {1: "SMEM8", 2: "SMEM16", 4: "SMEM32"}.get(size, "SMEM32")


def _fmt_mem(op, disp_bias=0):
    """Format a memory operand as a C expression (the address computation)."""
    parts = []
    if op.mem_base:
        parts.append(_fmt_reg(op.mem_base))
    if op.mem_index:
        idx = _fmt_reg(op.mem_index)
        if op.mem_scale and op.mem_scale > 1:
            parts.append(f"{idx} * {op.mem_scale}")
        else:
            parts.append(idx)
    disp = (op.mem_disp + disp_bias) & 0xFFFFFFFF if disp_bias else op.mem_disp
    if disp:
        if disp > 0x80000000:
            # Actually negative (two's complement)
            signed_disp = disp - 0x100000000
            if parts:
                parts.append(f"- {_fmt_imm(-signed_disp)}")
            else:
                parts.append(_fmt_imm(disp))
        else:
            parts.append(_fmt_imm(disp))
    if not parts:
        return "0"
    return " + ".join(parts)


def _fmt_mem_read(op, disp_bias=0):
    """Format reading from a memory operand."""
    accessor = _mem_accessor(op.mem_size)
    if op.mem_segment == "fs":
        return f"FS{op.mem_size * 8}({_fmt_mem(op, disp_bias)})"
    addr = _fmt_mem(op, disp_bias)
    return f"{accessor}({addr})"


def _fmt_mem_write(op, value_expr):
    """Format writing to a memory operand."""
    accessor = _mem_accessor(op.mem_size)
    if op.mem_segment == "fs":
        addr = _fmt_mem(op)
        write = f"FS{op.mem_size * 8}({addr}) = {value_expr};"
        if addr == "0" and op.mem_size == 4:
            return write[:-1] + f" /* fs:[0] */; recomp_trace_fs0_write({value_expr});"
        return write
    addr = _fmt_mem(op)
    return f"{accessor}({addr}) = {value_expr};"


def _is_mmx_reg(op):
    """True for the MMX register file (mm0-mm7), which the translator
    declares as uint64_t locals."""
    return op.type == "reg" and bool(op.reg) and op.reg.startswith("mm")


def _base_mnemonic(m):
    """Strip a LOCK prefix. Capstone folds it into the mnemonic string, so
    "lock inc" never matched any dispatch entry and the instruction was
    dropped. Atomicity is not modelled; the underlying operation is."""
    return m[5:] if m.startswith("lock ") else m


def _fmt_operand_read(op, disp_bias=0):
    """Format reading any operand type."""
    if op.type == "reg":
        return _fmt_reg(op.reg)
    elif op.type == "imm":
        return _fmt_imm(op.imm)
    elif op.type == "mem":
        return _fmt_mem_read(op, disp_bias)
    return "/* unknown operand */"


def _fmt_operand_write(op, value_expr):
    """Format writing to any operand type. Returns a C statement."""
    if op.type == "reg":
        return _fmt_set_reg(op.reg, value_expr)
    elif op.type == "mem":
        return _fmt_mem_write(op, value_expr)
    return f"/* cannot write to {op.type} */;"


# ── Condition code mapping ───────────────────────────────────

# Maps jcc mnemonic → (cmp_macro, test_macro, description)
# cmp_macro takes (lhs, rhs), test_macro takes (lhs, rhs)
COND_MAP = {
    "je":   ("CMP_EQ",  "TEST_Z",  "equal / zero"),
    "jz":   ("CMP_EQ",  "TEST_Z",  "zero"),
    "jne":  ("CMP_NE",  "TEST_NZ", "not equal / not zero"),
    "jnz":  ("CMP_NE",  "TEST_NZ", "not zero"),
    "jb":   ("CMP_B",   None,      "below (unsigned <)"),
    "jnae": ("CMP_B",   None,      "below"),
    "jae":  ("CMP_AE",  None,      "above or equal (unsigned >=)"),
    "jnb":  ("CMP_AE",  None,      "above or equal"),
    "jbe":  ("CMP_BE",  None,      "below or equal (unsigned <=)"),
    "jna":  ("CMP_BE",  None,      "below or equal"),
    "ja":   ("CMP_A",   None,      "above (unsigned >)"),
    "jl":   ("CMP_L",   "TEST_S",  "less (signed <)"),
    "jge":  ("CMP_GE",  None,      "greater or equal (signed >=)"),
    "jle":  ("CMP_LE",  None,      "less or equal (signed <=)"),
    "jg":   ("CMP_G",   None,      "greater (signed >)"),
    "js":   (None,       "TEST_S",  "sign (negative)"),
    "jns":  (None,       None,      "not sign (positive)"),
    "jo":   (None,       None,      "overflow"),
    "jno":  (None,       None,      "not overflow"),
    "jp":   (None,       None,      "parity"),
    "jnp":  (None,       None,      "not parity"),
    "jecxz": (None,      None,      "ecx is zero"),
    "jcxz":  (None,      None,      "cx is zero"),
}

# Instructions that set arithmetic flags (primary set, fully handled)
FLAG_SETTERS = frozenset({
    "cmp", "test", "sub", "add", "and", "or", "xor",
    "inc", "dec", "neg", "shl", "shr", "sar", "imul", "adc", "sbb",
    "comiss", "comisd", "ucomiss", "ucomisd",  # SSE float compare
})

# Additional instructions that modify EFLAGS (tracked but handled as generic)
_EFLAGS_SETTERS = frozenset({
    "shld", "shrd", "rol", "ror", "rcl", "rcr",  # Shifts/rotates set CF
    "bsf", "bsr",       # Bit scan sets ZF
    "bt", "bts", "btr", "btc",  # Bit test sets CF
    "cmpxchg",           # Compare-and-exchange sets ZF
    "xadd",              # Exchange-and-add sets flags
})

# Instructions with undefined/unpredictable flags (clear tracking)
_FLAGS_UNDEFINED = frozenset({
    "mul", "div", "idiv",  # Flags partially undefined
    "rdtsc", "cpuid",      # Special instructions
})

# Instructions that do NOT modify EFLAGS (preserve flag tracking)
_EFLAGS_PRESERVE = frozenset({
    # General-purpose data movement / stack
    "mov", "lea", "push", "pop", "nop", "leave", "ret",
    "movzx", "movsx", "xchg", "bswap",
    "cdq", "cwde", "cbw", "cwd",
    "lahf",
    "not",  # NOT does not modify flags
    "call",
    "int3", "int", "wait",
    "cld", "std", "cli", "sti",
    "pushfd", "popfd", "pushal",
    "sgdt", "ljmp", "sfence",
    # SSE scalar float
    "movss", "movsd",
    "addss", "subss", "mulss", "divss",
    "minss", "maxss", "sqrtss", "rsqrtss", "rcpss",
    "addsd", "subsd", "mulsd", "divsd",
    "minsd", "maxsd", "sqrtsd",
    "cvtsi2ss", "cvtss2si", "cvttss2si",
    "cvtsi2sd", "cvtsd2si", "cvttsd2si",
    "cvtss2sd", "cvtsd2ss",
    "cmpss", "cmpsd",
    "cmpltss", "cmpeqss", "cmpleps", "cmpneqss",
    # SSE packed float
    "movaps", "movups", "movlps", "movhps", "movlhps", "movhlps",
    "addps", "subps", "mulps", "divps",
    "minps", "maxps", "sqrtps", "rsqrtps", "rcpps",
    "shufps", "unpcklps", "unpckhps",
    "andps", "orps", "xorps", "andnps",
    "cmpps", "cmpneqps",
    "movmskps",
    # SSE2 packed double
    "movapd", "movupd",
    "addpd", "subpd", "mulpd", "divpd",
    # SSE/MMX integer
    "movd", "movq", "movntq",
    "emms",
    "paddb", "paddw", "paddd", "paddq",
    "psubb", "psubw", "psubd",
    "pmullw", "pmulhw", "pmulhuw", "pmaddwd",
    "pand", "pandn", "por", "pxor",
    "pcmpeqb", "pcmpeqw", "pcmpeqd",
    "pcmpgtb", "pcmpgtw", "pcmpgtd",
    "psllw", "pslld", "psllq",
    "psrlw", "psrld", "psrlq",
    "psraw", "psrad",
    "pshufw", "pshufd", "pshufhw", "pshuflw",
    "punpcklbw", "punpcklwd", "punpckldq", "punpcklqdq",
    "punpckhbw", "punpckhwd", "punpckhdq", "punpckhqdq",
    "packsswb", "packssdw", "packuswb",
    "pmovmskb",
    # String operations (without rep prefix)
    "stosb", "stosw", "stosd",
    "movsb", "movsw", "movsd",
    "lodsb", "lodsw", "lodsd",
    # Prefetch hints
    "prefetchnta", "prefetcht0", "prefetcht1", "prefetcht2",
})


def _make_condition(jcc, flag_setter, flag_ops):
    """
    Generate a C condition expression for a jcc based on what set the flags.
    Returns (cond_expr, description) or None.
    """
    cond_info = COND_MAP.get(jcc)
    if not cond_info:
        return None
    cmp_macro, test_macro, desc = cond_info

    flag_ops = _normalize_cmp_operands(flag_ops)

    if len(flag_ops) >= 2:
        lhs = _fmt_operand_read(flag_ops[0])
        rhs = _fmt_operand_read(flag_ops[1])
    elif len(flag_ops) == 1:
        lhs = _fmt_operand_read(flag_ops[0])
        rhs = None
    else:
        lhs = None
        rhs = None

    # ── FPU compare-to-EFLAGS and sahf: no standard operands ──
    if flag_setter in ("fcompi", "fcomip", "fucomi", "fucompi",
                        "fucomip", "fcomi", "sahf"):
        fpu_cmp_map = {
            "ja": ">", "jnbe": ">",
            "jae": ">=", "jnb": ">=", "jnc": ">=",
            "jb": "<", "jnae": "<", "jc": "<",
            "jbe": "<=", "jna": "<=",
            "je": "==", "jz": "==",
            "jne": "!=", "jnz": "!=",
        }
        op = fpu_cmp_map.get(jcc)
        if op:
            return f"(_fpu_cmp {op} 0) /* {flag_setter} */", desc
        if jcc == "jp":
            return "0 /* fpu: unordered/NaN */", desc
        if jcc == "jnp":
            return "1 /* fpu: ordered */", desc
        return None

    # If no operands available for other flag-setters, can't generate condition
    if lhs is None:
        return None

    # Flag math runs at the smallest operand width (1/2/4 bytes). Without
    # this, 8-bit ops like `test bl,bl; js` are zero-extended to 32 bits and
    # the sign flag can never be set (MM3 sub_0034E420 spin).
    size = 4
    if flag_ops:
        size = min(_flag_operand_size(o) for o in flag_ops)

    # ── comiss/ucomiss: float comparison, sets CF/ZF/PF ──
    if flag_setter in ("comiss", "comisd", "ucomiss", "ucomisd"):
        def _sse_op(op):
            if op.type == "reg":
                return op.reg
            elif op.type == "mem":
                if op.mem_size == 8:
                    return f"MEMD({_fmt_mem(op)})"
                return f"MEMF({_fmt_mem(op)})"
            return _fmt_operand_read(op)
        a = _sse_op(flag_ops[0]) if len(flag_ops) >= 1 else "0.0f"
        b = _sse_op(flag_ops[1]) if len(flag_ops) >= 2 else "0.0f"
        # comiss uses unsigned condition codes (CF, ZF)
        if jcc in ("ja", "jnbe"):
            return f"({a} > {b})", desc
        if jcc in ("jae", "jnb", "jnc"):
            return f"({a} >= {b})", desc
        if jcc in ("jb", "jnae", "jc"):
            return f"({a} < {b})", desc
        if jcc in ("jbe", "jna"):
            return f"({a} <= {b})", desc
        if jcc in ("je", "jz"):
            return f"({a} == {b})", desc
        if jcc in ("jne", "jnz"):
            return f"({a} != {b})", desc
        if jcc == "jp":
            return f"0 /* {jcc}: unordered/NaN */", desc
        if jcc == "jnp":
            return f"1 /* {jcc}: ordered */", desc
        return None

    # ── cmp: flags from (a - b), operands unchanged ──
    if flag_setter == "cmp":
        if cmp_macro:
            if jcc in ("jl", "jge", "jle", "jg"):
                return f"{cmp_macro}({_signed_cast(lhs, size)}, {_signed_cast(rhs, size)})", desc
            return f"{cmp_macro}({lhs}, {rhs})", desc
        if jcc == "js":
            return f"({_signed_cast(f'({lhs} - {rhs})', size)} < 0)", desc
        if jcc == "jns":
            return f"({_signed_cast(f'({lhs} - {rhs})', size)} >= 0)", desc
        if jcc in ("jp", "jnp"):
            return f"1 /* {jcc} after cmp - parity */", desc
        return None

    # ── test: flags from (a & b), operands unchanged ──
    if flag_setter == "test":
        if test_macro == "TEST_S":
            return f"{test_macro}({_signed_cast(lhs, size)}, {_signed_cast(rhs, size)})", desc
        if test_macro:
            return f"{test_macro}({lhs}, {rhs})", desc
        if cmp_macro:
            return f"{cmp_macro}({_signed_cast(f'({lhs} & {rhs})', size)}, 0)", desc
        if jcc == "js":
            return f"({_signed_cast(f'({lhs} & {rhs})', size)} < 0)", desc
        if jcc == "jns":
            return f"({_signed_cast(f'({lhs} & {rhs})', size)} >= 0)", desc
        if jcc == "jo":
            return "0", desc  # OF=0 after test
        if jcc == "jno":
            return "1", desc
        if jcc in ("jp", "jnp"):
            return f"1 /* {jcc} after test - parity */", desc
        return None

    # ── test ah, imm on the x87 status word (fnstsw ax; test ah, imm; jcc) ──
    # AH holds C0=AH0, C1=AH1, C2=AH2, C3=AH6. _fpu_cmp encodes C3/C2
    # (less/equal/greater; unordered collapses to equal). lift_basic_block
    # only marks the setter "fpu_test" when the test directly follows
    # fnstsw ax, so the flags genuinely come from the FPU compare. Mask
    # semantics for fcomp ST0 vs operand:
    #   0x40 (C3):    je <-> cmp!=0, jne <-> cmp==0, jp <-> cmp==0, jnp <-> cmp!=0
    #   0x41 (C3|C0): je <-> cmp>0,  jne <-> cmp<=0, jp <-> cmp<=0, jnp <-> cmp>0
    #   0x44 (C3|C2): je <-> cmp!=0, jne <-> cmp==0, jp <-> cmp==0, jnp <-> cmp!=0
    # jp/jnp after 0x41/0x44 diverge from the original for unordered/NaN only
    # (both status bits set -> even parity -> PF=1), the same limitation the
    # fcomi path already has.
    if flag_setter == "fpu_test":
        mask = 0
        if len(flag_ops) >= 1 and flag_ops[0].type == "imm":
            mask = flag_ops[0].imm & 0xFF
        if mask == 0x41:
            ge, le = "_fpu_cmp > 0", "_fpu_cmp <= 0"
        elif mask == 0x01:
            ge, le = "_fpu_cmp >= 0", "_fpu_cmp < 0"
        elif mask == 0x05:
            # C0|C2: jp taken iff C0 == C2 (ST >= operand / unordered)
            ge, le = "_fpu_cmp >= 0", "_fpu_cmp < 0"
        else:
            ge, le = "_fpu_cmp != 0", "_fpu_cmp == 0"
        if jcc in ("je", "jz"):
            return ge, f"fpu test ah,{mask:#x}"
        if jcc in ("jne", "jnz"):
            return le, f"fpu test ah,{mask:#x}"
        if jcc == "jp":
            return (le if mask == 0x40 else ge), f"fpu test ah,{mask:#x} parity"
        if jcc == "jnp":
            return (ge if mask == 0x40 else le), f"fpu test ah,{mask:#x} parity"
        return None

    # ── sub: a = a - b, flags from (a_orig - b) ──
    if flag_setter == "sub":
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc == "js":
            return f"({_signed_cast(lhs, size)} < 0)", desc
        if jcc == "jns":
            return f"({_signed_cast(lhs, size)} >= 0)", desc
        # Ordered: reconstruct original a = result + b
        if cmp_macro and rhs:
            return f"{cmp_macro}((uint32_t){lhs} + (uint32_t){rhs}, (uint32_t){rhs})", desc
        if jcc in ("jb", "jnae"):
            return f"((uint32_t){lhs} + (uint32_t){rhs} < (uint32_t){rhs})", desc
        if jcc in ("jae", "jnb"):
            return f"((uint32_t){lhs} + (uint32_t){rhs} >= (uint32_t){rhs})", desc
        if jcc in ("jl", "jnge"):
            return f"({_signed_cast(lhs, size)} < 0)", desc
        if jcc in ("jge", "jnl"):
            return f"({_signed_cast(lhs, size)} >= 0)", desc
        if jcc in ("jle", "jng"):
            return f"({_signed_cast(lhs, size)} <= 0)", desc
        if jcc in ("jg", "jnle"):
            return f"({_signed_cast(lhs, size)} > 0)", desc
        return None

    # ── add: a = a + b, flags from result ──
    if flag_setter == "add":
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc == "js":
            return f"({_signed_cast(lhs, size)} < 0)", desc
        if jcc == "jns":
            return f"({_signed_cast(lhs, size)} >= 0)", desc
        if jcc in ("jb", "jnae", "jc"):
            return f"({lhs} < (uint32_t){rhs})", desc
        if jcc in ("jae", "jnb", "jnc"):
            return f"({lhs} >= (uint32_t){rhs})", desc
        if jcc in ("jl", "jnge"):
            return f"({_signed_cast(lhs, size)} < 0)", desc
        if jcc in ("jge", "jnl"):
            return f"({_signed_cast(lhs, size)} >= 0)", desc
        if jcc in ("jle", "jng"):
            return f"({_signed_cast(lhs, size)} <= 0)", desc
        if jcc in ("jg", "jnle"):
            return f"({_signed_cast(lhs, size)} > 0)", desc
        return None

    # ── adc/sbb: result-based (like add/sub but with carry) ──
    if flag_setter in ("adc", "sbb"):
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc == "js":
            return f"({_signed_cast(lhs, size)} < 0)", desc
        if jcc == "jns":
            return f"({_signed_cast(lhs, size)} >= 0)", desc
        return None

    # ── and/or/xor: result-based, CF=0, OF=0 ──
    if flag_setter in ("and", "or", "xor"):
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc in ("js", "jl"):
            return f"({_signed_cast(lhs, size)} < 0)", desc
        if jcc in ("jns", "jge"):
            return f"({_signed_cast(lhs, size)} >= 0)", desc
        if jcc == "jle":
            return f"({_signed_cast(lhs, size)} <= 0)", desc
        if jcc == "jg":
            return f"({_signed_cast(lhs, size)} > 0)", desc
        if jcc in ("jb", "jnae", "jbe", "jna"):
            return "0", desc  # CF=0 after and/or/xor
        if jcc in ("jae", "jnb", "ja", "jnbe"):
            return "1", desc
        return None

    # ── dec/inc: result-based, CF unchanged ──
    if flag_setter in ("dec", "inc"):
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc == "js":
            return f"({_signed_cast(lhs, size)} < 0)", desc
        if jcc == "jns":
            return f"({_signed_cast(lhs, size)} >= 0)", desc
        if jcc in ("jl", "jle", "jg", "jge"):
            cast = _signed_cast(lhs, size)
            op = {"jl": "<", "jle": "<=", "jg": ">", "jge": ">="}[jcc]
            return f"({cast} {op} 0)", desc
        return None

    # ── neg: flags from (0 - a_orig), result is -a ──
    if flag_setter == "neg":
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc in ("jb", "jnae", "jc"):
            # CF=1 unless original was 0
            return f"({lhs} != 0)", desc
        if jcc in ("jae", "jnb", "jnc"):
            return f"({lhs} == 0)", desc
        if jcc == "js":
            return f"({_signed_cast(lhs, size)} < 0)", desc
        if jcc == "jns":
            return f"({_signed_cast(lhs, size)} >= 0)", desc
        if jcc in ("jg", "jnle"):
            return f"({_signed_cast(lhs, size)} > 0)", desc
        if jcc in ("jge", "jnl"):
            return f"({_signed_cast(lhs, size)} >= 0)", desc
        if jcc in ("jl", "jnge"):
            return f"({_signed_cast(lhs, size)} < 0)", desc
        if jcc in ("jle", "jng"):
            return f"({_signed_cast(lhs, size)} <= 0)", desc
        return None

    # ── shift: result-based ──
    if flag_setter in ("shl", "shr", "sar"):
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc == "js":
            return f"({_signed_cast(lhs, size)} < 0)", desc
        if jcc == "jns":
            return f"({_signed_cast(lhs, size)} >= 0)", desc
        return None

    # ── shld/shrd: double-precision shift, result-based ──
    if flag_setter in ("shld", "shrd"):
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc == "js":
            return f"({_signed_cast(lhs, size)} < 0)", desc
        if jcc == "jns":
            return f"({_signed_cast(lhs, size)} >= 0)", desc
        return None

    # ── rol/ror/rcl/rcr: rotation, only CF/OF affected ──
    if flag_setter in ("rol", "ror", "rcl", "rcr"):
        # ZF/SF not modified by rotations - can't resolve most conditions
        return None

    # ── bsf/bsr: bit scan, ZF set if source is zero ──
    if flag_setter in ("bsf", "bsr"):
        if rhs is None:
            return None
        if jcc in ("je", "jz"):
            return f"({rhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({rhs} != 0)", desc
        return None

    # ── bt/bts/btr/btc: bit test, sets CF ──
    if flag_setter in ("bt", "bts", "btr", "btc"):
        if rhs is None:
            return None
        if jcc in ("jb", "jnae", "jc"):
            return f"(({lhs} >> ({rhs} & 31)) & 1)", desc
        if jcc in ("jae", "jnb", "jnc"):
            return f"!(({lhs} >> ({rhs} & 31)) & 1)", desc
        return None

    # ── cmpxchg: compares accumulator with dest, sets ZF on match ──
    if flag_setter == "cmpxchg":
        if jcc in ("je", "jz"):
            return "_cmpx_zf", desc
        if jcc in ("jne", "jnz"):
            return "!_cmpx_zf", desc
        if jcc in ("jb", "jc", "jnae"):
            return "_cf", desc
        if jcc in ("jae", "jnc", "jnb"):
            return "!_cf", desc
        return None

    # ── xadd: exchange and add, flags from addition ──
    if flag_setter == "xadd":
        if jcc in ("je", "jz"):
            return "_cmpx_zf", desc
        if jcc in ("jne", "jnz"):
            return "!_cmpx_zf", desc
        if jcc in ("jb", "jc", "jnae"):
            return "_cf", desc
        if jcc in ("jae", "jnc", "jnb"):
            return "!_cf", desc
        return None

    # ── repe cmpsb / repne scasb: string comparison ──
    if "cmps" in flag_setter or "scas" in flag_setter:
        if jcc in ("je", "jz"):
            return "_cmps_zf", desc
        if jcc in ("jne", "jnz"):
            return "!_cmps_zf", desc
        if jcc in ("jb", "jc", "jnae"):
            return "_cf", desc
        if jcc in ("jae", "jnc", "jnb"):
            return "!_cf", desc
        return None

    return None


def _make_setcc_value(setcc_mnemonic, flag_setter, flag_ops):
    """Generate the condition expression for a SETcc instruction."""
    cc = setcc_mnemonic[3:]
    jcc = "j" + cc
    result = _make_condition(jcc, flag_setter, flag_ops)
    if result:
        return result[0]
    return None


def _make_cmovcc_cond(cmov_mnemonic, flag_setter, flag_ops):
    """Generate the condition expression for a CMOVcc instruction."""
    cc = cmov_mnemonic[4:]
    jcc = "j" + cc
    result = _make_condition(jcc, flag_setter, flag_ops)
    if result:
        return result[0]
    return None


# ── Pattern matching for flag-setter + jcc ────────────────────

# x87 status bits visible in AH after fnstsw ax: C0=0x01, C1=0x02, C2=0x04,
# C3=0x40. These are the masks MM3 uses with the test-ah idiom; _fpu_cmp can
# only resolve C3/C2, so masks pulling in C0/C1 keep the old behaviour.
_FPU_STATUS_TEST_MASKS = (0x40, 0x41, 0x44, 0x05, 0x01)


def _is_fpu_status_test(insn):
    """True for `test ah, imm` where imm selects x87 status bits."""
    if insn.mnemonic != "test" or len(insn.operands) < 2:
        return False
    o0, o1 = insn.operands[0], insn.operands[1]
    return (o0.type == "reg" and o0.reg == "ah"
            and o1.type == "imm" and (o1.imm & 0xFF) in _FPU_STATUS_TEST_MASKS)


def _writes_eax(insn):
    """True if an instruction writes eax/ax/al/ah (clobbers AH's FPU status)."""
    m = insn.mnemonic
    if m in ("test", "cmp", "push", "fnstsw", "sahf"):
        return False  # read-only on eax
    if m == "lahf":
        return True   # loads AH
    return any(op.type == "reg" and op.reg in ("eax", "ax", "al", "ah")
               for op in insn.operands)


def _emit_cond_goto(cond_expr, jcc, desc, target, lifter):
    """Emit a conditional goto or call for a jump target."""
    if target is None:
        return f"if ({cond_expr}) {{ /* {jcc}: {desc} - indirect */ }}"
    if lifter and lifter._is_external_target(target):
        name = lifter._call_target_name(target)
        # Tail call into another function/split piece: bridge our frame so
        # the target's "ebp = g_seh_ebp" prolog sees the same ebp the
        # original jcc carried (sub_001E839C -> sub_001E83BB esp wrap).
        return (f"if ({cond_expr}) {{ g_seh_ebp = ebp; {name}(); return; }}"
                f" /* {jcc}: {desc} */")
    return f"if ({cond_expr}) goto loc_{target:08X}; /* {jcc}: {desc} */"


def try_match_cmp_jcc(insns, idx, lifter=None):
    """
    Try to match a cmp/test + jcc pattern starting at insns[idx].
    Returns (c_statement, num_consumed) or None.
    """
    if idx + 1 >= len(insns):
        return None

    first = insns[idx]
    second = insns[idx + 1]

    if first.mnemonic not in ("cmp", "test") or not second.is_cond_jump:
        return None

    if len(first.operands) < 2:
        return None

    # test ah, imm on the x87 status word must not take the generic test+jcc
    # path (its operand AH is stale in C); lift_basic_block marks it fpu_test
    # and the jcc resolves against _fpu_cmp instead.
    if first.mnemonic == "test" and _is_fpu_status_test(first):
        return None

    result = _make_condition(second.mnemonic, first.mnemonic, first.operands)
    if not result:
        return None

    cond_expr, desc = result
    target = second.jump_target
    stmt = _emit_cond_goto(cond_expr, second.mnemonic, desc, target, lifter)
    if lifter is not None and lifter.func_start == 0x00086097:
        stmt = ("recomp_trace_sched_callback(0, MEM32(0x00362014), "
                "MEM8(ebp - 29), g_esp); " + stmt)
    return (stmt, 2)


# ── Single instruction lifting ───────────────────────────────

# MSVC's __SEH_prolog establishes the caller's frame pointer, so the lifter has
# to know which function it is. The address is per-title, and hardcoding it
# meant every other game silently got no frame set up after the call: ebp kept
# whatever stale value it had, and the first ebp-relative local access read
# through it. In Halo that surfaced as a read of 0xFFFFFFFC (ebp=0, [ebp-4]).
#
# Both helpers are compiler boilerplate with distinctive bodies, so detect them
# rather than asking every project to look them up by hand.
#
#   __SEH_prolog   mov eax, fs:[0]        64 A1 00 00 00 00
#                  lea ebp, [esp+0x10]    8D 6C 24 10
#   __SEH_epilog   mov fs:[0], ecx        64 89 0D 00 00 00 00
#                  leave; push ecx; ret   C9 51 C3
_SEH_PROLOG_MARKERS = (b"\x64\xa1\x00\x00\x00\x00", b"\x8d\x6c\x24\x10")
_SEH_EPILOG_MARKERS = (b"\x64\x89\x0d\x00\x00\x00\x00", b"\xc9\x51\xc3")

# Both are tiny; a large match is something else that happens to touch fs:[0].
_SEH_PROLOG_MAX_SIZE = 128
_SEH_EPILOG_MAX_SIZE = 64


def detect_seh_helpers(func_db, xbe_data, verbose=False):
    """Locate __SEH_prolog / __SEH_epilog in the target binary.

    Returns (prolog_addr, epilog_addr); either may be None if not found, which
    is normal for a title whose CRT does not use them.
    """
    from .config import va_to_file_offset

    prolog = epilog = None

    def _size_of(info):
        # "end" is a hex string in functions.json but BatchTranslator rewrites
        # it to an int in place, so accept either.
        try:
            size = int(info.get("size") or 0)
        except (TypeError, ValueError):
            size = 0
        if size:
            return size
        end = info.get("end")
        if isinstance(end, str):
            try:
                end = int(end, 16)
            except ValueError:
                return 0
        return (end - addr) if isinstance(end, int) else 0

    for addr in sorted(func_db):
        info = func_db[addr]
        size = _size_of(info)
        if size <= 0 or size > _SEH_PROLOG_MAX_SIZE:
            continue

        offset = va_to_file_offset(addr)
        if offset is None or xbe_data is None or offset + size > len(xbe_data):
            continue
        body = xbe_data[offset:offset + size]

        if (prolog is None and size <= _SEH_PROLOG_MAX_SIZE
                and all(m in body for m in _SEH_PROLOG_MARKERS)):
            prolog = addr
        elif (epilog is None and size <= _SEH_EPILOG_MAX_SIZE
                and all(m in body for m in _SEH_EPILOG_MARKERS)):
            epilog = addr

        if prolog is not None and epilog is not None:
            break

    if verbose:
        import sys
        fmt = lambda a: f"0x{a:08X}" if a else "not found"
        print(f"  SEH helpers: __SEH_prolog {fmt(prolog)}, "
              f"__SEH_epilog {fmt(epilog)}", file=sys.stderr)

    return prolog, epilog


# Direct (static) calls to these functions are routed through the manual
# dispatch (RECOMP_ICALL_SAFE) instead of calling the generated function
# directly, so recomp_manual.c can override mis-lifted functions. A function
# needs this when its indirect jump (switch table) targets were not lifted
# into the CFG (they are reached only at runtime via the table), leaving an
# unresolvable RECOMP_ITAIL that pops the wrong amount and drifts the
# simulated stack. Observed: sub_00093860 (a CRT memcpy whose byte-copy
# tails at 0x939BC+ are not in the CFG).
DISPATCH_DIRECT = {0x00093860, 0x000858F3}

# sub_00095B8C is the guest setjmp half of the D3DX unwind pair.  Calls to it
# must be wrapped in host setjmp/longjmp so sub_00095EB4 can unwind the native
# C call frames back to the saved continuation instead of returning into the
# wrong generated caller.
SETJMP_DIRECT = {0x00095B8C}

# Functions whose final indirect jmp is a guest longjmp (context restore),
# not a normal computed tail call.
LONGJMP_FUNCS = {0x00095EB4}

# Direct calls to these functions must push the real guest return
# address instead of the dummy zero because the callee reads [esp].
# Observed: sub_00095B8C stores [esp] into a D3DX jump context (+0x14)
# and sub_00095EB4 later tail-jumps through that slot.
RETURN_ADDRESS_READERS = {0x00095B8C}

class Lifter:
    """Translates x86 instructions to C statements."""

    def __init__(self, func_db=None, label_db=None, abi_db=None, xbe_data=None,
                 seh_prolog=None, seh_epilog=None):
        """
        func_db: dict of func_addr → func_info (for naming call targets)
        label_db: dict of addr → name (for kernel imports, etc.)
        abi_db: dict of addr → ABI info (for calling conventions)
        xbe_data: raw XBE file bytes (for reading jump tables)
        seh_prolog/seh_epilog: override the detected __SEH_prolog/__SEH_epilog
        """
        self.func_db = func_db or {}
        self.label_db = label_db or {}
        self.abi_db = abi_db or {}
        self.xbe_data = xbe_data
        self._fp_top = 0  # FPU stack top index
        self.func_start = 0  # Set per-function by translator
        self.func_end = 0
        # {callee_addr: cleanup_bytes or None} for direct-call ret-N comments
        # (None = cleanup unknown, so the icall-esp fixup stops at the call).
        self._callee_cleanup_cache = {}
        self._disasm = None  # lazy Disassembler for _callee_cleanup()
        # Every direct call target we emit a name for, as {addr: name}. The
        # batch translator diffs this against the functions it actually defined
        # so it can stub out the remainder (see translate_batch_split).
        self.referenced_calls = {}
        self.uses_ebp = False

        # Detect if either is missing, so overriding one does not silently
        # leave the other unset -- that is the bug this whole path fixes.
        if (seh_prolog is None or seh_epilog is None) and self.func_db:
            found_prolog, found_epilog = detect_seh_helpers(self.func_db, xbe_data)
            seh_prolog = seh_prolog if seh_prolog is not None else found_prolog
            seh_epilog = seh_epilog if seh_epilog is not None else found_epilog
        self.SEH_PROLOG = seh_prolog
        self.SEH_EPILOG = seh_epilog

    def _callee_cleanup(self, addr):
        """Return the arg bytes a direct callee pops via `ret N`, or None.

        Reads the callee's first few instructions from the XBE and returns
        N when every `ret` seen agrees (stdcall/thiscall cleanup). The
        icall-esp fixup uses this to skip an interleaved direct call's own
        arg pushes when scanning back for an icall's args; None means the
        fixup must stop at the call instead of guessing.
        """
        if addr in self._callee_cleanup_cache:
            return self._callee_cleanup_cache[addr]
        cleanup = None
        info = (self.func_db or {}).get(addr)
        if info is not None and self.xbe_data is not None:
            end = info.get("end") or (addr + info.get("size", 0))
            size = min(end - addr, 64)
            offset = va_to_file_offset(addr)
            if size > 0 and offset is not None:
                raw = self.xbe_data[offset:offset + size]
                if self._disasm is None:
                    self._disasm = Disassembler()
                insns = self._disasm.disassemble_function(raw, addr, addr + size)
                rets = set()
                for insn in insns[:16]:
                    if insn.mnemonic == "ret":
                        rets.add(insn.operands[0].imm
                                 if insn.operands and insn.operands[0].type == "imm"
                                 else 0)
                if len(rets) == 1:
                    cleanup = rets.pop()
        self._callee_cleanup_cache[addr] = cleanup
        return cleanup

    def _call_target_name(self, addr):
        """Get the name for a call target address.

        func_db wins over label_db. The function definition is emitted from
        func_db, so consulting labels first meant a renamed function was
        *defined* as cseries__sub_0008DB80 but *called* as sub_0008DB80 -- the
        disassembler's generic auto-label -- and the link failed on every
        function any naming pass had touched. Labels still cover call targets
        that are not known function starts.
        """
        if addr in self.func_db:
            name = self.func_db[addr].get("name", f"sub_{addr:08X}")
        elif addr in self.label_db:
            name = self.label_db[addr]
        else:
            name = f"sub_{addr:08X}"
        self.referenced_calls[addr] = name
        return name

    def lift_instruction(self, insn):
        """
        Translate a single x86 instruction to one or more C statements.
        Returns a list of C statement strings.
        """
        m = insn.mnemonic
        ops = insn.operands
        nops = len(ops)

        # LOCK is folded into the mnemonic by the disassembler, so the guard
        # this replaced (insn.prefix, always None) never fired and every
        # locked instruction fell through to the TODO comment. Strip it and
        # lift the underlying operation; atomicity is not modelled.
        m = _base_mnemonic(m)

        # ── NOP ──
        if m == "nop" or (m == "lea" and nops == 2 and
                          ops[0].type == "reg" and ops[1].type == "mem" and
                          ops[1].mem_base == ops[0].reg and
                          not ops[1].mem_index and ops[1].mem_disp == 0):
            return [f"/* nop */"]

        # ── Data movement ──
        if m == "mov":
            return self._lift_mov(insn, ops)
        if m == "movzx":
            return self._lift_movzx(insn, ops)
        if m == "movsx":
            return self._lift_movsx(insn, ops)
        if m == "lea":
            return self._lift_lea(insn, ops)
        if m == "xchg":
            return self._lift_xchg(insn, ops)
        if m == "cmpxchg":
            return self._lift_cmpxchg(insn, ops)
        if m == "xadd":
            return self._lift_xadd(insn, ops)

        # ── Stack ──
        if m == "push":
            return self._lift_push(insn, ops)
        if m == "pop":
            return self._lift_pop(insn, ops)

        # ── Arithmetic ──
        if m in ("add", "sub", "and", "or", "xor"):
            return self._lift_alu_binop(insn, ops, m)
        if m in ("inc", "dec"):
            return self._lift_inc_dec(insn, ops, m)
        if m == "neg":
            return self._lift_neg(insn, ops)
        if m == "not":
            return self._lift_not(insn, ops)
        if m == "imul":
            return self._lift_imul(insn, ops)
        if m in ("mul", "div", "idiv"):
            return self._lift_muldiv(insn, ops, m)
        if m == "sbb":
            return self._lift_sbb(insn, ops)
        if m == "adc":
            return self._lift_adc(insn, ops)
        if m in ("shl", "sal"):
            return self._lift_shift(insn, ops, "<<")
        if m == "shr":
            return self._lift_shift(insn, ops, ">>")
        if m == "sar":
            return self._lift_sar(insn, ops)
        if m in ("rol", "ror"):
            return self._lift_rotate(insn, ops, m)
        if m in ("rcl", "rcr"):
            return self._lift_rotate_carry(insn, ops, m)
        if m in ("bsf", "bsr"):
            return self._lift_bsf_bsr(insn, ops, m)

        # ── Comparison / test (standalone, not part of cmp+jcc pattern) ──
        if m == "cmp":
            return self._lift_cmp(insn, ops)
        if m == "test":
            return self._lift_test(insn, ops)

        # ── Control flow ──
        if m == "call":
            return self._lift_call(insn, ops)
        if m in ("ret", "retn", "retf"):
            return self._lift_ret(insn, ops)
        if m == "jmp":
            return self._lift_jmp(insn, ops)
        if insn.is_cond_jump:
            return self._lift_jcc(insn)

        # ── String operations ──
        if m.startswith("rep ") or m.startswith("repe ") or m.startswith("repne "):
            return self._lift_rep_string(insn, m)
        # movsd and cmpsd name two unrelated instructions: the string move
        # and compare, and the SSE2 scalar-double forms. Only the SSE forms
        # carry an xmm operand, so that is the discriminator. Without this
        # test "movsd xmm0, [eax]" lifted as a string move - it clobbered
        # esi, edi and guest memory, which is worse than not lifting it.
        if m in ("movsb", "movsd", "movsw", "stosb", "stosd", "stosw",
                 "lodsb", "lodsd", "lodsw",
                 "cmpsb", "cmpsw", "cmpsd", "scasb", "scasw", "scasd")                 and not any(o.type == "reg" and o.reg
                            and o.reg.startswith("xmm") for o in ops):
            return self._lift_string_op(insn, m)
        if m == "wait":
            return ["/* wait - FPU sync */"]

        # ── Misc ──
        if m == "cdq":
            return ["edx = ((int32_t)eax < 0) ? 0xFFFFFFFF : 0; /* cdq */"]
        if m == "cwde":
            return ["eax = SX16(eax); /* cwde */"]
        if m == "cbw":
            return ["SET_LO16(eax, SX8(eax)); /* cbw */"]
        if m == "bswap" and nops >= 1 and ops[0].type == "reg":
            r = _fmt_reg(ops[0].reg)
            return [f"{r} = BSWAP32({r}); /* bswap */"]
        if m == "int3":
            return ["__debugbreak(); /* int3 */"]
        if m == "int" and insn.op_str == "0x2d":
            return ["/* int 0x2d — Xbox kernel service dispatch */",
                    "eax = xbox_int_0x2d(eax, ecx, edx);"]
        if m in ("leave",):
            return ["esp = ebp;", "POP32(esp, ebp); /* leave */",
                    "g_seh_ebp = ebp; /* restore frame for callees */"]
        if m in ("cld", "std"):
            return [f"/* {m} - direction flag */"]
        if m == "lahf":
            return ["/* lahf - load AH from flags (used in FPU compare idiom) */"]
        if m == "sahf":
            return ["/* sahf - store AH to flags */"]
        if m == "shld":
            return self._lift_shld(insn, ops)
        if m == "shrd":
            return self._lift_shrd(insn, ops)
        if m == "bt":
            if len(ops) >= 2:
                return [f"/* bt {_fmt_operand_read(ops[0])}, {_fmt_operand_read(ops[1])} - bit test */"]
            return [f"/* bt {insn.op_str} */"]
        if m == "emms":
            return ["/* emms - empty MMX state */"]
        if m in ("sete", "setne", "setb", "setae", "setbe", "seta",
                 "setl", "setge", "setle", "setg", "sets", "setns"):
            return self._lift_setcc(insn, ops, m)
        if m in ("cmove", "cmovne", "cmovb", "cmovae", "cmovbe", "cmova",
                 "cmovl", "cmovge", "cmovle", "cmovg", "cmovs", "cmovns"):
            return self._lift_cmovcc(insn, ops, m)

        # ── SSE (scalar float) ──
        if m in ("movss", "movsd", "movaps", "movups", "movlps", "movhps",
                 "addss", "subss", "mulss", "divss", "sqrtss",
                 "addsd", "subsd", "mulsd", "divsd", "sqrtsd",
                 "minss", "maxss", "minsd", "maxsd",
                 "comiss", "comisd", "ucomiss", "ucomisd",
                 "cvtsi2ss", "cvtss2si", "cvttss2si",
                 "cvtsi2sd", "cvtsd2si", "cvttsd2si",
                 "cvtss2sd", "cvtsd2ss",
                 "xorps", "xorpd", "andps", "andpd", "orps", "orpd",
                 "andnps", "andnpd",
                 "movd", "movq", "movntq", "movntps",
                 "movapd", "movupd",
                 "shufps", "unpcklps", "unpckhps", "movlhps", "movhlps",
                 "addps", "subps", "mulps", "divps",
                 "addpd", "subpd", "mulpd", "divpd",
                 "minps", "maxps", "minpd", "maxpd", "rsqrtss", "rcpss",
                 "sqrtps", "rsqrtps", "rcpps",
                 "cmpneqps", "cmpeqps", "cmpltps", "cmpleps",
                 "cmpnltps", "cmpnleps",
                 "movmskps", "movmskpd",
                 "pand", "pandn", "por", "pxor", "pcmpgtd"):
            return self._lift_sse(insn, m, ops)

        # ── FPU ──
        if m.startswith("f"):
            return self._lift_fpu(insn, m, ops)

        # ── Privileged / special ──
        if m == "rdtsc":
            return ["/* rdtsc → edx:eax */",
                    "edx = (uint32_t)(__rdtsc() >> 32);",
                    "eax = (uint32_t)__rdtsc();"]
        if m == "cpuid":
            return ["/* cpuid */",
                    "{ int _cpu_info[4];",
                    "  __cpuidex(_cpu_info, (int)eax, (int)ecx);",
                    "  eax = _cpu_info[0]; ebx = _cpu_info[1];",
                    "  ecx = _cpu_info[2]; edx = _cpu_info[3]; }"]
        if m == "hlt":
            return ["/* hlt — yield CPU */",
                    "SwitchToThread();"]
        if m == "cli":
            return ["/* cli — no-op in user mode */"]
        if m == "sti":
            return ["/* sti — no-op in user mode */"]
        if m in ("in", "insb", "insd", "insw"):
            return ["/* in — zero in user mode */",
                    "eax = 0;"]
        if m in ("out", "outsb", "outsd", "outsw"):
            return ["/* out — no-op in user mode */"]

        # ── Unhandled ──
        return [f"/* TODO: {m} {insn.op_str} */"]

    # ── MOV family ──

    def _lift_mov(self, insn, ops):
        if nops := len(ops) < 2:
            return [f"/* mov: bad operands */"]
        src = _fmt_operand_read(ops[1])
        lines = [_fmt_operand_write(ops[0], src)]
        # Diagnostic write hooks for the D3D8LTCG internals (0x0034D8EE,
        # 0x00345740, and the 0x00340000-0x00358000 range) were removed.
        # They instrumented Microsoft's statically linked D3D8 library, which
        # is now served at its API boundary by the host D3D8 layer, so its
        # internals no longer execute. recomp_trace_ramht_write never had a
        # runtime implementation at all, so emitting it broke the link; the
        # pair that did was measured collapsing the run frontier from
        # IC 679,590 to IC ~12,140.
        if (ops[0].type == "mem" and ops[0].mem_base is None and
                not ops[0].mem_index and ops[0].mem_disp == 0x003C5CDC):
            lines.append(
                f"recomp_trace_gate_write(0x{insn.address:08X}, (uint32_t)({src}));")
        if (ops[0].type == "mem" and self.func_start in
                (0x00344410, 0x003444C0, 0x00344640) and
                ops[0].mem_base == "esi" and not ops[0].mem_index and
                ops[0].mem_disp in (0x2C, 0x30, 0x17C0)):
            lines.append(
                f"recomp_trace_ring_write(0x{insn.address:08X}, "
                f"{_fmt_mem(ops[0])});")
        if (ops[0].type == "mem" and self.func_start == 0x00344640 and
                ops[0].mem_base == "esi" and not ops[0].mem_index and
                ops[0].mem_disp == 0x1970):
            lines.append(f"recomp_trace_display_arm(0x{insn.address:08X}, {_fmt_mem(ops[0])});")
        if (ops[0].type == "mem" and self.func_start == 0x00344410 and
                ops[0].mem_base == "eax" and not ops[0].mem_index and
                ops[0].mem_disp in (0, 4, 8, 0xC, 0x10, 0x14)):
            lines.append(
                f"recomp_trace_ring_record_write(0x{insn.address:08X}, "
                f"{_fmt_mem(ops[0])});")
        if (ops[0].type == "mem" and ops[0].mem_base == "edi" and
                not ops[0].mem_index and ops[0].mem_disp in (0x72, 0x73)):
            lines.append(
                f"recomp_trace_callback_flag_write(0x{insn.address:08X}, "
                f"{_fmt_mem(ops[0])}, (uint32_t)({src}));")
        # "mov ebp, esp" establishes this function's frame pointer. Mirror it
        # into g_seh_ebp so a leaf callee that inherits the caller frame via
        # "ebp = g_seh_ebp" sees the current frame instead of a stale frame
        # from the last __SEH_prolog. Without this, such leaves read args
        # through a stale pointer (observed: sub_0007829E reading the arena
        # from an old frame and AVing on the copy destination).
        if (ops[0].type == "reg" and ops[0].reg == "ebp" and
                ops[1].type == "reg" and ops[1].reg == "esp"):
            lines.append("g_seh_ebp = ebp; /* bridge frame to callees */")
        return lines

    def _lift_movzx(self, insn, ops):
        if len(ops) < 2:
            return [f"/* movzx: bad operands */"]
        src = _fmt_operand_read(ops[1])
        if ops[1].type == "mem":
            if ops[1].mem_size == 1:
                src = f"ZX8({src})"
            elif ops[1].mem_size == 2:
                src = f"ZX16({src})"
        elif ops[1].type == "reg":
            r = ops[1].reg
            if r in ("al", "bl", "cl", "dl", "ah", "bh", "ch", "dh"):
                src = f"ZX8({src})"
            elif r in ("ax", "bx", "cx", "dx", "si", "di", "bp", "sp"):
                src = f"ZX16({src})"
        return [_fmt_operand_write(ops[0], src)]

    def _lift_movsx(self, insn, ops):
        if len(ops) < 2:
            return [f"/* movsx: bad operands */"]
        src = _fmt_operand_read(ops[1])
        if ops[1].type == "mem":
            accessor = _smem_accessor(ops[1].mem_size)
            addr = _fmt_mem(ops[1])
            src = f"(uint32_t)(int32_t){accessor}({addr})"
        elif ops[1].type == "reg":
            r = ops[1].reg
            if r in ("al", "bl", "cl", "dl", "ah", "bh", "ch", "dh"):
                src = f"SX8({src})"
            elif r in ("ax", "bx", "cx", "dx", "si", "di"):
                src = f"SX16({src})"
        return [_fmt_operand_write(ops[0], src)]

    def _lift_lea(self, insn, ops):
        if len(ops) < 2 or ops[1].type != "mem":
            return [f"/* lea: unexpected operands */"]
        addr_expr = _fmt_mem(ops[1])
        return [_fmt_operand_write(ops[0], addr_expr)]

    def _lift_xchg(self, insn, ops):
        if len(ops) < 2:
            return [f"/* xchg: bad operands */"]
        a = _fmt_operand_read(ops[0])
        b = _fmt_operand_read(ops[1])
        return [
            f"{{ uint32_t _tmp = {a};",
            _fmt_operand_write(ops[0], b),
            _fmt_operand_write(ops[1], "_tmp") + " }",
        ]

    # ── Atomics (lock-stripped; sequential semantics) ──

    @staticmethod
    def _op_width(op):
        """Operand width in bytes, for picking the accumulator sub-register."""
        if op.type == "mem":
            return getattr(op, "mem_size", 4) or 4
        if op.type == "reg":
            r = op.reg
            if r in ("al", "bl", "cl", "dl", "ah", "bh", "ch", "dh"):
                return 1
            if r in ("ax", "bx", "cx", "dx", "si", "di", "bp", "sp"):
                return 2
        return 4

    _ACC_READ = {1: "LO8(eax)", 2: "LO16(eax)", 4: "eax"}
    _ACC_WRITE = {1: "SET_LO8(eax, {0});", 2: "SET_LO16(eax, {0});",
                  4: "eax = {0};"}
    _WIDTH_MASK = {1: "0xFFu", 2: "0xFFFFu", 4: "0xFFFFFFFFu"}

    def _lift_cmpxchg(self, insn, ops):
        """CMPXCHG dst, src: compare the accumulator with dst; on equal store
        src into dst, otherwise load dst into the accumulator. ZF says which
        branch was taken, so it is published explicitly - reading it back off
        the operands afterwards cannot tell the two cases apart, because the
        failure path makes the accumulator equal to dst."""
        if len(ops) < 2:
            return ["/* cmpxchg: bad operands */"]
        w = self._op_width(ops[0])
        acc = self._ACC_READ.get(w, "eax")
        return [
            "{ uint32_t _t = " + _fmt_operand_read(ops[0]) + ", _a = " + acc + ";",
            "  _cf = (_a < _t); _cmpx_zf = (_a == _t);",
            "  if (_cmpx_zf) { " + _fmt_operand_write(ops[0], _fmt_operand_read(ops[1])) + " }",
            "  else { " + self._ACC_WRITE.get(w, "eax = {0};").format("_t") + " } } /* cmpxchg */",
        ]

    def _lift_xadd(self, insn, ops):
        """XADD dst, src: src receives the old dst, dst receives the sum.
        Flags come from the addition, the same way ADD sets them."""
        if len(ops) < 2:
            return ["/* xadd: bad operands */"]
        mask = self._WIDTH_MASK.get(self._op_width(ops[0]), "0xFFFFFFFFu")
        return [
            "{ uint32_t _o = " + _fmt_operand_read(ops[0]) + ", _s = " + _fmt_operand_read(ops[1]) + ";",
            "  uint32_t _r = (_o + _s) & " + mask + ";",
            "  " + _fmt_operand_write(ops[1], "_o"),
            "  " + _fmt_operand_write(ops[0], "_r"),
            "  _cf = (_r < _s); _cmpx_zf = (_r == 0); } /* xadd */",
        ]

    # ── Stack ──

    def _lift_push(self, insn, ops):
        if len(ops) < 1:
            return ["/* push: no operand */"]
        val = _fmt_operand_read(ops[0])
        return [f"PUSH32(esp, {val});"]

    def _lift_pop(self, insn, ops):
        if len(ops) < 1:
            return ["/* pop: no operand */"]
        if ops[0].type == "reg":
            r = ops[0].reg
            # Segment register pop → discard from stack
            if r in ("fs", "gs", "cs", "ds", "es", "ss"):
                return [f"{{ uint32_t _tmp; POP32(esp, _tmp); }} /* pop {r} - segment register */"]
            if r == "ebp":
                # Restore the caller frame and mirror it back into g_seh_ebp
                # (paired with the "mov ebp, esp" bridge above).
                return [f"POP32(esp, ebp);", "g_seh_ebp = ebp; /* restore frame for callees */"]
            return [f"POP32(esp, {r});"]
        else:
            return [f"{{ uint32_t _tmp; POP32(esp, _tmp); {_fmt_operand_write(ops[0], '_tmp')} }}"]

    # ── ALU binary operations ──

    def _lift_alu_binop(self, insn, ops, m):
        if len(ops) < 2:
            return [f"/* {m}: bad operands */"]
        c_op = {"add": "+", "sub": "-", "and": "&", "or": "|", "xor": "^"}[m]
        dst = _fmt_operand_read(ops[0])
        src = _fmt_operand_read(ops[1])
        # XOR reg, reg → zero (clears CF like any xor)
        if m == "xor" and ops[0].type == "reg" and ops[1].type == "reg" and ops[0].reg == ops[1].reg:
            return [_fmt_operand_write(ops[0], "0") + " /* xor self */", "_cf = 0; /* xor clears CF */"]
        expr = f"{dst} {c_op} {src}"
        # Store the carry flag for sbb/adc consumers. add/sub set it; and/or/xor clear it.
        if m == "sub":
            # CF = borrow = (dst < src) unsigned; must read dst before the write
            return [
                f"_cf = ((uint32_t)({dst}) < (uint32_t)({src})); /* sub: CF = borrow */",
                _fmt_operand_write(ops[0], expr)
            ]
        if m == "add":
            # CF = carry out = (sum < src) unsigned; dst now holds the sum
            return [
                _fmt_operand_write(ops[0], expr),
                f"_cf = ((uint32_t)({dst}) < (uint32_t)({src})); /* add: CF = carry out */"
            ]
        if m in ("and", "or", "xor"):
            return [_fmt_operand_write(ops[0], expr), "_cf = 0; /* and/or/xor clear CF */"]
        return [_fmt_operand_write(ops[0], expr)]

    def _lift_inc_dec(self, insn, ops, m):
        if len(ops) < 1:
            return [f"/* {m}: no operand */"]
        val = _fmt_operand_read(ops[0])
        delta = "1"
        op_char = "+" if m == "inc" else "-"
        # For sub-registers (al, cl, etc.), use the SET macro instead of ++
        if ops[0].type == "reg" and ops[0].reg in (
                "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp"):
            return [f"{val}{'++' if m == 'inc' else '--'};"]
        else:
            return [_fmt_operand_write(ops[0], f"{val} {op_char} {delta}")]

    def _lift_neg(self, insn, ops):
        if len(ops) < 1:
            return ["/* neg: no operand */"]
        val = _fmt_operand_read(ops[0])
        # x86 neg sets CF=1 iff the operand was non-zero. The common
        # "neg; sbb reg,reg; and" idiom (0 or value depending on NULL check)
        # depends on this carry flag being stored.
        return [
            _fmt_operand_write(ops[0], f"(uint32_t)(-(int32_t){val})"),
            f"_cf = ({val} != 0); /* neg: CF = (operand != 0) */"
        ]

    def _lift_not(self, insn, ops):
        if len(ops) < 1:
            return ["/* not: no operand */"]
        val = _fmt_operand_read(ops[0])
        return [_fmt_operand_write(ops[0], f"~{val}")]

    def _lift_sbb(self, insn, ops):
        """SBB: subtract with borrow. Common idiom: sbb reg, reg → -CF (0 or -1)."""
        if len(ops) < 2:
            return ["/* sbb: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        src = _fmt_operand_read(ops[1])
        # sbb reg, reg is a common idiom: result is 0 or 0xFFFFFFFF depending on CF
        if ops[0].type == "reg" and ops[1].type == "reg" and ops[0].reg == ops[1].reg:
            return [_fmt_operand_write(ops[0], "_cf ? 0xFFFFFFFF : 0") + " /* sbb self (CF extend) */"]
        return [_fmt_operand_write(ops[0], f"{dst} - {src} - _cf") + " /* sbb */"]

    def _lift_adc(self, insn, ops):
        """ADC: add with carry."""
        if len(ops) < 2:
            return ["/* adc: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        src = _fmt_operand_read(ops[1])
        return [_fmt_operand_write(ops[0], f"{dst} + {src} + _cf") + " /* adc */"]

    def _lift_shld(self, insn, ops):
        """SHLD: double-precision shift left."""
        if len(ops) < 3:
            return [f"/* shld: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        src = _fmt_operand_read(ops[1])
        cnt = _fmt_operand_read(ops[2])
        return [_fmt_operand_write(ops[0],
            f"({dst} << {cnt}) | ({src} >> (32 - {cnt}))") + " /* shld */"]

    def _lift_shrd(self, insn, ops):
        """SHRD: double-precision shift right."""
        if len(ops) < 3:
            return [f"/* shrd: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        src = _fmt_operand_read(ops[1])
        cnt = _fmt_operand_read(ops[2])
        return [_fmt_operand_write(ops[0],
            f"({dst} >> {cnt}) | ({src} << (32 - {cnt}))") + " /* shrd */"]

    def _lift_imul(self, insn, ops):
        nops = len(ops)
        if nops == 1:
            # One operand: edx:eax = eax * ops[0]
            src = _fmt_operand_read(ops[0])
            return [
                f"{{ int64_t _r = (int64_t)(int32_t)eax * (int64_t)(int32_t){src};",
                f"  eax = (uint32_t)_r; edx = (uint32_t)(_r >> 32); }}"
            ]
        elif nops == 2:
            # Two operand: dst = dst * src
            dst = _fmt_operand_read(ops[0])
            src = _fmt_operand_read(ops[1])
            return [_fmt_operand_write(ops[0], f"(uint32_t)((int32_t){dst} * (int32_t){src})")]
        elif nops == 3:
            # Three operand: dst = src1 * imm
            src = _fmt_operand_read(ops[1])
            imm = _fmt_operand_read(ops[2])
            return [_fmt_operand_write(ops[0], f"(uint32_t)((int32_t){src} * (int32_t){imm})")]
        return ["/* imul: unexpected form */"]

    def _lift_muldiv(self, insn, ops, m):
        if len(ops) < 1:
            return [f"/* {m}: no operand */"]
        src = _fmt_operand_read(ops[0])
        if m == "mul":
            return [
                f"{{ uint64_t _r = (uint64_t)eax * (uint64_t){src};",
                f"  eax = (uint32_t)_r; edx = (uint32_t)(_r >> 32); }}"
            ]
        elif m == "div":
            return [
                f"{{ uint64_t _dividend = ((uint64_t)edx << 32) | eax;",
                f"  eax = (uint32_t)(_dividend / (uint32_t){src});",
                f"  edx = (uint32_t)(_dividend % (uint32_t){src}); }}"
            ]
        elif m == "idiv":
            return [
                f"{{ int64_t _dividend = ((int64_t)(int32_t)edx << 32) | eax;",
                f"  eax = (uint32_t)((int32_t)(_dividend / (int32_t){src}));",
                f"  edx = (uint32_t)((int32_t)(_dividend % (int32_t){src})); }}"
            ]
        return [f"/* {m}: unhandled */"]

    def _lift_shift(self, insn, ops, c_op):
        if len(ops) < 2:
            return [f"/* shift: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        cnt = _fmt_operand_read(ops[1])
        # Store CF = the last bit shifted out (x86 masks the count to 5 bits,
        # which also keeps the C shift well-defined). _cf is unchanged for a
        # count of 0, matching x86.
        if c_op == "<<":
            return [f"{{ uint32_t _d = {dst}; uint32_t _c = ({cnt}) & 31;"
                    f" if (_c) _cf = ((_d >> (32 - _c)) & 1);"
                    f" {_fmt_operand_write(ops[0], '_d << _c')} }}"]
        return [f"{{ uint32_t _d = {dst}; uint32_t _c = ({cnt}) & 31;"
                f" if (_c) _cf = ((_d >> (_c - 1)) & 1);"
                f" {_fmt_operand_write(ops[0], '_d >> _c')} }}"]

    def _lift_sar(self, insn, ops):
        if len(ops) < 2:
            return ["/* sar: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        cnt = _fmt_operand_read(ops[1])
        return [f"{{ uint32_t _d = {dst}; uint32_t _c = ({cnt}) & 31;"
                f" if (_c) _cf = ((_d >> (_c - 1)) & 1);"
                f" {_fmt_operand_write(ops[0], '(uint32_t)((int32_t)_d >> _c)')} }}"]

    def _lift_rotate(self, insn, ops, m):
        if len(ops) < 2:
            return [f"/* {m}: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        cnt = _fmt_operand_read(ops[1])
        func = "ROL32" if m == "rol" else "ROR32"
        # CF = the bit that rotates out the top (rol) / bottom (ror)
        bit = f"((_d >> (32 - _c)) & 1)" if m == "rol" else f"((_d >> (_c - 1)) & 1)"
        return [f"{{ uint32_t _d = {dst}; uint32_t _c = ({cnt}) & 31;"
                f" if (_c) _cf = {bit};"
                f" {_fmt_operand_write(ops[0], f'{func}(_d, _c)')} }}"]

    def _lift_rotate_carry(self, insn, ops, m):
        """rcl/rcr - rotate through carry.

        These were listed as flag setters but had no emitter, so every site
        became a comment and the operand kept its old value - 1,945 sites in
        MM3's .text alone.

        CF participates as a 33rd bit, so the result depends on the incoming
        carry, and the outgoing carry must be published for a following adc,
        sbb or jc. Width handling matches _lift_rotate: the 32-bit form.
        """
        if len(ops) < 2:
            return [f"/* {m}: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        cnt = _fmt_operand_read(ops[1])
        func = "RCL32" if m == "rcl" else "RCR32"
        write = _fmt_operand_write(ops[0], "_r")
        return [f"{{ int _co = _cf; uint32_t _r = {func}({dst}, "
                f"(int)({cnt}) & 31, _cf, &_co);"
                f" {write} _cf = _co; }}"]

    def _lift_bsf_bsr(self, insn, ops, m):
        """bsf r, src / bsr r, src — bit scan, ZF set if src is zero."""
        if len(ops) < 2:
            return [f"/* {m}: bad operands */"]
        src = _fmt_operand_read(ops[1])
        func = "BSF32" if m == "bsf" else "BSR32"
        return [_fmt_operand_write(ops[0], f"{func}({src}) /* {m}: bit scan */")]

    # ── Compare / Test (standalone) ──

    def _lift_cmp(self, insn, ops):
        if len(ops) < 2:
            return ["/* cmp: bad operands */"]
        ops = _normalize_cmp_operands(ops)
        lhs = _fmt_operand_read(ops[0])
        rhs = _fmt_operand_read(ops[1])
        # Store CF for sbb/adc consumers. The following jcc re-evaluates the
        # operands itself, so this is purely for carry-dependent instructions.
        return [
            f"_cf = ((uint32_t)({lhs}) < (uint32_t)({rhs})); /* cmp: CF = (lhs < rhs) unsigned */",
            f"(void)0; /* cmp {lhs}, {rhs} - flags set for next jcc */"
        ]

    def _lift_test(self, insn, ops):
        if len(ops) < 2:
            return ["/* test: bad operands */"]
        ops = _normalize_cmp_operands(ops)
        lhs = _fmt_operand_read(ops[0])
        rhs = _fmt_operand_read(ops[1])
        return [
            "_cf = 0; /* test clears CF */",
            f"(void)0; /* test {lhs}, {rhs} - flags set for next jcc */"
        ]

    # ── Control flow ──

    def _build_call_args(self, target_addr):
        """Build argument list for a function call based on ABI data."""
        abi_info = self.abi_db.get(target_addr, {})
        cc = abi_info.get("calling_convention", "cdecl")
        num_params = abi_info.get("estimated_params", 0)

        args = []
        if cc in ("thiscall", "thiscall_cdecl"):
            args.append("(void*)(uintptr_t)ecx")
        for i in range(num_params):
            args.append(f"0 /* a{i+1} */")
        return ", ".join(args)

    # SEH prolog/epilog addresses - these functions modify ebp for their
    # caller.  After calling __SEH_prolog, the caller must read back ebp
    # from g_seh_ebp.  Before returning, __SEH_prolog writes g_seh_ebp.
    #
    # Per-title addresses, detected from the binary by detect_seh_helpers()
    # and assigned to the instance. The class values are only a fallback for
    # callers that construct a Lifter without a function database.
    SEH_PROLOG = None
    SEH_EPILOG = None

    def _lift_call(self, insn, ops):
        # x86 'call' pushes return address then jumps.
        # With global esp, we push a dummy return address (0) then call.
        # The callee's 'ret' will pop it back off.
        if insn.call_target:
            name = self._call_target_name(insn.call_target)
            if insn.call_target in SETJMP_DIRECT:
                return_slot = f"0x{insn.address + insn.size:08X}"
                jb = f"_mm3_jb_{insn.address:08X}"
                lines = ["{",
                         f"    jmp_buf {jb};",
                         f"    if (setjmp({jb}) == 0) {{",
                         f"        PUSH32(esp, {return_slot}); {name}(); /* call 0x{insn.call_target:08X} (guest setjmp) */",
                         f"        recomp_setjmp_register(MEM32(esp), &{jb});",
                         "    } else {"]
                if self.uses_ebp:
                    lines.append("        ebp = g_seh_ebp; /* longjmp restored caller frame */")
                lines += ["    }", "}"]
                return lines
            if insn.call_target in DISPATCH_DIRECT:
                # Route through the manual dispatch so recomp_manual.c can
                # override functions the lifter cannot generate correctly.
                lines = [f"PUSH32(esp, 0); RECOMP_ICALL_SAFE(0x{insn.call_target:08X}, _icall_esp); /* call 0x{insn.call_target:08X} */"]
            else:
                cleanup = self._callee_cleanup(insn.call_target)
                ret_note = "" if cleanup is None else f" ret {cleanup}"
                return_slot = "0"
                if insn.call_target in RETURN_ADDRESS_READERS:
                    return_slot = f"0x{insn.address + insn.size:08X}"
                lines = [f"PUSH32(esp, {return_slot}); {name}(); /* call 0x{insn.call_target:08X}{ret_note} */"]
                if insn.call_target == 0x0008B22E:
                    lines.insert(0, f"recomp_trace_b22e(0, 0x{insn.address:08X});")
                    lines.append(f"recomp_trace_b22e(1, 0x{insn.address:08X});")
                if insn.call_target == 0x000854CF:
                    lines.insert(0, f"recomp_trace_854cf(0, 0x{insn.address:08X}, esp);")
                    lines.append(f"recomp_trace_854cf(1, 0x{insn.address:08X}, esp);")
                if insn.call_target == 0x00096738:
                    lines.insert(0, f"recomp_trace_83d32_precall(0x{insn.address:08X}, (uint32_t)esp);")
                if insn.call_target == 0x000860AA:
                    lines.insert(0, f"recomp_trace_860aa_call(0x{insn.address:08X}, (uint32_t)eax, (uint32_t)ecx, (uint32_t)edx, (uint32_t)ebx, (uint32_t)esi, (uint32_t)edi, (uint32_t)esp);")
                if self.func_start == 0x00096825 and insn.call_target == 0x00083D49:
                    lines.append("recomp_trace_96825_after_83d49((uint32_t)eax, (uint32_t)esp, MEM32(ebp + 8));")
                if self.func_start == 0x001EC708 and insn.address == 0x001EC773:
                    lines.append("recomp_trace_sched_result(0x00342B20, 0x001EC773, (uint32_t)eax, (uint32_t)esp);")
            if self.func_start == 0x001BCBC0 and insn.address in (
                    0x001BCC8C, 0x001BCC84, 0x001BCC96,
                    0x001BCDDE, 0x001BCDE8, 0x001BCE11):
                lines.insert(0, f"recomp_trace_bcbcc0_direct(0, 0x{insn.call_target:08X}, 0x{insn.address:08X});")
                lines.append(f"recomp_trace_bcbcc0_direct(1, 0x{insn.call_target:08X}, 0x{insn.address:08X});")
            if insn.call_target == 0x001E73AF:
                lines.insert(0, f"recomp_trace_73af(0, 0x{insn.address:08X});")
                lines.append(f"recomp_trace_73af(1, 0x{insn.address:08X});")
            if insn.call_target == 0x00170ED1:
                lines.insert(0, f"recomp_trace_170ed1_edge(0, 0x{insn.address:08X});")
                lines.append(f"recomp_trace_170ed1_edge(1, 0x{insn.address:08X});")
            if self.func_start == 0x001E6EAD:
                lines.insert(0, f"recomp_trace_6ead_edge(0, 0x{insn.call_target:08X}, 0x{insn.address:08X});")
                lines.append(f"recomp_trace_6ead_edge(1, 0x{insn.call_target:08X}, 0x{insn.address:08X});")
            if self.func_start == 0x001BF86A:
                lines.insert(0, f"recomp_trace_bf86a_edge(0, 0x{insn.call_target:08X}, 0x{insn.address:08X});")
                lines.append(f"recomp_trace_bf86a_edge(1, 0x{insn.call_target:08X}, 0x{insn.address:08X});")
            if self.func_start == 0x001BF1D4:
                lines.insert(0, f"recomp_trace_bf1d4_edge(0, 0x{insn.call_target:08X}, 0x{insn.address:08X});")
                lines.append(f"recomp_trace_bf1d4_edge(1, 0x{insn.call_target:08X}, 0x{insn.address:08X});")
            if self.func_start == 0x001BE953:
                lines.insert(0, f"recomp_trace_be953_edge(0, 0x{insn.call_target:08X}, 0x{insn.address:08X});")
                lines.append(f"recomp_trace_be953_edge(1, 0x{insn.call_target:08X}, 0x{insn.address:08X});")
                if insn.address == 0x001BECC3:
                    lines.append("recomp_trace_be953_loop(0x001BECC3, MEM32(ebp + 0x24), MEM32(ebp + 0x28), MEM32(ebp + 0x2C), MEM32(ebp + 0x38), MEM32(ebp + 0x3C));")
            if self.func_start == 0x0003F1B0:
                lines.insert(0, f"recomp_trace_3f1b0_edge(0, 0x{insn.call_target:08X}, 0x{insn.address:08X});")
                lines.append(f"recomp_trace_3f1b0_edge(1, 0x{insn.call_target:08X}, 0x{insn.address:08X});")
            if self.func_start == 0x000127A9:
                lines.insert(0, f"recomp_trace_127a9_edge(0, 0x{insn.call_target:08X}, 0x{insn.address:08X});")
                lines.append(f"recomp_trace_127a9_edge(1, 0x{insn.call_target:08X}, 0x{insn.address:08X});")
            if self.func_start == 0x001BCE30:
                lines.insert(0, f"recomp_trace_bce30_edge(0, 0x{insn.call_target:08X}, 0x{insn.address:08X});")
                lines.append(f"recomp_trace_bce30_edge(1, 0x{insn.call_target:08X}, 0x{insn.address:08X});")
            if self.func_start == 0x0002E735:
                lines.insert(0, f"recomp_trace_2e735_edge(0, 0x{insn.call_target:08X}, 0x{insn.address:08X});")
                lines.append(f"recomp_trace_2e735_edge(1, 0x{insn.call_target:08X}, 0x{insn.address:08X});")
            if self.func_start == 0x001E73AF:
                lines.insert(0, f"recomp_trace_73af_inner(0, 0x{insn.call_target:08X}, 0x{insn.address:08X});")
                lines.append(f"recomp_trace_73af_inner(1, 0x{insn.call_target:08X}, 0x{insn.address:08X});")
            if insn.call_target == 0x0008872F:
                lines.insert(0, f"recomp_trace_guest_call(0x0008872F, 0x{insn.address:08X});")
            if insn.call_target == 0x00089CAB:
                lines.insert(0, f"recomp_trace_guest_call(0x00089CAB, 0x{insn.address:08X});")
            if insn.call_target == 0x001F3163:
                lines.insert(0, f"recomp_snapshot_f3163_call(0x{insn.address:08X});")
            if insn.call_target == 0x00042921:
                lines.insert(0, f"recomp_snapshot_42921_call(0x{insn.address:08X});")
            if insn.call_target == 0x00343BD0:
                lines.insert(0, f"recomp_trace_pump_entry(0x{insn.address:08X});")
            if insn.call_target == 0x001EC6EE:
                lines.insert(0, f"recomp_trace_frame_call(0x{insn.address:08X});")
            if insn.call_target == 0x00343E60:
                lines.insert(0, f"recomp_trace_pump_call(0x{insn.address:08X}, (uint32_t)eax);")
            if insn.call_target == 0x001EC7F7:
                lines.insert(0, f"recomp_trace_frame_call(0x{insn.address:08X});")
            if self.func_start in (0x00170ED1, 0x00170EE2):
                lines.insert(0, f"recomp_trace_callback_helper(0x{self.func_start:08X}, 0x{insn.address:08X});")
            if (insn.call_target in (0x001E73AF, 0x001E7627, 0x00344640, 0x00344A20,
                                     0x00344360, 0x00345740, 0x00346450,
                                     0x00348C99) or
                    (self.func_start == 0x001EC708 and insn.call_target == 0x001EC6EE) or
                    (self.func_start == 0x001E7627 and insn.call_target in (
                        0x001EC8CE, 0x0020F7EB, 0x001F33A2)) or
                    (self.func_start == 0x001E7627 and insn.call_target in (
                        0x001C032D, 0x001C01B5, 0x001ECD56,
                        0x001E7E29, 0x001E839C)) or
                    (self.func_start == 0x001EC8E6 and
                        insn.call_target == 0x001EC520) or
                    (self.func_start == 0x001E839C and insn.call_target in (
                        0x001E7D65, 0x001E82DB, 0x0017013D, 0x00025384,
                        0x00025339, 0x001EC708, 0x0016FF04, 0x0002539A,
                        0x001E7F1B, 0x001E7B41, 0x00170EC0, 0x001E7AF4,
                        0x00083B04)) or
                    (self.func_start == 0x001E7F1B and insn.call_target in (
                        0x00340460, 0x0033FD10, 0x00342AE0, 0x000F5050,
                        0x000F5027, 0x000F4FEF, 0x00342B00, 0x00342860)) or
                    (self.func_start == 0x00342B00 and
                        insn.call_target == 0x00347170) or
                         (self.func_start == 0x00347170 and
                          insn.call_target in (0x00344A20, 0x00346F40)) or
                         (self.func_start == 0x00344A20 and
                          insn.call_target == 0x00344640) or
                         (self.func_start == 0x00344640 and
                          insn.call_target in (0x00344410, 0x00344520,
                                               0x003444C0, 0x003444AB0))):
                lines.insert(0, f"recomp_trace_sched_call(0x{insn.call_target:08X}, 0x{insn.address:08X});")
                lines.append(
                    f"recomp_trace_sched_result(0x{insn.call_target:08X}, "
                    f"0x{insn.address:08X}, (uint32_t)eax, (uint32_t)esp);")
                if insn.call_target in (0x001E7F1B, 0x001E7B41):
                    lines.append(
                        f"recomp_trace_sched_loop_result(0x{insn.call_target:08X}, "
                        f"0x{insn.address:08X}, (uint32_t)eax, (uint32_t)esp);")
            # After __SEH_prolog/__SEH_epilog, read back the frame pointer.
            # Also after the alternate prolog variants (fs:[0] write + lea
            # ebp,[esp+N]) that establish ebp but are not the detected helper.
            if (insn.call_target in (self.SEH_PROLOG, self.SEH_EPILOG)
                    or insn.call_target in (0x00097AA4, 0x0009504E)):
                lines.append("ebp = g_seh_ebp; /* read back frame from SEH helper */")
            return lines
        elif len(ops) >= 1:
            # The dummy-return PUSH32(esp, 0) executes before the ICALL
            # target expression is evaluated, so an esp-relative operand reads
            # 4 bytes above the original 'call [esp+X]' slot. Bump the disp
            # to hit the slot the original instruction addressed.
            bias = 4 if (ops[0].type == "mem" and ops[0].mem_base == "esp") else 0
            target = _fmt_operand_read(ops[0], disp_bias=bias)
            if self.func_start == 0x001BE953:
                # Snapshot the address expression before the callee can clobber
                # EAX/other source registers. Re-reading it in the END trace
                # reports a different slot and can falsely call a valid target
                # zero/invalid.
                target_var = f"_be953_target_{insn.address:08X}"
                lines = [
                    f"uint32_t {target_var} = {target};",
                    f"recomp_trace_be953_icall(0, {target_var}, 0x{insn.address:08X});",
                    f"PUSH32(esp, 0); RECOMP_ICALL_SAFE({target_var}, _icall_esp); "
                    f"recomp_trace_be953_icall(1, {target_var}, 0x{insn.address:08X}); /* indirect call */",
                ]
            elif ((self.func_start == 0x001BCBC0 and
                   (insn.address in (0x001BCC84, 0x001BCD15) or
                   (0x001BCD00 <= insn.address <= 0x001BCD20 and
                    target == "MEM32(eax + 4)"))) or
                  (self.func_start == 0x001B9FCE and
                   target == "MEM32(eax + 0xC)")):
                # Snapshot the target before the item callback.  The callback
                # legitimately changes EAX, so re-reading the original
                # expression for the END trace can report a bogus target
                # (for example 0x12000000) even when the BEGIN target was
                # valid.  This is diagnostic-only and does not alter the call.
                target_var = f"_bcbcc0_target_{insn.address:08X}"
                lines = [
                    f"uint32_t {target_var} = {target};",
                    f"recomp_trace_bcbcc0_icall(0, {target_var}, 0x{insn.address:08X});",
                    f"PUSH32(esp, 0); RECOMP_ICALL_SAFE({target_var}, _icall_esp); "
                    f"recomp_trace_bcbcc0_icall(1, {target_var}, 0x{insn.address:08X}); /* indirect call */",
                ]
            else:
                lines = [f"PUSH32(esp, 0); RECOMP_ICALL_SAFE({target}, _icall_esp); /* indirect call */"]
            if self.func_start == 0x001B9FB0:
                lines.insert(0, f"recomp_trace_b9fb0_icall(0, {target}, 0x{insn.address:08X});")
                lines.append(f"recomp_trace_b9fb0_icall(1, {target}, 0x{insn.address:08X});")
            if self.func_start == 0x001EC520:
                lines.insert(0, f"recomp_trace_1ec520_icall(0, {target}, 0x{insn.address:08X});")
                lines.append(f"recomp_trace_1ec520_icall(1, {target}, 0x{insn.address:08X});")
            if self.func_start == 0x001B98E1:
                lines.insert(0, f"recomp_trace_b98e1_icall(0, {target}, 0x{insn.address:08X});")
                lines.append(f"recomp_trace_b98e1_icall(1, {target}, 0x{insn.address:08X});")
            if self.func_start == 0x00086097:
                lines.insert(0, "recomp_trace_sched_callback(0, MEM32(0x00362014), MEM32(esp), g_esp);")
                lines.append("recomp_trace_sched_callback(1, MEM32(0x00362014), eax, g_esp);")
            if self.func_start == 0x00344640 and not insn.call_target:
                lines = ["PUSH32(esp, 0);", "recomp_trace_sched_callback(0, edi, esi, g_esp);",
                         f"RECOMP_ICALL_SAFE({target}, _icall_esp); /* indirect call */"]
                lines.append(f"recomp_trace_sched_callback(1, {target}, eax, g_esp);")
            for slot in (0x00361F50, 0x003620A8, 0x003620A4):
                if f"0x{slot:X}" in target:
                    lines.insert(0, f"recomp_trace_init_icall(0x{slot:08X}, {target}, 0x{insn.address:08X});")
            # Mark indirect calls for post-processing by _fixup_icall_esp_save
            return lines
        return ["/* call: no target */"]

    def _lift_ret(self, insn, ops):
        # x86 'ret' pops return address from stack.
        # 'ret N' also pops N extra bytes (stdcall cleanup).
        # If this function IS __SEH_prolog or __SEH_epilog, bridge ebp
        # so the caller can read back the frame pointer.
        prefix = ""
        if (self.func_start in (self.SEH_PROLOG, self.SEH_EPILOG)
                or self.func_start in (0x00097AA4, 0x0009504E)):
            prefix = "g_seh_ebp = ebp; "
        if self.func_start in (0x0033FC40, 0x00348C99, 0x001E73AF,
                               0x001E7627, 0x001E77F3):
            prefix += (f"recomp_trace_render_return(0x{self.func_start:08X}, "
                       "(uint32_t)eax, (uint32_t)esp); ")
        if self.func_start in (0x00084709, 0x000860AA):
            prefix += f"recomp_trace_cleanup_return(0x{self.func_start:08X}, (uint32_t)eax, (uint32_t)esp); "
        if self.func_start == 0x001BF1D4:
            prefix += "recomp_trace_1bf1d4(1); "
        if self.func_start == 0x001BCE30:
            prefix += "recomp_trace_bce30(1); "
        if self.func_start == 0x001BCBC0:
            prefix += "recomp_trace_bcbcc0(1); "
        if len(ops) >= 1 and ops[0].type == "imm":
            n = ops[0].imm
            return [f"{prefix}esp += {4 + n}; return; /* ret {n} */"]
        return [f"{prefix}esp += 4; return; /* ret */"]

    def _is_external_target(self, addr):
        """Check if a jump target is outside the current function."""
        return not (self.func_start <= addr < self.func_end)

    def _read_jump_table(self, table_va, max_entries=256):
        """Read 32-bit jump table entries from the XBE at a given VA.
        Returns list of target addresses. Stops when an entry is not a
        valid code address or max_entries is reached."""
        if not self.xbe_data:
            return []
        offset = va_to_file_offset(table_va)
        if offset is None:
            return []
        targets = []
        for i in range(max_entries):
            o = offset + i * 4
            if o + 4 > len(self.xbe_data):
                break
            val = struct.unpack_from('<I', self.xbe_data, o)[0]
            if not is_code_address(val):
                break
            targets.append(val)
        return targets

    def _analyze_switch_table(self, ops):
        """Detect if an indirect jmp operand is an intra-function switch table.
        Pattern: jmp [reg*scale + table_base] or jmp [reg + table_base]
        Returns (targets: list[int]) if ALL table entries are within the current
        function, else empty list."""
        if not ops or ops[0].type != "mem":
            return []
        op = ops[0]
        # Need a table base (displacement) and an index register
        if not op.mem_disp or not (op.mem_index or op.mem_base):
            return []
        table_va = op.mem_disp
        targets = self._read_jump_table(table_va)
        if not targets:
            return []
        # Check that ALL targets are within the current function
        if all(self.func_start <= t < self.func_end for t in targets):
            return targets
        return []

    def _lift_jmp(self, insn, ops):
        if insn.jump_target:
            if self._is_external_target(insn.jump_target):
                # Tail call - no return address push (reuses current frame's)
                # Bridge ebp so the target function can inherit our frame pointer.
                name = self._call_target_name(insn.jump_target)
                lines = []
                if insn.jump_target == 0x00343E60:
                    lines.append(f"recomp_trace_pump_call(0x{insn.address:08X}, (uint32_t)eax);")
                lines.append(f"g_seh_ebp = ebp; {name}(); return; /* tail jmp 0x{insn.jump_target:08X} */")
                return lines
            return [f"goto loc_{insn.jump_target:08X};"]
        elif len(ops) >= 1:
            # Detect intra-function switch tables (computed gotos)
            switch_targets = self._analyze_switch_table(ops)
            if switch_targets:
                target_expr = _fmt_operand_read(ops[0])
                unique_targets = sorted(set(switch_targets))
                lines = [f"{{ uint32_t _jt = {target_expr}; /* switch: {len(switch_targets)} entries, {len(unique_targets)} targets */"]
                for t in unique_targets:
                    lines.append(f"if (_jt == 0x{t:08X}u) goto loc_{t:08X};")
                lines.append(f"g_seh_ebp = ebp; RECOMP_ITAIL(_jt); return; }}")
                return lines
            if self.func_start in LONGJMP_FUNCS:
                return [f"g_seh_ebp = ebp; recomp_guest_longjmp(edx); return; /* guest longjmp */"]
            target = _fmt_operand_read(ops[0])
            return [f"g_seh_ebp = ebp; RECOMP_ITAIL({target}); return; /* indirect tail jmp */"]
        return ["/* jmp: no target */"]

    def _lift_jcc(self, insn):
        """Standalone conditional jump (no flag-setter tracked)."""
        target = insn.jump_target
        jcc = insn.mnemonic

        # jecxz/jcxz: jump if ecx/cx is zero (not flag-based)
        if jcc in ("jecxz", "jcxz"):
            cond = "ecx == 0" if jcc == "jecxz" else "LO16(ecx) == 0"
            if target:
                if self._is_external_target(target):
                    name = self._call_target_name(target)
                    return [f"if ({cond}) {{ g_seh_ebp = ebp; {name}(); return; }} /* {jcc} */"]
                return [f"if ({cond}) goto loc_{target:08X}; /* {jcc} */"]
            return [f"/* {jcc} - no target */"]

        cond_info = COND_MAP.get(jcc)
        desc = cond_info[2] if cond_info else jcc
        if target:
            if self._is_external_target(target):
                name = self._call_target_name(target)
                return [f"if (_flags /* {jcc}: {desc} */) {{ g_seh_ebp = ebp; {name}(); return; }}"]
            return [f"if (_flags /* {jcc}: {desc} */) goto loc_{target:08X};"]
        return [f"/* {jcc}: {desc} - no target */"]

    # ── SETcc / CMOVcc ──

    def _lift_setcc(self, insn, ops, m):
        if len(ops) < 1:
            return [f"/* {m}: no operand */"]
        return [_fmt_operand_write(ops[0], f"_flags /* {m} */")]

    def _lift_cmovcc(self, insn, ops, m):
        if len(ops) < 2:
            return [f"/* {m}: bad operands */"]
        src = _fmt_operand_read(ops[1])
        return [f"if (_flags /* {m} */) {_fmt_operand_write(ops[0], src)}"]

    # ── String operations ──

    def _lift_rep_string(self, insn, m):
        if "movsb" in m:
            return ["memcpy((void*)XBOX_PTR(edi), (void*)XBOX_PTR(esi), ecx);",
                    "esi += ecx; edi += ecx; ecx = 0; /* rep movsb */"]
        if "movsd" in m:
            return ["memcpy((void*)XBOX_PTR(edi), (void*)XBOX_PTR(esi), ecx * 4);",
                    "esi += ecx * 4; edi += ecx * 4; ecx = 0; /* rep movsd */"]
        if "movsw" in m:
            return ["memcpy((void*)XBOX_PTR(edi), (void*)XBOX_PTR(esi), ecx * 2);",
                    "esi += ecx * 2; edi += ecx * 2; ecx = 0; /* rep movsw */"]
        if "stosb" in m:
            return ["memset((void*)XBOX_PTR(edi), (uint8_t)eax, ecx);",
                    "edi += ecx; ecx = 0; /* rep stosb */"]
        if "stosd" in m:
            return [
                "{ uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM32(edi + _i*4) = eax; }",
                "edi += ecx * 4; ecx = 0; /* rep stosd */"
            ]
        if "stosw" in m:
            return [
                "{ uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM16(edi + _i*2) = LO16(eax); }",
                "edi += ecx * 2; ecx = 0; /* rep stosw */"
            ]
        if "cmpsb" in m:
            return ["{ uint32_t _i; _cmps_zf = 1; "
                    "for (_i = 0; _i < ecx; _i++) { "
                    "if (MEM8(esi+_i) != MEM8(edi+_i)) { _cmps_zf = 0; "
                    "_cf = (MEM8(esi+_i) < MEM8(edi+_i)); break; } } "
                    "esi += _i; edi += _i; ecx -= _i; } /* repe cmpsb */"]
        if "cmpsw" in m:
            return ["{ uint32_t _i; _cmps_zf = 1; "
                    "for (_i = 0; _i < ecx; _i++) { "
                    "if (MEM16(esi+_i*2) != MEM16(edi+_i*2)) { _cmps_zf = 0; "
                    "_cf = (MEM16(esi+_i*2) < MEM16(edi+_i*2)); break; } } "
                    "esi += _i*2; edi += _i*2; ecx -= _i; } /* repe cmpsw */"]
        if "cmpsd" in m:
            return ["{ uint32_t _i; _cmps_zf = 1; "
                    "for (_i = 0; _i < ecx; _i++) { "
                    "if (MEM32(esi+_i*4) != MEM32(edi+_i*4)) { _cmps_zf = 0; "
                    "_cf = (MEM32(esi+_i*4) < MEM32(edi+_i*4)); break; } } "
                    "esi += _i*4; edi += _i*4; ecx -= _i; } /* repe cmpsd */"]
        if "scasb" in m:
            return ["{ uint32_t _i; _cmps_zf = 1; "
                    "for (_i = 0; _i < ecx; _i++) { "
                    "if (MEM8(edi+_i) != LO8(eax)) { _cmps_zf = 0; "
                    "_cf = (MEM8(edi+_i) < LO8(eax)); break; } } "
                    "edi += _i; ecx -= _i; } /* repne scasb */"]
        if "scasw" in m or "scasd" in m:
            return [f"/* {m} - string scan, ecx iterations */"]
        return [f"/* {m} */"]

    def _lift_string_op(self, insn, m):
        if m == "movsb":
            return ["MEM8(edi) = MEM8(esi); esi++; edi++; /* movsb */"]
        if m == "movsd":
            return ["MEM32(edi) = MEM32(esi); esi += 4; edi += 4; /* movsd */"]
        if m == "stosb":
            return ["MEM8(edi) = LO8(eax); edi++; /* stosb */"]
        if m == "stosd":
            return ["MEM32(edi) = eax; edi += 4; /* stosd */"]
        if m == "lodsb":
            return ["SET_LO8(eax, MEM8(esi)); esi++; /* lodsb */"]
        if m == "lodsd":
            return ["eax = MEM32(esi); esi += 4; /* lodsd */"]
        if m == "movsw":
            return ["MEM16(edi) = MEM16(esi); esi += 2; edi += 2; /* movsw */"]
        if m == "stosw":
            return ["MEM16(edi) = LO16(eax); edi += 2; /* stosw */"]
        if m == "lodsw":
            return ["SET_LO16(eax, MEM16(esi)); esi += 2; /* lodsw */"]
        # Bare (unprefixed) compare/scan. The repe/repne forms are handled by
        # _lift_rep_string; these single-step forms had no emitter, so 183
        # sites in MM3 silently compared nothing and left flags stale.
        # Flags are set as for SUB, and the pointers advance assuming DF=0,
        # matching the direction assumption the rep forms already make.
        if m == "cmpsb":
            return ["{ uint32_t _a = MEM8(esi), _b = MEM8(edi);"
                    " _cf = (_a < _b); _cmps_zf = (_a == _b);"
                    " esi++; edi++; } /* cmpsb */"]
        if m == "cmpsd":
            return ["{ uint32_t _a = MEM32(esi), _b = MEM32(edi);"
                    " _cf = (_a < _b); _cmps_zf = (_a == _b);"
                    " esi += 4; edi += 4; } /* cmpsd */"]
        if m == "cmpsw":
            return ["{ uint32_t _a = MEM16(esi), _b = MEM16(edi);"
                    " _cf = (_a < _b); _cmps_zf = (_a == _b);"
                    " esi += 2; edi += 2; } /* cmpsw */"]
        if m == "scasb":
            return ["{ uint32_t _a = LO8(eax), _b = MEM8(edi);"
                    " _cf = (_a < _b); _cmps_zf = (_a == _b);"
                    " edi++; } /* scasb */"]
        if m == "scasw":
            return ["{ uint32_t _a = LO16(eax), _b = MEM16(edi);"
                    " _cf = (_a < _b); _cmps_zf = (_a == _b);"
                    " edi += 2; } /* scasw */"]
        if m == "scasd":
            return ["{ uint32_t _a = eax, _b = MEM32(edi);"
                    " _cf = (_a < _b); _cmps_zf = (_a == _b);"
                    " edi += 4; } /* scasd */"]
        return [f"/* {m} */"]

    # ── FPU (x87) ──

    # ── SSE (scalar/packed float) ──

    def _lift_sse(self, insn, m, ops):
        """Translate SSE instructions to C float operations.

        An xmm register is modelled as float[4]. It used to be a single
        float, which meant the packed forms had nowhere to put three of
        their four results and were emitted as comments - they did nothing
        at all. Scalar forms operate on lane 0, which is what the hardware
        does.
        """
        # MOVQ/MOVNTQ on the MMX registers. These fell through to the
        # comment-only fallback at the bottom of this method, which meant an
        # MMX block copy lifted to its loop counter and pointer arithmetic
        # with every load and store missing: the loop ran the right number of
        # times and moved nothing. sub_000120B6 is one such copy and has 77
        # callers. The mm registers are already declared as uint64_t by the
        # translator, so only the memory side was missing.
        if m in ("movq", "movntq") and len(ops) >= 2 and (
                _is_mmx_reg(ops[0]) or _is_mmx_reg(ops[1])):
            return [_fmt_operand_write(ops[0], _fmt_operand_read(ops[1]))
                    + f" /* {m} */"]

        nops = len(ops)
        if nops < 1:
            return [f"/* {m}: no operands */"]

        # SSE register names (xmm0-xmm7) are used as float locals
        def _sse_read(op):
            if op.type == "reg":
                # Scalar forms touch only the low lane.
                return f"{op.reg}[0]" if op.reg.startswith("xmm") else op.reg
            elif op.type == "mem":
                if op.mem_size == 8:
                    return f"MEMD({_fmt_mem(op)})"
                return f"MEMF({_fmt_mem(op)})"
            elif op.type == "imm":
                return _fmt_imm(op.imm)
            return f"/* sse_read? */"

        def _sse_write(op, val):
            if op.type == "reg":
                if op.reg.startswith("xmm"):
                    return f"{op.reg}[0] = {val};"
                return f"{op.reg} = {val};"
            elif op.type == "mem":
                if op.mem_size == 8:
                    return f"MEMD({_fmt_mem(op)}) = {val};"
                return f"MEMF({_fmt_mem(op)}) = {val};"
            return f"/* sse_write? */;"

        def _lane(op, i):
            """Lane i of an xmm register or of a 16-byte memory operand."""
            if op.type == "reg":
                return f"{op.reg}[{i}]" if op.reg.startswith("xmm") else op.reg
            if op.type == "mem":
                return f"MEMF({_fmt_mem(op, 4 * i)})"
            if op.type == "imm":
                return _fmt_imm(op.imm)
            return "0.0f"

        def _lane_w(op, i, val):
            if op.type == "reg":
                if op.reg.startswith("xmm"):
                    return f"{op.reg}[{i}] = {val};"
                return f"{op.reg} = {val};"
            if op.type == "mem":
                return f"MEMF({_fmt_mem(op, 4 * i)}) = {val};"
            return "/* lane write? */;"

        def _packed(dst, make, note, src=None):
            """Emit four independent lanes. Each lane reads and writes only
            its own index, so this stays correct when dst aliases src."""
            body = " ".join(
                _lane_w(dst, i, make(i)) for i in range(4))
            return ["{ " + body + f" }} /* {note} */"]

        def _shuffle(dst, lanes, note):
            """Emit a lane permutation. The sources are read into temporaries
            first because a shuffle's destination is also one of its
            sources."""
            tmp = " ".join(f"float _s{i} = {lanes[i]};" for i in range(4))
            out = " ".join(_lane_w(dst, i, f"_s{i}") for i in range(4))
            return ["{ " + tmp + " " + out + f" }} /* {note} */"]

        # ── Moves ──
        if m in ("movaps", "movups", "movntps", "movapd", "movupd"):
            # A full 16-byte move, not the low lane it used to be.
            if nops >= 2:
                return _packed(ops[0], lambda i: _lane(ops[1], i), m)
            return [f"/* {m} {insn.op_str} */"]
        if m in ("movlps", "movhps"):
            # These move the low or high 64 bits, i.e. two lanes.
            if nops >= 2:
                base = 0 if m == "movlps" else 2
                body = " ".join(
                    _lane_w(ops[0], base + k,
                            _lane(ops[1], (base + k) if ops[1].type == "reg" else k))
                    for k in range(2))
                return ["{ " + body + f" }} /* {m} */"]
            return [f"/* {m} {insn.op_str} */"]
        if m in ("movss", "movsd"):
            if nops >= 2:
                store = _sse_write(ops[0], _sse_read(ops[1]))
                # Loading from memory into a register zeroes the upper lanes;
                # a register-to-register move leaves them alone.
                if ops[0].type == "reg" and ops[0].reg.startswith("xmm")                         and ops[1].type == "mem":
                    zero = " ".join(_lane_w(ops[0], i, "0.0f") for i in (1, 2, 3))
                    return ["{ " + store + " " + zero + f" }} /* {m} (zeroes upper lanes) */"]
                return [store + f" /* {m} */"]
            return [f"/* {m} {insn.op_str} */"]

        if m == "movd":
            if nops >= 2:
                src = _fmt_operand_read(ops[1]) if ops[1].type != "reg" or not ops[1].reg.startswith("xmm") else _sse_read(ops[1])
                if ops[0].type == "reg" and ops[0].reg.startswith("xmm"):
                    return [f"memcpy(&{ops[0].reg}[0], &{src}, 4); /* movd to xmm */"]
                else:
                    return [f"{_fmt_operand_write(ops[0], src)} /* movd */"]
            return [f"/* movd {insn.op_str} */"]

        # ── Arithmetic ──
        if m in ("addss", "addsd"):
            if nops >= 2:
                return [_sse_write(ops[0], f"{_sse_read(ops[0])} + {_sse_read(ops[1])}") + f" /* {m} */"]
        if m in ("subss", "subsd"):
            if nops >= 2:
                return [_sse_write(ops[0], f"{_sse_read(ops[0])} - {_sse_read(ops[1])}") + f" /* {m} */"]
        if m in ("mulss", "mulsd"):
            if nops >= 2:
                return [_sse_write(ops[0], f"{_sse_read(ops[0])} * {_sse_read(ops[1])}") + f" /* {m} */"]
        if m in ("divss", "divsd"):
            if nops >= 2:
                return [_sse_write(ops[0], f"{_sse_read(ops[0])} / {_sse_read(ops[1])}") + f" /* {m} */"]
        if m in ("sqrtss", "sqrtsd"):
            if nops >= 2:
                return [_sse_write(ops[0], f"sqrtf({_sse_read(ops[1])})") + f" /* {m} */"]
        if m in ("minss", "minsd"):
            if nops >= 2:
                a, b = _sse_read(ops[0]), _sse_read(ops[1])
                return [_sse_write(ops[0], f"({a} < {b} ? {a} : {b})") + f" /* {m} */"]
        if m in ("maxss", "maxsd"):
            if nops >= 2:
                a, b = _sse_read(ops[0]), _sse_read(ops[1])
                return [_sse_write(ops[0], f"({a} > {b} ? {a} : {b})") + f" /* {m} */"]

        # ── Packed arithmetic ──
        if m in ("addps", "subps", "mulps", "divps",
                 "addpd", "subpd", "mulpd", "divpd"):
            if nops >= 2:
                c_op = {"add": "+", "sub": "-", "mul": "*", "div": "/"}[m[:3]]
                return _packed(
                    ops[0],
                    lambda i: f"{_lane(ops[0], i)} {c_op} {_lane(ops[1], i)}",
                    m)

        # ── Conversions ──
        if m == "cvtsi2ss":
            if nops >= 2:
                src = _fmt_operand_read(ops[1])
                return [_sse_write(ops[0], f"(float)(int32_t){src}") + " /* cvtsi2ss */"]
        if m in ("cvtss2si", "cvttss2si"):
            if nops >= 2:
                return [_fmt_operand_write(ops[0], f"(int32_t){_sse_read(ops[1])}") + f" /* {m} */"]
        if m == "cvtsi2sd":
            if nops >= 2:
                src = _fmt_operand_read(ops[1])
                return [_sse_write(ops[0], f"(double)(int32_t){src}") + " /* cvtsi2sd */"]
        if m in ("cvtsd2si", "cvttsd2si"):
            if nops >= 2:
                return [_fmt_operand_write(ops[0], f"(int32_t){_sse_read(ops[1])}") + f" /* {m} */"]
        if m == "cvtss2sd":
            if nops >= 2:
                return [_sse_write(ops[0], f"(double){_sse_read(ops[1])}") + " /* cvtss2sd */"]
        if m == "cvtsd2ss":
            if nops >= 2:
                return [_sse_write(ops[0], f"(float){_sse_read(ops[1])}") + " /* cvtsd2ss */"]

        # ── Comparison ──
        if m in ("comiss", "comisd", "ucomiss", "ucomisd"):
            if nops >= 2:
                return [f"/* {m} {_sse_read(ops[0])}, {_sse_read(ops[1])} - sets EFLAGS */"]

        # ── Bitwise ──
        if m in ("xorps", "xorpd", "andps", "andpd", "orps", "orpd",
                 "andnps", "andnpd"):
            if nops >= 2:
                # xor with self clears the whole register, all four lanes.
                if m.startswith("xor") and ops[0].type == "reg"                         and ops[1].type == "reg" and ops[0].reg == ops[1].reg:
                    return _packed(ops[0], lambda i: "0.0f", f"{m} self = zero")
                fn = {"xor": "RECOMP_FXOR", "and": "RECOMP_FAND",
                      "orp": "RECOMP_FOR", "andn": "RECOMP_FANDN"}[
                          "andn" if m.startswith("andn") else m[:3]]
                return _packed(
                    ops[0],
                    lambda i: f"{fn}({_lane(ops[0], i)}, {_lane(ops[1], i)})",
                    m)

        # ── Packed min/max ──
        if m in ("minps", "maxps", "minpd", "maxpd"):
            if nops >= 2:
                cmp_op = "<" if m.startswith("min") else ">"
                return _packed(
                    ops[0],
                    lambda i: (f"({_lane(ops[0], i)} {cmp_op} {_lane(ops[1], i)}"
                               f" ? {_lane(ops[0], i)} : {_lane(ops[1], i)})"),
                    m)

        # ── Reciprocal / rsqrt ──
        if m == "rsqrtss":
            if nops >= 2:
                return [_sse_write(ops[0], f"1.0f / sqrtf({_sse_read(ops[1])})") + " /* rsqrtss */"]
        if m == "rcpss":
            if nops >= 2:
                return [_sse_write(ops[0], f"1.0f / {_sse_read(ops[1])}") + " /* rcpss */"]

        # ── Packed sqrt / reciprocal / rsqrt ──
        # These used to compute the low lane only, because the model had no
        # other lane to write. rsqrtps and sqrtps are the workhorses of 3D
        # vector normalisation, so three quarters of every normalise was
        # stale. All four lanes now.
        if m in ("sqrtps", "rsqrtps", "rcpps"):
            if nops >= 2:
                make = {
                    "sqrtps":  lambda i: f"sqrtf({_lane(ops[1], i)})",
                    "rsqrtps": lambda i: f"1.0f / sqrtf({_lane(ops[1], i)})",
                    "rcpps":   lambda i: f"1.0f / {_lane(ops[1], i)}",
                }[m]
                return _packed(ops[0], make, m)

        # ── Packed comparison ──
        if m in ("cmpneqps", "cmpeqps", "cmpltps", "cmpleps",
                 "cmpnltps", "cmpnleps"):
            if nops >= 2:
                c = {"cmpeqps": "==", "cmpneqps": "!=", "cmpltps": "<",
                     "cmpleps": "<=", "cmpnltps": ">=", "cmpnleps": ">"}[m]
                # A packed compare writes a lane mask, all ones or all zeros,
                # not a 0/1 value; the result is normally fed to andps.
                return _packed(
                    ops[0],
                    lambda i: (f"RECOMP_FMASK({_lane(ops[0], i)} {c} "
                               f"{_lane(ops[1], i)})"),
                    m)

        # ── Move mask ──
        if m in ("movmskps", "movmskpd"):
            # Was hardcoded to 0, so every branch on it took the same path.
            if nops >= 2 and ops[1].type == "reg" and ops[1].reg.startswith("xmm"):
                return [_fmt_operand_write(ops[0], f"RECOMP_MOVMSKPS({ops[1].reg})")
                        + f" /* {m} */"]
            if nops >= 2:
                return [f"/* {m} {insn.op_str} - source is not a register */"]

        # ── MMX / integer SIMD ──
        if m in ("pand", "pandn", "por", "pxor", "pcmpgtd"):
            if nops >= 2:
                return [f"/* {m} {insn.op_str} (MMX/SIMD integer) */"]

        # ── Shuffle/unpack ──
        if m in ("shufps", "unpcklps", "unpckhps", "movlhps", "movhlps"):
            if nops >= 2:
                a, b = ops[0], ops[1]
                if m == "unpcklps":
                    lanes = [_lane(a, 0), _lane(b, 0), _lane(a, 1), _lane(b, 1)]
                elif m == "unpckhps":
                    lanes = [_lane(a, 2), _lane(b, 2), _lane(a, 3), _lane(b, 3)]
                elif m == "movlhps":
                    lanes = [_lane(a, 0), _lane(a, 1), _lane(b, 0), _lane(b, 1)]
                elif m == "movhlps":
                    lanes = [_lane(b, 2), _lane(b, 3), _lane(a, 2), _lane(a, 3)]
                else:  # shufps: the immediate picks two lanes from each source
                    if nops < 3 or ops[2].type != "imm":
                        return [f"/* shufps {insn.op_str} - no immediate */"]
                    sel = ops[2].imm & 0xFF
                    lanes = [_lane(a, sel & 3), _lane(a, (sel >> 2) & 3),
                             _lane(b, (sel >> 4) & 3), _lane(b, (sel >> 6) & 3)]
                return _shuffle(a, lanes, f"{m} {insn.op_str}")
            return [f"/* {m} {insn.op_str} */"]

        return [f"/* SSE: {m} {insn.op_str} */"]

    # ── FPU (x87) ──

    def _lift_fpu(self, insn, m, ops):
        """Basic FPU instruction translation using double locals."""
        # FPU is complex. We translate common patterns to double operations.
        # Full accuracy would require an x87 stack emulator.

        if m == "fld":
            if len(ops) >= 1:
                if ops[0].type == "reg" and ops[0].reg.startswith("st("):
                    idx = int(ops[0].reg[3:-1])
                    src = (f"g_fp_stack[g_fp_top & 7]" if idx == 0
                           else f"g_fp_stack[(g_fp_top + {idx}) & 7]")
                    return [f"{{ double _fld_tmp = {src}; fp_push(_fld_tmp); }} /* fld {insn.op_str} */"]
                if ops[0].type == "mem":
                    if ops[0].mem_size == 4:
                        return [f"fp_push(MEMF({_fmt_mem(ops[0])})); /* fld float */"]
                    elif ops[0].mem_size == 8:
                        return [f"fp_push(MEMD({_fmt_mem(ops[0])})); /* fld double */"]
                    return [f"fp_push(MEMF({_fmt_mem(ops[0])})); /* fld */"]
            return [f"/* fld {insn.op_str} */"]

        if m in ("fst", "fstp"):
            if len(ops) >= 1 and ops[0].type == "mem":
                pop_code = " fp_popp();" if m == "fstp" else ""
                if ops[0].mem_size == 4:
                    return [f"MEMF({_fmt_mem(ops[0])}) = (float)fp_top();{pop_code} /* {m} */"]
                elif ops[0].mem_size == 8:
                    return [f"MEMD({_fmt_mem(ops[0])}) = fp_top();{pop_code} /* {m} */"]
            if len(ops) >= 1 and ops[0].type == "reg" and ops[0].reg.startswith("st("):
                idx = int(ops[0].reg[3:-1])
                slot = (f"g_fp_stack[g_fp_top & 7]" if idx == 0
                        else f"g_fp_stack[(g_fp_top + {idx}) & 7]")
                if m == "fstp":
                    if idx == 0:
                        return ["fp_popp(); /* fstp st(0) */"]
                    return [f"{slot} = fp_top(); fp_popp(); /* fstp {insn.op_str} */"]
                if idx != 0:
                    return [f"{slot} = fp_top(); /* fst {insn.op_str} */"]
            return [f"/* {m} {insn.op_str} */"]

        if m == "fild":
            if len(ops) >= 1 and ops[0].type == "mem":
                smem = _smem_accessor(ops[0].mem_size)
                return [f"fp_push((double){smem}({_fmt_mem(ops[0])})); /* fild */"]
            return [f"/* fild {insn.op_str} */"]

        if m in ("fist", "fistp"):
            if len(ops) >= 1 and ops[0].type == "mem":
                mem_acc = _mem_accessor(ops[0].mem_size)
                # The cast must match the store width. fistp with a qword
                # operand stores a 64-bit integer; truncating to int32_t
                # first would silently clamp every value that needs the
                # extra range. (Before MEM64 existed the store itself was
                # only four bytes wide, which hid this.)
                cast = {2: "int16_t", 8: "int64_t"}.get(ops[0].mem_size, "int32_t")
                return [f"{mem_acc}({_fmt_mem(ops[0])}) = ({cast})fp_top(); /* {m} */"]
            return [f"/* {m} {insn.op_str} */"]

        if m in ("fadd", "faddp", "fsub", "fsubp", "fmul", "fmulp", "fdiv", "fdivp"):
            arith = {"fadd": "+=", "faddp": "+=", "fsub": "-=", "fsubp": "-=",
                     "fmul": "*=", "fmulp": "*=", "fdiv": "/=", "fdivp": "/="}[m]
            if len(ops) >= 1 and ops[0].type == "mem":
                # Memory operand: ST0 op= [mem]. Push the operand so the
                # register pattern below computes old_ST0 op mem, then pop.
                acc = "MEMD" if ops[0].mem_size == 8 else "MEMF"
                return [f"fp_push({acc}({_fmt_mem(ops[0])})); fp_st1(){arith} fp_top(); fp_pop(); /* {m} [mem] */"]
            return [f"fp_st1(){arith} fp_top(); fp_pop(); /* {m} */"]
        if m in ("fsubr", "fdivr"):
            # ST0 = [mem] - ST0 / ST0 = [mem] / ST0: push the operand, compute
            # top = operand OP st1 (the pre-push ST0), then drop the old ST0.
            if len(ops) >= 1 and ops[0].type == "mem":
                acc = "MEMD" if ops[0].mem_size == 8 else "MEMF"
                op = "-" if m == "fsubr" else "/"
                return [f"fp_push({acc}({_fmt_mem(ops[0])})); fp_top() = fp_top() {op} fp_st1(); fp_pop(); /* {m} [mem] */"]
            # Not a memory operand: fall through to the stack-register forms
            # below, which handle the DC/DE encodings. Returning a comment
            # here is what left them unlifted.
        if m == "fchs":
            return [f"fp_top() = -fp_top(); /* fchs */"]
        if m == "fabs":
            return [f"fp_top() = fabs(fp_top()); /* fabs */"]
        if m == "fsqrt":
            return [f"fp_top() = sqrt(fp_top()); /* fsqrt */"]
        if m == "fxch":
            return [f"{{ double _t = fp_top(); fp_top() = fp_st1(); fp_st1() = _t; }} /* fxch */"]
        if m in ("fcom", "fcomp", "fcompp", "fucom", "fucomp", "fucompp"):
            # Set _fpu_cmp for the fcomp/fnstsw/sahf pattern
            if len(ops) >= 1 and ops[0].type == "mem":
                acc = "MEMD" if ops[0].mem_size == 8 else "MEMF"
                rhs = f"{acc}({_fmt_mem(ops[0])})"
            else:
                rhs = "fp_st1()"
            if m in ("fcomp", "fucomp"):
                pop_code = " fp_popp();"
            elif m in ("fcompp", "fucompp"):
                pop_code = " fp_popp(); fp_popp();"
            else:
                pop_code = ""
            return [f"_fpu_cmp = (fp_top() < {rhs}) ? -1 : (fp_top() > {rhs}) ? 1 : 0;"
                    f"{pop_code} /* {m} {insn.op_str} */"]
        if m in ("fcompi", "fcomip", "fucomi", "fucompi", "fucomip", "fcomi"):
            # These set EFLAGS directly (CF, ZF, PF) from FPU comparison
            # fcompi/fucompi pop st(0) after comparing; fcomi/fucomi do not
            pops = m.endswith("pi") or m.endswith("ip")
            pop_code = " fp_pop();" if pops else ""
            return [f"_fpu_cmp = (fp_top() < fp_st1()) ? -1 : (fp_top() > fp_st1()) ? 1 : 0;"
                    f"{pop_code} /* {m} */"]
        if m == "fnstsw":
            return [f"/* fnstsw {insn.op_str} - store FPU status word */"]
        if m == "fnstcw":
            return [f"/* fnstcw {insn.op_str} - store FPU control word */"]
        if m == "fldcw":
            return [f"/* fldcw {insn.op_str} - load FPU control word */"]
        if m == "fldz":
            return [f"fp_push(0.0); /* fldz */"]
        if m == "fld1":
            return [f"fp_push(1.0); /* fld1 */"]

        # ── Transcendentals ──
        # These had no emitter, so every one of them left ST0 untouched and
        # the caller read whatever was already there. fsin/fcos alone account
        # for 294 sites in .text; angle maths cannot work without them.
        if m in ("fsin", "fcos", "fsqrt", "frndint", "f2xm1"):
            fn = {"fsin": "sin(fp_top())", "fcos": "cos(fp_top())",
                  "fsqrt": "sqrt(fp_top())", "frndint": "rint(fp_top())",
                  "f2xm1": "pow(2.0, fp_top()) - 1.0"}[m]
            return [f"fp_top() = {fn}; /* {m} */"]
        if m == "fptan":
            # Computes the tangent, then pushes the 1.0 the ratio needs.
            return ["fp_top() = tan(fp_top()); fp_push(1.0); /* fptan */"]
        if m == "fsincos":
            return ["{ double _a = fp_top(); fp_top() = sin(_a);"
                    " fp_push(cos(_a)); } /* fsincos */"]
        if m == "fpatan":
            # ST1 = atan2(ST1, ST0), then pop, so the result ends up in ST0.
            return ["fp_st1() = atan2(fp_st1(), fp_top()); fp_pop();"
                    " /* fpatan */"]
        if m in ("fyl2x", "fyl2xp1"):
            arg = "fp_top()" if m == "fyl2x" else "(fp_top() + 1.0)"
            return [f"fp_st1() = fp_st1() * log2({arg}); fp_pop(); /* {m} */"]
        if m == "fscale":
            return ["fp_top() = fp_top() * pow(2.0, trunc(fp_st1()));"
                    " /* fscale */"]

        # ── Integer-operand arithmetic ──
        # fiadd and friends take a 16- or 32-bit integer from memory. They had
        # no emitter either, so the operand was simply never applied.
        if m in ("fiadd", "fisub", "fisubr", "fimul", "fidiv", "fidivr"):
            if len(ops) >= 1 and ops[0].type == "mem":
                if ops[0].mem_size == 2:
                    val = f"(double)(int16_t)MEM16({_fmt_mem(ops[0])})"
                else:
                    val = f"(double)(int32_t)MEM32({_fmt_mem(ops[0])})"
                if m in ("fisubr", "fidivr"):
                    # Reverse forms: the memory operand is the left side.
                    op = "-" if m == "fisubr" else "/"
                    return [f"fp_top() = {val} {op} fp_top(); /* {m} */"]
                op = {"fiadd": "+", "fisub": "-",
                      "fimul": "*", "fidiv": "/"}[m]
                return [f"fp_top() = fp_top() {op} {val}; /* {m} */"]
            return [f"/* {m} {insn.op_str} - operand is not memory */"]

        # ── Truncating store and pop ──
        if m == "fisttp":
            if len(ops) >= 1 and ops[0].type == "mem":
                acc = _mem_accessor(ops[0].mem_size)
                cast = {2: "int16_t", 8: "int64_t"}.get(ops[0].mem_size,
                                                        "int32_t")
                return [f"{acc}({_fmt_mem(ops[0])}) = ({cast})trunc(fp_top());"
                        f" fp_pop(); /* fisttp */"]
            return [f"/* fisttp {insn.op_str} - operand is not memory */"]

        # ── Reverse subtract/divide against a stack register ──
        # The DC and DE encodings put the destination at ST(i), not ST(0), and
        # reverse the operand order relative to the D8 forms. Getting this
        # backwards is silent, so the direction is spelled out: the memory
        # forms above already handle "ST0 = mem OP ST0".
        if m in ("fsubr", "fsubrp", "fdivr", "fdivrp"):
            if len(ops) >= 1 and ops[0].type == "reg" and ops[0].reg and                     ops[0].reg.startswith("st("):
                idx = int(ops[0].reg[3:-1])
                dst = ("fp_top()" if idx == 0 else
                       "fp_st1()" if idx == 1 else
                       f"g_fp_stack[(g_fp_top + {idx}) & 7]")
                op = "-" if m.startswith("fsub") else "/"
                pop = " fp_pop();" if m.endswith("p") else ""
                return [f"{dst} = fp_top() {op} {dst};{pop} /* {m} */"]
            return [f"/* FPU: {m} {insn.op_str} */"]

        return [f"/* FPU: {m} {insn.op_str} */"]


# Flag setters whose condition expressions read their operand values. A
# jcc/setcc/cmovcc consumer must evaluate those values as they were when the
# flags were set, so lift_basic_block snapshots them into temporaries at the
# setter. FPU compares and rep cmps/scas are excluded: their conditions read
# _fpu_cmp/_cmps_zf/_cf, not the operands.
_SNAPSHOT_SETTERS = (FLAG_SETTERS | _EFLAGS_SETTERS) - {
    "comiss", "comisd", "ucomiss", "ucomisd",
}


def _snapshot_flag_operands(stmts, insn, snap_counter):
    """Capture a flag setter's operand values into fresh temporaries.

    The C statements must be emitted immediately after the setter's own
    statements so dest-operand setters (sub/add/inc/dec/neg) capture the
    result. Returns snapshot_ops, Operand objects formatting to the
    temporaries. snap_counter is a one-element list (function-wide counter)
    so temp names stay unique within the generated function.
    """
    snapshot_ops = []
    ops = _normalize_cmp_operands(insn.operands[:2])
    for k, op in enumerate(ops):
        if op.type not in ("reg", "imm", "mem"):
            continue
        name = f"_fcmp_{snap_counter[0]}_{'a' if k == 0 else 'b'}"
        stmts.append(
            f"uint32_t {name} = {_fmt_operand_read(op)};"
            " /* snapshot flags operand */")
        snapshot_ops.append(Operand(type="reg", reg=name, size=_flag_operand_size(op)))
        snap_counter[0] += 1
    return snapshot_ops


def lift_basic_block(lifter, bb, flag_state=None, snap_counter=None,
                     fpu_cmp_available=None):
    """
    Lift a basic block to C statements.
    Tracks flags to generate proper conditions for jcc/setcc/cmovcc.

    Args:
        lifter: Lifter instance
        bb: BasicBlock with instructions
        flag_state: tuple of (flag_setter_mnemonic, flag_operands) from a
                    preceding block, or None. flag_operands may be snapshot
                    temporaries captured when flags were set.
        snap_counter: one-element list shared across all blocks of one
                    generated function (uniquifies snapshot temp names).
        fpu_cmp_available: whether the generated function declares/sets
                    _fpu_cmp (an in-function fcomp/fcomi). If None, detected
                    from this block's own instructions. Fragments that start
                    mid-comparison (fnstsw ax without a local fcomp) consume
                    the caller's x87 status, which the recompiler does not
                    model; those keep the old generic test handling.

    Returns:
        (stmts, flag_state) where stmts is a list of C statement strings
        and flag_state is a tuple for passing to the next block.
    """
    stmts = []
    insns = bb.instructions
    i = 0

    if (lifter.func_start == 0x00344410 and insns and
            insns[0].address == lifter.func_start):
        stmts.append("recomp_trace_ring_source((uint32_t)edi);")
    if lifter.func_start == 0x000F388B and insns and insns[0].address == lifter.func_start:
        stmts.append("recomp_trace_0f388b_call((uint32_t)eax, (uint32_t)ecx, (uint32_t)edx, (uint32_t)ebx, (uint32_t)esi, (uint32_t)edi, (uint32_t)esp);")
    if lifter.func_start in (0x00084709, 0x000860AA) and insns and insns[0].address == lifter.func_start:
        stmts.append("recomp_trace_cleanup_entry((uint32_t)esp, MEM32(esp), MEM32(esp + 4u), MEM32(esp + 8u), MEM32(esp + 0xCu));")

    # Track the last instruction that set flags
    if snap_counter is None:
        snap_counter = [0]
    if flag_state:
        last_flag_setter, last_flag_ops = flag_state
    else:
        last_flag_setter = None
        last_flag_ops = []
    # Set when the last fnstsw ax was followed only by instructions that keep
    # AH intact, so `test ah, imm` branches on the FPU status, not a stale C
    # register. Block-local: fnstsw -> test -> jcc is always one block in the
    # MSVC FPU-comparison idioms; a cross-block split just falls back to the
    # old (unproven) generic test handling.
    ah_is_fpu = False
    if fpu_cmp_available is None:
        fpu_cmp_available = any(
            insn.mnemonic in ("fcom", "fcomp", "fcompp", "fucom", "fucomp",
                              "fucompp", "fcomi", "fcompi", "fcomip",
                              "fucomi", "fucompi", "fucomip")
            for insn in insns)

    while i < len(insns):
        curr = insns[i]

        # Try cmp/test + jcc pattern first (2-instruction match)
        fpu_test = (ah_is_fpu and fpu_cmp_available
                    and _is_fpu_status_test(curr))
        match = (None if fpu_test
                 else try_match_cmp_jcc(insns, i, lifter=lifter))
        if match:
            stmt, consumed = match
            # Snapshot BEFORE the conditional goto. cmp/test only reads its
            # operands, so the snapshot values are identical on the taken and
            # fall-through paths; but the taken-path successors inherit this
            # flag_state too, and emitting the snapshot after the goto leaves
            # those temporaries uninitialized when the branch is taken, so a
            # later jcc/setcc/cmovcc in a successor evaluates garbage flags.
            flag_insn = insns[i]
            last_flag_ops = _snapshot_flag_operands(
                stmts, flag_insn, snap_counter)
            if (lifter.func_start == 0x001BCBC0
                    and insns[i + 1].address in (0x001BCCA5, 0x001BCE20)):
                stmts.append(
                    "recomp_trace_bcbcc0_loop((uint32_t)ZX16(MEM16(ebp + 0x2A)), "
                    "(uint32_t)MEM32(ebp + 0x68), "
                    "CMP_B(MEM32(ebp + 0x68), ZX16(MEM16(ebp + 0x2A))));")
            if (lifter.func_start == 0x001BE953
                    and curr.address == 0x001BF13C):
                stmts.append(
                    "recomp_trace_be953_predicate(0x001BF13C, "
                    "MEM32(ebp + 0x74), esi, MEM32(ebp + 0x28), "
                    "CMP_NE(MEM32(ebp + 0x74), esi));")
            stmts.append(stmt)
            last_flag_setter = flag_insn.mnemonic
            i += consumed
            continue

        # Handle jecxz/jcxz specially (not flag-based)
        if curr.mnemonic in ("jecxz", "jcxz"):
            results = lifter._lift_jcc(curr)
            stmts.extend(results)
            i += 1
            continue

        # Check if this instruction uses flags (jcc, setcc, cmovcc)
        if curr.is_cond_jump and last_flag_setter:
            result = _make_condition(
                curr.mnemonic, last_flag_setter, last_flag_ops)
            if result:
                cond_expr, desc = result
                target = curr.jump_target
                stmt = _emit_cond_goto(
                    cond_expr, curr.mnemonic, desc, target, lifter)
                stmts.append(stmt)
                i += 1
                continue

        if (curr.mnemonic in ("sete", "setne", "setb", "setae", "setbe",
                              "seta", "setl", "setge", "setle", "setg",
                              "sets", "setns")
                and last_flag_setter and len(curr.operands) >= 1):
            cond = _make_setcc_value(
                curr.mnemonic, last_flag_setter, last_flag_ops)
            if cond:
                stmts.append(
                    _fmt_operand_write(curr.operands[0],
                                       f"({cond}) ? 1 : 0")
                    + f" /* {curr.mnemonic} */")
                i += 1
                continue

        if (curr.mnemonic in ("cmove", "cmovne", "cmovb", "cmovae",
                              "cmovbe", "cmova", "cmovl", "cmovge",
                              "cmovle", "cmovg", "cmovs", "cmovns")
                and last_flag_setter and len(curr.operands) >= 2):
            cond = _make_cmovcc_cond(
                curr.mnemonic, last_flag_setter, last_flag_ops)
            if cond:
                src = _fmt_operand_read(curr.operands[1])
                stmts.append(
                    f"if ({cond}) "
                    + _fmt_operand_write(curr.operands[0], src)
                    + f" /* {curr.mnemonic} */")
                i += 1
                continue

        # Xbox DbgPrint idiom: `int 0x2d; int3` is one debugger command.
        # The kernel service consumes both and returns normally. Translating
        # the trailing int3 to __debugbreak() traps on a valid debug print.
        if (curr.mnemonic == "int3" and i > 0 and
                insns[i - 1].mnemonic == "int" and
                getattr(insns[i - 1], "op_str", "") == "0x2d"):
            stmts.append("/* int3 after int 0x2d: consumed by debug service */")
            i += 1
            continue

        # Lift the instruction normally
        results = lifter.lift_instruction(insns[i])
        stmts.extend(results)
        # Track flag-setting instructions
        if fpu_test:
            # test ah, imm on the x87 status word: no operand snapshot (the
            # C register AH is stale; _fpu_cmp holds the compare result).
            last_flag_setter = "fpu_test"
            last_flag_ops = [curr.operands[1]]
        elif _base_mnemonic(curr.mnemonic) in FLAG_SETTERS:
            if curr.mnemonic in _SNAPSHOT_SETTERS:
                last_flag_ops = _snapshot_flag_operands(
                    stmts, curr, snap_counter)
            else:
                last_flag_ops = list(curr.operands)
            last_flag_setter = _base_mnemonic(curr.mnemonic)
        elif _base_mnemonic(curr.mnemonic) in _FLAGS_UNDEFINED:
            # Flags are undefined after these - clear tracking
            last_flag_setter = None
            last_flag_ops = []
        elif _base_mnemonic(curr.mnemonic) in _EFLAGS_SETTERS:
            # Additional flag-setting instructions
            base = _base_mnemonic(curr.mnemonic)
            if base in ("cmpxchg", "xadd"):
                # These publish _cmpx_zf themselves, so the condition never
                # reads the operands; snapshotting them would emit dead reads
                # of guest memory the emitter has just overwritten. Keep the
                # operand list, which _condition_for requires to be non-empty.
                last_flag_ops = list(curr.operands)
            else:
                last_flag_ops = _snapshot_flag_operands(
                    stmts, curr, snap_counter)
            last_flag_setter = base
        elif _base_mnemonic(curr.mnemonic) in _EFLAGS_PRESERVE:
            pass  # These don't affect EFLAGS
        elif curr.mnemonic in ("fcompi", "fcomip", "fucomi", "fucompi",
                                "fucomip", "fcomi"):
            # FPU compare-to-EFLAGS: sets CF, ZF, PF directly
            last_flag_setter = curr.mnemonic
            last_flag_ops = list(curr.operands)
        elif curr.mnemonic == "sahf":
            # sahf loads AH into flags - typically after fnstsw ax
            # in the fcomp/fnstsw/sahf pattern for FPU comparisons
            last_flag_setter = "sahf"
            last_flag_ops = list(curr.operands)
        elif curr.mnemonic.startswith("f") or curr.mnemonic.startswith("cmov"):
            pass  # FPU and already-handled CMOVcc
        elif curr.mnemonic.startswith("j"):
            pass  # Jumps don't set flags
        elif curr.mnemonic.startswith("set"):
            pass  # SETcc doesn't set flags
        elif curr.mnemonic.startswith("rep"):
            # rep movsb/movsd = data copy, preserves flags
            # repe cmpsb/repne scasb = comparison, sets flags
            rest = curr.op_str.strip() if hasattr(curr, 'op_str') else ""
            raw_m = curr.mnemonic
            if "cmps" in raw_m or "scas" in raw_m:
                last_flag_setter = raw_m
                last_flag_ops = list(curr.operands)
            elif "cmps" in rest or "scas" in rest:
                last_flag_setter = raw_m
                last_flag_ops = list(curr.operands)
            else:
                pass  # rep movs/stos = data movement, flags preserved
        elif "cmps" in curr.mnemonic or "scas" in curr.mnemonic:
            # Bare (unprefixed) cmpsb/scasb and friends. Without this they
            # reached the clear-state arm below and the following jcc was
            # emitted as a standalone jump against a stale flag variable.
            last_flag_setter = curr.mnemonic
            last_flag_ops = list(curr.operands)
        else:
            # Unknown instruction - conservatively clear flag state
            last_flag_setter = None
            last_flag_ops = []

        # Track whether AH still holds the x87 status word from fnstsw ax.
        if curr.mnemonic == "fnstsw" and len(curr.operands) >= 1 and \
                curr.operands[0].type == "reg" and curr.operands[0].reg == "ax":
            ah_is_fpu = True
        elif curr.mnemonic == "call":
            ah_is_fpu = False
        elif _writes_eax(curr):
            ah_is_fpu = False

        i += 1

    out_flag_state = (last_flag_setter, last_flag_ops) if last_flag_setter else None
    return stmts, out_flag_state
