// This file is originally from CPython 3.8, with modifications for Pyston

/* Boolean object interface */

#ifndef Py_BOOLOBJECT_H
#define Py_BOOLOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif


PyAPI_DATA(PyTypeObject) PyBool_Type;

#define PyBool_Check(x) (Py_TYPE(x) == &PyBool_Type)

/* Py_False and Py_True are the only two bools in existence.
Don't forget to apply Py_INCREF() when returning either!!! */

/* Don't use these directly */
// Pyston change: these are stored as pointers, not as static globals
PyAPI_DATA(PyObject) *pyston_True, *pyston_False;

/* Use these macros */
#define Py_False ((PyObject *) pyston_False)
#define Py_True ((PyObject *) pyston_True)

/* Macros for returning Py_True or Py_False, respectively */
#define Py_RETURN_TRUE return Py_INCREF(Py_True), Py_True
#define Py_RETURN_FALSE return Py_INCREF(Py_False), Py_False

/* Function to return a bool from a C long */
PyAPI_FUNC(PyObject *) PyBool_FromLong(long) PYSTON_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#endif /* !Py_BOOLOBJECT_H */
