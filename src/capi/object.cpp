// Copyright (c) 2014-2016 Dropbox, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdarg.h>
#include <string.h>

#include "Python.h"

#include "capi/typeobject.h"
#include "capi/types.h"
#include "core/threading.h"
#include "core/types.h"
#include "runtime/import.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

static int merge_class_dict(PyObject* dict, PyObject* aclass) noexcept {
    PyObject* classdict;
    PyObject* bases;

    assert(PyDict_Check(dict));
    assert(aclass);

    /* Merge in the type's dict (if any). */
    classdict = PyObject_GetAttrString(aclass, "__dict__");
    if (classdict == NULL)
        PyErr_Clear();
    else {
        int status = PyDict_Update(dict, classdict);
        Py_DECREF(classdict);
        if (status < 0)
            return -1;
    }

    /* Recursively merge in the base types' (if any) dicts. */
    bases = PyObject_GetAttrString(aclass, "__bases__");
    if (bases == NULL)
        PyErr_Clear();
    else {
        /* We have no guarantee that bases is a real tuple */
        Py_ssize_t i, n;
        n = PySequence_Size(bases); /* This better be right */
        if (n < 0)
            PyErr_Clear();
        else {
            for (i = 0; i < n; i++) {
                int status;
                PyObject* base = PySequence_GetItem(bases, i);
                if (base == NULL) {
                    Py_DECREF(bases);
                    return -1;
                }
                status = merge_class_dict(dict, base);
                Py_DECREF(base);
                if (status < 0) {
                    Py_DECREF(bases);
                    return -1;
                }
            }
        }
        Py_DECREF(bases);
    }
    return 0;
}

/* Helper for PyObject_Dir without arguments: returns the local scope. */
static PyObject* _dir_locals(void) {
    PyObject* names;
    PyObject* locals = PyEval_GetLocals();

    if (locals == NULL) {
        PyErr_SetString(PyExc_SystemError, "frame does not exist");
        return NULL;
    }

    names = PyMapping_Keys(locals);
    if (!names)
        return NULL;
    if (!PyList_Check(names)) {
        PyErr_Format(PyExc_TypeError, "dir(): expected keys() of locals to be a list, "
                                      "not '%.200s'",
                     Py_TYPE(names)->tp_name);
        Py_DECREF(names);
        return NULL;
    }
    /* the locals don't need to be DECREF'd */
    return names;
}

/* Helper for PyObject_Dir of type objects: returns __dict__ and __bases__.
   We deliberately don't suck up its __class__, as methods belonging to the
   metaclass would probably be more confusing than helpful.
*/
static PyObject* _specialized_dir_type(PyObject* obj) noexcept {
    PyObject* result = NULL;
    PyObject* dict = PyDict_New();

    if (dict != NULL && merge_class_dict(dict, obj) == 0)
        result = PyDict_Keys(dict);

    Py_XDECREF(dict);
    return result;
}

/* Helper for PyObject_Dir of module objects: returns the module's __dict__. */
static PyObject* _specialized_dir_module(PyObject* obj) noexcept {
    PyObject* result = NULL;
    PyObject* dict = PyObject_GetAttrString(obj, "__dict__");

    if (dict != NULL) {
        if (PyDict_Check(dict))
            result = PyDict_Keys(dict);
        else if (dict->cls == attrwrapper_cls)
            result = attrwrapperKeys(dict);
        else {
            const char* name = PyModule_GetName(obj);
            if (name)
                PyErr_Format(PyExc_TypeError, "%.200s.__dict__ is not a dictionary", name);
        }
    }

    Py_XDECREF(dict);
    return result;
}

/* Helper for PyObject_Dir of generic objects: returns __dict__, __class__,
   and recursively up the __class__.__bases__ chain.
*/
static PyObject* _generic_dir(PyObject* obj) noexcept {
    PyObject* result = NULL;
    PyObject* dict = NULL;
    PyObject* itsclass = NULL;

    /* Get __dict__ (which may or may not be a real dict...) */
    dict = PyObject_GetAttrString(obj, "__dict__");
    if (dict == NULL) {
        PyErr_Clear();
        dict = PyDict_New();
    } else if (dict->cls == attrwrapper_cls) {
        auto new_dict = PyDict_New();
        PyDict_Update(new_dict, dict);
        Py_DECREF(dict);
        dict = new_dict;
    } else if (!PyDict_Check(dict)) {
        Py_DECREF(dict);
        dict = PyDict_New();
    } else {
        /* Copy __dict__ to avoid mutating it. */
        PyObject* temp = PyDict_Copy(dict);
        Py_DECREF(dict);
        dict = temp;
    }

    if (dict == NULL)
        goto error;

    /* Merge in attrs reachable from its class. */
    itsclass = PyObject_GetAttrString(obj, "__class__");
    if (itsclass == NULL)
        /* XXX(tomer): Perhaps fall back to obj->ob_type if no
                       __class__ exists? */
        PyErr_Clear();
    else {
        if (merge_class_dict(dict, itsclass) != 0)
            goto error;
    }

    result = PyDict_Keys(dict);
/* fall through */
error:
    Py_XDECREF(itsclass);
    Py_XDECREF(dict);
    return result;
}

/* Helper for PyObject_Dir: object introspection.
   This calls one of the above specialized versions if no __dir__ method
   exists. */
static PyObject* _dir_object(PyObject* obj) noexcept {
    PyObject* result = NULL;
    static PyObject* dir_str = NULL;
    PyObject* dirfunc;

    assert(obj);
    dirfunc = _PyObject_LookupSpecial(obj, "__dir__", &dir_str);
    if (PyErr_Occurred())
        return NULL;
    if (dirfunc == NULL) {
        /* use default implementation */
        if (PyModule_Check(obj))
            result = _specialized_dir_module(obj);
        else if (PyType_Check(obj))
            result = _specialized_dir_type(obj);
        else
            result = _generic_dir(obj);
    } else {
        /* use __dir__ */
        result = PyObject_CallFunctionObjArgs(dirfunc, NULL);
        Py_DECREF(dirfunc);
        if (result == NULL)
            return NULL;

        /* Like CPython 3, accept any iterable and turn it into a list */
        PyObject* list = PySequence_List(result);
        Py_DECREF(result);
        result = list;
    }

    return result;
}

/* Implementation of dir() -- if obj is NULL, returns the names in the current
   (local) scope.  Otherwise, performs introspection of the object: returns a
   sorted list of attribute names (supposedly) accessible from the object
*/
extern "C" PyObject* PyObject_Dir(PyObject* obj) noexcept {
    PyObject* result;

    if (obj == NULL)
        /* no object -- introspect the locals */
        result = _dir_locals();
    else
        /* object -- introspect the object */
        result = _dir_object(obj);

    assert(result == NULL || PyList_Check(result));

    if (result != NULL && PyList_Sort(result) != 0) {
        /* sorting the list failed */
        Py_CLEAR(result);
    }

    return result;
}

extern "C" PyObject* PyObject_Repr(PyObject* v) noexcept {
    PyObject* res;
    if (PyErr_CheckSignals())
        return NULL;
#ifdef USE_STACKCHECK
    if (PyOS_CheckStack()) {
        PyErr_SetString(PyExc_MemoryError, "stack overflow");
        return NULL;
    }
#endif
    if (v == NULL)
        return PyUnicode_FromString("<NULL>");
    if (Py_TYPE(v)->tp_repr == NULL)
        return PyUnicode_FromFormat("<%s object at %p>", Py_TYPE(v)->tp_name, v);

    /* It is possible for a type to have a tp_repr representation that loops
       infinitely. */
    if (Py_EnterRecursiveCall(" while getting the repr of an object"))
        return NULL;
    res = (*Py_TYPE(v)->tp_repr)(v);
    Py_LeaveRecursiveCall();
    if (res == NULL)
        return NULL;
    if (!PyUnicode_Check(res)) {
        PyErr_Format(PyExc_TypeError, "__repr__ returned non-string (type %.200s)", Py_TYPE(res)->tp_name);
        Py_DECREF(res);
        return NULL;
    }
    return res;
}

extern "C" PyObject* PyObject_Str(PyObject* v) noexcept {
    PyObject* res;
    if (PyErr_CheckSignals())
        return NULL;
    if (v == NULL)
        return PyUnicode_FromString("<NULL>");
    if (PyUnicode_CheckExact(v)) {
        Py_INCREF(v);
        return v;
    }
    if (Py_TYPE(v)->tp_str == NULL)
        return PyObject_Repr(v);

    /* It is possible for a type to have a tp_str representation that loops
       infinitely. */
    if (Py_EnterRecursiveCall(" while getting the str of an object"))
        return NULL;
    res = (*Py_TYPE(v)->tp_str)(v);
    Py_LeaveRecursiveCall();
    if (res == NULL)
        return NULL;
    if (!PyUnicode_Check(res)) {
        PyErr_Format(PyExc_TypeError, "__str__ returned non-string (type %.200s)", Py_TYPE(res)->tp_name);
        Py_DECREF(res);
        return NULL;
    }
    return res;
}

extern "C" PyObject* PyObject_ASCII(PyObject* v) noexcept {
    PyObject *repr, *ascii, *res;

    repr = PyObject_Repr(v);
    if (repr == NULL)
        return NULL;

    if (PyUnicode_IS_ASCII(repr))
        return repr;

    /* repr is guaranteed to be a PyUnicode object by PyObject_Repr */
    ascii = _PyUnicode_AsASCIIString(repr, "backslashreplace");
    Py_DECREF(repr);
    if (ascii == NULL)
        return NULL;

    res = PyUnicode_DecodeASCII(PyBytes_AS_STRING(ascii), PyBytes_GET_SIZE(ascii), NULL);

    Py_DECREF(ascii);
    return res;
}

extern "C" PyObject* PyObject_Bytes(PyObject* v) noexcept {
    static PyObject* bytes_str = NULL;
    PyObject *result, *func;

    if (v == NULL)
        return PyBytes_FromString("<NULL>");

    if (PyBytes_CheckExact(v)) {
        Py_INCREF(v);
        return v;
    }

    func = _PyObject_LookupSpecial(v, "__bytes__", &bytes_str);
    if (func != NULL) {
        result = _PyObject_CallNoArg(func);
        Py_DECREF(func);
        if (result == NULL)
            return NULL;
        if (!PyBytes_Check(result)) {
            PyErr_Format(PyExc_TypeError, "__bytes__ returned non-bytes (type %.200s)", Py_TYPE(result)->tp_name);
            Py_DECREF(result);
            return NULL;
        }
        return result;
    } else if (PyErr_Occurred())
        return NULL;
    return PyBytes_FromObject(v);
}

extern "C" PyObject* PyObject_SelfIter(PyObject* obj) noexcept {
    Py_INCREF(obj);
    return obj;
}

extern "C" int PyObject_GenericSetAttr(PyObject* obj, PyObject* name, PyObject* value) noexcept {
    if (!PyUnicode_Check(name)) {
        if (PyUnicode_Check(name)) {
            name = PyUnicode_AsEncodedString(name, NULL, NULL);
            if (name == NULL)
                return -1;
        } else {
            PyErr_Format(PyExc_TypeError, "attribute name must be string, not '%.200s'", Py_TYPE(name)->tp_name);
            return -1;
        }
    } else {
        Py_INCREF(name);
    }
    AUTO_DECREF(name);

    BoxedString* str = static_cast<BoxedString*>(name);
    incref(str);
    internStringMortalInplace(str);
    AUTO_DECREF(str);

    assert(PyUnicode_Check(name));
    try {
        if (value == NULL)
            delattrGeneric(obj, str, NULL);
        else
            setattrGeneric<NOT_REWRITABLE>(obj, str, incref(value), NULL);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
    return 0;
}

extern "C" int PyObject_SetAttr(PyObject* obj, PyObject* name, PyObject* value) noexcept {
    if (!PyUnicode_Check(name)) {
        if (PyUnicode_Check(name)) {
            name = PyUnicode_AsEncodedString(name, NULL, NULL);
            if (name == NULL)
                return -1;
        } else {
            PyErr_Format(PyExc_TypeError, "attribute name must be string, not '%.200s'", Py_TYPE(name)->tp_name);
            return -1;
        }
    }

    BoxedString* name_str = static_cast<BoxedString*>(name);
    Py_INCREF(name_str);
    internStringMortalInplace(name_str);
    AUTO_DECREF(name_str);

    assert(PyUnicode_Check(name));
    try {
        if (value == NULL)
            delattr(obj, name_str);
        else
            setattr(obj, name_str, incref(value));
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
    return 0;
}

extern "C" int PyObject_SetAttrString(PyObject* v, const char* name, PyObject* w) noexcept {
    try {
        setattr(v, autoDecref(internStringMortal(name)), incref(w));
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
    return 0;
}

extern "C" PyObject* PyObject_GetAttrString(PyObject* o, const char* attr) noexcept {
    try {
        Box* r = getattrInternal<ExceptionStyle::CXX>(o, autoDecref(internStringMortal(attr)));
        if (!r)
            PyErr_Format(PyExc_AttributeError, "'%.50s' object has no attribute '%.400s'", o->cls->tp_name, attr);
        return r;
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" int PyObject_HasAttr(PyObject* v, PyObject* name) noexcept {
    PyObject* res = PyObject_GetAttr(v, name);
    if (res != NULL) {
        Py_DECREF(res);
        return 1;
    }
    PyErr_Clear();
    return 0;
}

extern "C" int PyObject_HasAttrString(PyObject* v, const char* name) noexcept {
    PyObject* res = PyObject_GetAttrString(v, name);
    if (res != NULL) {
        Py_DECREF(res);
        return 1;
    }
    PyErr_Clear();
    return 0;
}

// I'm not sure how we can support this one:
extern "C" PyObject** _PyObject_GetDictPtr(PyObject* obj) noexcept {
    Py_ssize_t dictoffset;
    PyTypeObject* tp = Py_TYPE(obj);

    if (!tp->instancesHaveHCAttrs()) {
        dictoffset = tp->tp_dictoffset;
        if (dictoffset == 0)
            return NULL;
        if (dictoffset < 0) {
            Py_ssize_t tsize;
            size_t size;

            tsize = ((PyVarObject*)obj)->ob_size;
            if (tsize < 0)
                tsize = -tsize;
            size = _PyObject_VAR_SIZE(tp, tsize);

            dictoffset += (long)size;
            assert(dictoffset > 0);
            assert(dictoffset % SIZEOF_VOID_P == 0);
        }
        return (PyObject**)((char*)obj + dictoffset);
    } else {
        fatalOrError(PyExc_NotImplementedError, "unimplemented for hcattrs");
        return nullptr;
    }
}

// This function returns the copy of object's dict, not a writable dict.
extern "C" PyObject* PyObject_GetDictCopy(PyObject* obj) noexcept {
    PyTypeObject* tp = Py_TYPE(obj);
    if (!tp->instancesHaveHCAttrs()) {
        if (!tp->instancesHaveDictAttrs())
            return NULL;
        return PyDict_Copy(obj->getDict());
    } else {
        Box* attrwrapper = obj->getAttrWrapper();
        // unwritable dict, this is just a copy of object's dict.
        return attrwrapperToDict(attrwrapper);
    }
}

extern "C" void PyObject_ClearDict(PyObject* obj) noexcept {
    obj->clearAttrsForDealloc();
}

// This API just add or update key-value pairs. It will not remove existed items
// if the key is not contained in the new dict.
extern "C" void PyObject_UpdateDict(PyObject* obj, PyObject* dict) noexcept {
    PyObject* d_key, *d_value;
    Py_ssize_t i = 0;
    while (PyDict_Next(dict, &i, &d_key, &d_value)) {
        if (PyObject_SetAttr(obj, d_key, d_value) < 0)
            return;
    }
}

/* These methods are used to control infinite recursion in repr, str, print,
   etc.  Container objects that may recursively contain themselves,
   e.g. builtin dictionaries and lists, should used Py_ReprEnter() and
   Py_ReprLeave() to avoid infinite recursion.

   Py_ReprEnter() returns 0 the first time it is called for a particular
   object and 1 every time thereafter.  It returns -1 if an exception
   occurred.  Py_ReprLeave() has no return value.

   See dictobject.c and listobject.c for examples of use.
*/

#define KEY "Py_Repr"

extern "C" int Py_ReprEnter(PyObject* obj) noexcept {
    PyObject* dict;
    PyObject* list;
    Py_ssize_t i;

    dict = PyThreadState_GetDict();
    if (dict == NULL)
        return 0;
    list = PyDict_GetItemString(dict, KEY);
    if (list == NULL) {
        list = PyList_New(0);
        if (list == NULL)
            return -1;
        if (PyDict_SetItemString(dict, KEY, list) < 0)
            return -1;
        Py_DECREF(list);
    }
    i = PyList_GET_SIZE(list);
    while (--i >= 0) {
        if (PyList_GET_ITEM(list, i) == obj)
            return 1;
    }
    PyList_Append(list, obj);
    return 0;
}

extern "C" void Py_ReprLeave(PyObject* obj) noexcept {
    PyObject* dict;
    PyObject* list;
    Py_ssize_t i;

    dict = PyThreadState_GetDict();
    if (dict == NULL)
        return;
    list = PyDict_GetItemString(dict, KEY);
    if (list == NULL || !PyList_Check(list))
        return;
    i = PyList_GET_SIZE(list);
    /* Count backwards because we always expect obj to be list[-1] */
    while (--i >= 0) {
        if (PyList_GET_ITEM(list, i) == obj) {
            PyList_SetSlice(list, i, i + 1, NULL);
            break;
        }
    }
}

/* Map rich comparison operators to their swapped version, e.g. LT <--> GT */
extern "C" {
int _Py_SwappedOp[] = { Py_GT, Py_GE, Py_EQ, Py_NE, Py_LT, Py_LE };
}

static const char* const opstrings[] = { "<", "<=", "==", "!=", ">", ">=" };

/* Return (new reference to) Py_True or Py_False. */
PyObject* convert_3way_to_object(int op, int c) noexcept {
    PyObject* result;
    switch (op) {
        case Py_LT:
            c = c < 0;
            break;
        case Py_LE:
            c = c <= 0;
            break;
        case Py_EQ:
            c = c == 0;
            break;
        case Py_NE:
            c = c != 0;
            break;
        case Py_GT:
            c = c > 0;
            break;
        case Py_GE:
            c = c >= 0;
            break;
    }
    result = c ? Py_True : Py_False;
    Py_INCREF(result);
    return result;
}

/* What to do when neither side implements the comparison: == and != fall back to identity,
   the other operators raise a TypeError. */
PyObject* richcompare_fallback(PyObject* v, PyObject* w, int op) noexcept {
    PyObject* res;
    switch (op) {
        case Py_EQ:
            res = (v == w) ? Py_True : Py_False;
            break;
        case Py_NE:
            res = (v != w) ? Py_True : Py_False;
            break;
        default:
            PyErr_Format(PyExc_TypeError, "'%s' not supported between instances of '%.100s' and '%.100s'",
                         opstrings[op], v->cls->tp_name, w->cls->tp_name);
            return NULL;
    }
    Py_INCREF(res);
    return res;
}

/* Perform a rich comparison, raising TypeError when the requested comparison
   operator is not supported. */
static PyObject* do_richcompare(PyObject* v, PyObject* w, int op) noexcept {
    richcmpfunc f;
    PyObject* res;
    int checked_reverse_op = 0;

    if (_PyPyston_ReflectedFirst(w->cls, v->cls) && (f = w->cls->tp_richcompare) != NULL) {
        checked_reverse_op = 1;
        res = (*f)(w, v, _Py_SwappedOp[op]);
        if (res != Py_NotImplemented)
            return res;
        Py_DECREF(res);
    }
    if ((f = v->cls->tp_richcompare) != NULL) {
        res = (*f)(v, w, op);
        if (res != Py_NotImplemented)
            return res;
        Py_DECREF(res);
    }
    if (!checked_reverse_op && (f = w->cls->tp_richcompare) != NULL) {
        res = (*f)(w, v, _Py_SwappedOp[op]);
        if (res != Py_NotImplemented)
            return res;
        Py_DECREF(res);
    }
    return richcompare_fallback(v, w, op);
}

/* Perform a rich comparison with object result.  This wraps do_richcompare()
   with a check for NULL arguments and a recursion check. */
extern "C" PyObject* PyObject_RichCompare(PyObject* v, PyObject* w, int op) noexcept {
    PyObject* res;

    assert(Py_LT <= op && op <= Py_GE);
    if (v == NULL || w == NULL) {
        if (!PyErr_Occurred())
            PyErr_BadInternalCall();
        return NULL;
    }
    if (Py_EnterRecursiveCall(" in comparison"))
        return NULL;
    res = do_richcompare(v, w, op);
    Py_LeaveRecursiveCall();
    return res;
}

/* Return -1 if error; 1 if v op w; 0 if not (v op w). */
extern "C" int PyObject_RichCompareBool(PyObject* v, PyObject* w, int op) noexcept {
    PyObject* res;
    int ok;

    /* Quick result when objects are the same.
       Guarantees that identity implies equality. */
    if (v == w) {
        if (op == Py_EQ)
            return 1;
        else if (op == Py_NE)
            return 0;
    }

    res = PyObject_RichCompare(v, w, op);
    if (res == NULL)
        return -1;
    if (PyBool_Check(res))
        ok = (res == Py_True);
    else
        ok = PyObject_IsTrue(res);
    Py_DECREF(res);
    return ok;
}

#ifdef Py_REF_DEBUG
extern "C" {
Py_ssize_t _Py_RefTotal;
}

extern "C" Py_ssize_t _Py_GetRefTotal(void) noexcept {
    PyObject* o;
    Py_ssize_t total = _Py_RefTotal;
/* ignore the references to the dummy object of the dicts and sets
 *        because they are not reliable and not useful (now that the
 *               hash table code is well-tested) */

// Pyston change: not sure what these are
#if 0
    o = _PyDict_Dummy();
    if (o != NULL)
        total -= o->ob_refcnt;
    o = _PySet_Dummy();
    if (o != NULL)
        total -= o->ob_refcnt;
#endif
    return total;
}
#endif /* Py_REF_DEBUG */

#ifdef Py_REF_DEBUG
/* Log a fatal error; doesn't return. */
extern "C" void _Py_NegativeRefcount(const char* fname, int lineno, PyObject* op) noexcept {
    char buf[300];

    PyOS_snprintf(buf, sizeof(buf), "%s:%i object at %p has negative ref count "
                                    "%" PY_FORMAT_SIZE_T "d.  \033[40mwatch -l *(long*)%p\033[0m",
                  fname, lineno, op, op->ob_refcnt, &op->ob_refcnt);
    Py_FatalError(buf);
}

#endif /* Py_REF_DEBUG */

extern "C" {
int _PyTrash_delete_nesting = 0;
PyObject* _PyTrash_delete_later = NULL;
}

extern "C" void _PyTrash_thread_deposit_object(PyObject* op) noexcept {
    PyThreadState* tstate = PyThreadState_GET();
    assert(PyObject_IS_GC(op));
    assert(!_PyObject_GC_IS_TRACKED(op));
    assert(op->ob_refcnt == 0);
    _PyGCHead_SET_PREV(_Py_AS_GC(op), (PyGC_Head*)tstate->trash_delete_later);
    tstate->trash_delete_later = op;
}

extern "C" void _PyTrash_thread_destroy_chain() noexcept {
    PyThreadState* tstate = PyThreadState_GET();
    while (tstate->trash_delete_later) {
        PyObject* op = tstate->trash_delete_later;
        destructor dealloc = Py_TYPE(op)->tp_dealloc;

        tstate->trash_delete_later = (PyObject*)_PyGCHead_PREV(_Py_AS_GC(op));

        /* Call the deallocator directly.  This used to try to
         * fool Py_DECREF into calling it indirectly, but
         * Py_DECREF was already called on this object, and in
         * assorted non-release builds calling Py_DECREF again ends
         * up distorting allocation statistics.
         */
        assert(op->ob_refcnt == 0);
        ++tstate->trash_delete_nesting;
        (*dealloc)(op);
        --tstate->trash_delete_nesting;
    }
}

#ifdef Py_TRACE_REFS
/* Head of circular doubly-linked list of all objects.  These are linked
 * together via the _ob_prev and _ob_next members of a PyObject, which
 * exist only in a Py_TRACE_REFS build.
 */
extern "C" {
// static PyObject refchain = { &refchain, &refchain };
PyObject refchain(Box::createRefchain());
}

/* Insert op at the front of the list of all objects.  If force is true,
 * op is added even if _ob_prev and _ob_next are non-NULL already.  If
 * force is false amd _ob_prev or _ob_next are non-NULL, do nothing.
 * force should be true if and only if op points to freshly allocated,
 * uninitialized memory, or you've unlinked op from the list and are
 * relinking it into the front.
 * Note that objects are normally added to the list via _Py_NewReference,
 * which is called by PyObject_Init.  Not all objects are initialized that
 * way, though; exceptions include statically allocated type objects, and
 * statically allocated singletons (like Py_True and Py_None).
 */
extern "C" void _Py_AddToAllObjects(PyObject* op, int force) noexcept {
#ifdef Py_DEBUG
    if (!force) {
        /* If it's initialized memory, op must be in or out of
         * the list unambiguously.
         */
        assert((op->_ob_prev == NULL) == (op->_ob_next == NULL));
    }
#endif
    if (force || op->_ob_prev == NULL) {
        op->_ob_next = refchain._ob_next;
        op->_ob_prev = &refchain;
        refchain._ob_next->_ob_prev = op;
        refchain._ob_next = op;
    }
}
#endif /* Py_TRACE_REFS */

#ifdef Py_TRACE_REFS

extern "C" void _Py_NewReference(PyObject* op) noexcept {
    _Py_INC_REFTOTAL;
    op->ob_refcnt = 1;
    _Py_AddToAllObjects(op, 1);
    _Py_INC_TPALLOCS(op);
}

extern "C" void _Py_ForgetReference(register PyObject* op) noexcept {
#ifdef SLOW_UNREF_CHECK
    register PyObject* p;
#endif
    if (op->ob_refcnt < 0)
        Py_FatalError("UNREF negative refcnt");
    if (op == &refchain || op->_ob_prev->_ob_next != op || op->_ob_next->_ob_prev != op)
        Py_FatalError("UNREF invalid object");
#ifdef SLOW_UNREF_CHECK
    for (p = refchain._ob_next; p != &refchain; p = p->_ob_next) {
        if (p == op)
            break;
    }
    if (p == &refchain) /* Not found */
        Py_FatalError("UNREF unknown object");
#endif
    op->_ob_next->_ob_prev = op->_ob_prev;
    op->_ob_prev->_ob_next = op->_ob_next;
    op->_ob_next = op->_ob_prev = NULL;
    _Py_INC_TPFREES(op);
}

#undef _Py_Dealloc
extern "C" void _Py_Dealloc(PyObject* op) noexcept {
    destructor dealloc = Py_TYPE(op)->tp_dealloc;
    _Py_ForgetReference(op);
    (*dealloc)(op);
}

/* Print all live objects.  Because PyObject_Print is called, the
 * interpreter must be in a healthy state.
 */
extern "C" void _Py_PrintReferences(FILE* fp) noexcept {
    PyObject* op;
    fprintf(fp, "Remaining objects:\n");
    for (op = refchain._ob_next; op != &refchain; op = op->_ob_next) {
        fprintf(fp, "%p [%" PY_FORMAT_SIZE_T "d] ", op, op->ob_refcnt);
        if (PyObject_Print(op, fp, 0) != 0)
            PyErr_Clear();
        putc('\n', fp);
    }
}

/* Print the addresses of all live objects.  Unlike _Py_PrintReferences, this
 * doesn't make any calls to the Python C API, so is always safe to call.
 */
extern "C" void _Py_PrintReferenceAddressesCapped(FILE* fp, int max_to_print) noexcept;

extern "C" void _Py_PrintReferenceAddresses(FILE* fp) noexcept {
    _Py_PrintReferenceAddressesCapped(fp, INT_MAX);
}

extern "C" void _Py_PrintReferenceAddressesCapped(FILE* fp, int max_to_print) noexcept {
    PyObject* op;
    fprintf(fp, "Remaining object addresses:\n");
    int found = 0;
    for (op = refchain._ob_next; op != &refchain; op = op->_ob_next) {
        found++;
        if (found <= max_to_print) {
            fprintf(fp, "%p [%" PY_FORMAT_SIZE_T "d] %s     \033[40mwatch -l ((PyObject*)%p)->ob_refcnt\033[0m\n", op,
                    op->ob_refcnt, Py_TYPE(op)->tp_name, op);
        }
    }
    if (found > max_to_print) {
        fprintf(fp, "%d more found (but not printed)\n", found - max_to_print);
    }
}

extern "C" PyObject* _Py_GetObjects(PyObject* self, PyObject* args) noexcept {
    int i, n;
    PyObject* t = NULL;
    PyObject* res, *op;

    if (!PyArg_ParseTuple(args, "i|O", &n, &t))
        return NULL;
    op = refchain._ob_next;
    res = PyList_New(0);
    if (res == NULL)
        return NULL;
    for (i = 0; (n == 0 || i < n) && op != &refchain; i++) {
        while (op == self || op == args || op == res || op == t || (t != NULL && Py_TYPE(op) != (PyTypeObject*)t)) {
            op = op->_ob_next;
            if (op == &refchain)
                return res;
        }
        if (PyList_Append(res, op) < 0) {
            Py_DECREF(res);
            return NULL;
        }
        op = op->_ob_next;
    }
    return res;
}

#endif
}

namespace pyston {

#ifndef Py_TRACE_REFS
#undef _Py_Dealloc
extern "C" void _Py_Dealloc(PyObject* op) noexcept {
    destructor dealloc = Py_TYPE(op)->tp_dealloc;
    (*dealloc)(op);
}
#endif

// The tracemalloc module isn't built, so tracing is never turned on.
extern "C" int _PyTraceMalloc_NewReference(PyObject* op) noexcept {
    return 0;
}

#ifdef Py_REF_DEBUG
extern "C" void _PyDebug_PrintTotalRefs(void) noexcept {
    fprintf(stderr, "[%" PY_FORMAT_SIZE_T "d refs]\n", _Py_GetRefTotal());
}
#endif

extern "C" int _PyObject_IsFreed(PyObject* op) noexcept {
    // TODO: CPython checks for the debug allocator's dead byte patterns here.
    return 0;
}

extern "C" void _PyObject_AssertFailed(PyObject* obj, const char* expr, const char* msg, const char* file, int line,
                                       const char* function) noexcept {
    fprintf(stderr, "%s:%d: ", file, line);
    if (function)
        fprintf(stderr, "%s: ", function);
    fflush(stderr);
    if (expr)
        fprintf(stderr, "Assertion \"%s\" failed", expr);
    else
        fprintf(stderr, "Assertion failed");
    if (msg)
        fprintf(stderr, ": %s", msg);
    fprintf(stderr, "\n");
    fflush(stderr);
    if (obj == NULL)
        fprintf(stderr, "<object at NULL>\n");
    else
        fprintf(stderr, "object address  : %p\nobject type     : %s\n", obj, Py_TYPE(obj)->tp_name);
    Py_FatalError("_PyObject_AssertFailed");
}

extern "C" void PyObject_CallFinalizer(PyObject* self) noexcept {
    PyTypeObject* tp = Py_TYPE(self);

    if (tp->tp_finalize == NULL)
        return;
    /* tp_finalize should only be called once. */
    if (PyType_IS_GC(tp) && _PyGC_FINALIZED(self))
        return;

    tp->tp_finalize(self);
    if (PyType_IS_GC(tp)) {
        _PyGC_SET_FINALIZED(self);
    }
}

extern "C" int PyObject_CallFinalizerFromDealloc(PyObject* self) noexcept {
    if (self->ob_refcnt != 0) {
        Py_FatalError("PyObject_CallFinalizerFromDealloc called on object with a non-zero refcount");
    }

    /* Temporarily resurrect the object. */
    self->ob_refcnt = 1;

    PyObject_CallFinalizer(self);

    assert(self->ob_refcnt > 0);
    if (--self->ob_refcnt == 0) {
        return 0; /* this is the normal path out */
    }

    /* tp_finalize resurrected it! */
    Py_ssize_t refcnt = self->ob_refcnt;
    _Py_NewReference(self);
    self->ob_refcnt = refcnt;
    return -1;
}

extern "C" PyObject* PyObject_GenericGetDict(PyObject* obj, void* context) noexcept {
    if (obj->cls->instancesHaveHCAttrs())
        return incref(obj->getAttrWrapper());

    PyObject** dictptr = _PyObject_GetDictPtr(obj);
    if (dictptr == NULL) {
        PyErr_SetString(PyExc_AttributeError, "This object has no __dict__");
        return NULL;
    }
    PyObject* dict = *dictptr;
    if (dict == NULL) {
        *dictptr = dict = PyDict_New();
    }
    Py_XINCREF(dict);
    return dict;
}

extern "C" int PyObject_GenericSetDict(PyObject* obj, PyObject* value, void* context) noexcept {
    if (value == NULL) {
        PyErr_SetString(PyExc_TypeError, "cannot delete __dict__");
        return -1;
    }
    if (!PyDict_Check(value)) {
        PyErr_Format(PyExc_TypeError, "__dict__ must be set to a dictionary, not a '%.200s'", Py_TYPE(value)->tp_name);
        return -1;
    }

    if (obj->cls->instancesHaveHCAttrs()) {
        Py_INCREF(value);
        obj->setDictBacked(value);
        return 0;
    }

    PyObject** dictptr = _PyObject_GetDictPtr(obj);
    if (dictptr == NULL) {
        PyErr_SetString(PyExc_AttributeError, "This object has no __dict__");
        return -1;
    }
    Py_INCREF(value);
    Py_XSETREF(*dictptr, value);
    return 0;
}

extern "C" int _PyObject_LookupAttr(PyObject* v, PyObject* name, PyObject** result) noexcept {
    *result = PyObject_GetAttr(v, name);
    if (*result != NULL)
        return 1;
    if (!PyErr_ExceptionMatches(PyExc_AttributeError))
        return -1;
    PyErr_Clear();
    return 0;
}

extern "C" int _PyObject_LookupAttrId(PyObject* v, _Py_Identifier* name, PyObject** result) noexcept {
    PyObject* oname = _PyUnicode_FromId(name); /* borrowed */
    if (!oname) {
        *result = NULL;
        return -1;
    }
    return _PyObject_LookupAttr(v, oname, result);
}

extern "C" PyObject* _PyObject_GetAttrId(PyObject* v, _Py_Identifier* name) noexcept {
    PyObject* oname = _PyUnicode_FromId(name); /* borrowed */
    if (!oname)
        return NULL;
    return PyObject_GetAttr(v, oname);
}

extern "C" int _PyObject_HasAttrId(PyObject* v, _Py_Identifier* name) noexcept {
    PyObject* oname = _PyUnicode_FromId(name); /* borrowed */
    if (!oname)
        return -1;
    return PyObject_HasAttr(v, oname);
}

extern "C" int _PyObject_SetAttrId(PyObject* v, _Py_Identifier* name, PyObject* w) noexcept {
    PyObject* oname = _PyUnicode_FromId(name); /* borrowed */
    if (!oname)
        return -1;
    return PyObject_SetAttr(v, oname, w);
}

extern "C" int _PyObject_GetMethod(PyObject* obj, PyObject* name, PyObject** method) noexcept {
    // TODO: CPython avoids creating a bound method here.
    *method = PyObject_GetAttr(obj, name);
    return 0;
}

extern "C" int _PyObject_IsAbstract(PyObject* obj) noexcept {
    int res;
    PyObject* isabstract;

    if (obj == NULL)
        return 0;

    res = _PyObject_LookupAttr(obj, autoDecref(internStringMortal("__isabstractmethod__")), &isabstract);
    if (res > 0) {
        res = PyObject_IsTrue(isabstract);
        Py_DECREF(isabstract);
    }
    return res;
}
}
