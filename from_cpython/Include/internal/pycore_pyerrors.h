#ifndef Py_INTERNAL_PYERRORS_H
#define Py_INTERNAL_PYERRORS_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

static inline PyObject* _PyErr_Occurred(PyThreadState *tstate)
{
    return tstate == NULL ? NULL : tstate->curexc_type;
}


PyAPI_FUNC(void) _PyErr_Fetch(
    PyThreadState *tstate,
    PyObject **type,
    PyObject **value,
    PyObject **traceback) PYSTON_NOEXCEPT;

PyAPI_FUNC(int) _PyErr_ExceptionMatches(
    PyThreadState *tstate,
    PyObject *exc) PYSTON_NOEXCEPT;

PyAPI_FUNC(void) _PyErr_Restore(
    PyThreadState *tstate,
    PyObject *type,
    PyObject *value,
    PyObject *traceback) PYSTON_NOEXCEPT;

PyAPI_FUNC(void) _PyErr_SetObject(
    PyThreadState *tstate,
    PyObject *type,
    PyObject *value) PYSTON_NOEXCEPT;

PyAPI_FUNC(void) _PyErr_Clear(PyThreadState *tstate) PYSTON_NOEXCEPT;

PyAPI_FUNC(void) _PyErr_SetNone(PyThreadState *tstate, PyObject *exception) PYSTON_NOEXCEPT;

PyAPI_FUNC(void) _PyErr_SetString(
    PyThreadState *tstate,
    PyObject *exception,
    const char *string) PYSTON_NOEXCEPT;

PyAPI_FUNC(PyObject *) _PyErr_Format(
    PyThreadState *tstate,
    PyObject *exception,
    const char *format,
    ...) PYSTON_NOEXCEPT;

PyAPI_FUNC(void) _PyErr_NormalizeException(
    PyThreadState *tstate,
    PyObject **exc,
    PyObject **val,
    PyObject **tb) PYSTON_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_PYERRORS_H */
