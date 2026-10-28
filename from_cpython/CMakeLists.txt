# Copy any changed stdlib files to the destination:
file(GLOB_RECURSE STDLIB_SRCS Lib/*.py)

file(GLOB_RECURSE STD_INCLUDES Include/*.h)
set(STDLIB_TARGETS "")
foreach(STDLIB_FILE ${STDLIB_SRCS})
    file(RELATIVE_PATH FN_REL ${CMAKE_SOURCE_DIR}/from_cpython/Lib ${STDLIB_FILE})
    set(TARGET ${CMAKE_BINARY_DIR}/lib/python3.8/${FN_REL})

    add_custom_command(OUTPUT ${TARGET} COMMAND
                       ${CMAKE_COMMAND} -E copy_if_different ${STDLIB_FILE} ${TARGET}
                       DEPENDS ${STDLIB_FILE}
                       COMMENT "Copying Python stdlib ${FN_REL}"
                       )
    set(STDLIB_TARGETS ${STDLIB_TARGETS} ${TARGET})

    get_filename_component(DIR ${FN_REL} DIRECTORY)
    install(FILES ${STDLIB_FILE} DESTINATION lib/python3.8/${DIR})
endforeach(STDLIB_FILE)
foreach(STDLIB_FILE ${STD_INCLUDES})
    file(RELATIVE_PATH FN_REL ${CMAKE_SOURCE_DIR}/from_cpython/Include ${STDLIB_FILE})
    set(TARGET ${CMAKE_BINARY_DIR}/include/python3.8/${FN_REL})

    add_custom_command(OUTPUT ${TARGET} COMMAND
                       ${CMAKE_COMMAND} -E copy_if_different ${STDLIB_FILE} ${TARGET}
                       DEPENDS ${STDLIB_FILE}
                       COMMENT "Copying Python header ${FN_REL}"
                       )
    set(STDLIB_TARGETS ${STDLIB_TARGETS} ${TARGET})

    get_filename_component(DIR ${FN_REL} DIRECTORY)
    install(FILES ${STDLIB_FILE} DESTINATION include/python3.8/${DIR})
endforeach(STDLIB_FILE)
add_custom_target(copy_stdlib ALL DEPENDS ${STDLIB_TARGETS})

# compile specified files in from_cpython/Modules
set(STDMODULE_SRCS
    Modules/_abc.c
    Modules/_bisectmodule.c
    Modules/_codecsmodule.c
    Modules/_collectionsmodule.c
    Modules/_functoolsmodule.c
    Modules/_heapqmodule.c
    Modules/_io/_iomodule.c
    Modules/_io/bufferedio.c
    Modules/_io/bytesio.c
    Modules/_io/fileio.c
    Modules/_io/iobase.c
    Modules/_io/stringio.c
    Modules/_io/textio.c
    Modules/_localemodule.c
    Modules/_math.c
    Modules/_operator.c
    Modules/_randommodule.c
    Modules/_sre.c
    Modules/_stat.c
    Modules/_struct.c
    Modules/_weakref.c
    Modules/atexitmodule.c
    Modules/errnomodule.c
    Modules/gcmodule.c
    Modules/itertoolsmodule.c
    Modules/mathmodule.c
    Modules/posixmodule.c
    Modules/sha512module.c
    Modules/signalmodule.c
    Modules/timemodule.c
    Modules/unicodedata.c
    )

# compile specified files in from_cpython/Objects
set(STDOBJECT_SRCS
    Objects/abstract.c
    Objects/accu.c
    Objects/bytearrayobject.c
    Objects/bytes_methods.c
    Objects/bytesobject.c
    Objects/call.c
    Objects/capsule.c
    Objects/cellobject.c
    Objects/complexobject.c
    Objects/descrobject.c
    Objects/dictobject.c
    Objects/exceptions.c
    Objects/fileobject.c
    Objects/floatobject.c
    Objects/iterobject.c
    Objects/listobject.c
    Objects/memoryobject.c
    Objects/namespaceobject.c
    Objects/obmalloc.c
    Objects/rangeobject.c
    Objects/pyston_static_types.c
    Objects/sliceobject.c
    Objects/structseq.c
    Objects/unicodectype.c
    Objects/unicodeobject.c
    Objects/weakrefobject.c
    )

# compile specified files in from_cpython/Python
set(STDPYTHON_SRCS
    Python/asdl.c
    Python/bltinmodule.c
    Python/bootstrap_hash.c
    Python/ast.c
    Python/codecs.c
    Python/dtoa.c
    Python/errors.c
    Python/fileutils.c
    Python/formatter_unicode.c
    Python/getargs.c
    Python/graminit.c
    Python/modsupport.c
    Python/mystrtoul.c
    Python/pyarena.c
    Python/pyctype.c
    Python/pyhash.c
    Python/pymath.c
    Python/pystrcmp.c
    Python/pystrhex.c
    Python/pystrtod.c
    Python/Python-ast.c
    Python/pytime.c
    Python/structmember.c
    Python/symtable.c
    Python/pyston_pystate.c
    Python/pythonrun.c
    Python/thread.c
    Python/traceback.c
    Python/_warnings.c
    )

# compile specified files in from_cpython/Parser
set(STDPARSER_SRCS
    Parser/acceler.c
    Parser/grammar1.c
    Parser/listnode.c
    Parser/myreadline.c
    Parser/node.c
    Parser/parser.c
    Parser/parsetok.c
    Parser/token.c
    Parser/tokenizer.c
    )

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-missing-field-initializers -Wno-tautological-compare -Wno-type-limits -Wno-unused-result -Wno-strict-aliasing -DPy_BUILD_CORE -DPy_BUILD_CORE_BUILTIN")

# Warnings that newer compilers added and that the CPython 2.7 sources trip
# all over the place (e.g. casting method implementations to PyCFunction).
include(CheckCCompilerFlag)
foreach(FLAG cast-function-type-mismatch cast-function-type incompatible-pointer-types-discards-qualifiers
             discarded-qualifiers deprecated-declarations single-bit-bitfield-constant-conversion
             null-pointer-subtraction misleading-indentation implicit-const-int-float-conversion)
  string(MAKE_C_IDENTIFIER "C_SUPPORTS_W${FLAG}" FLAG_VAR)
  check_c_compiler_flag("-W${FLAG}" ${FLAG_VAR})
  if(${FLAG_VAR})
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-${FLAG}")
  endif()
endforeach()

add_library(FROM_CPYTHON OBJECT ${STDMODULE_SRCS} ${STDOBJECT_SRCS} ${STDPYTHON_SRCS} ${STDPARSER_SRCS})
add_dependencies(FROM_CPYTHON copy_stdlib)

