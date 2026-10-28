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

#ifndef PYSTON_CAPI_TYPES_H
#define PYSTON_CAPI_TYPES_H

#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

typedef PyObject* (*wrapperfunc)(PyObject* self, PyObject* args, void* wrapped);
typedef PyObject* (*wrapperfunc_kwds)(PyObject* self, PyObject* args, void* wrapped, PyObject* kwds);

// Call a METH_FASTCALL (or METH_FASTCALL | METH_KEYWORDS) function with a tuple of positional
// arguments and an optional dict of keyword arguments.
Box* capiCallFastcall(Box* passthrough, PyCFunction func, Box* args) noexcept;
Box* capiCallFastcallKeywords(Box* passthrough, PyCFunction func, Box* args, Box* kwargs) noexcept;

class BoxedCApiFunction : public Box {
public:
    PyMethodDef* method_def;
    PyObject* passthrough;
    Box* module;
    Box* weakreflist;
    vectorcallfunc vectorcall;

public:
    BoxedCApiFunction(PyMethodDef* method_def, Box* passthrough, Box* module_name)
        : method_def(method_def), passthrough(passthrough), module(module_name), weakreflist(NULL), vectorcall(NULL) {
        assert(!module || PyUnicode_Check(module_name));
        Py_XINCREF(passthrough);
        Py_XINCREF(module);
    }

    DEFAULT_CLASS_SIMPLE(capifunc_cls, true);

    PyCFunction getFunction() { return method_def->ml_meth; }

    template <ExceptionStyle S> static Box* __repr__(Box* _self) noexcept(S == CAPI) {
        if (!isSubclass(_self->cls, capifunc_cls))
            return setDescrTypeError<S>(_self, "capifunc", "__repr__");
        BoxedCApiFunction* self = (BoxedCApiFunction*)_self;
        if (self->passthrough == NULL)
            return callCAPIFromStyle<S>(PyUnicode_FromFormat, "<built-in function %s>", self->method_def->ml_name);
        return callCAPIFromStyle<S>(PyUnicode_FromFormat, "<built-in method %s of %s object at %p>",
                                    self->method_def->ml_name, self->passthrough->cls->tp_name, self->passthrough);
    }

    static Box* __call__(BoxedCApiFunction* self, BoxedTuple* varargs, BoxedDict* kwargs);
    template <ExceptionStyle S>
    static Box* tppCall(Box* _self, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2, Box* arg3,
                        Box** args, const std::vector<BoxedString*>* keyword_names) noexcept(S == CAPI);

    static Box* getname(Box* b, void*) noexcept {
        RELEASE_ASSERT(b->cls == capifunc_cls, "");
        const char* s = static_cast<BoxedCApiFunction*>(b)->method_def->ml_name;
        if (s)
            return boxString(s);
        return incref(Py_None);
    }

    static Box* doc(Box* b, void*) noexcept {
        RELEASE_ASSERT(b->cls == capifunc_cls, "");
        const char* s = static_cast<BoxedCApiFunction*>(b)->method_def->ml_doc;
        if (s)
            return boxString(s);
        return incref(Py_None);
    }

    static void dealloc(Box* _o) noexcept {
        BoxedCApiFunction* o = (BoxedCApiFunction*)_o;

        _PyObject_GC_UNTRACK(o);
        Py_XDECREF(o->module);
        Py_XDECREF(o->passthrough);
        o->cls->tp_free(o);
    }

    static int traverse(Box* _o, visitproc visit, void* arg) noexcept {
        BoxedCApiFunction* o = (BoxedCApiFunction*)_o;

        Py_VISIT(o->module);
        Py_VISIT(o->passthrough);
        return 0;
    }

    static int clear(Box* _o) noexcept {
        BoxedCApiFunction* o = (BoxedCApiFunction*)_o;

        Py_CLEAR(o->module);
        Py_CLEAR(o->passthrough);
        return 0;
    }
};
static_assert(sizeof(BoxedCApiFunction) == sizeof(PyCFunctionObject), "");
static_assert(offsetof(BoxedCApiFunction, method_def) == offsetof(PyCFunctionObject, m_ml), "");
static_assert(offsetof(BoxedCApiFunction, passthrough) == offsetof(PyCFunctionObject, m_self), "");
static_assert(offsetof(BoxedCApiFunction, module) == offsetof(PyCFunctionObject, m_module), "");
static_assert(offsetof(BoxedCApiFunction, weakreflist) == offsetof(PyCFunctionObject, m_weakreflist), "");

PyObject* richcompare_fallback(PyObject* v, PyObject* w, int op) noexcept;
PyObject* convert_3way_to_object(int op, int c) noexcept;

} // namespace pyston

#endif
