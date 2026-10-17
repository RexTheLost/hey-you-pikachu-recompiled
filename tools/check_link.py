"""Check that every function the recompiled code calls has a definition:
emitted by N64Recomp, provided by N64ModernRuntime, or by our src/ glue.

Usage: python check_link.py   (run from the repo root after recompiling)
"""
import glob
import re

emitted, called = set(), set()
for f in glob.glob('RecompiledFuncs/funcs_*.c'):
    s = open(f).read()
    emitted |= set(re.findall(r'^RECOMP_FUNC void (\w+)\(', s, re.M))
    called |= set(re.findall(r'\b(\w+)\(rdram, ctx\);', s))

provided = set()
for f in glob.glob('external/N64ModernRuntime/**/*.c*', recursive=True) + glob.glob('src/**/*.c*', recursive=True):
    provided |= set(re.findall(r'\b(\w+_recomp)\s*\(\s*(?:RDRAM_ARG|uint8_t\s*\*\s*rdram)', open(f, errors='ignore').read()))

missing = sorted(c for c in called - emitted - provided - {'LOOKUP_FUNC'})
print(f'{len(emitted)} emitted, {len(called)} call targets, {len(missing)} unresolved')
for m in missing:
    print('  unresolved:', m)
raise SystemExit(1 if missing else 0)
