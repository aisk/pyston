#!/bin/bash
# Sets up the dependencies that Pyston expects to find in ~/pyston_deps:
#
#  - llvm-trunk: LLVM and clang at the revision in llvm_revision.txt, with our
#    patches applied.  Downloaded as source snapshots from the llvm-mirror
#    GitHub repositories, so a full git-svn clone isn't required.
#  - libstdcxx5: libstdc++ headers from GCC 5.  The bundled clang is too old to
#    parse a modern libstdc++, so it uses these to compile the runtime bitcode.
#  - python2.7: CPython 2.7.8, which the tests compare Pyston's behavior with.
#
# Each step is skipped if its output directory already exists.

set -euo pipefail

SRC_DIR=$(cd "$(dirname "$0")/.." && pwd)
DEPS_DIR=${DEPS_DIR:-$HOME/pyston_deps}

# Snapshots corresponding to llvm_revision.txt (svn r230300).
LLVM_REVISION=230300
LLVM_COMMIT=aecbb87ee8036ba8d2732d5c9fcc2c83a8c8b8d7
CLANG_COMMIT=89afb52800035ce6357347cf0902e2023e3c06c1

# These patches were committed upstream before r230300.
SKIP_LLVM_PATCHES="0001 0002 0003 0004 0007"

LIBSTDCXX_DEB=http://archive.ubuntu.com/ubuntu/pool/main/g/gcc-5/libstdc++-5-dev_5.4.0-6ubuntu1~16.04.12_amd64.deb

CPYTHON_VERSION=2.7.8

if [ "$(cat "$SRC_DIR/llvm_revision.txt")" != "$LLVM_REVISION" ]; then
    echo "llvm_revision.txt changed; update the commits in $0" >&2
    exit 1
fi

mkdir -p "$DEPS_DIR"
TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

git_init_and_commit() {
    git -C "$1" init -q
    git -C "$1" add -A
    git -C "$1" -c user.name=pyston -c user.email=pyston@localhost commit -q -m "$2"
}

git_am() {
    git -C "$1" -c user.name=pyston -c user.email=pyston@localhost am -q "$2"
}

setup_llvm() {
    local llvm_dir=$DEPS_DIR/llvm-trunk
    if [ -d "$llvm_dir" ]; then
        echo "$llvm_dir already exists, skipping"
        return
    fi

    echo "Downloading LLVM and clang r$LLVM_REVISION"
    mkdir -p "$TMP_DIR/llvm-trunk/tools/clang"
    curl -fsSL "https://codeload.github.com/llvm-mirror/llvm/tar.gz/$LLVM_COMMIT" |
        tar xz -C "$TMP_DIR/llvm-trunk" --strip-components=1
    curl -fsSL "https://codeload.github.com/llvm-mirror/clang/tar.gz/$CLANG_COMMIT" |
        tar xz -C "$TMP_DIR/llvm-trunk/tools/clang" --strip-components=1

    # LLVM's .gitignore excludes tools/clang, so they are separate repositories.
    git_init_and_commit "$TMP_DIR/llvm-trunk" "llvm r$LLVM_REVISION"
    git_init_and_commit "$TMP_DIR/llvm-trunk/tools/clang" "clang r$LLVM_REVISION"

    local patch
    for patch in "$SRC_DIR"/llvm_patches/*.patch; do
        case " $SKIP_LLVM_PATCHES " in
            *" $(basename "$patch" | cut -c1-4) "*) continue ;;
        esac
        echo "Applying $(basename "$patch")"
        git_am "$TMP_DIR/llvm-trunk" "$patch"
    done
    for patch in "$SRC_DIR"/clang_patches/*.patch; do
        echo "Applying $(basename "$patch")"
        git_am "$TMP_DIR/llvm-trunk/tools/clang" "$patch"
    done

    mv "$TMP_DIR/llvm-trunk" "$llvm_dir"
}

setup_libstdcxx() {
    local libstdcxx_dir=$DEPS_DIR/libstdcxx5
    if [ -d "$libstdcxx_dir" ]; then
        echo "$libstdcxx_dir already exists, skipping"
        return
    fi

    echo "Downloading libstdc++ 5 headers"
    curl -fsSL -o "$TMP_DIR/libstdcxx.deb" "$LIBSTDCXX_DEB"
    dpkg-deb -x "$TMP_DIR/libstdcxx.deb" "$TMP_DIR/libstdcxx"
    mkdir -p "$TMP_DIR/libstdcxx5"
    cp -rL "$TMP_DIR/libstdcxx/usr/include/c++/5" "$TMP_DIR/libstdcxx5/include"
    cp -rL "$TMP_DIR/libstdcxx/usr/include/x86_64-linux-gnu/c++/5" "$TMP_DIR/libstdcxx5/include-arch"
    mv "$TMP_DIR/libstdcxx5" "$libstdcxx_dir"
}

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

setup_llvm
setup_libstdcxx
setup_reference_python
echo "Done.  Dependencies are in $DEPS_DIR"
