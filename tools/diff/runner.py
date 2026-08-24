"""Small runner boundary; Unicorn is optional and never silently substituted."""
import json
from pathlib import Path

class RunnerUnavailable(RuntimeError):
    pass

def run_unicorn(case, *, trace=True):
    try:
        from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
        from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX,
            UC_X86_REG_EDX, UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP,
            UC_X86_REG_ESP, UC_X86_REG_EFLAGS, UC_X86_REG_EIP)
    except ImportError as error:
        raise RunnerUnavailable("install unicorn to run the x86 oracle") from error
    state = case.data["state"]
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    pages = {}
    for page in case.data["memory"]:
        address = int(page["address"], 0) if isinstance(page["address"], str) else page["address"]
        blob = bytes.fromhex(page["data"])
        size = (len(blob) + 0xFFF) & ~0xFFF
        uc.mem_map(address & ~0xFFF, max(size, 0x1000))
        uc.mem_write(address, blob)
        pages[address] = len(blob)
    code = bytes.fromhex(case.data["code"])
    entry = int(case.data["entry_eip"], 0) if isinstance(case.data["entry_eip"], str) else case.data["entry_eip"]
    uc.mem_map(entry & ~0xFFF, max(0x1000, (len(code) + (entry & 0xFFF) + 0xFFF) & ~0xFFF))
    uc.mem_write(entry, code)
    regs = {"eax": UC_X86_REG_EAX, "ebx": UC_X86_REG_EBX, "ecx": UC_X86_REG_ECX,
            "edx": UC_X86_REG_EDX, "esi": UC_X86_REG_ESI, "edi": UC_X86_REG_EDI,
            "ebp": UC_X86_REG_EBP, "esp": UC_X86_REG_ESP, "eflags": UC_X86_REG_EFLAGS}
    for name, reg in regs.items():
        uc.reg_write(reg, int(state[name], 0) if isinstance(state[name], str) else state[name])
    checkpoints = []
    stop = case.data["stop"]
    limit = int(stop.get("instructions", 1000000))
    stop_eip = stop.get("eip")
    if isinstance(stop_eip, str):
        stop_eip = int(stop_eip, 0)
    def hook(_, address, size, __):
        if trace:
            checkpoints.append({"eip": address, **{name: uc.reg_read(reg) for name, reg in regs.items()}})
        if stop_eip is not None and address == stop_eip:
            uc.emu_stop()
        if stop.get("ret") and 0 <= address - entry < len(code) and code[address - entry] in (0xC2, 0xC3):
            uc.emu_stop()
    from unicorn import UC_HOOK_CODE
    uc.hook_add(UC_HOOK_CODE, hook)
    try:
        uc.emu_start(entry, 0, count=limit)
    except Exception as error:
        checkpoints.append({"eip": uc.reg_read(UC_X86_REG_EIP), "exception": type(error).__name__})
    return checkpoints

def save_trace(path, trace):
    Path(path).write_text(json.dumps(trace, indent=2) + "\n", encoding="utf-8")
