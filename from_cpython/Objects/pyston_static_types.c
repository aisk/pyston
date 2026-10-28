// Pyston change: storage for the builtin types that are implemented in C++.
//
// CPython code takes the address of these types in static initializers (for example
// PyVarObject_HEAD_INIT(&PyType_Type, 0)), so they have to be real symbols.  They start out
// zeroed, and setupRuntime() constructs the BoxedClass objects in place.

#include "Python.h"

PyTypeObject PyBaseObject_Type;
PyTypeObject PyType_Type;
PyTypeObject PySuper_Type;
PyTypeObject _PyNone_Type;
PyTypeObject _PyNotImplemented_Type;
PyTypeObject PyBool_Type;
PyTypeObject PyLong_Type;
PyTypeObject PyFloat_Type;
PyTypeObject PyComplex_Type;
PyTypeObject PyList_Type;
PyTypeObject PyTuple_Type;
PyTypeObject PyDict_Type;
PyTypeObject PyDictIterKey_Type;
PyTypeObject PyDictIterValue_Type;
PyTypeObject PyDictIterItem_Type;
PyTypeObject PySet_Type;
PyTypeObject PyFrozenSet_Type;
PyTypeObject PyFunction_Type;
PyTypeObject PyMethod_Type;
PyTypeObject PyCFunction_Type;
PyTypeObject PyModule_Type;
PyTypeObject PyCode_Type;
PyTypeObject PyFrame_Type;
PyTypeObject PyGen_Type;
PyTypeObject PyProperty_Type;
PyTypeObject PyStaticMethod_Type;
PyTypeObject PyClassMethod_Type;
PyTypeObject PyEnum_Type;

// From Objects/moduleobject.c
PyTypeObject PyModuleDef_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "moduledef",                                /* tp_name */
    sizeof(struct PyModuleDef),                 /* tp_basicsize */
    0,                                          /* tp_itemsize */
};
