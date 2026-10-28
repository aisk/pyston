
/* Module object interface */

#ifndef Py_MODULEOBJECT_H
#define Py_MODULEOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

PyAPI_DATA(PyTypeObject) PyModule_Type;

#define PyModule_Check(op) PyObject_TypeCheck(op, &PyModule_Type)
#define PyModule_CheckExact(op) (Py_TYPE(op) == &PyModule_Type)

#if !defined(Py_LIMITED_API) || Py_LIMITED_API+0 >= 0x03030000
PyAPI_FUNC(PyObject *) PyModule_NewObject(
    PyObject *name
    ) PYSTON_NOEXCEPT;
#endif
PyAPI_FUNC(PyObject *) PyModule_New(
    const char *name            /* UTF-8 encoded string */
    ) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyModule_GetDict(PyObject *) PYSTON_NOEXCEPT;
#if !defined(Py_LIMITED_API) || Py_LIMITED_API+0 >= 0x03030000
PyAPI_FUNC(PyObject *) PyModule_GetNameObject(PyObject *) PYSTON_NOEXCEPT;
#endif
PyAPI_FUNC(const char *) PyModule_GetName(PyObject *) PYSTON_NOEXCEPT;
Py_DEPRECATED(3.2) PyAPI_FUNC(const char *) PyModule_GetFilename(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyModule_GetFilenameObject(PyObject *) PYSTON_NOEXCEPT;
#ifndef Py_LIMITED_API
PyAPI_FUNC(void) _PyModule_Clear(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) _PyModule_ClearDict(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) _PyModuleSpec_IsInitializing(PyObject *) PYSTON_NOEXCEPT;
#endif
PyAPI_FUNC(struct PyModuleDef*) PyModule_GetDef(PyObject*) PYSTON_NOEXCEPT;
PyAPI_FUNC(void*) PyModule_GetState(PyObject*) PYSTON_NOEXCEPT;

#if !defined(Py_LIMITED_API) || Py_LIMITED_API+0 >= 0x03050000
/* New in 3.5 */
PyAPI_FUNC(PyObject *) PyModuleDef_Init(struct PyModuleDef*) PYSTON_NOEXCEPT;
PyAPI_DATA(PyTypeObject) PyModuleDef_Type;
#endif

typedef struct PyModuleDef_Base {
  PyObject_HEAD
  PyObject* (*m_init)(void);
  Py_ssize_t m_index;
  PyObject* m_copy;
} PyModuleDef_Base;

#define PyModuleDef_HEAD_INIT { \
    PyObject_HEAD_INIT(NULL)    \
    NULL, /* m_init */          \
    0,    /* m_index */         \
    NULL, /* m_copy */          \
  }

struct PyModuleDef_Slot;
#if !defined(Py_LIMITED_API) || Py_LIMITED_API+0 >= 0x03050000
/* New in 3.5 */
typedef struct PyModuleDef_Slot{
    int slot;
    void *value;
} PyModuleDef_Slot;

#define Py_mod_create 1
#define Py_mod_exec 2

#ifndef Py_LIMITED_API
#define _Py_mod_LAST_SLOT 2
#endif

#endif /* New in 3.5 */

typedef struct PyModuleDef{
  PyModuleDef_Base m_base;
  const char* m_name;
  const char* m_doc;
  Py_ssize_t m_size;
  PyMethodDef *m_methods;
  struct PyModuleDef_Slot* m_slots;
  traverseproc m_traverse;
  inquiry m_clear;
  freefunc m_free;
} PyModuleDef;

#ifdef __cplusplus
}
#endif
#endif /* !Py_MODULEOBJECT_H */
