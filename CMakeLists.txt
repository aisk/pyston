cmake_minimum_required(VERSION 3.10)
project(pyston C CXX ASM)

# Some build_deps still declare compatibility with very
# old CMake versions, which CMake 4 refuses unless a minimum policy is given.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "")

set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

include(ExternalProject)

set(DEPS_DIR $ENV{HOME}/pyston_deps)

if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Pyston does not support 32-bit systems yet")
endif()

# set build type to release by default
set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build, options are: Debug Release." FORCE)
endif()
if(NOT ${CMAKE_BUILD_TYPE} STREQUAL "Release" AND NOT ${CMAKE_BUILD_TYPE} STREQUAL "Debug")
  message(FATAL_ERROR "CMAKE_BUILD_TYPE must be set to Release or Debug")
endif()

if(UNIX)
  find_program(MAKE_PRG NAMES gmake make)
  if(MAKE_PRG)
    execute_process(
      COMMAND "${MAKE_PRG}" --version
      OUTPUT_VARIABLE MAKE_VERSION_INFO)
    if(NOT "${OUTPUT_VARIABLE}" MATCHES ".*GNU.*")
      unset(MAKE_PRG)
    endif()
  endif()
  if(NOT MAKE_PRG)
    message(FATAL_ERROR "GNU Make is required to build the dependencies.")
  else()
    message(STATUS "Found GNU Make at ${MAKE_PRG}")
  endif()
endif()

option(ENABLE_CCACHE "enable caching compiler output" ON)
option(ENABLE_GIL "threading use GIL" ON)
option(ENABLE_GOLD "enable the gold linker" ON)
option(ENABLE_GPERFTOOLS "enable the google performance tools" OFF)
option(ENABLE_GRWL "threading use GRWL" OFF)
option(ENABLE_INTEL_JIT_EVENTS "LLVM support for Intel JIT Events API" OFF)
option(ENABLE_LLVM_DEBUG "LLVM debug symbols" OFF)
option(ENABLE_OPROFILE "enable oprofile support" OFF)
option(ENABLE_SELF_HOST "use pyston to test pyston" OFF)
option(ENABLE_VALGRIND "pyston valgrind support" OFF)
if(${CMAKE_BUILD_TYPE} STREQUAL "Debug")
  option(ENABLE_REF_DEBUG "enable memory and refcounting debugging" ON)
else()
  option(ENABLE_REF_DEBUG "enable memory and refcounting debugging" OFF)
endif()

option(ENABLE_PGO "enable -fprofile-generate/-fprofile-use" OFF)
option(ENABLE_LTO "enable -flto" OFF)

# automatically use ccache if found
if(ENABLE_CCACHE AND NOT ENABLE_PGO)
  find_program(CCACHE ccache)
  if(CCACHE)
    message(STATUS "found ccache ${CCACHE}")
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "CCACHE_CPP2=yes ${CCACHE}")
    set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK ${CCACHE})
  endif()
endif()

# automatically use the gold linker if found
if(ENABLE_GOLD)
  find_program(GOLD_LINKER ld.gold)
  if(GOLD_LINKER)
    message(STATUS "found the gold linker ${GOLD_LINKER}")
    set(CMAKE_LINKER "${GOLD_LINKER}")
    set(CMAKE_CXX_LINK_FLAGS "${CMAKE_CXX_LINK_FLAGS} -B${CMAKE_SOURCE_DIR}/tools/build_system")
    if(NOT ENABLE_PGO)
      # let pgo determine the ordering
      set(CMAKE_CXX_LINK_FLAGS "${CMAKE_CXX_LINK_FLAGS} -Wl,--section-ordering-file,${CMAKE_CURRENT_SOURCE_DIR}/section_ordering.txt")
    endif()
  endif()
endif()


# pyston self host mode
if(ENABLE_SELF_HOST)
  set(PYTHON_EXE "pyston")
else()
  find_program(PYTHON_EXE python3)
endif()

# Pyston implements Python 3.8 semantics, so the tests compare its output against
# a real CPython 3.8.  Build it from the v3.8.20 tag into ${DEPS_DIR}/python3.8
# or point REFERENCE_PYTHON at an existing one.
find_program(REFERENCE_PYTHON NAMES python3.8 HINTS ${DEPS_DIR}/python3.8/bin NO_DEFAULT_PATH
             DOC "CPython 3.8 interpreter that the tests compare against")
if(NOT REFERENCE_PYTHON)
  message(WARNING "CPython 3.8 not found; set REFERENCE_PYTHON to run the tests")
endif()

# initial clang flags (set here so they're used when building llvm)
set(CLANG_FLAGS "-Qunused-arguments -fcolor-diagnostics" CACHE STRING "Clang specific C and CXX flags")
if("${CMAKE_C_COMPILER_ID}" STREQUAL "Clang")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CLANG_FLAGS}")
endif()
if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CLANG_FLAGS}")
endif()

if(ENABLE_LTO)
  set(LTO_FLAGS "-flto")
endif()

if(ENABLE_PGO)
  if(NOT "${PROFILE_DIR}" STREQUAL "")
    set(PROFILE_STATE "${PROFILE_STATE}=${PROFILE_DIR}")
  endif()
  set(PGO_FLAGS "-fprofile-${PROFILE_STATE} -fprofile-correction")
endif()

if(ENABLE_REF_DEBUG)
 add_definitions(-DPy_REF_DEBUG)
 add_definitions(-DPYMALLOC_DEBUG)
 add_definitions(-DPy_TRACE_REFS)
endif()

macro(ADD_PROFILE_FLAGS)
  set(PROFILE_FLAGS "${PGO_FLAGS} ${LTO_FLAGS}")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${PROFILE_FLAGS}")
endmacro()

# used in src/runtime/inline/CMakeLists.txt to remove the flags we added above
macro(REMOVE_PROFILE_FLAGS)
  string(REPLACE "${PROFILE_FLAGS}" " " CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
  string(REPLACE "${PROFILE_FLAGS}" " " CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  string(REPLACE "${PROFILE_FLAGS}" " " CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
  string(REPLACE "${PROFILE_FLAGS}" " " CMAKE_CXX_LINK_FLAGS "${CMAKE_CXX_LINK_FLAGS}")
  string(REPLACE "${PROFILE_FLAGS}" " " CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
endmacro()

# llvm disable debug info unless ENABLE_LLVM_DEBUG is ON
if(${CMAKE_BUILD_TYPE} STREQUAL "Debug" AND NOT ENABLE_LLVM_DEBUG)
  set(CMAKE_CXX_FLAGS_DEBUG "-O3" CACHE STRING "" FORCE)
endif()

add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/gitmodules
                   COMMAND git submodule update --init build_deps
                   COMMAND cmake -E touch ${CMAKE_BINARY_DIR}/gitmodules
                   WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                   DEPENDS ${CMAKE_SOURCE_DIR}/.gitmodules)
add_custom_target(gitsubmodules DEPENDS ${CMAKE_BINARY_DIR}/gitmodules)

# jemalloc
ExternalProject_Add(libjemalloc
                    BUILD_BYPRODUCTS ${CMAKE_BINARY_DIR}/jemalloc/lib/libjemalloc.a
                    PREFIX jemalloc-build
                    SOURCE_DIR ${CMAKE_SOURCE_DIR}/build_deps/jemalloc
                    DEPENDS gitsubmodules
                    UPDATE_COMMAND autoconf
                    CONFIGURE_COMMAND ${CMAKE_SOURCE_DIR}/build_deps/jemalloc/configure --prefix=${CMAKE_BINARY_DIR}/jemalloc --enable-autogen --enable-prof --enable-prof-libunwind
                    BUILD_COMMAND ${MAKE_PRG}
                    INSTALL_COMMAND ${MAKE_PRG} install_bin install_lib
                    LOG_UPDATE ON
                    LOG_CONFIGURE ON
                    LOG_BUILD ON
                    LOG_INSTALL ON)

#
# CMake (<3.3) does not support BUILD_BYPRODUCTS.
# This is a problem for ninja, which will not know how to build the generated file.
# Here are a couple hacks to get around it:
#
# Add a copy step.  This just hides the dependency but it seems to work.
# add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/jemalloc/lib/libjemalloc_copied.a DEPENDS libjemalloc COMMAND cp "${CMAKE_BINARY_DIR}/jemalloc/lib/libjemalloc.a" ${CMAKE_BINARY_DIR}/jemalloc/lib/libjemalloc_copied.a)
# add_custom_target(libjemalloc_copied DEPENDS ${CMAKE_BINARY_DIR}/jemalloc/lib/libjemalloc_copied.a)
#
# Hack option #2: the existence of the custom target tells ninja that libjemalloc.a will get built somehow.
# The name of the target doesn't matter.
add_custom_target(libjemalloc_byproducts DEPENDS ${CMAKE_BINARY_DIR}/jemalloc/lib/libjemalloc.a)
#
# Hack option #3: delete the .so's and use `-ljemalloc` on the link line so that ninja doesn't know about the dependency.
# ExternalProject_Add_Step(libjemalloc disable_shared
# DEPENDEES install
# COMMAND sh -c "rm -v ${CMAKE_BINARY_DIR}/jemalloc/lib/*.so*"
# )

# libunwind patches
set(LIBUNWIND_PATCHES
  ${CMAKE_SOURCE_DIR}/libunwind_patches/0001-pyston-add-lots-of-comments.patch
  ${CMAKE_SOURCE_DIR}/libunwind_patches/0002-pyston-stop-x86_64-setcontext-restoring-uninitialize.patch
  ${CMAKE_SOURCE_DIR}/libunwind_patches/0003-use-a-sorted-array-for-registered-objects-and-do-a-b.patch
  ${CMAKE_SOURCE_DIR}/libunwind_patches/0004-disable-libunwind-doc-generation.patch
  ${CMAKE_SOURCE_DIR}/libunwind_patches/9999-is-patched-marker.patch
)

add_custom_command(OUTPUT ${CMAKE_SOURCE_DIR}/build_deps/libunwind/pyston_patched
                   COMMAND git submodule update build_deps/libunwind
                   COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/git_am_automated.py build_deps/libunwind ${LIBUNWIND_PATCHES}
                   WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                   DEPENDS ${LIBUNWIND_PATCHES}
                   DEPENDS gitsubmodules)
add_custom_target(libunwind_patched DEPENDS ${CMAKE_SOURCE_DIR}/build_deps/libunwind/pyston_patched)

# llvm
# The runtime bitcode (src/runtime/inline) has to be produced by a clang that
# matches the LLVM we link against, so look for both in the same installation.
set(LLVM_REQUIRED_VERSION 21)
find_package(LLVM REQUIRED CONFIG HINTS /usr/lib/llvm-${LLVM_REQUIRED_VERSION}/lib/cmake/llvm)
if(NOT LLVM_VERSION_MAJOR EQUAL LLVM_REQUIRED_VERSION)
  message(FATAL_ERROR "LLVM ${LLVM_REQUIRED_VERSION} is required, found ${LLVM_PACKAGE_VERSION}")
endif()
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")
if(ENABLE_INTEL_JIT_EVENTS)
  set(INTEL_JIT_EVENTS_LIB "inteljitevents")
  add_definitions(-DENABLE_INTEL_JIT_EVENTS=1)
endif()
if(LLVM_LINK_LLVM_DYLIB)
  set(LLVM_LIBS LLVM)
else()
  llvm_map_components_to_libnames(LLVM_LIBS core mcjit native bitreader bitwriter ipo irreader debuginfodwarf instrumentation passes ${INTEL_JIT_EVENTS_LIB})
endif()
find_program(LLVM_CLANGXX clang++ HINTS ${LLVM_TOOLS_BINARY_DIR} NO_DEFAULT_PATH)
find_program(LLVM_CLANG_FORMAT clang-format HINTS ${LLVM_TOOLS_BINARY_DIR} NO_DEFAULT_PATH)
if(NOT LLVM_CLANGXX)
  message(FATAL_ERROR "clang++ not found in ${LLVM_TOOLS_BINARY_DIR}")
endif()

# libunwind
# libunwind relies on tentative definitions in its headers, which stopped
# working when GCC 10 made -fno-common the default.
if("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
  set(LIBUNWIND_CFLAGS "CFLAGS=-O0 -g -fcommon")
  set(LIBUNWIND_DEBUG "--enable-debug")
  set(LIBUNWIND_DEBUG_FRAME "--enable-debug-frame")
  set(LIBUNWIND_CONSERVATIVE_CHECKS "--enable-conservative-checks")
else()
  set(LIBUNWIND_CFLAGS "CFLAGS=-O2 -g -fcommon")
  set(LIBUNWIND_CONSERVATIVE_CHECKS "--disable-conservative-checks")
endif()
ExternalProject_Add(libunwind
                    PREFIX build_deps/libunwind
                    SOURCE_DIR ${CMAKE_SOURCE_DIR}/build_deps/libunwind
                    DEPENDS libunwind_patched
                    UPDATE_COMMAND autoreconf -i
                    CONFIGURE_COMMAND ${CMAKE_SOURCE_DIR}/build_deps/libunwind/configure ${LIBUNWIND_CFLAGS} --prefix=${CMAKE_BINARY_DIR}/build_deps/libunwind --enable-shared=0 --disable-block-signals ${LIBUNWIND_CONSERVATIVE_CHECKS} ${LIBUNWIND_DEBUG} ${LIBUNWIND_DEBUG_FRAME}
                    BUILD_COMMAND ${MAKE_PRG} -j${TEST_THREADS}
                    LOG_UPDATE ON
                    LOG_CONFIGURE ON
                    LOG_BUILD ON
                    LOG_INSTALL ON)
# Tell CMake that patching libunwind means that we need to rebuild it:
ExternalProject_Add_Step(libunwind forcebuild
    DEPENDS ${CMAKE_SOURCE_DIR}/build_deps/libunwind/pyston_patched
    DEPENDERS build
)
# Tell CMake that rebuilding libunwind will touch the build files (why doesn't it know this??)
# Otherwise, if you do something that triggers a rebuild (not a fresh build) of libunwind,
# it will take another build to realize that any source files that #include libunwind.h
# need to get rebuilt.
SET_SOURCE_FILES_PROPERTIES(
    ${CMAKE_BINARY_DIR}/build_deps/libunwind/include/libunwind.h PROPERTIES OBJECT_DEPENDS ${CMAKE_SOURCE_DIR}/build_deps/libunwind/pyston_patched
)

# lz4
add_subdirectory(build_deps/lz4/cmake_unofficial EXCLUDE_FROM_ALL)
add_dependencies(lz4 gitsubmodules)

# valgrind
if(ENABLE_VALGRIND)
  find_package(Valgrind REQUIRED)
  include_directories(${VALGRIND_INCLUDE_DIR})
  message(STATUS "Including valgrind ${VALGRIND_INCLUDE_DIR}")
else()
  add_definitions(-DNVALGRIND)
endif()

if(ENABLE_GRWL)
  add_definitions(-DTHREADING_USE_GIL=0 -DTHREADING_USE_GRWL=1)
else()
  add_definitions(-DTHREADING_USE_GIL=1 -DTHREADING_USE_GRWL=0)
endif()

if(ENABLE_GPERFTOOLS)
  set(OPTIONAL_LIBRARIES ${OPTIONAL_LIBRARIES} profiler)
endif()

if(ENABLE_OPROFILE)
  set(OPTIONAL_LIBRARIES ${OPTIONAL_LIBRARIES} opagent)
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Werror -Wreturn-type -Wno-sign-compare -Wno-unused -Wno-unused-parameter -fno-omit-frame-pointer -g")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_C_FLAGS} -std=c++17 -fno-rtti -fexceptions -fvisibility-inlines-hidden -ffunction-sections -fdata-sections -Woverloaded-virtual -Wno-invalid-offsetof -Wcast-qual -Wno-sign-conversion -Wnon-virtual-dtor -Winit-self -Wmissing-include-dirs -Wstrict-overflow=5 -Wpointer-arith -Wtype-limits -Wwrite-strings -Wempty-body -Waggregate-return -Wmissing-field-initializers -Wredundant-decls -Winline -Wint-to-pointer-cast -Wlong-long -Wvla -Wno-attributes -g")

# Warnings that newer compilers added, triggered by the headers of the bundled
# LLVM and by CPython-style code (casting functions to PyCFunction, 'register'
# which C++17 removed).
include(CheckCXXCompilerFlag)
foreach(FLAG deprecated-copy deprecated-register register deprecated-declarations nontrivial-memcall
             cast-function-type-mismatch address-of-packed-member bitwise-instead-of-logical
             implicit-const-int-float-conversion range-loop-construct expansion-to-defined)
  string(MAKE_C_IDENTIFIER "CXX_SUPPORTS_W${FLAG}" FLAG_VAR)
  check_cxx_compiler_flag("-W${FLAG}" ${FLAG_VAR})
  if(${FLAG_VAR})
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-${FLAG}")
  endif()
endforeach()

set(CLANG_FLAGS "${CLANG_FLAGS} -Wimplicit-int -Wstrict-prototypes -Wold-style-definition -Wnested-externs -Wpointer-to-int-cast -Wno-mismatched-tags -Wno-extern-c-compat")
if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CLANG_FLAGS}")
endif()

if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-long-long -Wno-aggregate-return -Wno-inline -Wno-redundant-decls -Wno-strict-overflow")
  set(CMAKE_CXX_LINK_FLAGS "${CMAKE_CXX_LINK_FLAGS} -Wl,--no-as-needed") # without this, we don't seem to get jemalloc on gcc builds
endif()

# Modern toolchains link position independent executables by default, but the
# runtime bitcode is compiled as non-PIC code, and the JIT
# has only ever been tested in a non-PIE address space layout.
check_cxx_compiler_flag(-no-pie CXX_SUPPORTS_NO_PIE)
if(CXX_SUPPORTS_NO_PIE)
  set(CMAKE_CXX_LINK_FLAGS "${CMAKE_CXX_LINK_FLAGS} -no-pie")
endif()

# Our custom C++ unwinder (src/runtime/cxx_unwind.cpp) doesn't apply
# DW_CFA_GNU_args_size when jumping to a landing pad, so make sure the compiler
# never passes stack arguments with push instructions, which is what produces it.
if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
  set(NO_PUSH_ARGS_FLAGS "-mllvm -no-x86-call-frame-opt")
elseif("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
  set(NO_PUSH_ARGS_FLAGS "-maccumulate-outgoing-args")
endif()
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${NO_PUSH_ARGS_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${NO_PUSH_ARGS_FLAGS}")

add_definitions(${LLVM_DEFINITIONS})
add_definitions(-DDEFAULT_PYTHON_MAJOR_VERSION=3 -DDEFAULT_PYTHON_MINOR_VERSION=8 -DDEFAULT_PYTHON_MICRO_VERSION=20) # Python 3.8.20

include_directories(${CMAKE_BINARY_DIR}/include/python3.8)
include_directories(${CMAKE_BINARY_DIR}/include/python3.8/internal)
include_directories(${LLVM_INCLUDE_DIRS})

find_package(LibLZMA REQUIRED)
link_directories(${CMAKE_BINARY_DIR}/build_deps/libunwind/lib)
link_directories(${LLVM_LIBRARY_DIRS})

ADD_PROFILE_FLAGS()

include(sysconfigdata)

# The JIT resolves runtime symbols through dlsym, and C extensions link against
# the executable, so export all symbols (CMake < 3.4 did this by default).
set(CMAKE_ENABLE_EXPORTS ON)

add_subdirectory(from_cpython)
add_subdirectory(src)
add_subdirectory(test/unittests)
add_subdirectory(tools)

# There are supposed to be better ways [1] to add link dependencies, but none of them worked for me.
# [1] http://www.cmake.org/pipermail/cmake/2010-May/037206.html
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/linkdeps_dummy.c COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/linkdeps_dummy.c DEPENDS ${CMAKE_SOURCE_DIR}/section_ordering.txt)

add_executable(pyston $<TARGET_OBJECTS:PYSTON_MAIN_OBJECT> $<TARGET_OBJECTS:PYSTON_OBJECTS> $<TARGET_OBJECTS:FROM_CPYTHON> linkdeps_dummy.c)
add_custom_command(TARGET pyston POST_BUILD COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_BINARY_DIR}/pyston ${CMAKE_BINARY_DIR}/python DEPENDS pyston)

# Wrap the stdlib in --whole-archive to force all the symbols to be included and eventually exported
target_link_libraries(pyston -Wl,--whole-archive stdlib -Wl,--no-whole-archive pthread m readline gmp mpfr crypto liblz4 util ${LLVM_LIBS} ${LIBLZMA_LIBRARIES} ${OPTIONAL_LIBRARIES} ${CMAKE_BINARY_DIR}/jemalloc/lib/libjemalloc.a unwind)
add_dependencies(pyston libjemalloc)

# test
enable_testing()
set(TEST_THREADS 1 CACHE STRING "number of pyston test threads")
set(PYTHONIOENCODING utf-8)

add_test(NAME lint COMMAND ${PYTHON_EXE} ${CMAKE_SOURCE_DIR}/tools/lint.py WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/src)
add_test(NAME check-format COMMAND ${CMAKE_SOURCE_DIR}/tools/check_format.sh ${LLVM_CLANG_FORMAT} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/src)
add_test(NAME analysis_unittest COMMAND analysis_unittest)

# test/py38 compares the output with CPython 3.8, in each execution tier
macro(add_py38_test testname)
  add_test(NAME py38_${testname} COMMAND ${CMAKE_SOURCE_DIR}/test/py38/run.sh ${ARGN})
  set_tests_properties(py38_${testname} PROPERTIES ENVIRONMENT "PYSTON=${CMAKE_BINARY_DIR}/pyston;CPYTHON=${REFERENCE_PYTHON}")
endmacro()
add_py38_test(defaults)
add_py38_test(interpreter -Y)
add_py38_test(force_llvm -n)


# format
file(GLOB_RECURSE FORMAT_FILES ${CMAKE_SOURCE_DIR}/src/*.h ${CMAKE_SOURCE_DIR}/src/*.cpp)
add_custom_target(format ${CMAKE_SOURCE_DIR}/tools/do_format.sh ${LLVM_CLANG_FORMAT} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/src)
add_custom_target(check-format ${CMAKE_SOURCE_DIR}/tools/check_format.sh ${LLVM_CLANG_FORMAT} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/src)

# lint
add_custom_target(lint ${PYTHON_EXE} ${CMAKE_SOURCE_DIR}/tools/lint.py WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/src)

# check
add_custom_target(check-deps DEPENDS pyston copy_stdlib unittests)
add_custom_target(check-pyston COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure DEPENDS check-deps)

# {run,dbg,perf,memcheck,memleaks,cachegrind}_TESTNAME
file(GLOB RUNTARGETS ${CMAKE_SOURCE_DIR}/microbenchmarks/*.py ${CMAKE_SOURCE_DIR}/minibenchmarks/*.py)
foreach(RUNTARGET ${RUNTARGETS})
  get_filename_component(BASEFILENAME ${RUNTARGET} NAME_WE)
  add_custom_target(run_${BASEFILENAME} ./pyston -q ${RUNTARGET} DEPENDS pyston)
  add_custom_target(dbg_${BASEFILENAME} gdb --ex "set confirm off" --ex "handle SIGUSR2 pass nostop noprint" --ex run --ex "bt 20" --args ./pyston -q ${RUNTARGET} DEPENDS pyston)
  add_custom_target(perf_${BASEFILENAME} perf record -g -- ./pyston -q -p ${RUNTARGET}
                    COMMAND perf report -v -n -g flat,1000 | bash ${CMAKE_SOURCE_DIR}/tools/cumulate.sh | less -S)

  if(ENABLE_VALGRIND)
    add_custom_target(memcheck_${BASEFILENAME} valgrind --tool=memcheck --leak-check=no --db-attach=yes ./pyston ${RUNTARGET} DEPENDS pyston)
    add_custom_target(memleaks_${BASEFILENAME} valgrind --tool=memcheck --leak-check=full --leak-resolution=low --show-reachable=yes ./pyston ${RUNTARGET} DEPENDS pyston)
    add_custom_target(cachegrind_${BASEFILENAME} valgrind --tool=cachegrind ./pyston ${RUNTARGET} DEPENDS pyston)
  endif()
endforeach()

# doxygen
find_package(Doxygen)
if(DOXYGEN_FOUND)
  configure_file(${CMAKE_SOURCE_DIR}/docs/Doxyfile.in ${CMAKE_BINARY_DIR}/Doxyfile @ONLY)
  add_custom_target(docs ${DOXYGEN_EXECUTABLE} ${CMAKE_BINARY_DIR}/Doxyfile WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
else()
  add_custom_target(docs COMMAND ${CMAKE_COMMAND} -E echo "Can't create docs, doxygen not installed \(try sudo apt-get install doxygen grpahviz on Ubuntu and then rerun cmake\)" VERBATIM)
endif()

find_package(Clang CONFIG QUIET HINTS ${LLVM_INSTALL_PREFIX}/lib/cmake/clang)
if(Clang_FOUND)
  add_subdirectory(plugins/refcount_checker EXCLUDE_FROM_ALL)
endif()


# CPack config.  I think this needs to come after any other code, since it will
# look at whatever install targets have already been set up.

set(CPACK_GENERATOR "TGZ")

set(CMAKE_EXECUTABLE_FORMAT "ELF") # Otherwise cmake thinks this is a cross-compile
install(TARGETS pyston DESTINATION ".")

set(CPACK_PACKAGE_VERSION_MAJOR "0")
set(CPACK_PACKAGE_VERSION_MINOR "7")
set(CPACK_PACKAGE_VERSION_PATCH "0")

set(CPACK_SYSTEM_NAME "linux64")

set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_SOURCE_DIR}/LICENSE)
set(CPACK_PACKAGE_DESCRIPTION_FILE ${CMAKE_SOURCE_DIR}/README.md)

install(FILES LICENSE README.md DESTINATION ".")

install(FILES test/lib/virtualenv/virtualenv.py DESTINATION virtualenv)
install(FILES test/lib/virtualenv/virtualenv_support/pip-6.0.8-py2.py3-none-any.whl DESTINATION virtualenv/virtualenv_support)
install(FILES test/lib/virtualenv/virtualenv_support/setuptools-12.0.5-py2.py3-none-any.whl DESTINATION virtualenv/virtualenv_support)

include(CPack)


# last file added (need to change this if we add a file that is added via a glob):
# from_cpython/Include/genobject.h
