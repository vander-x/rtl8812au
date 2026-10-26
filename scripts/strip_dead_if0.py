#!/usr/bin/env python3
"""
Remove dead `#if 0 ... #endif` preprocessor blocks from C source files,
tracking nesting depth so a nested #if/#ifdef/#ifndef inside the dead
block, and its matching #endif, are correctly consumed as part of the
same dead block instead of ending it early.

Only removes blocks whose opening line is *exactly* `#if 0` (optionally
indented), so `#if 0 && SOMETHING` or similar conditional expressions
are left untouched - the goal is removing unconditionally-dead debug/
reference code, not changing build configuration.

Does NOT touch #else/#elif inside a dead #if 0 block: the whole
#if 0 ... #endif, else-branch included, is dead when the top condition
is a literal 0, so it's all removed together.
"""
import re
import sys
from pathlib import Path

IF_RE = re.compile(r'^\s*#\s*if\s*0\s*$')
IF_ANY_RE = re.compile(r'^\s*#\s*(if|ifdef|ifndef)\b')
ENDIF_RE = re.compile(r'^\s*#\s*endif\b')


def strip_file(path: Path) -> int:
    text = path.read_text(errors='replace')
    lines = text.split('\n')
    out = []
    i = 0
    removed = 0
    while i < len(lines):
        if IF_RE.match(lines[i]):
            # Found a dead block start; find its matching #endif by depth.
            depth = 1
            j = i + 1
            while j < len(lines) and depth > 0:
                if IF_ANY_RE.match(lines[j]):
                    depth += 1
                elif ENDIF_RE.match(lines[j]):
                    depth -= 1
                j += 1
            if depth == 0:
                # lines[i:j] is the whole dead block (#if 0 .. matching #endif)
                removed += (j - i)
                i = j
                continue
            # Unbalanced (shouldn't happen in valid C) - bail out, keep as-is.
        out.append(lines[i])
        i += 1
    if removed:
        path.write_text('\n'.join(out))
    return removed


def main():
    roots = sys.argv[1:] or ['.']
    total_files = 0
    total_lines = 0
    for root in roots:
        for path in Path(root).rglob('*'):
            if path.suffix not in ('.c', '.h'):
                continue
            if '/tools/' in str(path):
                continue  # leave userspace utilities alone
            n = strip_file(path)
            if n:
                total_files += 1
                total_lines += n
                print(f'{path}: -{n} lines')
    print(f'\nTotal: {total_lines} lines removed across {total_files} files')


if __name__ == '__main__':
    main()
