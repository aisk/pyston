#ifndef Py_TRACEBACK_H
#define Py_TRACEBACK_H
#ifdef __cplusplus
extern "C" {
#endif

struct _frame;
struct _traceback;

/* Traceback interface */

PyAPI_FUNC(int) PyTraceBack_Here(struct _frame *) PYSTON_NOEXCEPT;
// Pyston addition: adds a traceback entry to *tb instead of the thread state
PyAPI_FUNC(int) PyTraceBack_Here_Tb(struct _frame *, struct _traceback **) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyTraceBack_Print(PyObject *, PyObject *) PYSTON_NOEXCEPT;

/* Reveal traceback type so we can typecheck traceback objects */
PyAPI_DATA(PyTypeObject) PyTraceBack_Type;
#define PyTraceBack_Check(v) (Py_TYPE(v) == &PyTraceBack_Type)


#ifndef Py_LIMITED_API
#  define Py_CPYTHON_TRACEBACK_H
#  include  "cpython/traceback.h"
#  undef Py_CPYTHON_TRACEBACK_H
#endif

#ifdef __cplusplus
}
#endif
#endif /* !Py_TRACEBACK_H */
