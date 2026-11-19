#!/bin/zsh
# tools/host/compare.sh <baseline> : replay the scripts and compare every frame and status line byte for byte
cd "${0:A:h}/../.."
T=$(mktemp -d)
./tools/host/verify.sh "$T" >/dev/null 2>&1 || { echo "BUILD OR RUN FAILED"; exit 1; }
if diff -rq "$1" "$T" >/dev/null; then echo "IDENTICAL ($(ls $T | wc -l | tr -d ' ') files)"; else echo "DIFFERENT:"; diff -rq "$1" "$T" | head; exit 1; fi
