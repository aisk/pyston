#ifndef Py_CPYTHON_PYLIFECYCLE_H
#  error "this header file must not be included directly"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Only used by applications that embed the interpreter and need to
 * override the standard encoding determination mechanism
 */
PyAPI_FUNC(int) Py_SetStandardStreamEncoding(const char *encoding,
                                             const char *errors) PYSTON_NOEXCEPT;

/* PEP 432 Multi-phase initialization API (Private while provisional!) */

PyAPI_FUNC(PyStatus) Py_PreInitialize(
    const PyPreConfig *src_config) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyStatus) Py_PreInitializeFromBytesArgs(
    const PyPreConfig *src_config,
    Py_ssize_t argc,
    char **argv) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyStatus) Py_PreInitializeFromArgs(
    const PyPreConfig *src_config,
    Py_ssize_t argc,
    wchar_t **argv) PYSTON_NOEXCEPT;

PyAPI_FUNC(int) _Py_IsCoreInitialized(void) PYSTON_NOEXCEPT;


/* Initialization and finalization */

PyAPI_FUNC(PyStatus) Py_InitializeFromConfig(
    const PyConfig *config) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyStatus) _Py_InitializeFromArgs(
    const PyConfig *config,
    Py_ssize_t argc,
    char * const *argv) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyStatus) _Py_InitializeFromWideArgs(
    const PyConfig *config,
    Py_ssize_t argc,
    wchar_t * const *argv) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyStatus) _Py_InitializeMain(void) PYSTON_NOEXCEPT;

PyAPI_FUNC(int) Py_RunMain(void) PYSTON_NOEXCEPT;


PyAPI_FUNC(void) _Py_NO_RETURN Py_ExitStatusException(PyStatus err) PYSTON_NOEXCEPT;

/* Py_PyAtExit is for the atexit module, Py_AtExit is for low-level
 * exit functions.
 */
PyAPI_FUNC(void) _Py_PyAtExit(void (*func)(PyObject *), PyObject *) PYSTON_NOEXCEPT;

/* Restore signals that the interpreter has called SIG_IGN on to SIG_DFL. */
PyAPI_FUNC(void) _Py_RestoreSignals(void) PYSTON_NOEXCEPT;

PyAPI_FUNC(int) Py_FdIsInteractive(FILE *, const char *) PYSTON_NOEXCEPT;

PyAPI_FUNC(void) _Py_SetProgramFullPath(const wchar_t *) PYSTON_NOEXCEPT;

PyAPI_FUNC(const char *) _Py_gitidentifier(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(const char *) _Py_gitversion(void) PYSTON_NOEXCEPT;

PyAPI_FUNC(int) _Py_IsFinalizing(void) PYSTON_NOEXCEPT;

/* Random */
PyAPI_FUNC(int) _PyOS_URandom(void *buffer, Py_ssize_t size) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) _PyOS_URandomNonblock(void *buffer, Py_ssize_t size) PYSTON_NOEXCEPT;

/* Legacy locale support */
PyAPI_FUNC(int) _Py_CoerceLegacyLocale(int warn) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) _Py_LegacyLocaleDetected(int warn) PYSTON_NOEXCEPT;
PyAPI_FUNC(char *) _Py_SetLocaleFromEnv(int category) PYSTON_NOEXCEPT;

#ifdef __cplusplus
}
#endif
