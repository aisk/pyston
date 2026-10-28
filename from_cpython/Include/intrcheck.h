
#ifndef Py_INTRCHECK_H
#define Py_INTRCHECK_H
#ifdef __cplusplus
extern "C" {
#endif

PyAPI_FUNC(int) PyOS_InterruptOccurred(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyOS_InitInterrupts(void) PYSTON_NOEXCEPT;
#ifdef HAVE_FORK
#if !defined(Py_LIMITED_API) || Py_LIMITED_API+0 >= 0x03070000
PyAPI_FUNC(void) PyOS_BeforeFork(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyOS_AfterFork_Parent(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyOS_AfterFork_Child(void) PYSTON_NOEXCEPT;
#endif
#endif
/* Deprecated, please use PyOS_AfterFork_Child() instead */
Py_DEPRECATED(3.7) PyAPI_FUNC(void) PyOS_AfterFork(void) PYSTON_NOEXCEPT;

#ifndef Py_LIMITED_API
PyAPI_FUNC(int) _PyOS_IsMainThread(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) _PySignal_AfterFork(void) PYSTON_NOEXCEPT;

#ifdef MS_WINDOWS
/* windows.h is not included by Python.h so use void* instead of HANDLE */
PyAPI_FUNC(void*) _PyOS_SigintEvent(void) PYSTON_NOEXCEPT;
#endif
#endif /* !Py_LIMITED_API */

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTRCHECK_H */
