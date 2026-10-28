// This file is originally from CPython 3.8, with modifications for Pyston


/* System module interface */

#ifndef Py_SYSMODULE_H
#define Py_SYSMODULE_H
#ifdef __cplusplus
extern "C" {
#endif

PyAPI_FUNC(BORROWED(PyObject *)) PySys_GetObject(const char *) PYSTON_NOEXCEPT;
// Pyston addition: get the sys.modules dict
PyAPI_FUNC(BORROWED(PyObject *)) PySys_GetModulesDict(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PySys_SetObject(const char *, PyObject *) PYSTON_NOEXCEPT;

PyAPI_FUNC(void) PySys_SetArgv(int, wchar_t **) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PySys_SetArgvEx(int, wchar_t **, int) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PySys_SetPath(const wchar_t *) PYSTON_NOEXCEPT;

PyAPI_FUNC(void) PySys_WriteStdout(const char *format, ...) PYSTON_NOEXCEPT
                 Py_GCC_ATTRIBUTE((format(printf, 1, 2)));
PyAPI_FUNC(void) PySys_WriteStderr(const char *format, ...) PYSTON_NOEXCEPT
                 Py_GCC_ATTRIBUTE((format(printf, 1, 2)));
PyAPI_FUNC(void) PySys_FormatStdout(const char *format, ...) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PySys_FormatStderr(const char *format, ...) PYSTON_NOEXCEPT;

PyAPI_FUNC(void) PySys_ResetWarnOptions(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PySys_AddWarnOption(const wchar_t *) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PySys_AddWarnOptionUnicode(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PySys_HasWarnOptions(void) PYSTON_NOEXCEPT;

PyAPI_FUNC(void) PySys_AddXOption(const wchar_t *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PySys_GetXOptions(void) PYSTON_NOEXCEPT;

#ifndef Py_LIMITED_API
#  define Py_CPYTHON_SYSMODULE_H
#  include  "cpython/sysmodule.h"
#  undef Py_CPYTHON_SYSMODULE_H
#endif

#ifdef __cplusplus
}
#endif
#endif /* !Py_SYSMODULE_H */
