#!/bin/bash
# Runs each test with CPython 3.8 and Pyston and compares the output.
# usage: run.sh [pyston args...]   e.g. run.sh -n to force the LLVM tier
PYSTON=$(realpath "${PYSTON:-$(dirname "$0")/../../build/P38Debug/pyston}")
CPYTHON=${CPYTHON:-~/pyston_deps/python3.8/bin/python3.8}
cd "$(dirname "$0")"
OUT=$(mktemp -d)
status=0
for t in *.py; do
    "$CPYTHON" "$t" > "$OUT/$t.expected" 2>&1
    "$PYSTON" "$@" "$t" > "$OUT/$t.out" 2>&1
    if diff -u "$OUT/$t.expected" "$OUT/$t.out" > "$OUT/$t.diff"; then
        echo "PASS $t"
    else
        echo "FAIL $t"
        head -20 "$OUT/$t.diff"
        status=1
    fi
done
rm -rf "$OUT"
exit $status
