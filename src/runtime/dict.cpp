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

#include "runtime/dict.h"

#include "capi/typeobject.h"
#include "capi/types.h"
#include "core/common.h"
#include "core/stats.h"
#include "core/types.h"
#include "runtime/ics.h"
#include "runtime/inline/list.h"
#include "runtime/objmodel.h"
#include "runtime/set.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

extern "C" {
BoxedClass* dictiterkey_cls = &PyDictIterKey_Type;
BoxedClass* dictitervalue_cls = &PyDictIterValue_Type;
BoxedClass* dictiteritem_cls = &PyDictIterItem_Type;
}

static void _dictSetStolen(BoxedDict* self, BoxAndHash k, STOLEN(Box*) v) {
    Box** slot = NULL;
    try {
        slot = &self->d[k];
    } catch (ExcInfo e) {
        Py_DECREF(v);
        throw e;
    }
    Box* old_val = *slot;

    *slot = v;

    if (old_val) {
        Py_DECREF(old_val);
    } else {
        Py_INCREF(k.value);
    }
}

static void _dictSetStolen(BoxedDict* self, Box* k, STOLEN(Box*) v) {
    BoxAndHash hashed_key;
    try {
        hashed_key = BoxAndHash(k);
    } catch (ExcInfo e) {
        Py_DECREF(v);
        throw e;
    }
    return _dictSetStolen(self, hashed_key, v);
}

static void _dictSet(BoxedDict* self, BoxAndHash k, Box* v) {
    _dictSetStolen(self, k, incref(v));
}

extern "C" void dictSetInternal(Box* self, STOLEN(Box*) k, STOLEN(Box*) v) {
    assert(self->cls == dict_cls);
    AUTO_DECREF(v);
    AUTO_DECREF(k);

    _dictSet(static_cast<BoxedDict*>(self), k, v);
}

Box* dictRepr(BoxedDict* self) {
    std::vector<char> chars;
    int status = Py_ReprEnter((PyObject*)self);
    if (status != 0) {
        if (status < 0)
            throwCAPIException();

        chars.push_back('{');
        chars.push_back('.');
        chars.push_back('.');
        chars.push_back('.');
        chars.push_back('}');
        return boxString(llvm::StringRef(&chars[0], chars.size()));
    }

    try {
        chars.push_back('{');
        bool first = true;
        for (const auto& p : *self) {
            if (!first) {
                chars.push_back(',');
                chars.push_back(' ');
            }
            first = false;
            BoxedString* k = static_cast<BoxedString*>(repr(p.first));
            AUTO_DECREF(k);
            BoxedString* v = static_cast<BoxedString*>(repr(p.second));
            AUTO_DECREF(v);
            chars.insert(chars.end(), k->s().begin(), k->s().end());
            chars.push_back(':');
            chars.push_back(' ');
            chars.insert(chars.end(), v->s().begin(), v->s().end());
        }
        chars.push_back('}');
    } catch (ExcInfo e) {
        Py_ReprLeave((PyObject*)self);
        throw e;
    }
    Py_ReprLeave((PyObject*)self);
    return boxString(llvm::StringRef(&chars[0], chars.size()));
}

Box* dictCopy(BoxedDict* self) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor 'copy' requires a 'dict' object but received a '%s'", getTypeName(self));

    BoxedDict* r = new BoxedDict();
    for (auto&& p : self->d) {
        Py_INCREF(p.first.value);
        Py_INCREF(p.second);
    }
    r->d.copyFrom(self->d);
    return r;
}

Box* dictItems(BoxedDict* self) {
    BoxedList* rtn = new BoxedList();

    rtn->ensure(self->d.size());
    for (const auto& p : *self) {
        BoxedTuple* t = BoxedTuple::create({ p.first, p.second });
        listAppendInternalStolen(rtn, t);
    }

    return rtn;
}

Box* dictValues(BoxedDict* self) {
    BoxedList* rtn = new BoxedList();
    rtn->ensure(self->d.size());
    for (const auto& p : *self) {
        listAppendInternal(rtn, p.second);
    }
    return rtn;
}

Box* dictKeys(BoxedDict* self) {
    RELEASE_ASSERT(PyDict_Check(self), "");

    BoxedList* rtn = new BoxedList();
    rtn->ensure(self->d.size());
    for (const auto& p : *self) {
        listAppendInternal(rtn, p.first);
    }
    return rtn;
}

static PyObject* dict_helper(PyObject* mp, std::function<Box*(BoxedDict*)> f) noexcept {
    if (mp == NULL || !PyDict_Check(mp)) {
        PyErr_BadInternalCall();
        return NULL;
    }

    try {
        return f(static_cast<BoxedDict*>(mp));
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" PyObject* PyDict_Keys(PyObject* mp) noexcept {
    return dict_helper(mp, dictKeys);
}

extern "C" PyObject* PyDict_Values(PyObject* mp) noexcept {
    return dict_helper(mp, dictValues);
}

extern "C" PyObject* PyDict_Items(PyObject* mp) noexcept {
    return dict_helper(mp, dictItems);
}

// Analoguous to CPython's, used for sq_ slots.
static Py_ssize_t dict_length(PyDictObject* mp) {
    return ((BoxedDict*)mp)->d.size();
}

Box* dictLen(BoxedDict* self) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor '__len__' requires a 'dict' object but received a '%s'",
                       getTypeName(self));

    return boxInt(self->d.size());
}

extern "C" Py_ssize_t PyDict_Size(PyObject* op) noexcept {
    if (op->cls == attrwrapper_cls)
        return PyObject_Size(op);

    RELEASE_ASSERT(PyDict_Check(op), "");
    return static_cast<BoxedDict*>(op)->d.size();
}

extern "C" void PyDict_Clear(PyObject* op) noexcept {
    if (op->cls == attrwrapper_cls) {
        attrwrapperClear(op);
        return;
    }

    RELEASE_ASSERT(PyDict_Check(op), "");
    // Empty the dict before dropping the references, since that can run code that uses it.
    BoxedDict::DictMap old;
    old.swap(static_cast<BoxedDict*>(op)->d);
    for (auto p : old) {
        Py_DECREF(p.first.value);
        Py_DECREF(p.second);
    }
}

Box* dictClear(BoxedDict* self) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor 'clear' requires a 'dict' object but received a '%s'", getTypeName(self));

    PyDict_Clear(self);
    Py_RETURN_NONE;
}

extern "C" PyObject* PyDict_Copy(PyObject* o) noexcept {
    RELEASE_ASSERT(PyDict_Check(o) || o->cls == attrwrapper_cls, "");
    try {
        if (o->cls == attrwrapper_cls)
            return attrwrapperToDict(o);

        return dictCopy(static_cast<BoxedDict*>(o));
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" int PyDict_Update(PyObject* a, PyObject* b) noexcept {
    return PyDict_Merge(a, b, 1);
}

template <enum ExceptionStyle S> Box* dictGetitem(BoxedDict* self, Box* k) noexcept(S == CAPI) {
    if (!PyDict_Check(self)) {
        if (S == CAPI) {
            PyErr_Format(TypeError, "descriptor '__getitem__' requires a 'dict' object but received a '%s'",
                         getTypeName(self));
            return NULL;
        } else {
            raiseExcHelper(TypeError, "descriptor '__getitem__' requires a 'dict' object but received a '%s'",
                           getTypeName(self));
        }
    }

    BoxedDict::DictMap::iterator it;
    try {
        it = self->d.find(k);
    } catch (ExcInfo e) {
        if (S == CAPI) {
            setCAPIException(e);
            return NULL;
        } else {
            throw e;
        }
    }

    if (it == self->d.end()) {
        // Try calling __missing__ if this is a subclass
        if (self->cls != dict_cls) {
            // Special-case defaultdict, assuming that it's the main time we will actually hit this.
            // We could just use a single runtime IC here, or have a small cache that maps type->runtimeic.
            // Or use a polymorphic runtime ic.
            static BoxedClass* defaultdict_cls = NULL;
            static CallattrIC defaultdict_ic;
            if (defaultdict_cls == NULL && strcmp(self->cls->tp_name, "collections.defaultdict") == 0) {
                defaultdict_cls = self->cls;
            }

            static BoxedString* missing_str = getStaticString("__missing__");
            CallattrFlags callattr_flags{.cls_only = true, .null_on_nonexistent = true, .argspec = ArgPassSpec(1) };
            Box* r;
            try {
                if (self->cls == defaultdict_cls) {
                    r = defaultdict_ic.call(self, missing_str, callattr_flags, k, NULL, NULL, NULL, NULL);
                } else {
                    r = callattr(self, missing_str, callattr_flags, k, NULL, NULL, NULL, NULL);
                }
            } catch (ExcInfo e) {
                if (S == CAPI) {
                    setCAPIException(e);
                    return NULL;
                } else
                    throw e;
            }
            if (r)
                return r;
        }

        if (S == CAPI) {
            PyErr_SetObject(KeyError, autoDecref(BoxedTuple::create1(k)));
            return NULL;
        } else
            raiseExcHelper(KeyError, k);
    }
    return incref(it->second);
}

extern "C" PyObject* PyDict_New() noexcept {
    return new BoxedDict();
}

// We don't assume that dicts passed to PyDict are necessarily dicts, since there are a couple places
// that we provide dict-like objects instead of proper dicts.
// The performance should hopefully be comparable to the CPython fast case, since we can use
// runtimeICs.
extern "C" int PyDict_SetItem(PyObject* mp, PyObject* _key, PyObject* _item) noexcept {
    if (PyDict_Check(mp)) {
        try {
            _dictSet(static_cast<BoxedDict*>(mp), _key, _item);
        } catch (ExcInfo e) {
            setCAPIException(e);
            return -1;
        }
        return 0;
    }

    ASSERT(mp->cls == attrwrapper_cls, "%s", getTypeName(mp));

    try {
        attrwrapperSet(mp, _key, _item);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
    return 0;
}

extern "C" int PyDict_SetItemString(PyObject* mp, const char* key, PyObject* item) noexcept {
    Box* key_s;
    try {
        key_s = boxString(key);
    } catch (ExcInfo e) {
        abort();
    }

    return PyDict_SetItem(mp, autoDecref(key_s), item);
}

extern "C" BORROWED(PyObject*) PyDict_GetItem(PyObject* dict, PyObject* key) noexcept {
    ASSERT(PyDict_Check(dict) || dict->cls == attrwrapper_cls, "%s", getTypeName(dict));
    if (PyDict_Check(dict)) {
        BoxedDict* d = static_cast<BoxedDict*>(dict);

        BoxAndHash h;
        try {
            h = BoxAndHash(key);
        } catch (ExcInfo e) {
            e.clear();
            return NULL;
        }

        /* preserve the existing exception */
        PyObject* err_type, *err_value, *err_tb;
        PyErr_Fetch(&err_type, &err_value, &err_tb);
        Box* b = NULL;
        try {
            b = d->getOrNull(h);
        } catch (ExcInfo e) {
            e.clear();
        }
        /* ignore errors */
        PyErr_Restore(err_type, err_value, err_tb);
        return b;
    }

    assert(dict->cls == attrwrapper_cls);

    auto&& tstate = _PyThreadState_UncheckedGet();
    if (tstate != NULL && tstate->curexc_type != NULL) {
        /* preserve the existing exception */
        PyObject* err_type, *err_value, *err_tb;
        PyErr_Fetch(&err_type, &err_value, &err_tb);
        Box* b = PyObject_GetItem(dict, key);
        /* ignore errors */
        PyErr_Restore(err_type, err_value, err_tb);
        Py_XDECREF(b);
        return b;
    } else {
        Box* b = PyObject_GetItem(dict, key);
        if (b == NULL)
            PyErr_Clear();
        else
            Py_DECREF(b);
        return b;
    }
}

extern "C" BORROWED(PyObject*) PyDict_GetItemWithError(PyObject* dict, PyObject* key) noexcept {
    if (!PyDict_Check(dict)) {
        assert(dict->cls == attrwrapper_cls);
        Box* b = PyObject_GetItem(dict, key);
        if (b == NULL) {
            if (PyErr_ExceptionMatches(PyExc_KeyError))
                PyErr_Clear();
            return NULL;
        }
        Py_DECREF(b);
        return b;
    }

    try {
        return static_cast<BoxedDict*>(dict)->getOrNull(BoxAndHash(key));
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" BORROWED(PyObject*) _PyDict_GetItem_KnownHash(PyObject* dict, PyObject* key, Py_hash_t hash) noexcept {
    return PyDict_GetItemWithError(dict, key);
}

extern "C" int _PyDict_SetItem_KnownHash(PyObject* dict, PyObject* key, PyObject* item, Py_hash_t hash) noexcept {
    return PyDict_SetItem(dict, key, item);
}

extern "C" BORROWED(PyObject*) _PyDict_GetItemId(PyObject* dict, struct _Py_Identifier* key) noexcept {
    PyObject* kv = _PyUnicode_FromId(key); /* borrowed */
    if (kv == NULL) {
        PyErr_Clear();
        return NULL;
    }
    return PyDict_GetItem(dict, kv);
}

extern "C" BORROWED(PyObject*) _PyDict_GetItemIdWithError(PyObject* dict, struct _Py_Identifier* key) noexcept {
    PyObject* kv = _PyUnicode_FromId(key); /* borrowed */
    if (kv == NULL)
        return NULL;
    return PyDict_GetItemWithError(dict, kv);
}

extern "C" BORROWED(PyObject*) _PyDict_GetItemStringWithError(PyObject* dict, const char* key) noexcept {
    PyObject* kv = PyUnicode_FromString(key);
    if (kv == NULL)
        return NULL;
    AUTO_DECREF(kv);
    return PyDict_GetItemWithError(dict, kv);
}

extern "C" int _PyDict_SetItemId(PyObject* dict, struct _Py_Identifier* key, PyObject* item) noexcept {
    PyObject* kv = _PyUnicode_FromId(key); /* borrowed */
    if (kv == NULL)
        return -1;
    return PyDict_SetItem(dict, kv, item);
}

extern "C" int _PyDict_DelItemId(PyObject* dict, struct _Py_Identifier* key) noexcept {
    PyObject* kv = _PyUnicode_FromId(key); /* borrowed */
    if (kv == NULL)
        return -1;
    return PyDict_DelItem(dict, kv);
}

extern "C" int _PyDict_DelItemIf(PyObject* op, PyObject* key, int (*predicate)(PyObject* value)) noexcept {
    PyObject* value = PyDict_GetItemWithError(op, key);
    if (value == NULL) {
        if (PyErr_Occurred())
            return -1;
        _PyErr_SetKeyError(key);
        return -1;
    }
    int res = predicate(value);
    if (res == -1)
        return -1;
    if (res > 0)
        return PyDict_DelItem(op, key);
    return 0;
}

extern "C" int _PyDict_HasOnlyStringKeys(PyObject* dict) noexcept {
    Py_ssize_t pos = 0;
    PyObject* key, *value;
    assert(PyDict_Check(dict));
    while (PyDict_Next(dict, &pos, &key, &value)) {
        if (!PyUnicode_Check(key))
            return 0;
    }
    return 1;
}

extern "C" PyObject* _PyDict_Pop_KnownHash(PyObject* dict, PyObject* key, Py_hash_t hash,
                                           PyObject* deflt) noexcept {
    PyObject* value = PyDict_GetItemWithError(dict, key);
    if (value == NULL) {
        if (PyErr_Occurred())
            return NULL;
        if (deflt) {
            Py_INCREF(deflt);
            return deflt;
        }
        _PyErr_SetKeyError(key);
        return NULL;
    }
    Py_INCREF(value);
    if (PyDict_DelItem(dict, key) < 0) {
        Py_DECREF(value);
        return NULL;
    }
    return value;
}

extern "C" PyObject* _PyDict_Pop(PyObject* dict, PyObject* key, PyObject* deflt) noexcept {
    return _PyDict_Pop_KnownHash(dict, key, 0, deflt);
}

extern "C" PyObject* _PyDict_NewPresized(Py_ssize_t minused) noexcept {
    return PyDict_New();
}

extern "C" int PyDict_ClearFreeList() noexcept {
    return 0;
}

extern "C" int PyDict_Next(PyObject* op, Py_ssize_t* ppos, PyObject** pkey, PyObject** pvalue) noexcept {
    assert(PyDict_Check(op));
    BoxedDict* self = static_cast<BoxedDict*>(op);

    // *ppos is the position in the dict's entry array to continue from.
    auto it = self->d.fromPosition(*ppos);
    if (it == self->d.end())
        return 0;

    if (pkey)
        *pkey = it->first.value;
    if (pvalue)
        *pvalue = it->second;
    *ppos = it.position() + 1;
    return 1;
}

extern "C" int _PyDict_Next(PyObject* op, Py_ssize_t* ppos, PyObject** pkey, PyObject** pvalue,
                            Py_hash_t* phash) noexcept {
    PyObject* key;
    if (!PyDict_Next(op, ppos, &key, pvalue))
        return 0;
    if (pkey)
        *pkey = key;
    if (phash) {
        // TODO: the hash is stored in the dict, but PyDict_Next doesn't give it to us.
        *phash = PyObject_Hash(key);
        assert(*phash != -1);
    }
    return 1;
}

extern "C" BORROWED(PyObject*) PyDict_GetItemString(PyObject* dict, const char* key) noexcept {
    if (dict->cls == attrwrapper_cls)
        return unwrapAttrWrapper(dict)->getattr(autoDecref(internStringMortal(key)));

    Box* key_s;
    try {
        key_s = boxString(key);
    } catch (ExcInfo e) {
        abort();
    }
    return PyDict_GetItem(dict, autoDecref(key_s));
}

Box* dictSetitem(BoxedDict* self, Box* k, Box* v) {
    _dictSet(self, k, v);

    Py_RETURN_NONE;
}

Box* dictDelitem(BoxedDict* self, Box* k) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor '__delitem__' requires a 'dict' object but received a '%s'",
                       getTypeName(self));

    auto it = self->d.find(k);
    if (it == self->d.end()) {
        raiseExcHelper(KeyError, k);
    }

    Box* old_k = it->first.value;
    Box* v = it->second;
    self->d.erase(it);
    Py_DECREF(v);
    Py_DECREF(old_k);

    Py_RETURN_NONE;
}

// Analoguous to CPython's, used for sq_ slots.
static int dict_ass_sub(PyDictObject* mp, PyObject* v, PyObject* w) noexcept {
    try {
        Box* res;
        if (w == NULL) {
            res = dictDelitem((BoxedDict*)mp, v);
        } else {
            res = dictSetitem((BoxedDict*)mp, v, w);
        }
        assert(res == Py_None);
        Py_DECREF(res);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
    return 0;
}

extern "C" int PyDict_DelItem(PyObject* op, PyObject* key) noexcept {
    if (PyDict_Check(op)) {
        BoxedDict* self = static_cast<BoxedDict*>(op);
        decltype(self->d)::iterator it;
        try {
            it = self->d.find(key);
        } catch (ExcInfo e) {
            setCAPIException(e);
            return -1;
        }
        if (it == self->d.end()) {
            PyErr_SetObject(KeyError, autoDecref(BoxedTuple::create1(key)));
            return -1;
        }

        Box* v = it->second;
        Box* k = it->first.value;
        self->d.erase(it);
        Py_DECREF(v);
        Py_DECREF(k);

        return 0;
    }

    ASSERT(op->cls == attrwrapper_cls, "%s", getTypeName(op));
    return PyObject_DelItem(op, key);
}

extern "C" int PyDict_DelItemString(PyObject* v, const char* key) noexcept {
    PyObject* kv;
    int err;
    kv = PyUnicode_FromString(key);
    if (kv == NULL)
        return -1;
    err = PyDict_DelItem(v, kv);
    Py_DECREF(kv);
    return err;
}

Box* dictPop(BoxedDict* self, Box* k, Box* d) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor 'pop' requires a 'dict' object but received a '%s'", getTypeName(self));

    auto it = self->d.find(k);
    if (it == self->d.end()) {
        if (d)
            return incref(d);

        raiseExcHelper(KeyError, k);
    }

    Box* rtn = it->second;
    Box* old_k = it->first.value;
    self->d.erase(it);
    Py_DECREF(old_k);
    return rtn;
}

Box* dictPopitem(BoxedDict* self) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor 'popitem' requires a 'dict' object but received a '%s'",
                       getTypeName(self));

    // Like CPython, popitem() removes the most recently added item.
    auto it = self->d.last();
    if (it == self->d.end()) {
        raiseExcHelper(KeyError, "popitem(): dictionary is empty");
    }

    Box* key = it->first.value;
    Box* value = it->second;
    self->d.erase(it);

    auto rtn = BoxedTuple::create({ key, value });
    Py_DECREF(key);
    Py_DECREF(value);
    return rtn;
}

Box* dictGet(BoxedDict* self, Box* k, Box* d) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor 'get' requires a 'dict' object but received a '%s'", getTypeName(self));

    auto it = self->d.find(k);
    if (it == self->d.end())
        return incref(d);

    return incref(it->second);
}

Box* dictSetdefault(BoxedDict* self, Box* k, Box* v) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor 'setdefault' requires a 'dict' object but received a '%s'",
                       getTypeName(self));

    BoxAndHash k_hash(k);
    auto it = self->d.find(k_hash);
    if (it != self->d.end())
        return incref(it->second);

    Py_INCREF(k);
    Py_INCREF(v);
    self->d.insert(std::make_pair(k_hash, v));

    return incref(v);
}

Box* dictContains(BoxedDict* self, Box* k) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor '__contains__' requires a 'dict' object but received a '%s'",
                       getTypeName(self));

    return boxBool(self->d.count(k) != 0);
}

/* Return 1 if `key` is in dict `op`, 0 if not, and -1 on error. */
extern "C" int PyDict_Contains(PyObject* op, PyObject* key) noexcept {
    try {
        if (op->cls == attrwrapper_cls) {
            if (key->cls == str_cls) {
                BoxedString* key_str = (BoxedString*)key;
                Py_INCREF(key_str);
                internStringMortalInplace(key_str);
                AUTO_DECREF(key_str);
                return unwrapAttrWrapper(op)->hasattr(key_str);
            }

            Box* rtn = PyObject_CallMethod(op, "__contains__", "O", key);
            if (!rtn)
                return -1;
            return autoDecref(rtn) == Py_True;
        }

        BoxedDict* mp = (BoxedDict*)op;
        assert(PyDict_Check(mp));
        return mp->getOrNull(key) ? 1 : 0;
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
}


Box* dictNonzero(BoxedDict* self) {
    return boxBool(self->d.size());
}

Box* dictFromkeys(Box* cls, Box* iterable, Box* default_value) {
    auto rtn = new BoxedDict();
    if (PyAnySet_Check(iterable)) {
        for (auto&& elt : ((BoxedSet*)iterable)->s) {
            Py_INCREF(elt.value);
            Py_INCREF(default_value);
            rtn->d.insert(std::make_pair(elt, default_value));
        }
    } else {
        for (Box* e : iterable->pyElements()) {
            AUTO_DECREF(e);
            _dictSet(rtn, e, default_value);
        }
    }

    return rtn;
}

static Box* dictEq(BoxedDict* self, Box* _rhs) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor '__eq__' requires a 'dict' object but received a '%s'",
                       getTypeName(self));

    if (_rhs->cls == attrwrapper_cls) {
        _rhs = attrwrapperToDict(_rhs);
    } else {
        Py_INCREF(_rhs);
    }
    AUTO_DECREF(_rhs);

    if (!PyDict_Check(_rhs))
        return incref(NotImplemented);

    BoxedDict* rhs = static_cast<BoxedDict*>(_rhs);

    if (self->d.size() != rhs->d.size())
        Py_RETURN_FALSE;

    // Comparing keys and values can run code that mutates either dict, so walk self by position
    // and hold references to the entries being compared.
    uint32_t pos = 0;
    while (true) {
        auto it = self->d.fromPosition(pos);
        if (it == self->d.end())
            break;
        pos = it.position() + 1;

        BoxAndHash key = it->first;
        Box* value = it->second;
        Py_INCREF(key.value);
        AUTO_DECREF(key.value);
        Py_INCREF(value);
        AUTO_DECREF(value);

        auto rhs_it = rhs->d.find(key);
        if (rhs_it == rhs->d.end())
            Py_RETURN_FALSE;
        Box* rhs_value = incref(rhs_it->second);
        AUTO_DECREF(rhs_value);
        if (!PyEq()(value, rhs_value))
            Py_RETURN_FALSE;
    }

    Py_RETURN_TRUE;
}

static Box* dictNe(BoxedDict* self, Box* _rhs) {
    Box* eq = dictEq(self, _rhs);
    if (eq == NotImplemented)
        return eq;
    AUTO_DECREF(eq);
    if (eq == Py_True)
        Py_RETURN_FALSE;
    Py_RETURN_TRUE;
}


extern "C" Box* dictNew(Box* _cls, BoxedTuple* args, BoxedDict* kwargs) {
    if (!PyType_Check(_cls))
        raiseExcHelper(TypeError, "dict.__new__(X): X is not a type object (%s)", getTypeName(_cls));

    BoxedClass* cls = static_cast<BoxedClass*>(_cls);
    if (!isSubclass(cls, dict_cls))
        raiseExcHelper(TypeError, "dict.__new__(%s): %s is not a subtype of dict", getNameOfClass(cls),
                       getNameOfClass(cls));

    return new (cls) BoxedDict();
}

void dictMerge(BoxedDict* self, Box* other) {
    if (PyDict_Check(other)) {
        // Hashing and comparing keys can run code that mutates `other`, so walk it by position.
        BoxedDict* o = static_cast<BoxedDict*>(other);
        auto size = o->d.size();
        uint32_t pos = 0;
        while (true) {
            auto it = o->d.fromPosition(pos);
            if (it == o->d.end())
                break;
            pos = it.position() + 1;
            BoxAndHash key = it->first;
            Box* value = it->second;
            Py_INCREF(key.value);
            AUTO_DECREF(key.value);
            Py_INCREF(value);
            AUTO_DECREF(value);
            _dictSet(self, key, value);
            if (o->d.size() != size)
                raiseExcHelper(RuntimeError, "dict mutated during update");
        }
        return;
    }

    Box* keys;
    if (other->cls == attrwrapper_cls) {
        keys = attrwrapperKeys(other);
    } else {
        static BoxedString* keys_str = getStaticString("keys");
        CallattrFlags callattr_flags{.cls_only = false, .null_on_nonexistent = true, .argspec = ArgPassSpec(0) };
        keys = callattr(other, keys_str, callattr_flags, NULL, NULL, NULL, NULL, NULL);
    }
    assert(keys);
    AUTO_DECREF(keys);

    for (Box* k : keys->pyElements()) {
        AUTO_DECREF(k);
        _dictSetStolen(self, k, getitemInternal<CXX>(other, k));
    }
}

void dictMergeFromSeq2(BoxedDict* self, Box* other) {
    int idx = 0;

    // raises if not iterable
    for (Box* element : other->pyElements()) {
        AUTO_DECREF(element);

        // should this check subclasses? anyway to check if something is iterable...
        if (element->cls == list_cls) {
            BoxedList* list = static_cast<BoxedList*>(element);
            if (list->size != 2)
                raiseExcHelper(ValueError, "dictionary update sequence element #%d has length %ld; 2 is required", idx,
                               list->size);

            // Hashing the key can mutate the list.
            Box* key = incref(list->elts->elts[0]);
            AUTO_DECREF(key);
            Box* value = incref(list->elts->elts[1]);
            AUTO_DECREF(value);
            _dictSet(self, key, value);
        } else if (element->cls == tuple_cls) {
            BoxedTuple* tuple = static_cast<BoxedTuple*>(element);
            if (tuple->size() != 2)
                raiseExcHelper(ValueError, "dictionary update sequence element #%d has length %ld; 2 is required", idx,
                               tuple->size());

            _dictSet(self, tuple->elts[0], tuple->elts[1]);
        } else
            raiseExcHelper(TypeError, "cannot convert dictionary update sequence element #%d to a sequence", idx);

        idx++;
    }
}

extern "C" int PyDict_Merge(PyObject* a, PyObject* b, int override_) noexcept {
    try {
        if (a == NULL || !PyDict_Check(a) || b == NULL) {
            if (a && b && a->cls == attrwrapper_cls) {
                RELEASE_ASSERT(PyDict_Check(b) && override_ == 1, "");
                for (auto&& item : *(BoxedDict*)b) {
                    setitem(a, item.first, item.second);
                }
                return 0;
            }

            PyErr_BadInternalCall();
            return -1;
        }

        if (override_ != 1)
            Py_FatalError("unimplemented");

        dictMerge(static_cast<BoxedDict*>(a), b);
        return 0;
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
}

Box* dictUpdate(BoxedDict* self, BoxedTuple* args, BoxedDict* kwargs) {
    assert(args->cls == tuple_cls);
    assert(!kwargs || kwargs->cls == dict_cls);

    if (args->size() > 1)
        raiseExcHelper(TypeError, "update expected at most 1 arguments, got %ld", args->size());
    if (args->size()) {
        Box* arg = args->elts[0];
        static BoxedString* keys_str = getStaticString("keys");
        if (PyObject_HasAttr(arg, keys_str)) {
            dictMerge(self, arg);
        } else {
            dictMergeFromSeq2(self, arg);
        }
    }

    if (kwargs && kwargs->d.size())
        dictMerge(self, kwargs);

    Py_RETURN_NONE;
}

extern "C" Box* dictInit(BoxedDict* self, BoxedTuple* args, BoxedDict* kwargs) {
    int args_sz = args->size();
    int kwargs_sz = kwargs ? kwargs->d.size() : 0;

    // CPython accepts a single positional and keyword arguments, in any combination
    if (args_sz > 1)
        raiseExcHelper(TypeError, "dict expected at most 1 arguments, got %d", args_sz);

    autoDecref(dictUpdate(self, args, kwargs));

    if (kwargs) {
        // handle keyword arguments by merging (possibly over positional entries per CPy)
        assert(kwargs->cls == dict_cls);

        for (const auto& p : kwargs->d) {
            _dictSet(self, p.first, p.second);
        }
    }

    Py_RETURN_NONE;
}

static int dict_init(PyObject* self, PyObject* args, PyObject* kwds) noexcept {
    assert(PyDict_Check(self));
    try {
        Box* r = dictInit(static_cast<BoxedDict*>(self), static_cast<BoxedTuple*>(args), static_cast<BoxedDict*>(kwds));
        Py_DECREF(r);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
    return 0;
}

static Box* dict_repr(PyObject* self) noexcept {
    assert(PyDict_Check(self));
    try {
        return dictRepr(static_cast<BoxedDict*>(self));
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

static PyObject* dict_richcompare(PyObject* v, PyObject* w, int op) noexcept {
    Box* res;

    if (v->cls == attrwrapper_cls)
        return dict_richcompare(autoDecref(attrwrapperToDict(v)), w, op);
    if (w->cls == attrwrapper_cls)
        return dict_richcompare(v, autoDecref(attrwrapperToDict(w)), op);

    if (!PyDict_Check(v) || !PyDict_Check(w)) {
        res = incref(Py_NotImplemented);
    } else if (op == Py_EQ) {
        try {
            res = dictEq((BoxedDict*)v, (BoxedDict*)w);
        } catch (ExcInfo e) {
            setCAPIException(e);
            return NULL;
        }
    } else if (op == Py_NE) {
        try {
            res = dictNe((BoxedDict*)v, (BoxedDict*)w);
        } catch (ExcInfo e) {
            setCAPIException(e);
            return NULL;
        }
    } else {
        res = incref(Py_NotImplemented);
    }
    return res;
}

void BoxedDict::dealloc(Box* b) noexcept {
    if (_PyObject_GC_IS_TRACKED(b))
        _PyObject_GC_UNTRACK(b);

    assert(PyDict_Check(b));

    BoxedDict::clear(b);

    b->cls->tp_free(b);
}

int BoxedDict::traverse(PyObject* op, visitproc visit, void* arg) noexcept {
    Py_ssize_t i = 0;
    PyObject* pk;
    PyObject* pv;

    while (PyDict_Next(op, &i, &pk, &pv)) {
        Py_VISIT(pk);
        Py_VISIT(pv);
    }
    return 0;
}

int BoxedDict::clear(PyObject* op) noexcept {
    PyDict_Clear(op);
    return 0;
}

// Pyston change: CPython untracks dicts that can't be part of a cycle to reduce GC pressure.
// It would also have to re-track them once GC-aware objects get added, which we don't do, so
// this is a no-op.  This isn't as important to us because we don't have as many dict objects.
extern "C" void _PyDict_MaybeUntrack(PyObject* op) noexcept {
}

// We use cpythons dictview implementation from dictobject.c
Box* dictViewKeys(BoxedDict* d) {
    Box* rtn = _PyDictView_New(d, &PyDictKeys_Type);
    if (!rtn)
        throwCAPIException();
    return rtn;
}
Box* dictViewValues(BoxedDict* d) {
    Box* rtn = _PyDictView_New(d, &PyDictValues_Type);
    if (!rtn)
        throwCAPIException();
    return rtn;
}
Box* dictViewItems(BoxedDict* d) {
    Box* rtn = _PyDictView_New(d, &PyDictItems_Type);
    if (!rtn)
        throwCAPIException();
    return rtn;
}


// This function gets called from dictobject.c
extern "C" PyObject* dictiter_new(PyDictObject* dict, PyTypeObject* iter_type) noexcept {
    return new (iter_type) BoxedDictIterator((BoxedDict*)dict);
}


void setupDict() {
    static PyMappingMethods dict_as_mapping;
    dict_cls->tp_as_mapping = &dict_as_mapping;
    static PySequenceMethods dict_as_sequence;
    dict_cls->tp_as_sequence = &dict_as_sequence;

    BoxedClass::createStatic(dictiterkey_cls, type_cls, object_cls, 0, 0, sizeof(BoxedDictIterator), false,
                             "dict_keyiterator", true, (destructor)BoxedDictIterator::dealloc, NULL, true,
                             (traverseproc)BoxedDictIterator::traverse, NOCLEAR);
    BoxedClass::createStatic(dictitervalue_cls, type_cls, object_cls, 0, 0, sizeof(BoxedDictIterator), false,
                             "dict_valueiterator", true, (destructor)BoxedDictIterator::dealloc, NULL, true,
                             (traverseproc)BoxedDictIterator::traverse, NOCLEAR);
    BoxedClass::createStatic(dictiteritem_cls, type_cls, object_cls, 0, 0, sizeof(BoxedDictIterator), false,
                             "dict_itemiterator", true, (destructor)BoxedDictIterator::dealloc, NULL, true,
                             (traverseproc)BoxedDictIterator::traverse, NOCLEAR);

    dictiterkey_cls->instances_are_nonzero = dictitervalue_cls->instances_are_nonzero
        = dictiteritem_cls->instances_are_nonzero = true;

    dict_cls->tp_hash = PyObject_HashNotImplemented;
    dict_cls->tp_richcompare = dict_richcompare;

    dict_cls->giveAttr("__len__", new BoxedFunction(BoxedCode::create((void*)dictLen, BOXED_INT, 1, "dict.__len__")));
    dict_cls->giveAttr("__new__",
                       new BoxedFunction(BoxedCode::create((void*)dictNew, UNKNOWN, 1, true, true, "dict.__new__")));
    dict_cls->giveAttr("__init__",
                       new BoxedFunction(BoxedCode::create((void*)dictInit, NONE, 1, true, true, "dict.__init__")));
    dict_cls->giveAttr("__repr__", new BoxedFunction(BoxedCode::create((void*)dictRepr, STR, 1, "dict.__repr__")));

    dict_cls->giveAttr("__hash__", incref(Py_None));
    dict_cls->giveAttr("__iter__", new BoxedFunction(BoxedCode::create(
                                       (void*)dictIterKeys, typeFromClass(dictiterkey_cls), 1, "dict.__iter__")));

    dict_cls->giveAttr("update",
                       new BoxedFunction(BoxedCode::create((void*)dictUpdate, NONE, 1, true, true, "dict.update")));

    dict_cls->giveAttr("clear", new BoxedFunction(BoxedCode::create((void*)dictClear, NONE, 1, "dict.clear")));
    dict_cls->giveAttr("copy", new BoxedFunction(BoxedCode::create((void*)dictCopy, DICT, 1, "dict.copy")));

    dict_cls->giveAttr("items", new BoxedFunction(BoxedCode::create((void*)dictViewItems, UNKNOWN, 1, "dict.items")));

    dict_cls->giveAttr("values", new BoxedFunction(BoxedCode::create((void*)dictViewValues, UNKNOWN, 1, "dict.values")));

    dict_cls->giveAttr("keys", new BoxedFunction(BoxedCode::create((void*)dictViewKeys, UNKNOWN, 1, "dict.keys")));

    dict_cls->giveAttr(
        "pop", new BoxedFunction(BoxedCode::create((void*)dictPop, UNKNOWN, 3, false, false, "dict.pop"), { NULL }));
    dict_cls->giveAttr("popitem",
                       new BoxedFunction(BoxedCode::create((void*)dictPopitem, BOXED_TUPLE, 1, "dict.popitem")));

    auto* fromkeys_func = new BoxedFunction(
        BoxedCode::create((void*)dictFromkeys, DICT, 3, false, false, "dict.fromkeys"), { Py_None });
    dict_cls->giveAttr("fromkeys", new BoxedClassmethod(fromkeys_func));
    Py_DECREF(fromkeys_func);


    dict_cls->giveAttr(
        "get", new BoxedFunction(BoxedCode::create((void*)dictGet, UNKNOWN, 3, false, false, "dict.get"), { Py_None }));

    dict_cls->giveAttr("setdefault", new BoxedFunction(BoxedCode::create((void*)dictSetdefault, UNKNOWN, 3, false,
                                                                         false, "dict.setdefault"),
                                                       { Py_None }));

    auto dict_getitem
        = BoxedCode::create((void*)dictGetitem<CXX>, UNKNOWN, 2, "dict.__getitem__", "", ParamNames::empty(), CXX);
    dict_getitem->addVersion((void*)dictGetitem<CAPI>, UNKNOWN, CAPI);
    dict_cls->giveAttr("__getitem__", new BoxedFunction(dict_getitem));
    dict_cls->giveAttr("__setitem__",
                       new BoxedFunction(BoxedCode::create((void*)dictSetitem, NONE, 3, "dict.__setitem__")));
    dict_cls->giveAttr("__delitem__",
                       new BoxedFunction(BoxedCode::create((void*)dictDelitem, UNKNOWN, 2, "dict.__delitem__")));
    dict_cls->giveAttr("__contains__",
                       new BoxedFunction(BoxedCode::create((void*)dictContains, BOXED_BOOL, 2, "dict.__contains__")));


    add_operators(dict_cls);
    dict_cls->freeze();

    // create the dictonary iterator types
    for (BoxedClass* iter_type : { dictiterkey_cls, dictitervalue_cls, dictiteritem_cls }) {
        BoxedCode* hasnext = BoxedCode::create((void*)dictIterHasnextUnboxed, BOOL, 1, "dict.__hasnext__");
        hasnext->addVersion((void*)dictIterHasnext, BOXED_BOOL);
        iter_type->giveAttr("__hasnext__", new BoxedFunction(hasnext));
        iter_type->giveAttr("__iter__", new BoxedFunction(BoxedCode::create((void*)dictIterIter,
                                                                            typeFromClass(iter_type), 1, "__iter__")));
        iter_type->giveAttr("__next__",
                            new BoxedFunction(BoxedCode::create((void*)dictIterNext, UNKNOWN, 1, "__next__")));
        iter_type->freeze();
        iter_type->tp_iter = PyObject_SelfIter;
        iter_type->tp_iternext = dictiter_next;
        iter_type->tp_flags &= ~Py_TPFLAGS_BASETYPE; // subclassing is not allowed
    }


    // Manually set some tp_* slots *after* calling freeze() -> fixup_slot_dispatchers().
    // fixup_slot_dispatchers will insert a wrapper like slot_tp_init into tp_init, which calls the python-level
    // __init__ function.  This is all well and good, until a C extension tries to subclass from dict and then
    // creates a new tp_init function which calls Py_DictType.tp_init().  That tp_init is slot_tp_init, which calls
    // self.__init__, which is the *subclasses* init function not dict's.
    //
    // This seems to happen pretty rarely, and only with dict, so for now let's just work around it by manually
    // setting the couple functions that get used.
    //
    // I'm not sure if CPython has a better mechanism for this, since I assume they allow having extension classes
    // subclass Python classes.
    dict_cls->tp_init = dict_init;
    dict_cls->tp_repr = dict_repr;
    dict_cls->tp_iter = dict_iter;

    dict_cls->tp_as_mapping->mp_length = (lenfunc)dict_length;
    dict_cls->tp_as_mapping->mp_subscript = (binaryfunc)dictGetitem<CAPI>;
    dict_cls->tp_as_mapping->mp_ass_subscript = (objobjargproc)dict_ass_sub;

    dict_cls->tp_as_sequence->sq_contains = (objobjproc)PyDict_Contains;

    PyType_Ready(&PyDictKeys_Type);
    PyType_Ready(&PyDictValues_Type);
    PyType_Ready(&PyDictItems_Type);
}
}
