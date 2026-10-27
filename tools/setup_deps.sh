#!/bin/bash
# Builds CPython 2.7.8 into ~/pyston_deps/python2.7.  The tests compare Pyston's
# behavior with it.  Nothing is done if that directory already exists.

set -euo pipefail

SRC_DIR=$(cd "$(dirname "$0")/.." && pwd)
DEPS_DIR=${DEPS_DIR:-$HOME/pyston_deps}

CPYTHON_VERSION=2.7.8

mkdir -p "$DEPS_DIR"
TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

setup_reference_python() {
    local python_dir=$DEPS_DIR/python2.7
    if [ -d "$python_dir" ]; then
        echo "$python_dir already exists, skipping"
        return
    fi

    echo "Building CPython $CPYTHON_VERSION"
    curl -fsSL "https://www.python.org/ftp/python/$CPYTHON_VERSION/Python-$CPYTHON_VERSION.tgz" | tar xz -C "$TMP_DIR"
    local src=$TMP_DIR/Python-$CPYTHON_VERSION
    local patch
    for patch in "$SRC_DIR"/reference_python_patches/*.patch; do
        patch -d "$src" -p1 -s < "$patch"
    done
    # CPython 2.7 predates C23, where bool, true and false became keywords.
    (cd "$src" && ./configure --prefix="$python_dir" CFLAGS="-std=gnu11" > configure.log &&
        make -j"$(nproc)" > make.log && make install > install.log)
}

setup_reference_python
echo "Done.  Dependencies are in $DEPS_DIR"
