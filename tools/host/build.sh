#!/bin/zsh
# tools/host/build.sh : build the host simulator from main/engine into out/sim
set -e
cd "${0:A:h}/../.."
mkdir -p out/sim
cc -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -o out/sim/sim tools/host/sim.c main/engine/*.c -lm 2>&1 | grep -v "reducing alignment" || true
