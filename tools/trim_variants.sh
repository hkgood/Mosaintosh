#!/bin/bash
# Build three progressively smaller System 7.5.3 disks and boot each on the
# headless Mac Plus, so the trimming is decided by what still boots.
set -e
cd "$(dirname "$0")/.."
PY=${PY:-build/venv/bin/python}
S=":System Folder"

# A: drop what a 1 bpp Plus never uses: HyperCard, laser fonts, colour-only panels.
A=(":HyperCard 2.4" "$S:Fonts:Palatino" "$S:Fonts:Times" "$S:Fonts:Courier"
   "$S:Fonts:Helvetica" "$S:Fonts:Symbol" "$S:Control Panels:Monitors & Sound"
   "$S:Control Panels:Monitors" "$S:Control Panels:Map" "$S:Extensions:Finder Help")
# B: also the 7.5.2 update (PCI Power Macs).
B=("${A[@]}" "$S:System 7.5.2 Update")
# C: also the 7.5 update (the 7.5.1/7.5.3 fixes).
C=("${B[@]}" "$S:System 7.5 Update")

mkdir -p build/trim
rm -f build/trim/*
$PY tools/trim_disk.py assets/System.img build/trim/A.img 10 "${A[@]}" | tail -1
$PY tools/trim_disk.py assets/System.img build/trim/B.img 8 "${B[@]}" | tail -1
$PY tools/trim_disk.py assets/System.img build/trim/C.img 6 "${C[@]}" | tail -1
for v in A B C; do
    echo "== $v"
    ./build/headless -r assets/MacPlus-v3.rom -d build/trim/$v.img -t 90 -s 30 -o build/trim/$v | tail -1
done
python3 tools/pbm2png.py build/trim/*_090s.pbm
