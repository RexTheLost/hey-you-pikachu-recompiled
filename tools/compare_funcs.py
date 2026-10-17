"""Compare our function starts (syms/hyp.syms.toml) with an independent list
(Ghidra DumpFunctions output). Reports starts the other tool found inside text
that we lack, and vice versa.

Usage: python compare_funcs.py <hyp.syms.toml> <ghidra_funcs.txt>
"""
import re
import sys

TEXT = [(0x80000400, 0x80060050), (0x8010AAF0, 0x80123430)]
DATA_RANGES = [(0x8004AB94, 0x8004C8A0)]
in_text = lambda a: any(lo <= a < hi for lo, hi in TEXT) and not any(lo <= a < hi for lo, hi in DATA_RANGES)

ours = {int(m.group(2), 16): (m.group(1), int(m.group(3), 16)) for m in re.finditer(
    r'name = "(\w+)", vram = 0x([0-9A-F]+), size = 0x([0-9A-F]+)', open(sys.argv[1]).read())}
theirs = {}
for line in open(sys.argv[2]):
    a, size, name = line.split()
    theirs[int(a, 16)] = (name, int(size, 16))

def containing(a):
    best = max((s for s in ours if s <= a), default=None)
    return f'{ours[best][0]}+0x{a - best:X}' if best is not None else '?'

only_theirs = sorted(a for a in theirs if in_text(a) and a not in ours)
only_ours = sorted(a for a in ours if a not in theirs)
print(f'ours {len(ours)}, ghidra {len(theirs)} ({sum(map(in_text, theirs))} in text)')
print(f'ghidra-only starts in text: {len(only_theirs)}')
for a in only_theirs:
    print(f'  {a:08X} ghidra={theirs[a][0]} inside {containing(a)}')
print(f'ours-only starts: {len(only_ours)}')
