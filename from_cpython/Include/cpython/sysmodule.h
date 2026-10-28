#ifndef Py_CPYTHON_SYSMODULE_H
#  error "this header file must not be included directly"
#endif

#ifdef __cplusplus
extern "C" {
#endif

PyAPI_FUNC(PyObject *) _PySys_GetObjectId(_Py_Identifier *key) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) _PySys_SetObjectId(_Py_Identifier *key, PyObject *) PYSTON_NOEXCEPT;

PyAPI_FUNC(size_t) _PySys_GetSizeOf(PyObject *) PYSTON_NOEXCEPT;

typedef int(*Py_AuditHookFunction)(const char *, PyObject *, void *);

PyAPI_FUNC(int) PySys_Audit(const char*, const char *, ...) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PySys_AddAuditHook(Py_AuditHookFunction, void*) PYSTON_NOEXCEPT;

#ifdef __cplusplus
}
#endif
