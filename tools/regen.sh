#!/bin/sh
# Regenerate RecompiledFuncs/ from rom/baserom.us.z64 and verify it.
# Needs: .venv with spimdisasm (or PY=...), and build/n64recomp/N64Recomp.exe
# (built from lib/N64ModernRuntime/N64Recomp). Run from the repo root.
set -e
PY=${PY:-.venv/Scripts/python}

# 1. code sections: main, game overlay, 23 scene overlays (from the ROM's table)
mkdir -p build asm
$PY -I tools/sections.py rom/baserom.us.z64 build/sections.json

# 2. disassemble each section's .text
$PY -I - <<'EOF'
import json, os, shutil, subprocess, sys
py = sys.executable
for s in json.load(open('build/sections.json')):
    tmp = f'asm/_{s["name"]}'
    shutil.rmtree(tmp, ignore_errors=True)
    subprocess.run([py, '-I', '-m', 'spimdisasm', 'singleFileDisasm', 'rom/baserom.us.z64', tmp,
                    '--start', hex(s['rom']), '--end', hex(s['rom'] + s['text_end'] - s['vram']),
                    '--vram', hex(s['vram'])], check=True, stdout=subprocess.DEVNULL)
    (src,) = [f for f in os.listdir(tmp) if f.endswith('.s')]
    os.replace(os.path.join(tmp, src), f'asm/{s["name"]}.s')
    shutil.rmtree(tmp)
EOF

# 3. symbol file
$PY -I tools/gen_syms.py build/sections.json rom/baserom.us.z64 syms/n64sym.txt syms/manual_names.txt syms/hyp.syms.toml

# 4. recompile
rm -rf RecompiledFuncs
build/n64recomp/N64Recomp.exe hyp.recomp.toml > recomp.log 2>&1 || { tail -20 recomp.log; exit 1; }

# 5. audio microcode (RSP)
build/n64recomp/RSPRecomp.exe rsp/n_aspMain.us.toml > /dev/null

# 6. every call target must be defined somewhere
$PY -I tools/check_link.py

# 7. generated C must compile
ls RecompiledFuncs/*.c | xargs -P 8 -I{} gcc -fsyntax-only -Ilib/N64ModernRuntime/N64Recomp/include -IRecompiledFuncs \
    -include include/hyp_extra_decls.h {}
echo "regen OK"
