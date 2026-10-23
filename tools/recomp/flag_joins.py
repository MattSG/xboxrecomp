"""Materialize ZF where a je/jne reads the never-set `_flags` fallback.

The lifter keeps flags lazily: a cmp leaves its operands in _fa/_fb, an
arithmetic op leaves its result in _fa, and a test leaves both operands. When a
je/jne has no flag state in its own block it falls back to `_flags`, which
nothing assigns, so the branch is never taken. This pass runs over generated
C and rewrites those branches to test a per-function `_zf`, which each flag
producer that reaches the branch stores.

Usage: python -m tools.recomp.flag_joins <gen_dir>

A branch whose flag producers cannot all be identified is left as it was and
reported with the line that blocked it, so a silent miscompile stays visible.
"""
import re
import sys
from pathlib import Path

FLAG_NEUTRAL = re.compile(
    r'^\s*(?:$|/\*.*\*/$|\(void\)|_(?:cf|zf) = [^;]*;(?: /\*.*\*/)?$'
    r'|if \(.*\) (?:goto loc_[0-9A-F]+;|\{ .*return; \}|\(void\)0; /\*.*\*/)(?: /\*.*\*/)?$)')
FLAG_LABEL = re.compile(r'^(loc_[0-9A-F]+): ;$')
FLAG_JUMP_OUT = re.compile(r'^\s*(goto |return\b|esp \+= \d+; return)')
FLAG_FUNC = re.compile(r'^\w[^;]*\b(sub_[0-9A-F]+)\(void\)\s*$')
JCC = re.compile(r'^(\s*)if \(_flags /\* (je|jne): [^*]*\*/\) (.*)$')
FALLBACK_DECL = 'int _flags = 0; /* fallback flag var */'


def func_name(lines, i):
    """The sub_XXXX whose definition is the nearest one above line i."""
    return next((lines[k].split('(')[0].split()[-1] for k in range(i, -1, -1)
                 if FLAG_FUNC.match(lines[k])), '?')


def flag_setter_zf(text):
    """The C statement that sets _zf from a flag-producing line, or None.

    Each form matches the lifter's own je/jne condition for that setter: cmp and
    test keep their operands in _fa/_fb, and the result-based setters keep the
    result in _fa.
    """
    if '/* cmp ' in text:
        return '_zf = (_fa == _fb);'
    if '/* test ' in text:
        return '_zf = ((_fa & _fb) == 0);'
    if re.search(r'/\* (\w+ )?(sub|dec|inc|add|and|or|xor|neg) result', text):
        return '_zf = (_fa == 0);'
    return None


def fix_flag_joins(out):
    """Rewrite each je/jne that reads the `_flags` fallback to test `_zf`.

    Walks back from each such branch through statements that leave the flags
    alone (labels, conditional jumps, _cf writes) to every flag producer that
    reaches it, across labels and their gotos. When all producers are one of the
    forms in flag_setter_zf, each stores ZF in a per-function `_zf` and the branch
    tests it. Otherwise the branch is left as it was and listed.

    Returns (fixed, unfixed): the branch count rewritten, and a list of
    (file, function, label, line, reason) for every je/jne still on `_flags`.
    """
    out = Path(out)
    fixed = 0
    unfixed = []
    for path in sorted(out.glob('recomp_*.c')):
        lines = path.read_text().splitlines()
        targets = {}
        for i, line in enumerate(lines):
            for m in re.finditer(r'goto (loc_[0-9A-F]+);', line):
                targets.setdefault(m.group(1), []).append(i)
        blocked = []

        def sources(j, seen=frozenset()):
            """Lines that set the flags in effect after line j, or None when one is unknown."""
            while j >= 0 and FLAG_NEUTRAL.match(lines[j]):
                j -= 1
            if j < 0:
                blocked.append(None)  # function entry: flags are undefined
                return None
            label = FLAG_LABEL.match(lines[j])
            if label:
                if label.group(1) in seen:
                    return []
                seen = seen | {label.group(1)}
                found = []
                for g in targets.get(label.group(1), []):
                    s = sources(g - 1, seen)
                    if s is None:
                        return None
                    found += s
                prev = j - 1
                while prev >= 0 and not lines[prev].strip():
                    prev -= 1
                if prev >= 0 and not FLAG_JUMP_OUT.match(lines[prev]):
                    s = sources(prev, seen)
                    if s is None:
                        return None
                    found += s
                return found
            if flag_setter_zf(lines[j]) is None:
                blocked.append(j)
                return None
            return [j]

        inserts = {}   # setter line -> _zf statement
        rewrites = {}  # branch line -> replacement
        decls = set()
        for i, line in enumerate(lines):
            m = JCC.match(line)
            if not m:
                continue
            indent, cond, rest = m.groups()
            if '(void)0; /* goto' in rest or 'RECOMP_DEAD_BRANCH(' in rest:
                # The translator drops a goto whose label is not in the function.
                # That branch has no flag state to materialize.
                unfixed.append((path.name, func_name(lines, i), '-', i + 1,
                                'target label outside this function; translator dropped the jump'))
                continue
            func = func_name(lines, i)
            prev = next((k for k in range(i - 1, -1, -1) if lines[k].strip()), None)
            label = FLAG_LABEL.match(lines[prev]) if prev is not None else None
            blocked.clear()
            srcs = sources(i - 1)
            decl = next((k for k in range(i, -1, -1) if FALLBACK_DECL in lines[k]), None)
            if srcs is None or not srcs or decl is None:
                if srcs is not None and not srcs:
                    reason = 'no flag producer reaches it'
                elif decl is None:
                    reason = 'no _flags declaration'
                else:
                    at = blocked[-1] if blocked else None
                    reason = ('function entry' if at is None else
                              f'blocked at {path.name}:{at + 1}: {lines[at].strip()[:90]}')
                unfixed.append((path.name, func, label.group(1) if label else '-', i + 1, reason))
                continue
            for j in srcs:
                indent_j = lines[j][:len(lines[j]) - len(lines[j].lstrip())]
                inserts[j] = f'{indent_j}{flag_setter_zf(lines[j])}'
            decls.add(decl)
            rewrites[i] = f'{indent}if ({"" if cond == "je" else "!"}_zf) {rest}'
            fixed += 1
        if not rewrites:
            continue
        for d in decls:
            if '_zf' not in lines[d]:
                lines[d] += ' int _zf = 0;'
        new = []
        for k, line in enumerate(lines):
            new.append(rewrites.get(k, line))
            if k in inserts:
                new.append(inserts[k])
        path.write_text('\n'.join(new) + '\n')
    print(f'flag joins materialized: {fixed}')
    print(f'je/jne left on the _flags fallback: {len(unfixed)}')
    for name, func, label, line, reason in unfixed:
        print(f'  {name}:{line} {func} {label}: {reason}')
    return fixed, unfixed


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 1:
        print(__doc__)
        return 2
    fix_flag_joins(argv[0])
    return 0


if __name__ == '__main__':
    sys.exit(main())
