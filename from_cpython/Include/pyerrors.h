// This file is originally from CPython 3.8, with modifications for Pyston

#ifndef Py_ERRORS_H
#define Py_ERRORS_H
#ifdef __cplusplus
extern "C" {
#endif

/* Error handling definitions */

PyAPI_FUNC(void) PyErr_SetNone(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyErr_SetObject(PyObject *, PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyErr_SetString(
    PyObject *exception,
    const char *string   /* decoded from utf-8 */
    ) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyErr_Occurred(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyErr_Clear(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyErr_Fetch(PyObject **, PyObject **, PyObject **) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyErr_Restore(PyObject *, PyObject *, PyObject *) PYSTON_NOEXCEPT;
#if !defined(Py_LIMITED_API) || Py_LIMITED_API+0 >= 0x03030000
PyAPI_FUNC(void) PyErr_GetExcInfo(PyObject **, PyObject **, PyObject **) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyErr_SetExcInfo(PyObject *, PyObject *, PyObject *) PYSTON_NOEXCEPT;
#endif

/* Defined in Python/pylifecycle.c */
PyAPI_FUNC(void) _Py_NO_RETURN Py_FatalError(const char *message) PYSTON_NOEXCEPT;

#if defined(Py_DEBUG) || defined(Py_LIMITED_API)
#define _PyErr_OCCURRED() PyErr_Occurred()
#else
#define _PyErr_OCCURRED() (PyThreadState_GET()->curexc_type)
#endif

/* Error testing and normalization */
PyAPI_FUNC(int) PyErr_GivenExceptionMatches(PyObject *, PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyErr_ExceptionMatches(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyErr_NormalizeException(PyObject**, PyObject**, PyObject**) PYSTON_NOEXCEPT;

/* Traceback manipulation (PEP 3134) */
PyAPI_FUNC(int) PyException_SetTraceback(PyObject *, PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyException_GetTraceback(PyObject *) PYSTON_NOEXCEPT;

/* Cause manipulation (PEP 3134) */
PyAPI_FUNC(PyObject *) PyException_GetCause(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyException_SetCause(PyObject *, PyObject *) PYSTON_NOEXCEPT;

/* Context manipulation (PEP 3134) */
PyAPI_FUNC(PyObject *) PyException_GetContext(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyException_SetContext(PyObject *, PyObject *) PYSTON_NOEXCEPT;

/* */

#define PyExceptionClass_Check(x)                                       \
    (PyType_Check((x)) &&                                               \
     PyType_FastSubclass((PyTypeObject*)(x), Py_TPFLAGS_BASE_EXC_SUBCLASS))

#define PyExceptionInstance_Check(x)                    \
    PyType_FastSubclass(Py_TYPE(x), Py_TPFLAGS_BASE_EXC_SUBCLASS)

PyAPI_FUNC(const char *) PyExceptionClass_Name(PyObject *) PYSTON_NOEXCEPT;

#define PyExceptionInstance_Class(x) ((PyObject*)Py_TYPE(x))


/* Predefined exceptions */

PyAPI_DATA(PyObject *) PyExc_BaseException;
PyAPI_DATA(PyObject *) PyExc_Exception;
#if !defined(Py_LIMITED_API) || Py_LIMITED_API+0 >= 0x03050000
PyAPI_DATA(PyObject *) PyExc_StopAsyncIteration;
#endif
PyAPI_DATA(PyObject *) PyExc_StopIteration;
PyAPI_DATA(PyObject *) PyExc_GeneratorExit;
PyAPI_DATA(PyObject *) PyExc_ArithmeticError;
PyAPI_DATA(PyObject *) PyExc_LookupError;

PyAPI_DATA(PyObject *) PyExc_AssertionError;
PyAPI_DATA(PyObject *) PyExc_AttributeError;
PyAPI_DATA(PyObject *) PyExc_BufferError;
PyAPI_DATA(PyObject *) PyExc_EOFError;
PyAPI_DATA(PyObject *) PyExc_FloatingPointError;
PyAPI_DATA(PyObject *) PyExc_OSError;
PyAPI_DATA(PyObject *) PyExc_ImportError;
#if !defined(Py_LIMITED_API) || Py_LIMITED_API+0 >= 0x03060000
PyAPI_DATA(PyObject *) PyExc_ModuleNotFoundError;
#endif
PyAPI_DATA(PyObject *) PyExc_IndexError;
PyAPI_DATA(PyObject *) PyExc_KeyError;
PyAPI_DATA(PyObject *) PyExc_KeyboardInterrupt;
PyAPI_DATA(PyObject *) PyExc_MemoryError;
PyAPI_DATA(PyObject *) PyExc_NameError;
PyAPI_DATA(PyObject *) PyExc_OverflowError;
PyAPI_DATA(PyObject *) PyExc_RuntimeError;
#if !defined(Py_LIMITED_API) || Py_LIMITED_API+0 >= 0x03050000
PyAPI_DATA(PyObject *) PyExc_RecursionError;
#endif
PyAPI_DATA(PyObject *) PyExc_NotImplementedError;
PyAPI_DATA(PyObject *) PyExc_SyntaxError;
PyAPI_DATA(PyObject *) PyExc_IndentationError;
PyAPI_DATA(PyObject *) PyExc_TabError;
PyAPI_DATA(PyObject *) PyExc_ReferenceError;
PyAPI_DATA(PyObject *) PyExc_SystemError;
PyAPI_DATA(PyObject *) PyExc_SystemExit;
PyAPI_DATA(PyObject *) PyExc_TypeError;
PyAPI_DATA(PyObject *) PyExc_UnboundLocalError;
PyAPI_DATA(PyObject *) PyExc_UnicodeError;
PyAPI_DATA(PyObject *) PyExc_UnicodeEncodeError;
PyAPI_DATA(PyObject *) PyExc_UnicodeDecodeError;
PyAPI_DATA(PyObject *) PyExc_UnicodeTranslateError;
PyAPI_DATA(PyObject *) PyExc_ValueError;
PyAPI_DATA(PyObject *) PyExc_ZeroDivisionError;

#if !defined(Py_LIMITED_API) || Py_LIMITED_API+0 >= 0x03030000
PyAPI_DATA(PyObject *) PyExc_BlockingIOError;
PyAPI_DATA(PyObject *) PyExc_BrokenPipeError;
PyAPI_DATA(PyObject *) PyExc_ChildProcessError;
PyAPI_DATA(PyObject *) PyExc_ConnectionError;
PyAPI_DATA(PyObject *) PyExc_ConnectionAbortedError;
PyAPI_DATA(PyObject *) PyExc_ConnectionRefusedError;
PyAPI_DATA(PyObject *) PyExc_ConnectionResetError;
PyAPI_DATA(PyObject *) PyExc_FileExistsError;
PyAPI_DATA(PyObject *) PyExc_FileNotFoundError;
PyAPI_DATA(PyObject *) PyExc_InterruptedError;
PyAPI_DATA(PyObject *) PyExc_IsADirectoryError;
PyAPI_DATA(PyObject *) PyExc_NotADirectoryError;
PyAPI_DATA(PyObject *) PyExc_PermissionError;
PyAPI_DATA(PyObject *) PyExc_ProcessLookupError;
PyAPI_DATA(PyObject *) PyExc_TimeoutError;
#endif


/* Compatibility aliases */
PyAPI_DATA(PyObject *) PyExc_EnvironmentError;
PyAPI_DATA(PyObject *) PyExc_IOError;
#ifdef MS_WINDOWS
PyAPI_DATA(PyObject *) PyExc_WindowsError;
#endif

/* Predefined warning categories */
PyAPI_DATA(PyObject *) PyExc_Warning;
PyAPI_DATA(PyObject *) PyExc_UserWarning;
PyAPI_DATA(PyObject *) PyExc_DeprecationWarning;
PyAPI_DATA(PyObject *) PyExc_PendingDeprecationWarning;
PyAPI_DATA(PyObject *) PyExc_SyntaxWarning;
PyAPI_DATA(PyObject *) PyExc_RuntimeWarning;
PyAPI_DATA(PyObject *) PyExc_FutureWarning;
PyAPI_DATA(PyObject *) PyExc_ImportWarning;
PyAPI_DATA(PyObject *) PyExc_UnicodeWarning;
PyAPI_DATA(PyObject *) PyExc_BytesWarning;
PyAPI_DATA(PyObject *) PyExc_ResourceWarning;


/* Convenience functions */

PyAPI_FUNC(int) PyErr_BadArgument(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyErr_NoMemory(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyErr_SetFromErrno(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyErr_SetFromErrnoWithFilenameObject(
    PyObject *, PyObject *) PYSTON_NOEXCEPT;
#if !defined(Py_LIMITED_API) || Py_LIMITED_API+0 >= 0x03040000
PyAPI_FUNC(PyObject *) PyErr_SetFromErrnoWithFilenameObjects(
    PyObject *, PyObject *, PyObject *) PYSTON_NOEXCEPT;
#endif
PyAPI_FUNC(PyObject *) PyErr_SetFromErrnoWithFilename(
    PyObject *exc,
    const char *filename   /* decoded from the filesystem encoding */
    ) PYSTON_NOEXCEPT;

PyAPI_FUNC(PyObject *) PyErr_Format(
    PyObject *exception,
    const char *format,   /* ASCII-encoded string  */
    ...
    ) PYSTON_NOEXCEPT;
#if !defined(Py_LIMITED_API) || Py_LIMITED_API+0 >= 0x03050000
PyAPI_FUNC(PyObject *) PyErr_FormatV(
    PyObject *exception,
    const char *format,
    va_list vargs) PYSTON_NOEXCEPT;
#endif

#ifdef MS_WINDOWS
PyAPI_FUNC(PyObject *) PyErr_SetFromWindowsErrWithFilename(
    int ierr,
    const char *filename        /* decoded from the filesystem encoding */
    ) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyErr_SetFromWindowsErr(int) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyErr_SetExcFromWindowsErrWithFilenameObject(
    PyObject *,int, PyObject *) PYSTON_NOEXCEPT;
#if !defined(Py_LIMITED_API) || Py_LIMITED_API+0 >= 0x03040000
PyAPI_FUNC(PyObject *) PyErr_SetExcFromWindowsErrWithFilenameObjects(
    PyObject *,int, PyObject *, PyObject *) PYSTON_NOEXCEPT;
#endif
PyAPI_FUNC(PyObject *) PyErr_SetExcFromWindowsErrWithFilename(
    PyObject *exc,
    int ierr,
    const char *filename        /* decoded from the filesystem encoding */
    ) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyErr_SetExcFromWindowsErr(PyObject *, int) PYSTON_NOEXCEPT;
#endif /* MS_WINDOWS */

#if !defined(Py_LIMITED_API) || Py_LIMITED_API+0 >= 0x03060000
PyAPI_FUNC(PyObject *) PyErr_SetImportErrorSubclass(PyObject *, PyObject *,
    PyObject *, PyObject *) PYSTON_NOEXCEPT;
#endif
#if !defined(Py_LIMITED_API) || Py_LIMITED_API+0 >= 0x03030000
PyAPI_FUNC(PyObject *) PyErr_SetImportError(PyObject *, PyObject *,
    PyObject *) PYSTON_NOEXCEPT;
#endif

/* Export the old function so that the existing API remains available: */
PyAPI_FUNC(void) PyErr_BadInternalCall(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) _PyErr_BadInternalCall(const char *filename, int lineno) PYSTON_NOEXCEPT;
/* Mask the old API with a call to the new API for code compiled under
   Python 2.0: */
#define PyErr_BadInternalCall() _PyErr_BadInternalCall(__FILE__, __LINE__)

// Pyston addition: allocate + initialize an instance of type `type`.
// arg represents the single value that will be passed to the constructor;
// a NULL value represents passing zero arguments, and a tuple value will
// not be expanded into multiple arguments.
// In the common cases this will be faster than creating the instance using
// PyObject_Call(type, PyTuple_Pack(1, arg), NULL)
PyAPI_FUNC(PyObject *) PyErr_CreateExceptionInstance(PyObject* type, PyObject* arg) PYSTON_NOEXCEPT;

/* Function to create a new exception */
PyAPI_FUNC(PyObject *) PyErr_NewException(
    const char *name, PyObject *base, PyObject *dict) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyErr_NewExceptionWithDoc(
    const char *name, const char *doc, PyObject *base, PyObject *dict) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyErr_WriteUnraisable(PyObject *) PYSTON_NOEXCEPT;


/* In signalmodule.c */
PyAPI_FUNC(int) PyErr_CheckSignals(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyErr_SetInterrupt(void) PYSTON_NOEXCEPT;

/* Support for adding program text to SyntaxErrors */
PyAPI_FUNC(void) PyErr_SyntaxLocation(
    const char *filename,       /* decoded from the filesystem encoding */
    int lineno) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyErr_SyntaxLocationEx(
    const char *filename,       /* decoded from the filesystem encoding */
    int lineno,
    int col_offset) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyErr_ProgramText(
    const char *filename,       /* decoded from the filesystem encoding */
    int lineno) PYSTON_NOEXCEPT;

/* The following functions are used to create and modify unicode
   exceptions from C */

/* create a UnicodeDecodeError object */
PyAPI_FUNC(PyObject *) PyUnicodeDecodeError_Create(
    const char *encoding,       /* UTF-8 encoded string */
    const char *object,
    Py_ssize_t length,
    Py_ssize_t start,
    Py_ssize_t end,
    const char *reason          /* UTF-8 encoded string */
    ) PYSTON_NOEXCEPT;

/* get the encoding attribute */
PyAPI_FUNC(PyObject *) PyUnicodeEncodeError_GetEncoding(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyUnicodeDecodeError_GetEncoding(PyObject *) PYSTON_NOEXCEPT;

/* get the object attribute */
PyAPI_FUNC(PyObject *) PyUnicodeEncodeError_GetObject(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyUnicodeDecodeError_GetObject(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyUnicodeTranslateError_GetObject(PyObject *) PYSTON_NOEXCEPT;

/* get the value of the start attribute (the int * may not be NULL)
   return 0 on success, -1 on failure */
PyAPI_FUNC(int) PyUnicodeEncodeError_GetStart(PyObject *, Py_ssize_t *) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyUnicodeDecodeError_GetStart(PyObject *, Py_ssize_t *) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyUnicodeTranslateError_GetStart(PyObject *, Py_ssize_t *) PYSTON_NOEXCEPT;

/* assign a new value to the start attribute
   return 0 on success, -1 on failure */
PyAPI_FUNC(int) PyUnicodeEncodeError_SetStart(PyObject *, Py_ssize_t) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyUnicodeDecodeError_SetStart(PyObject *, Py_ssize_t) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyUnicodeTranslateError_SetStart(PyObject *, Py_ssize_t) PYSTON_NOEXCEPT;

/* get the value of the end attribute (the int *may not be NULL)
 return 0 on success, -1 on failure */
PyAPI_FUNC(int) PyUnicodeEncodeError_GetEnd(PyObject *, Py_ssize_t *) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyUnicodeDecodeError_GetEnd(PyObject *, Py_ssize_t *) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyUnicodeTranslateError_GetEnd(PyObject *, Py_ssize_t *) PYSTON_NOEXCEPT;

/* assign a new value to the end attribute
   return 0 on success, -1 on failure */
PyAPI_FUNC(int) PyUnicodeEncodeError_SetEnd(PyObject *, Py_ssize_t) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyUnicodeDecodeError_SetEnd(PyObject *, Py_ssize_t) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyUnicodeTranslateError_SetEnd(PyObject *, Py_ssize_t) PYSTON_NOEXCEPT;

/* get the value of the reason attribute */
PyAPI_FUNC(PyObject *) PyUnicodeEncodeError_GetReason(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyUnicodeDecodeError_GetReason(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyUnicodeTranslateError_GetReason(PyObject *) PYSTON_NOEXCEPT;

/* assign a new value to the reason attribute
   return 0 on success, -1 on failure */
PyAPI_FUNC(int) PyUnicodeEncodeError_SetReason(
    PyObject *exc,
    const char *reason          /* UTF-8 encoded string */
    ) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyUnicodeDecodeError_SetReason(
    PyObject *exc,
    const char *reason          /* UTF-8 encoded string */
    ) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyUnicodeTranslateError_SetReason(
    PyObject *exc,
    const char *reason          /* UTF-8 encoded string */
    ) PYSTON_NOEXCEPT;

/* These APIs aren't really part of the error implementation, but
   often needed to format error messages; the native C lib APIs are
   not available on all platforms, which is why we provide emulations
   for those platforms in Python/mysnprintf.c,
   WARNING:  The return value of snprintf varies across platforms; do
   not rely on any particular behavior; eventually the C99 defn may
   be reliable.
*/
#if defined(MS_WIN32) && !defined(HAVE_SNPRINTF)
# define HAVE_SNPRINTF
# define snprintf _snprintf
# define vsnprintf _vsnprintf
#endif

#include <stdarg.h>
PyAPI_FUNC(int) PyOS_snprintf(char *str, size_t size, const char  *format, ...) PYSTON_NOEXCEPT
                        Py_GCC_ATTRIBUTE((format(printf, 3, 4)));
PyAPI_FUNC(int) PyOS_vsnprintf(char *str, size_t size, const char  *format, va_list va) PYSTON_NOEXCEPT
                        Py_GCC_ATTRIBUTE((format(printf, 3, 0)));

#ifndef Py_LIMITED_API
#  define Py_CPYTHON_ERRORS_H
#  include  "cpython/pyerrors.h"
#  undef Py_CPYTHON_ERRORS_H
#endif

#ifdef __cplusplus
}
#endif
#endif /* !Py_ERRORS_H */
