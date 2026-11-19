#!/bin/zsh
# tools/host/verify.sh <outdir> : play fixed scripts in all four map/style modes, keep every shot
set -e
cd "${0:A:h}/../.."
./tools/host/build.sh
mkdir -p "$1"
for m in 0 1; do for st in 0 1; do for s in s0 s4 s7 s8 s10; do
  rm -f out/sim/*.ppm(N)
  KC_MAP=$m KC_STYLE=$st ./out/sim/sim tools/host/$s.txt out/sim | grep -v "^avg" > "$1/${s}_${m}${st}.txt"
  for f in out/sim/*.ppm(N); do cp $f "$1/${s}_${m}${st}_${f:t}"; done
done; done; done
