// This file is originally from CPython 3.8, with modifications for Pyston

#ifndef Py_PYTHON_H
#define Py_PYTHON_H

#define PYSTON_VERSION "0.7"
/* Since this is a "meta-include" file, no #ifdef __cplusplus / extern "C" { */

/* Include nearly all Python header files */

#include "patchlevel.h"
#include "pyconfig.h"
#include "pymacconfig.h"

#include <limits.h>

#ifndef UCHAR_MAX
#error "Something's broken.  UCHAR_MAX should be defined in limits.h."
#endif

#if UCHAR_MAX != 255
#error "Python's source code assumes C's unsigned char is an 8-bit type."
#endif

#if defined(__sgi) && !defined(_SGI_MP_SOURCE)
#define _SGI_MP_SOURCE
#endif

#include <stdio.h>
#ifndef NULL
#   error "Python.h requires that stdio.h define NULL."
#endif

#include <string.h>
#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#include <stdlib.h>
#ifndef MS_WINDOWS
#include <unistd.h>
#endif
#ifdef HAVE_CRYPT_H
#if defined(HAVE_CRYPT_R) && !defined(_GNU_SOURCE)
/* Required for glibc to expose the crypt_r() function prototype. */
#  define _GNU_SOURCE
#  define _Py_GNU_SOURCE_FOR_CRYPT
#endif
#include <crypt.h>
#ifdef _Py_GNU_SOURCE_FOR_CRYPT
/* Don't leak the _GNU_SOURCE define to other headers. */
#  undef _GNU_SOURCE
#  undef _Py_GNU_SOURCE_FOR_CRYPT
#endif
#endif

/* For size_t? */
#ifdef HAVE_STDDEF_H
#include <stddef.h>
#endif

/* CAUTION:  Build setups should ensure that NDEBUG is defined on the
 * compiler command line when building Python in release mode; else
 * assert() calls won't be removed.
 */
#include <assert.h>

#include "pyport.h"
#include "pymacro.h"

/* A convenient way for code to know if clang's memory sanitizer is enabled. */
#if defined(__has_feature)
#  if __has_feature(memory_sanitizer)
#    if !defined(_Py_MEMORY_SANITIZER)
#      define _Py_MEMORY_SANITIZER
#    endif
#  endif
#endif

/* Debug-mode build with pymalloc implies PYMALLOC_DEBUG.
 *  PYMALLOC_DEBUG is in error if pymalloc is not in use.
 */
#if defined(Py_DEBUG) && defined(WITH_PYMALLOC) && !defined(PYMALLOC_DEBUG)
#define PYMALLOC_DEBUG
#endif
#if defined(PYMALLOC_DEBUG) && !defined(WITH_PYMALLOC)
#error "PYMALLOC_DEBUG requires WITH_PYMALLOC"
#endif
#include "pymath.h"
#include "pytime.h"
#include "pymem.h"

#include "object.h"
#include "objimpl.h"
#include "typeslots.h"
#include "pyhash.h"

#include "pydebug.h"

#include "bytearrayobject.h"
#include "bytesobject.h"
#include "unicodeobject.h"
#include "longobject.h"
#include "longintrepr.h"
#include "boolobject.h"
#include "floatobject.h"
#include "complexobject.h"
#include "rangeobject.h"
#include "memoryobject.h"
#include "tupleobject.h"
#include "listobject.h"
#include "dictobject.h"
#include "odictobject.h"
#include "enumobject.h"
#include "setobject.h"
#include "methodobject.h"
#include "moduleobject.h"
#include "funcobject.h"
#include "classobject.h"
#include "fileobject.h"
#include "pycapsule.h"
#include "traceback.h"
#include "sliceobject.h"
#include "cellobject.h"
#include "iterobject.h"
#include "genobject.h"
#include "descrobject.h"
#include "warnings.h"
#include "weakrefobject.h"
#include "structseq.h"
#include "namespaceobject.h"
#include "picklebufobject.h"


#ifdef __cplusplus
extern "C" {
#endif

// Pyston additions:
// These new APIS give access to our fast hidden-class-based attributes implementation.
// Ideally in the future this will just be "storage strategy" of dicts and all Python
// dicts will benefit from it, but for now classes have to explicitly opt-in to having
// these kinds of attrs.
struct _hcattrs {
    char _data[16];
};
#ifndef _PYSTON_API
typedef struct _hcattrs PyHcAttrs;
#else
namespace pyston {
class HCAttrs;
}
typedef pyston::HCAttrs PyHcAttrs;
#endif
PyAPI_FUNC(void) PyObject_InitHcAttrs(PyHcAttrs*) PYSTON_NOEXCEPT;
PyAPI_FUNC(BORROWED(PyObject*)) PyObject_GetAttrWrapper(PyObject*) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyType_RequestHcAttrs(PyTypeObject*, int offset) PYSTON_NOEXCEPT;
// Sets a descriptor on the type so that the attrs are available via __dict__
PyAPI_FUNC(void) PyType_GiveHcAttrsDictDescr(PyTypeObject*) PYSTON_NOEXCEPT;
// These functions directly manipulate the hcattrs storage, bypassing any getattro
// or descriptor logic.  This is the equivallent of callling PyDict_GetItemString
// on an instance's dict.
// These functions try to mimic the Dict versions as much as possible, so for example
// the PyObject_GetHcAttrString function does not set an exception.
PyAPI_FUNC(PyObject*) PyObject_GetHcAttrString(PyObject*, const char*) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyObject_SetHcAttrString(PyObject*, const char*, PyObject*) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyObject_DelHcAttrString(PyObject*, const char*) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyObject_ClearHcAttrs(PyHcAttrs*) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyObject_TraverseHcAttrs(PyHcAttrs*, visitproc visit, void* arg) PYSTON_NOEXCEPT;

// Workaround: call this instead of setting tp_dict.
PyAPI_FUNC(void) PyType_SetDict(PyTypeObject*, STOLEN(PyObject*)) PYSTON_NOEXCEPT;

// Pyston addition: register an object as a "static constant".  Current purpose is that this will
// get decref'd when the interpreter shuts down.  This functions returns its argument.
// PyType_Ready calls this automatically.
PyAPI_FUNC(PyObject*) PyGC_RegisterStaticConstant(PyObject*) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyGC_RegisterStaticConstantLocation(PyObject**) PYSTON_NOEXCEPT;

// Gets gc.garbage
PyAPI_FUNC(PyObject*) _PyGC_GetGarbage(void) PYSTON_NOEXCEPT;

// Pyston addition:
PyAPI_FUNC(void) PyGC_Enable(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyGC_Disable(void) PYSTON_NOEXCEPT;

#ifdef Py_TRACE_REFS
// This function is a semi-smart leak finder.  Using the cycle-collector
// infrastructure, it will find all non-heap references remaining.  This is
// an improvement over calling _Py_PrintReferenceAddresses, since this will
// automatically filter out any objects that are only indirectly leaked.
//
// This will destroy the heap, so it has to be the last thing called.
PyAPI_FUNC(void) _PyGC_FindLeaks(PyObject**) PYSTON_NOEXCEPT;
#endif

#ifdef __cplusplus
}
#endif

#include "codecs.h"
#include "pyerrors.h"

#include "cpython/initconfig.h"
#include "pystate.h"
#include "context.h"

#include "pyarena.h"
#include "modsupport.h"
#include "compile.h"
#include "pythonrun.h"
#include "pylifecycle.h"
#include "ceval.h"
#include "sysmodule.h"
#include "osmodule.h"
#include "intrcheck.h"
#include "import.h"

#include "abstract.h"
#include "bltinmodule.h"

#include "eval.h"

#include "pyctype.h"
#include "pystrtod.h"
#include "pystrcmp.h"
#include "dtoa.h"
#include "fileutils.h"
#include "pyfpe.h"
#include "tracemalloc.h"

#endif /* !Py_PYTHON_H */
