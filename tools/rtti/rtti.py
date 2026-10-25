"""
tools/rtti/rtti.py

Recover C++ classes, vtables and virtual methods from an XBE's MSVC RTTI.

The supported record patterns connect decorated class names, locators,
hierarchies and method tables. Recovered addresses are candidate function starts,
not proof of boundaries or ABI. See docs/technical/rtti-recovery.md.

Images without supported records may yield empty results. That does not prove
that every compiler/layout variant has been recognized.

Structures (32-bit MSVC; all fields are plain VAs, with none of the
image-relative indirection x64 RTTI uses):

    TypeDescriptor            { void *vfptr; void *spare; char name[]; }
    CompleteObjectLocator     { u32 sig; u32 offset; u32 cdOffset;
                                TypeDescriptor *pTD; ClassHierarchyDescriptor *pCD; }
    ClassHierarchyDescriptor  { u32 sig; u32 attributes; u32 numBaseClasses;
                                BaseClassDescriptor **pBaseClassArray; }
    BaseClassDescriptor       { TypeDescriptor *pTD; u32 numContainedBases;
                                PMD where; u32 attributes; }
"""

import re
import struct

from tools.abi_analysis.xbe_min import XbeFile

TD_NAME = re.compile(rb"\.\?A[VU][A-Za-z0-9_@?$]{2,250}@@\x00")

# Sections whose contents are executable, so a vtable slot pointing into one is
# plausibly a method. Honor executable flags too; named SDK sections are
# retained for images that omit their executable flag.
CODE_SECTIONS = (".text", "D3D", "D3DX", "XGRPH", "DSOUND", "XPP", "XONLINE",
                 "XNET", "WMVDEC", "XACTENG")
# Sections that can hold vtables and RTTI records.
DATA_SECTIONS = (".rdata", ".data")


def demangle(name):
    """'.?AVWidget@@' -> 'Widget'. Template arguments stay decorated."""
    if name.startswith((".?AV", ".?AU")) and name.endswith("@@"):
        name = name[4:-2]
    if name.startswith("?$"):
        name = name[2:]
    return name


class Image:
    """Raw-bounded view over an XBE. Only file-backed bytes are addressable:
    a section's virtual_size runs past raw_size for BSS, and reading there
    would walk off the end of the buffer."""

    def __init__(self, path):
        xbe = XbeFile(path)
        self.d = xbe.data
        self.secs, self.code = [], []
        for section in xbe.sections:
            size = min(section.raw_size, max(0, len(self.d) - section.raw_address))
            va = section.virtual_address
            self.secs.append((va, section.raw_address, size, section.name))
            if section.flags & 0x4 or section.name in CODE_SECTIONS:
                self.code.append((va, va + size))

    def raw(self, va, size=1):
        for v, r, rs, _ in self.secs:
            if size > 0 and v <= va and va + size <= v + rs:
                return r + (va - v)
        return None

    def u32(self, va):
        r = self.raw(va, 4)
        return struct.unpack_from("<I", self.d, r)[0] if r is not None else None

    def is_code(self, va):
        return any(lo <= va < hi for lo, hi in self.code)


def type_descriptors(img):
    """{typedescriptor_va: decorated_name}. The name field is 8 bytes in."""
    out = {}
    for v, r, rs, _ in img.secs:
        for m in TD_NAME.finditer(img.d[r:r + rs]):
            out[v + m.start() - 8] = m.group()[:-1].decode("latin1")
    return out


def locators(img, td):
    """{col_va: (decorated_name, subobject_offset, class_hierarchy_desc_va)}."""
    out = {}
    for v, r, rs, _ in img.secs:
        for off in range(0, max(0, rs - 20), 4):
            sig, sub, _cd, ptd, pcd = struct.unpack_from("<5I", img.d, r + off)
            if sig != 0 or ptd not in td or img.raw(pcd) is None:
                continue
            out[v + off] = (td[ptd], sub, pcd)
    return out


def hierarchy(img, td, cols):
    """{decorated_name: [base names]}, in MSVC's depth-first preorder.

    The order is preorder, NOT most-derived-to-least: trailing entries are
    secondary inheritance branches. Do not read the last entry as the root.
    """
    out = {}
    for name, _sub, pcd in cols.values():
        if name in out:
            continue
        n, pba = img.u32(pcd + 8), img.u32(pcd + 12)
        if not n or not (0 < n < 200) or pba is None or img.raw(pba) is None:
            continue
        bases = []
        for i in range(n):
            b = img.u32(pba + i * 4)
            if b is None or img.raw(b) is None:
                break
            ptd = img.u32(b)
            if ptd not in td:
                break
            bases.append(td[ptd])
        else:
            out[name] = bases
    return out


def vtables(img, cols):
    """[(vtable_va, decorated_name, subobject_offset, [method VAs])].

    One entry per COL reference, so a class using multiple inheritance appears
    once per subobject.
    """
    out = []
    for v, r, rs, n in img.secs:
        if n not in DATA_SECTIONS:
            continue
        for off in range(0, max(0, rs - 4), 4):
            w = struct.unpack_from("<I", img.d, r + off)[0]
            if w not in cols:
                continue
            start = v + off + 4
            methods = []
            while img.is_code(img.u32(start + len(methods) * 4) or 0):
                methods.append(img.u32(start + len(methods) * 4))
            if methods:
                name, sub, _ = cols[w]
                out.append((start, name, sub, methods))
    return out


def recover(path):
    """Recover supported records from a caller image; absence is not full coverage."""
    img = Image(path)
    td = type_descriptors(img)
    cols = locators(img, td)
    vts = vtables(img, cols)

    # The primary vtable is the subobject-offset-0 one; longest wins on ties.
    primary_len, primary_va = {}, {}
    for va, name, sub, methods in vts:
        if sub == 0 and len(methods) > primary_len.get(name, 0):
            primary_len[name], primary_va[name] = len(methods), va

    return {
        "image": img,
        "type_descriptors": td,
        "locators": cols,
        "hierarchy": hierarchy(img, td, cols),
        "vtables": vts,
        "primary_len": primary_len,
        "primary_va": primary_va,
    }


def seeds(result):
    """Sorted virtual-method addresses, for tools.disasm --seed-functions.

    These are candidate starts from recovered vtables, not proven function
    boundaries. Validate them against the input and downstream analysis.
    """
    return sorted({m for _, _, _, ms in result["vtables"] for m in ms})


def owning_class(result):
    """{method_va: owner class name} for methods whose owner is well defined.

    Propose the unique candidate that is an ancestor of all holding classes,
    or omit the method when no unique candidate exists. This uses recovered
    hierarchy sets, not complete MSVC layout or declaration modelling.
    """
    ancestors = {demangle(k): {demangle(b) for b in v}
                 for k, v in result["hierarchy"].items()}
    out = {}
    for addr, classes in methods_by_class(result).items():
        if len(classes) == 1:
            out[addr] = next(iter(classes))
            continue
        owners = [c for c in classes
                  if all(c in ancestors.get(o, ()) for o in classes)]
        if len(owners) == 1:
            out[addr] = owners[0]
    return out


def names(result):
    """{"0xADDR": "Class__ADDR"} for tools/ghidra_naming/merge_names.py --apply.

    The method's own name is not recoverable -- RTTI carries class names, not
    member names -- so the address is kept to stay unique and the class is
    prepended as a heuristic label for source and diagnostic navigation.
    """
    return {f"0x{addr:08X}": f"{owner}__{addr:08X}"
            for addr, owner in owning_class(result).items()}


def methods_by_class(result):
    """{method_va: {demangled class names whose vtable holds it}}.

    A method in exactly one set is uniquely attributable. Which *ancestor*
    declared a given slot is deliberately not inferred -- with multiple
    inheritance that needs MSVC layout modelling, and the preorder base array
    does not answer it.
    """
    out = {}
    for _va, name, _sub, ms in result["vtables"]:
        for m in ms:
            out.setdefault(m, set()).add(demangle(name))
    return out
