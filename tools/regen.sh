#!/bin/sh
# Regenerate RecompiledFuncs/ from rom/baserom.us.z64 and verify it.
# Needs: python with spimdisasm (set PY=... to a venv's python), and a built
# external/N64Recomp/build/N64Recomp.exe. Run from the repo root.
set -e
PY=${PY:-python}

# 1. disassemble the two code sections (main .text, game overlay .text)
rm -rf asm/_main asm/_ovl
$PY -I -m spimdisasm singleFileDisasm rom/baserom.us.z64 asm/_main --start 0x1000 --end 0x60C50 --vram 0x80000400 >/dev/null
$PY -I -m spimdisasm singleFileDisasm rom/baserom.us.z64 asm/_ovl --start 0xB3C70 --end 0xCC5B0 --vram 0x8010AAF0 >/dev/null
mv asm/_main/*.s asm/main.s && mv asm/_ovl/*.s asm/game_ovl.s && rm -rf asm/_main asm/_ovl

# 2. symbol file
$PY -I tools/gen_syms.py asm/main.s asm/game_ovl.s syms/n64sym.txt syms/manual_names.txt syms/hyp.syms.toml

# 3. recompile
rm -rf RecompiledFuncs
external/N64Recomp/build/N64Recomp.exe hyp.recomp.toml > recomp.log 2>&1 || { tail -20 recomp.log; exit 1; }

# 4. every call target must be defined somewhere
$PY -I tools/check_link.py

# 5. generated C must compile
ls RecompiledFuncs/*.c | xargs -P 8 -I{} gcc -fsyntax-only -Iexternal/N64Recomp/include -IRecompiledFuncs \
    -include include/hyp_extra_decls.h {}
echo "regen OK"
