Pyston currently only supports installing from source; the following instructions have fairly tested as working on Ubuntu, and are extensively verified as not working on Mac.  (Please see issue [#165](https://github.com/dropbox/pyston/issues/165) for discussion on enabling OSX support, which is pretty difficult.)

The build instructions assume that you will put the Pyston source code in `~/pyston` and put the dependencies in `~/pyston_deps`.  If you want to change the dependency dir, you'll have to change the value of the the `DEPS_DIR` variable in `CMakeLists.txt`.

# CMake build system

We use a Makefile as a sort of "lite development environment"; it helps manage multiple cmake configurations and has some testing helpers.

### Prerequisites

The build is tested on Debian testing with clang 21, LLVM 21, GCC 16, CMake 4 and OpenSSL 3.  Pyston links against the system LLVM, which has to be version 21, and uses the matching clang to compile the runtime bitcode.

**Debian/Ubuntu**
```
sudo apt-get install -yq git cmake ninja-build ccache clang-21 llvm-21-dev clang-format-21 autoconf automake libtool pkg-config curl \
    libncurses-dev liblzma-dev libreadline-dev libgmp-dev libmpfr-dev libssl-dev libsqlite3-dev libbz2-dev libffi-dev zlib1g-dev
```

`libgtest-dev` is needed for the unit tests and is optional.

### Building and testing

`tools/setup_deps.sh` builds CPython 2.7.8 into `~/pyston_deps`, which the tests compare Pyston's output against (with the patches in `reference_python_patches/`).

```
git clone https://github.com/dropbox/pyston.git ~/pyston
cd ~/pyston
git submodule update --init build_deps
tools/setup_deps.sh

mkdir -p build/Release && cd build/Release
CC=clang-21 CXX=clang++-21 cmake ~/pyston -GNinja -DCMAKE_BUILD_TYPE=Release
ninja check-deps
ctest --output-on-failure
```

If LLVM 21 isn't installed under `/usr/lib/llvm-21`, point CMake at it with `-DLLVM_DIR=/path/to/lib/cmake/llvm`.  The `Makefile` wraps these steps for development, but expects `clang` and `clang++` on the `PATH`.

See the main README for more information about available make targets and options.


#### CMake options

- `-DCMAKE_BUILD_TYPE=Debug` (defaults to Release)
- `-DENABLE_LLVM_DEBUG=1` for full LLVM debug
- `-DENABLE_CCACHE=0` to disable ccache
- `-DREFERENCE_PYTHON=/path/to/python2.7` to use a different CPython 2.7 for the tests

# Optional dependencies

There are a number of optional dependencies that the build system knows about, but aren't strictly necessary for building and running Pyston.  Most of them are related to developing and debugging:

### GCC 4.8.2

Pyston (and LLVM) requires a fairly modern [host compiler](http://llvm.org/docs/GettingStarted.html#host-c-toolchain-both-compiler-and-standard-library), so you will have to compile a recent GCC if it's not already available for your system. The easiest thing to do is to just create a fresh build of GCC:

```
sudo apt-get install libgmp-dev libmpfr-dev libmpc-dev make build-essential libtool zip gcc-multilib autogen
cd ~/pyston_deps
wget http://ftpmirror.gnu.org/gcc/gcc-4.8.2/gcc-4.8.2.tar.bz2
tar xvf gcc-4.8.2.tar.bz2
mkdir gcc-4.8.2-{build,install}
cd gcc-4.8.2-build
# Space- and time-saving configuration:
../gcc-4.8.2/configure --disable-bootstrap --enable-languages=c,c++ --prefix=$HOME/pyston_deps/gcc-4.8.2-install
# full configuration:
# ../gcc-4.8.2/configure --prefix=$HOME/pyston_deps/gcc-4.8.2-install
# Specifying LIBRARY_PATH is a workaround to get gcc to compile on newer Ubuntus with multiarch
LIBRARY_PATH=/usr/lib32 make -j4
make check
make install
```


### gdb
A modern gdb is highly recommended; users on Ubuntu 12.04 should definitely update:

```
cd ~/pyston_deps
wget http://ftp.gnu.org/gnu/gdb/gdb-7.6.2.tar.gz
tar xvf gdb-7.6.2.tar.gz
cd gdb-7.6.2
./configure
make -j4
cd ~/pyston
echo "GDB := \$(DEPS_DIR)/gdb-7.6.2/gdb/gdb --data-directory \$(DEPS_DIR)/gdb-7.6.2/gdb/data-directory" >> Makefile.local
```

#### getting gdb to pretty-print STL types

GDB ships with python scripts that tell it how to pretty-print STL containers, but they only work against the stdlib in the corresponding version of GCC. Unfortunately, we compile against an old version of gcc's C++ stdlib. Here's how to get GDB to use the *old* pretty-print scripts for GCC 4.8.2. (Note: Have only tested this with gdb 7.8.1-ubuntu4 and gcc-4.8.2.)

If you've built GCC, the GDB scripts will be in `~/pyston_deps/gcc-4.8.2-install/share/gcc-4.8.2/python`. Unfortunately they're for Python 2, and GDB 7.8.1 uses Python 3. So we'll use `2to3` to update them:

```
cd ~/pyston_deps/gcc-4.8.2-install/share/gcc-4.8.2/python/libstdcxx/v6
2to3 -w printers.py
```

Don't worry, `2to3` makes a backup of the file it changes if you need to go back. Now, edit your `~/.gdbinit` file to contain the following:

```
python
import sys
sys.path.insert(0, '/YOUR/HOME/DIR/pyston_deps/gcc-4.8.2-install/share/gcc-4.8.2/python')
from libstdcxx.v6.printers import register_libstdcxx_printers
register_libstdcxx_printers(None)
print('--- Registered cxx printers for gcc 4.8.2! ---')
```

And there you go!

### gold

gold is highly recommended as a faster linker, and Pyston contains build-system support for automatically using gold if available.  gold may already be installed on your system; you can check by typing `which gold`.  If it's not installed already:

```
cd ~/pyston_deps
sudo apt-get install bison
wget http://ftp.gnu.org/gnu/binutils/binutils-2.24.tar.gz
tar xvf binutils-2.24.tar.gz
mkdir binutils-2.24-build
cd binutils-2.24-build
../binutils-2.24/configure --enable-gold --enable-plugins --disable-werror
make all-gold -j4
```

### perf
The `perf` tool is the best way we've found to profile JIT'd code; you can find more details in docs/PROFILING.

```
sudo apt-get install linux-tools
sudo apt-get install linux-tools-`uname -r`
# may need to strip off the -generic from that last one
```

# Misc tools

These are only useful in specific development situations:

### valgrind

Since Pyston uses a custom memory allocator, it makes use of the valgrind client hooks to help valgrind understand it.  This means that you'll need a copy of the valgrind includes around; I also suggest that if you want to use valgrind, you use a recent version since there are some instructions that LLVM emits that somewhat-recent versions of valgrind don't understand.

To install:

```
cd ~/pyston_deps
wget http://valgrind.org/downloads/valgrind-3.10.0.tar.bz2
tar xvf valgrind-3.10.0.tar.bz2
mkdir valgrind-3.10.0-install
cd valgrind-3.10.0
./configure --prefix=$HOME/pyston_deps/valgrind-3.10.0-install
make -j4
make install
sudo apt-get install libc6-dbg
cd ~/pyston
echo "ENABLE_VALGRIND := 1" >> Makefile.local
```

### gperftools (-lprofiler)
```
download from http://code.google.com/p/gperftools/downloads/list
standard ./configure, make, make install
```

### Debug build of CPython

Having a debug-enabled CPython can be useful for debugging issues with our extension modules.  To get it set up:

```
sudo apt-get install python3.5-dbg
cd ~/pyston_deps
mkdir python-src
cd python-src
apt-get source python3.5-dbg
```

Then, run `make dbgpy_TESTNAME` and it will launch the test under gdb.  There's also a [wiki page](https://wiki.python.org/moin/DebuggingWithGdb) with some extra Python-specific GDB commands.

### doxygen
```
sudo apt-get install doxygen graphviz

# then run cmake and invoke the docs target
ninja docs

# now within the Pyston build directory open docs/html/index.html with a browser
```
