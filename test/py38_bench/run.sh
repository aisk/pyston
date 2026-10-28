#!/bin/bash
# Compares the benchmarks on CPython 3.8, a modern CPython and Pyston (default
# tiers and forced LLVM tier). Prints the first iteration and the steady state
# time of each, in seconds.
# usage: run.sh [bench.py...]   (WARMUP and ITERS override the iteration counts)
cd "$(dirname "$0")"
PYSTON=$(realpath "${PYSTON:-../../build/P38Release/pyston}")
CPYTHON38=${CPYTHON38:-~/pyston_deps/python3.8/bin/python3.8}
CPYTHON=${CPYTHON:-/usr/bin/python3}
WARMUP=${WARMUP:-3}
ITERS=${ITERS:-5}

names=(cpy38 cpython pyston pyston-n)
cmds=("$CPYTHON38" "$CPYTHON" "$PYSTON" "$PYSTON -n")

benches=("$@")
[ ${#benches[@]} -eq 0 ] && benches=($(ls *.py | grep -v '^harness.py$'))

printf "%-20s" bench
for n in "${names[@]}"; do printf "%22s" "$n"; done
echo
for b in "${benches[@]}"; do
    printf "%-20s" "${b%.py}"
    for c in "${cmds[@]}"; do
        if out=$($c harness.py "$b" "$WARMUP" "$ITERS" 2>&1); then
            first=$(sed -n 's/.*first=\([0-9.]*\).*/\1/p' <<< "$out")
            steady=$(sed -n 's/.*steady=\([0-9.]*\).*/\1/p' <<< "$out")
            printf "%22s" "$first/$steady"
        else
            printf "%22s" ERR
        fi
    done
    echo
done
