#ifndef Py_CPYTHON_FILEOBJECT_H
#  error "this header file must not be included directly"
#endif

#ifdef __cplusplus
extern "C" {
#endif

PyAPI_FUNC(char *) Py_UniversalNewlineFgets(char *, int, FILE*, PyObject *) PYSTON_NOEXCEPT;

/* The std printer acts as a preliminary sys.stderr until the new io
   infrastructure is in place. */
PyAPI_FUNC(PyObject *) PyFile_NewStdPrinter(int) PYSTON_NOEXCEPT;
PyAPI_DATA(PyTypeObject) PyStdPrinter_Type;

typedef PyObject * (*Py_OpenCodeHookFunction)(PyObject *, void *);

PyAPI_FUNC(PyObject *) PyFile_OpenCode(const char *utf8path) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyFile_OpenCodeObject(PyObject *path) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyFile_SetOpenCodeHook(Py_OpenCodeHookFunction hook, void *userData) PYSTON_NOEXCEPT;

#ifdef __cplusplus
}
#endif
