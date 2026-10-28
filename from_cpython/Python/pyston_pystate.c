// Pyston change: the parts of CPython's Python/pystate.c and Python/pylifecycle.c that the C code
// from CPython needs.  Pyston manages thread states itself (src/core/threading.cpp); this file only
// owns _PyRuntime and the single interpreter state.

#include "Python.h"

#include <dlfcn.h>

#include "pycore_pystate.h"

_PyRuntimeState _PyRuntime = _PyRuntimeState_INIT;

PyInterpreterState _Pyston_main_interp;

void
_PystonRuntime_Init(void)
{
    PyInterpreterState *interp = &_Pyston_main_interp;

    _PyGC_Initialize(&_PyRuntime.gc);
    _PyRuntime.main_thread = PyThread_get_thread_ident();
    _PyRuntime.interpreters.main = interp;
    _PyRuntime.interpreters.head = interp;
    _PyRuntime.interpreters.next_id = 1;

    interp->id = 0;
    interp->id_refcount = -1;
    interp->check_interval = 100;
    interp->eval_frame = NULL;
    interp->dlopenflags = RTLD_NOW;

    // Python 3.8's defaults on Linux, see config_init_fs_encoding() in Python/initconfig.c
    interp->config.filesystem_encoding = L"utf-8";
    interp->config.filesystem_errors = L"surrogateescape";
    interp->config.stdio_encoding = L"utf-8";
    interp->config.stdio_errors = L"strict";
    interp->config.isolated = 0;
    interp->config.use_environment = 1;
}

PyInterpreterState *
_PyInterpreterState_Get(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (tstate == NULL) {
        Py_FatalError("_PyInterpreterState_Get(): no current thread state");
    }
    return tstate->interp;
}

PyInterpreterState *
_PyGILState_GetInterpreterStateUnsafe(void)
{
    return &_Pyston_main_interp;
}

PyInterpreterState *
PyInterpreterState_Main(void)
{
    return &_Pyston_main_interp;
}

int64_t
PyInterpreterState_GetID(PyInterpreterState *interp)
{
    return interp->id;
}

PyObject *
_PyInterpreterState_GetMainModule(PyInterpreterState *interp)
{
    if (interp->modules == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "interpreter not initialized");
        return NULL;
    }
    return PyMapping_GetItemString(interp->modules, "__main__");
}

/* These are only used after fork(), which Pyston doesn't support yet */
void
_PyRuntimeState_ReInitThreads(_PyRuntimeState *runtime)
{
}

void
_PyInterpreterState_DeleteExceptMain(_PyRuntimeState *runtime)
{
}

void
_PyGILState_Reinit(_PyRuntimeState *runtime)
{
}

/* From Python/pylifecycle.c */

void
_Py_PyAtExit(void (*func)(PyObject *), PyObject *module)
{
    PyInterpreterState *is = _PyInterpreterState_Get();

    /* Guard against API misuse (see bpo-17852) */
    assert(is->pyexitfunc == NULL || is->pyexitfunc == func);

    is->pyexitfunc = func;
    is->pyexitmodule = module;
}

void
_PystonCallPyExitFuncs(void)
{
    PyInterpreterState *interp = &_Pyston_main_interp;
    if (interp->pyexitfunc == NULL)
        return;

    (*interp->pyexitfunc)(interp->pyexitmodule);
    PyErr_Clear();
}

int
_PystonFlushStdFiles(void)
{
    _Py_IDENTIFIER(stdout);
    _Py_IDENTIFIER(stderr);
    _Py_IDENTIFIER(flush);
    PyObject *fout = _PySys_GetObjectId(&PyId_stdout);
    PyObject *ferr = _PySys_GetObjectId(&PyId_stderr);
    PyObject *tmp;
    int status = 0;

    if (fout != NULL && fout != Py_None) {
        tmp = _PyObject_CallMethodId(fout, &PyId_flush, NULL);
        if (tmp == NULL) {
            PyErr_WriteUnraisable(fout);
            status = -1;
        }
        else
            Py_DECREF(tmp);
    }

    if (ferr != NULL && ferr != Py_None) {
        tmp = _PyObject_CallMethodId(ferr, &PyId_flush, NULL);
        if (tmp == NULL) {
            PyErr_Clear();
            status = -1;
        }
        else
            Py_DECREF(tmp);
    }

    return status;
}

void
_PystonInterp_SetModules(PyObject *modules, PyObject *sysdict)
{
    Py_XSETREF(_Pyston_main_interp.modules, (Py_INCREF(modules), modules));
    Py_XSETREF(_Pyston_main_interp.sysdict, (Py_INCREF(sysdict), sysdict));
}

void
_PystonInterp_SetBuiltins(PyObject *builtins)
{
    Py_XSETREF(_Pyston_main_interp.builtins, (Py_INCREF(builtins), builtins));
}
