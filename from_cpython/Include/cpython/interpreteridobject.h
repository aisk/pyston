#ifndef Py_CPYTHON_INTERPRETERIDOBJECT_H
#  error "this header file must not be included directly"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Interpreter ID Object */

PyAPI_DATA(PyTypeObject) _PyInterpreterID_Type;

PyAPI_FUNC(PyObject *) _PyInterpreterID_New(int64_t) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) _PyInterpreterState_GetIDObject(PyInterpreterState *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyInterpreterState *) _PyInterpreterID_LookUp(PyObject *) PYSTON_NOEXCEPT;

#ifdef __cplusplus
}
#endif
