from dataclasses import dataclass

@dataclass
class Divergence:
    index: int
    reason: str
    oracle: dict
    recomp: dict

def _diff(a, b):
    for key in sorted(set(a) | set(b)):
        if a.get(key) != b.get(key):
            return key
    return None

def first_divergence(oracle, recomp):
    for index, (left, right) in enumerate(zip(oracle, recomp)):
        field = _diff(left, right)
        if field:
            return Divergence(index, field, left, right)
    if len(oracle) != len(recomp):
        index = min(len(oracle), len(recomp))
        return Divergence(index, "trace length", oracle[index:] if index < len(oracle) else {}, recomp[index:] if index < len(recomp) else {})
    return None

def format_divergence(divergence):
    if not divergence:
        return "MATCH"
    o, r = divergence.oracle, divergence.recomp
    eip = o.get("eip", r.get("eip", 0))
    return (f"FIRST DIVERGENCE\ncheckpoint: {divergence.index}\n"
            f"guest EIP: 0x{eip:08X}\nfield: {divergence.reason}\n"
            f"oracle: {o.get(divergence.reason)!r}\n"
            f"recomp: {r.get(divergence.reason)!r}")
