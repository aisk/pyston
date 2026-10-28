#ifndef Py_CPYTHON_DICTOBJECT_H
#  error "this header file must not be included directly"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _dictkeysobject PyDictKeysObject;

/* The ma_values pointer is NULL for a combined table
 * or points to an array of PyObject* for a split table
 */
// Pyston change: this is not the format we're using
typedef struct {
    PyObject_HEAD
    char _filler[48];
} PyDictObject;

PyAPI_FUNC(PyObject *) _PyDict_GetItem_KnownHash(PyObject *mp, PyObject *key,
                                       Py_hash_t hash) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) _PyDict_GetItemIdWithError(PyObject *dp,
                                                  struct _Py_Identifier *key) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) _PyDict_GetItemStringWithError(PyObject *, const char *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyDict_SetDefault(
    PyObject *mp, PyObject *key, PyObject *defaultobj) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) _PyDict_SetItem_KnownHash(PyObject *mp, PyObject *key,
                                          PyObject *item, Py_hash_t hash) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) _PyDict_DelItem_KnownHash(PyObject *mp, PyObject *key,
                                          Py_hash_t hash) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) _PyDict_DelItemIf(PyObject *mp, PyObject *key,
                                  int (*predicate)(PyObject *value)) PYSTON_NOEXCEPT;
PyDictKeysObject *_PyDict_NewKeysForClass(void);
PyAPI_FUNC(PyObject *) PyObject_GenericGetDict(PyObject *, void *) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) _PyDict_Next(
    PyObject *mp, Py_ssize_t *pos, PyObject **key, PyObject **value, Py_hash_t *hash) PYSTON_NOEXCEPT;

/* Get the number of items of a dictionary. */
// Pyston change: we don't use CPython's dict layout
#define PyDict_GET_SIZE(mp)  PyDict_Size((PyObject*)(mp))
PyAPI_FUNC(int) _PyDict_Contains(PyObject *mp, PyObject *key, Py_hash_t hash) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) _PyDict_NewPresized(Py_ssize_t minused) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) _PyDict_MaybeUntrack(PyObject *mp) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) _PyDict_HasOnlyStringKeys(PyObject *mp) PYSTON_NOEXCEPT;
Py_ssize_t _PyDict_KeysSize(PyDictKeysObject *keys);
PyAPI_FUNC(Py_ssize_t) _PyDict_SizeOf(PyDictObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) _PyDict_Pop(PyObject *, PyObject *, PyObject *) PYSTON_NOEXCEPT;
PyObject *_PyDict_Pop_KnownHash(PyObject *, PyObject *, Py_hash_t, PyObject *) PYSTON_NOEXCEPT;
PyObject *_PyDict_FromKeys(PyObject *, PyObject *, PyObject *);
#define _PyDict_HasSplitTable(d) ((d)->ma_values != NULL)

PyAPI_FUNC(int) PyDict_ClearFreeList(void) PYSTON_NOEXCEPT;

/* Like PyDict_Merge, but override can be 0, 1 or 2.  If override is 0,
   the first occurrence of a key wins, if override is 1, the last occurrence
   of a key wins, if override is 2, a KeyError with conflicting key as
   argument is raised.
*/
PyAPI_FUNC(int) _PyDict_MergeEx(PyObject *mp, PyObject *other, int override) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) _PyDict_GetItemId(PyObject *dp, struct _Py_Identifier *key) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) _PyDict_SetItemId(PyObject *dp, struct _Py_Identifier *key, PyObject *item) PYSTON_NOEXCEPT;

PyAPI_FUNC(int) _PyDict_DelItemId(PyObject *mp, struct _Py_Identifier *key) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) _PyDict_DebugMallocStats(FILE *out) PYSTON_NOEXCEPT;

int _PyObjectDict_SetItem(PyTypeObject *tp, PyObject **dictptr, PyObject *name, PyObject *value);
PyObject *_PyDict_LoadGlobal(PyDictObject *, PyDictObject *, PyObject *);

/* _PyDictView */

typedef struct {
    PyObject_HEAD
    PyDictObject *dv_dict;
} _PyDictViewObject;

PyAPI_FUNC(PyObject *) _PyDictView_New(PyObject *, PyTypeObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) _PyDictView_Intersect(PyObject* self, PyObject *other) PYSTON_NOEXCEPT;

#ifdef __cplusplus
}
#endif
