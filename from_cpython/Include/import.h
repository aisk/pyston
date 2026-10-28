
/* Module definition and import interface */

#ifndef Py_IMPORT_H
#define Py_IMPORT_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_LIMITED_API
PyMODINIT_FUNC PyInit__imp(void);
#endif /* !Py_LIMITED_API */
PyAPI_FUNC(long) PyImport_GetMagicNumber(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(const char *) PyImport_GetMagicTag(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyImport_ExecCodeModule(
    const char *name,           /* UTF-8 encoded string */
    PyObject *co
    ) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyImport_ExecCodeModuleEx(
    const char *name,           /* UTF-8 encoded string */
    PyObject *co,
    const char *pathname        /* decoded from the filesystem encoding */
    ) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyImport_ExecCodeModuleWithPathnames(
    const char *name,           /* UTF-8 encoded string */
    PyObject *co,
    const char *pathname,       /* decoded from the filesystem encoding */
    const char *cpathname       /* decoded from the filesystem encoding */
    ) PYSTON_NOEXCEPT;
#if !defined(Py_LIMITED_API) || Py_LIMITED_API+0 >= 0x03030000
PyAPI_FUNC(PyObject *) PyImport_ExecCodeModuleObject(
    PyObject *name,
    PyObject *co,
    PyObject *pathname,
    PyObject *cpathname
    ) PYSTON_NOEXCEPT;
#endif
PyAPI_FUNC(PyObject *) PyImport_GetModuleDict(void) PYSTON_NOEXCEPT;
#if !defined(Py_LIMITED_API) || Py_LIMITED_API+0 >= 0x03070000
PyAPI_FUNC(PyObject *) PyImport_GetModule(PyObject *name) PYSTON_NOEXCEPT;
#endif
#ifndef Py_LIMITED_API
PyAPI_FUNC(int) _PyImport_IsInitialized(PyInterpreterState *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) _PyImport_GetModuleId(struct _Py_Identifier *name) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) _PyImport_AddModuleObject(PyObject *name,
                                                 PyObject *modules) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) _PyImport_SetModule(PyObject *name, PyObject *module) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) _PyImport_SetModuleString(const char *name, PyObject* module) PYSTON_NOEXCEPT;
#endif
#if !defined(Py_LIMITED_API) || Py_LIMITED_API+0 >= 0x03030000
PyAPI_FUNC(PyObject *) PyImport_AddModuleObject(
    PyObject *name
    ) PYSTON_NOEXCEPT;
#endif
PyAPI_FUNC(PyObject *) PyImport_AddModule(
    const char *name            /* UTF-8 encoded string */
    ) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyImport_ImportModule(
    const char *name            /* UTF-8 encoded string */
    ) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyImport_ImportModuleNoBlock(
    const char *name            /* UTF-8 encoded string */
    ) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyImport_ImportModuleLevel(
    const char *name,           /* UTF-8 encoded string */
    PyObject *globals,
    PyObject *locals,
    PyObject *fromlist,
    int level
    ) PYSTON_NOEXCEPT;
#if !defined(Py_LIMITED_API) || Py_LIMITED_API+0 >= 0x03050000
PyAPI_FUNC(PyObject *) PyImport_ImportModuleLevelObject(
    PyObject *name,
    PyObject *globals,
    PyObject *locals,
    PyObject *fromlist,
    int level
    ) PYSTON_NOEXCEPT;
#endif

#define PyImport_ImportModuleEx(n, g, l, f) \
    PyImport_ImportModuleLevel(n, g, l, f, 0)

PyAPI_FUNC(PyObject *) PyImport_GetImporter(PyObject *path) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyImport_Import(PyObject *name) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyImport_ReloadModule(PyObject *m) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyImport_Cleanup(void) PYSTON_NOEXCEPT;
#if !defined(Py_LIMITED_API) || Py_LIMITED_API+0 >= 0x03030000
PyAPI_FUNC(int) PyImport_ImportFrozenModuleObject(
    PyObject *name
    ) PYSTON_NOEXCEPT;
#endif
PyAPI_FUNC(int) PyImport_ImportFrozenModule(
    const char *name            /* UTF-8 encoded string */
    ) PYSTON_NOEXCEPT;

#ifndef Py_LIMITED_API
PyAPI_FUNC(void) _PyImport_AcquireLock(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) _PyImport_ReleaseLock(void) PYSTON_NOEXCEPT;

PyAPI_FUNC(void) _PyImport_ReInitLock(void) PYSTON_NOEXCEPT;

PyAPI_FUNC(PyObject *) _PyImport_FindBuiltin(
    const char *name,            /* UTF-8 encoded string */
    PyObject *modules
    ) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) _PyImport_FindExtensionObject(PyObject *, PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) _PyImport_FindExtensionObjectEx(PyObject *, PyObject *,
                                                       PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) _PyImport_FixupBuiltin(
    PyObject *mod,
    const char *name,            /* UTF-8 encoded string */
    PyObject *modules
    ) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) _PyImport_FixupExtensionObject(PyObject*, PyObject *,
                                               PyObject *, PyObject *) PYSTON_NOEXCEPT;

struct _inittab {
    const char *name;           /* ASCII encoded string */
    PyObject* (*initfunc)(void);
};
PyAPI_DATA(struct _inittab *) PyImport_Inittab;
PyAPI_FUNC(int) PyImport_ExtendInittab(struct _inittab *newtab) PYSTON_NOEXCEPT;
#endif /* Py_LIMITED_API */

PyAPI_DATA(PyTypeObject) PyNullImporter_Type;

PyAPI_FUNC(int) PyImport_AppendInittab(
    const char *name,           /* ASCII encoded string */
    PyObject* (*initfunc)(void)
    ) PYSTON_NOEXCEPT;

#ifndef Py_LIMITED_API
struct _frozen {
    const char *name;                 /* ASCII encoded string */
    const unsigned char *code;
    int size;
};

/* Embedding apps may change this pointer to point to their favorite
   collection of frozen modules: */

PyAPI_DATA(const struct _frozen *) PyImport_FrozenModules;
#endif

#ifdef __cplusplus
}
#endif
#endif /* !Py_IMPORT_H */
