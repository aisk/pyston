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

#include <algorithm>
#include <cfloat>
#include <cstddef>
#include <err.h>

#include "llvm/Support/FileSystem.h"

#include "Python.h"
#include "codegen/cpython_ast.h"

#include "capi/typeobject.h"
#include "capi/types.h"
#include "codegen/ast_interpreter.h"
#include "codegen/irgen/hooks.h"
#include "codegen/parser.h"
#include "codegen/unwinding.h"
#include "core/types.h"
#include "runtime/ics.h"
#include "runtime/import.h"
#include "runtime/inline/list.h"
#include "runtime/iterobject.h"
#include "runtime/list.h"
#include "runtime/long.h"
#include "runtime/objmodel.h"
#include "runtime/rewrite_args.h"
#include "runtime/set.h"
#include "runtime/super.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

extern "C" {
// From CPython's Python/preconfig.c and Python/initconfig.c.  Pyston always uses UTF-8 for file
// names, like CPython 3.8 does on Linux in the C.UTF-8 locale.
const char* Py_FileSystemDefaultEncoding = "utf-8";
int Py_HasFileSystemDefaultEncoding = 1;
const char* Py_FileSystemDefaultEncodeErrors = "surrogateescape";
int _Py_HasFileSystemDefaultEncodeErrors = 1;
int Py_UTF8Mode = 0;

PyMethodDef* _PystonBuiltin_GetMethods(void);
void _PystonInterp_SetBuiltins(PyObject* builtins);
}

extern "C" PyObject* _PyEval_GetBuiltinId(_Py_Identifier* name) noexcept {
    PyObject* attr = _PyObject_GetAttrId(builtins_module, name);
    if (attr == NULL && !PyErr_Occurred())
        PyErr_SetObject(PyExc_AttributeError, _PyUnicode_FromId(name));
    return attr;
}

extern "C" Box* trap() {
    raise(SIGTRAP);

    Py_RETURN_NONE;
}

/* Helper for PyObject_Dir.
   Merge the __dict__ of aclass into dict, and recursively also all
   the __dict__s of aclass's base classes.  The order of merging isn't
   defined, as it's expected that only the final set of dict keys is
   interesting.
   Return 0 on success, -1 on error.
*/

extern "C" Box* dir(Box* obj) {
    Box* r = PyObject_Dir(obj);
    if (!r)
        throwCAPIException();
    return r;
}

extern "C" Box* vars(Box* obj) {
    if (!obj)
        return incref(PyEval_GetLocals());

    static BoxedString* dict_str = getStaticString("__dict__");
    Box* rtn = getattrInternal<ExceptionStyle::CAPI>(obj, dict_str);
    if (!rtn)
        raiseExcHelper(TypeError, "vars() argument must have __dict__ attribute");
    return rtn;
}

extern "C" Box* abs_(Box* x) {
    Box* rtn = PyNumber_Absolute(x);
    if (!rtn)
        throwCAPIException();
    return rtn;
}

extern "C" Box* all(Box* container) {
    for (Box* e : container->pyElements()) {
        AUTO_DECREF(e);
        if (!nonzero(e)) {
            return boxBool(false);
        }
    }
    return boxBool(true);
}

extern "C" Box* any(Box* container) {
    for (Box* e : container->pyElements()) {
        AUTO_DECREF(e);
        if (nonzero(e)) {
            return boxBool(true);
        }
    }
    return boxBool(false);
}

extern "C" Box* next(Box* iterator, Box* _default) noexcept {
    if (!PyIter_Check(iterator)) {
        PyErr_Format(PyExc_TypeError, "%.200s object is not an iterator", iterator->cls->tp_name);
        return NULL;
    }

    Box* rtn;

    if (iterator->cls->tp_iternext == slot_tp_iternext) {
        rtn = iterator->cls->call_nextIC(iterator);
    } else {
        rtn = iterator->cls->tp_iternext(iterator);
    }

    if (rtn != NULL) {
        return rtn;
    } else if (_default != NULL) {
        if (PyErr_Occurred()) {
            if (!PyErr_ExceptionMatches(PyExc_StopIteration))
                return NULL;
            PyErr_Clear();
        }
        Py_INCREF(_default);
        return _default;
    } else if (PyErr_Occurred()) {
        return NULL;
    } else {
        PyErr_SetNone(PyExc_StopIteration);
        return NULL;
    }
}

extern "C" Box* id(Box* arg) {
    i64 addr = (i64)(arg) ^ 0xdeadbeef00000003;
    return boxInt(addr);
}


// Copied from CPython with some minor modifications
Box* isinstance_func(Box* obj, Box* cls) {
    int rtn = PyObject_IsInstance(obj, cls);
    if (rtn < 0)
        throwCAPIException();
    return boxBool(rtn);
}

Box* issubclass_func(Box* child, Box* parent) {
    int rtn = PyObject_IsSubclass(child, parent);
    if (rtn < 0)
        throwCAPIException();
    return boxBool(rtn);
}

Box* bltinImport(Box* name, Box* globals, Box* locals, Box** args) {
    Box* fromlist = args[0];
    Box* level = args[1];

    // __import__ takes a 'locals' argument, but it doesn't get used in CPython.
    // Well, it gets passed to PyImport_ImportModuleLevel() and then import_module_level(),
    // which ignores it.  So we don't even pass it through.

    name = coerceUnicodeToStr<CXX>(name);
    AUTO_DECREF(name);

    if (name->cls != str_cls) {
        raiseExcHelper(TypeError, "__import__() argument 1 must be string, not %s", getTypeName(name));
    }

    if (level->cls != int_cls) {
        raiseExcHelper(TypeError, "an integer is required");
    }

    Box* rtn
        = PyImport_ImportModuleLevel(((BoxedString*)name)->c_str(), globals, NULL, fromlist, ((BoxedInt*)level)->n);
    if (!rtn)
        throwCAPIException();
    return rtn;
}

Box* delattrFunc(Box* obj, Box* _str) {
    _str = coerceUnicodeToStr<CXX>(_str);

    if (_str->cls != str_cls) {
        Py_DECREF(_str);
        raiseExcHelper(TypeError, "attribute name must be string, not '%s'", getTypeName(_str));
    }
    BoxedString* str = static_cast<BoxedString*>(_str);
    internStringMortalInplace(str);
    AUTO_DECREF(str);

    delattr(obj, str);
    return incref(Py_None);
}

static Box* getattrFuncHelper(STOLEN(Box*) return_val, Box* obj, BoxedString* str, Box* default_val) noexcept {
    assert(PyUnicode_Check(str));

    if (return_val)
        return return_val;

    bool exc = PyErr_Occurred();
    if (exc && !PyErr_ExceptionMatches(AttributeError))
        return NULL;

    if (default_val) {
        if (exc)
            PyErr_Clear();
        return incref(default_val);
    }
    if (!exc)
        raiseAttributeErrorCapi(obj, str->s());
    return NULL;
}

template <ExceptionStyle S>
Box* getattrFuncInternal(BoxedFunctionBase* func, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1,
                         Box* arg2, Box* arg3, Box** args, const std::vector<BoxedString*>* keyword_names) {
    static Box* defaults[] = { NULL };

    auto continuation = [=](CallRewriteArgs* rewrite_args, Box* arg1, Box* arg2, Box* arg3, Box** args) {
        Box* obj = arg1;
        Box* _str = arg2;
        Box* default_value = arg3;

        if (rewrite_args) {
            // We need to make sure that the attribute string will be the same.
            // Even though the passed string might not be the exact attribute name
            // that we end up looking up (because we need to encode it or intern it),
            // guarding on that object means (for strings and unicode) that the string
            // value is fixed.
            if (!PyUnicode_CheckExact(_str) && !PyUnicode_CheckExact(_str))
                rewrite_args = NULL;
            else {
                if (PyUnicode_CheckExact(_str) && PyUnicode_CHECK_INTERNED(_str) == SSTATE_INTERNED_IMMORTAL) {
                    // can avoid keeping the extra gc reference
                } else {
                    rewrite_args->rewriter->addGCReference(_str);
                }

                rewrite_args->arg2->addGuard((intptr_t)arg2);
            }
        }

        _str = coerceUnicodeToStr<S>(_str);

        if (S == CAPI && !_str)
            return (Box*)NULL;

        if (!PyUnicode_Check(_str)) {
            Py_DECREF(_str);
            if (S == CAPI) {
                PyErr_SetString(TypeError, "getattr(): attribute name must be string");
                return (Box*)NULL;
            } else
                raiseExcHelper(TypeError, "getattr(): attribute name must be string");
        }

        BoxedString* str = static_cast<BoxedString*>(_str);
        if (!PyUnicode_CHECK_INTERNED(str))
            internStringMortalInplace(str);
        AUTO_DECREF(str);

        Box* rtn;
        RewriterVar* r_rtn;
        if (rewrite_args) {
            GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, rewrite_args->arg1, rewrite_args->destination);
            rtn = getattrInternal<CAPI>(obj, str, &grewrite_args);
            // TODO could make the return valid in the NOEXC_POSSIBLE case via a helper
            if (!grewrite_args.isSuccessful())
                rewrite_args = NULL;
            else {
                ReturnConvention return_convention;
                std::tie(r_rtn, return_convention) = grewrite_args.getReturn();

                // Convert to NOEXC_POSSIBLE:
                if (return_convention == ReturnConvention::NO_RETURN) {
                    return_convention = ReturnConvention::NOEXC_POSSIBLE;
                    r_rtn = rewrite_args->rewriter->loadConst(0);
                } else if (return_convention == ReturnConvention::MAYBE_EXC) {
                    if (default_value)
                        rewrite_args = NULL;
                }
                assert(!rewrite_args || return_convention == ReturnConvention::NOEXC_POSSIBLE
                       || return_convention == ReturnConvention::HAS_RETURN
                       || return_convention == ReturnConvention::CAPI_RETURN
                       || (default_value == NULL && return_convention == ReturnConvention::MAYBE_EXC));
            }
        } else {
            rtn = getattrInternal<CAPI>(obj, str);
        }

        if (rewrite_args) {
            assert(PyUnicode_CHECK_INTERNED(str) == SSTATE_INTERNED_IMMORTAL);
            RewriterVar* r_str = rewrite_args->rewriter->loadConst((intptr_t)str, Location::forArg(2));
            RewriterVar* final_rtn
                = rewrite_args->rewriter->call(false, (void*)getattrFuncHelper, r_rtn, rewrite_args->arg1, r_str,
                                               rewrite_args->arg3)->setType(RefType::OWNED);
            r_rtn->refConsumed();

            if (S == CXX)
                rewrite_args->rewriter->checkAndThrowCAPIException(final_rtn);
            rewrite_args->out_success = true;
            rewrite_args->out_rtn = final_rtn;
        }

        Box* r = getattrFuncHelper(rtn, obj, str, default_value);
        if (S == CXX && !r)
            throwCAPIException();
        return r;
    };

    return callCXXFromStyle<S>([&]() {
        return rearrangeArgumentsAndCall(ParamReceiveSpec(3, 1, false, false), NULL, "getattr", defaults, rewrite_args,
                                         argspec, arg1, arg2, arg3, args, keyword_names, continuation);
    });
}

Box* setattrFunc(Box* obj, Box* _str, Box* value) {
    _str = coerceUnicodeToStr<CXX>(_str);

    if (_str->cls != str_cls) {
        Py_DECREF(_str);
        raiseExcHelper(TypeError, "attribute name must be string, not '%s'", _str->cls->tp_name);
    }

    BoxedString* str = static_cast<BoxedString*>(_str);
    internStringMortalInplace(str);
    AUTO_DECREF(str);

    setattr(obj, str, incref(value));
    return incref(Py_None);
}

// Not sure if this should be stealing or not:
static Box* hasattrFuncHelper(STOLEN(Box*) return_val) noexcept {
    AUTO_XDECREF(return_val);

    if (return_val)
        Py_RETURN_TRUE;

    if (PyErr_Occurred()) {
        if (!PyErr_ExceptionMatches(PyExc_Exception))
            return NULL;

        PyErr_Clear();
    }
    Py_RETURN_FALSE;
}

template <ExceptionStyle S>
Box* hasattrFuncInternal(BoxedFunctionBase* func, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1,
                         Box* arg2, Box* arg3, Box** args, const std::vector<BoxedString*>* keyword_names) {
    auto continuation = [=](CallRewriteArgs* rewrite_args, Box* arg1, Box* arg2, Box* arg3, Box** args) {
        Box* obj = arg1;
        Box* _str = arg2;

        if (rewrite_args) {
            // We need to make sure that the attribute string will be the same.
            // Even though the passed string might not be the exact attribute name
            // that we end up looking up (because we need to encode it or intern it),
            // guarding on that object means (for strings and unicode) that the string
            // value is fixed.
            if (!PyUnicode_CheckExact(_str) && !PyUnicode_CheckExact(_str))
                rewrite_args = NULL;
            else {
                if (PyUnicode_CheckExact(_str) && PyUnicode_CHECK_INTERNED(_str) == SSTATE_INTERNED_IMMORTAL) {
                    // can avoid keeping the extra gc reference
                } else {
                    rewrite_args->rewriter->addGCReference(_str);
                }

                rewrite_args->arg2->addGuard((intptr_t)arg2);
            }
        }

        _str = coerceUnicodeToStr<S>(_str);

        if (S == CAPI && !_str)
            return (Box*)NULL;

        if (!PyUnicode_Check(_str)) {
            Py_DECREF(_str);
            if (S == CAPI) {
                PyErr_SetString(TypeError, "hasattr(): attribute name must be string");
                return (Box*)NULL;
            } else
                raiseExcHelper(TypeError, "hasattr(): attribute name must be string");
        }

        BoxedString* str = static_cast<BoxedString*>(_str);

        if (!PyUnicode_CHECK_INTERNED(str))
            internStringMortalInplace(str);
        AUTO_DECREF(str);

        Box* rtn;
        RewriterVar* r_rtn;
        if (rewrite_args) {
            GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, rewrite_args->arg1, rewrite_args->destination);
            rtn = getattrInternal<CAPI>(obj, str, &grewrite_args);
            if (!grewrite_args.isSuccessful())
                rewrite_args = NULL;
            else {
                ReturnConvention return_convention;
                std::tie(r_rtn, return_convention) = grewrite_args.getReturn();

                // Convert to NOEXC_POSSIBLE:
                if (return_convention == ReturnConvention::NO_RETURN) {
                    return_convention = ReturnConvention::NOEXC_POSSIBLE;
                    r_rtn = rewrite_args->rewriter->loadConst(0);
                } else if (return_convention == ReturnConvention::MAYBE_EXC) {
                    rewrite_args = NULL;
                }
                assert(!rewrite_args || return_convention == ReturnConvention::NOEXC_POSSIBLE
                       || return_convention == ReturnConvention::HAS_RETURN
                       || return_convention == ReturnConvention::CAPI_RETURN);
            }
        } else {
            rtn = getattrInternal<CAPI>(obj, str);
        }

        if (rewrite_args) {
            RewriterVar* final_rtn
                = rewrite_args->rewriter->call(false, (void*)hasattrFuncHelper, r_rtn)->setType(RefType::OWNED);
            r_rtn->refConsumed();

            if (S == CXX)
                rewrite_args->rewriter->checkAndThrowCAPIException(final_rtn);
            rewrite_args->out_success = true;
            rewrite_args->out_rtn = final_rtn;
        }

        Box* r = hasattrFuncHelper(rtn);
        if (S == CXX && !r)
            throwCAPIException();
        return r;
    };

    return callCXXFromStyle<S>([&]() {
        return rearrangeArgumentsAndCall(ParamReceiveSpec(2, 0, false, false), NULL, "hasattr", NULL, rewrite_args,
                                         argspec, arg1, arg2, arg3, args, keyword_names, continuation);
    });
}

// from cpython, bltinmodule.c
static Box* callable(Box* obj) {
    return PyBool_FromLong((long)PyCallable_Check(obj));
}

BoxedModule* builtins_module;

static Box* notimplementedRepr(Box* self) {
    assert(self == NotImplemented);
    return boxString("NotImplemented");
}

class BoxedException : public Box {
public:
    HCAttrs attrs;
    BoxedException() {}

    static Box* __reduce__(Box* self) {
        RELEASE_ASSERT(isSubclass(self->cls, BaseException), "");
        BoxedException* exc = static_cast<BoxedException*>(self);
        return BoxedTuple::create({ self->cls, EmptyTuple, self->getAttrWrapper() });
    }
};

class BoxedEnumerate : public Box {
private:
    BoxIteratorRange range;
    BoxIterator iterator, iterator_end;
    int64_t idx;
    BoxedLong* idx_long;

public:
    BoxedEnumerate(BoxIteratorRange range, int64_t idx, BoxedLong* idx_long)
        : range(std::move(range)),
          iterator(this->range.begin()),
          iterator_end(this->range.end()),
          idx(idx),
          idx_long(idx_long) {
        Py_XINCREF(idx_long);
    }

    DEFAULT_CLASS_SIMPLE(enumerate_cls, true);

    static Box* new_(Box* cls, Box* obj, Box* start) {
        if (!PyType_Check(cls) || !isSubclass(static_cast<BoxedClass*>(cls), enumerate_cls))
            raiseExcHelper(TypeError, "enumerate.__new__(X): X is not a subtype of enumerate");
        start = PyNumber_Index(start);
        if (!start)
            throwCAPIException();
        AUTO_DECREF(start);
        int64_t idx = PyLong_AsSsize_t(start);
        BoxedLong* idx_long = NULL;
        if (idx == -1 && PyErr_Occurred()) {
            PyErr_Clear();
            assert(PyLong_Check(start));
            idx_long = (BoxedLong*)start;
        }
        auto&& range = obj->pyElements();
        return new (static_cast<BoxedClass*>(cls)) BoxedEnumerate(std::move(range), idx, idx_long);
    }

    static Box* iter(Box* _self) noexcept {
        assert(PyObject_TypeCheck(_self, enumerate_cls));
        BoxedEnumerate* self = static_cast<BoxedEnumerate*>(_self);
        return incref(self);
    }

    static Box* next(Box* _self) {
        assert(PyObject_TypeCheck(_self, enumerate_cls));
        BoxedEnumerate* self = static_cast<BoxedEnumerate*>(_self);
        if (self->iterator == self->iterator_end)
            raiseExcHelper(StopIteration, (const char*)NULL);
        Box* val = *self->iterator;
        AUTO_DECREF(val);
        ++self->iterator;
        Box* rtn;
        if (self->idx_long)
            rtn = BoxedTuple::create({ self->idx_long, val });
        else
            rtn = BoxedTuple::create({ autoDecref(boxInt(self->idx)), val });

        // check if incrementing the counter would overflow it, if so switch to long counter
        if (self->idx == PY_SSIZE_T_MAX) {
            assert(!self->idx_long);
            self->idx_long = boxLong(self->idx);
            self->idx = -1;
        }
        if (self->idx_long)
            self->idx_long = (BoxedLong*)longAdd(autoDecref(self->idx_long), autoDecref(boxInt(1)));
        else
            ++self->idx;
        return rtn;
    }

    static Box* hasnext(Box* _self) {
        assert(PyObject_TypeCheck(_self, enumerate_cls));
        BoxedEnumerate* self = static_cast<BoxedEnumerate*>(_self);
        return boxBool(self->iterator != self->iterator_end);
    }

    static void dealloc(Box* b) noexcept {
        assert(PyObject_TypeCheck(b, enumerate_cls));
        BoxedEnumerate* self = static_cast<BoxedEnumerate*>(b);

        PyObject_GC_UnTrack(self);

        Py_XDECREF(self->idx_long);
        self->iterator.~BoxIterator();
        self->iterator_end.~BoxIterator();
        self->range.~BoxIteratorRange();

        self->cls->tp_free(self);
    }

    static int traverse(Box* b, visitproc visit, void* arg) noexcept {
        assert(PyObject_TypeCheck(b, enumerate_cls));
        BoxedEnumerate* self = static_cast<BoxedEnumerate*>(b);

        Py_VISIT(self->idx_long);
        Py_TRAVERSE(self->range);

        return 0;
    }
};

static Box* globals() {
    return incref(getGlobalsDict());
}

static Box* locals() {
    return incref(fastLocalsToBoxedLocals());
}

extern "C" BORROWED(PyObject*) PyEval_GetLocals(void) noexcept {
    try {
        return fastLocalsToBoxedLocals();
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" BORROWED(PyObject*) PyEval_GetGlobals(void) noexcept {
    try {
        return getGlobalsDict();
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" BORROWED(PyObject*) PyEval_GetBuiltins(void) noexcept {
    return builtins_module->getAttrWrapper();
}

Box* divmod(Box* lhs, Box* rhs) {
    return binopInternal<NOT_REWRITABLE, false>(lhs, rhs, AST_TYPE::DivMod, NULL);
}

Box* powFunc(Box* x, Box* y, Box* z) {
    Box* rtn = PyNumber_Power(x, y, z);
    if (!rtn)
        throwCAPIException();
    return rtn;
}

Box* getreversed(Box* o) {
    static BoxedString* reversed_str = getStaticString("__reversed__");

    // common case:
    if (o->cls == list_cls) {
        return listReversed(o);
    }

    // TODO add rewriting to this?  probably want to try to avoid this path though
    CallattrFlags callattr_flags{.cls_only = true, .null_on_nonexistent = true, .argspec = ArgPassSpec(0) };
    Box* r = callattr(o, reversed_str, callattr_flags, NULL, NULL, NULL, NULL, NULL);
    if (r)
        return r;

    static BoxedString* getitem_str = getStaticString("__getitem__");
    if (!typeLookup(o->cls, getitem_str)) {
        raiseExcHelper(TypeError, "'%s' object is not iterable", getTypeName(o));
    }
    int64_t len = unboxedLen(o); // this will throw an exception if __len__ isn't there

    return new (seqreviter_cls) BoxedSeqIter(o, len - 1);
}

Box* pydump(Box* p, BoxedInt* level) {
    dumpEx(p, level->n);
    return incref(Py_None);
}

Box* pydumpAddr(Box* p) {
    if (p->cls != int_cls)
        raiseExcHelper(TypeError, "Requires an int");

    dump((void*)static_cast<BoxedInt*>(p)->n);
    return incref(Py_None);
}

Box* builtinIter(Box* obj, Box* sentinel) {
    if (sentinel == NULL)
        return getiter(obj);

    if (!PyCallable_Check(obj)) {
        raiseExcHelper(TypeError, "iter(v, w): v must be callable");
    }

    Box* r = PyCallIter_New(obj, sentinel);
    if (!r)
        throwCAPIException();
    return r;
}

// Copied from builtin_raw_input, but without the argument handling
// 'v' is the prompt, and can be NULL corresponding to the arg not getting passed.
static PyObject* builtin_eval(PyObject* self, PyObject* args) noexcept {
    PyObject* cmd, *result, * tmp = NULL;
    PyObject* globals = Py_None, * locals = Py_None;
    const char* str;
    PyCompilerFlags cf = _PyCompilerFlags_INIT;

    if (!PyArg_UnpackTuple(args, "eval", 1, 3, &cmd, &globals, &locals))
        return NULL;
    if (locals != Py_None && !PyMapping_Check(locals)) {
        PyErr_SetString(PyExc_TypeError, "locals must be a mapping");
        return NULL;
    }
    // Pyston change:
    // if (globals != Py_None && !PyDict_Check(globals)) {
    if (globals != Py_None && !PyDict_Check(globals) && globals->cls != attrwrapper_cls) {
        PyErr_SetString(PyExc_TypeError, PyMapping_Check(globals)
                                             ? "globals must be a real dict; try eval(expr, {}, mapping)"
                                             : "globals must be a dict");
        return NULL;
    }
    if (globals == Py_None) {
        globals = PyEval_GetGlobals();
        if (locals == Py_None)
            locals = PyEval_GetLocals();
    } else if (locals == Py_None)
        locals = globals;

    if (globals == NULL || locals == NULL) {
        PyErr_SetString(PyExc_TypeError, "eval must be given globals and locals "
                                         "when called without a frame");
        return NULL;
    }

    if (PyDict_GetItemString(globals, "__builtins__") == NULL) {
        if (PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins()) != 0)
            return NULL;
    }

    if (PyCode_Check(cmd)) {
// Pyston change:
#if 0
        if (PyCode_GetNumFree((PyCodeObject *)cmd) > 0) {
            PyErr_SetString(PyExc_TypeError,
        "code object passed to eval() may not contain free variables");
            return NULL;
        }
#endif
        return PyEval_EvalCode(cmd, globals, locals);
    }

    cf.cf_flags = PyCF_SOURCE_IS_UTF8;
    str = _Py_SourceAsString(cmd, "eval", "string, bytes or code", &cf, &tmp);
    if (str == NULL)
        return NULL;

    while (*str == ' ' || *str == '\t')
        str++;

    (void)PyEval_MergeCompilerFlags(&cf);
    result = PyRun_StringFlags(str, Py_eval_input, globals, locals, &cf);
    Py_XDECREF(tmp);
    return result;
}

static PyObject* builtin_exec(PyObject* self, PyObject* args, PyObject* kwds) noexcept {
    static const char* kwlist[] = { "source", "globals", "locals", NULL };
    PyObject* source, * globals = Py_None, * locals = Py_None;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|OO:exec", const_cast<char**>(kwlist), &source, &globals, &locals))
        return NULL;
    try {
        FutureFlags caller_future_flags = 0;
        BoxedCode* caller = getTopPythonFunction();
        if (caller)
            caller_future_flags = caller->source->future_flags;
        exec(source, globals, locals, caller_future_flags);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
    Py_RETURN_NONE;
}


PyDoc_STRVAR(print_doc, "print(value, ..., sep=' ', end='\\n', file=sys.stdout)\n\
\n\
Prints the values to a stream, or to sys.stdout by default.\n\
Optional keyword arguments:\n\
file: a file-like object (stream); defaults to the current sys.stdout.\n\
sep:  string inserted between values, default a space.\n\
end:  string appended after the last value, default a newline.");

PyDoc_STRVAR(range_doc, "range(stop) -> list of integers\n\
range(start, stop[, step]) -> list of integers\n\
\n\
Return a list containing an arithmetic progression of integers.\n\
range(i, j) returns [i, i+1, i+2, ..., j-1]; start (!) defaults to 0.\n\
When step is given, it specifies the increment (or decrement).\n\
For example, range(4) returns [0, 1, 2, 3].  The end point is omitted!\n\
These are exactly the valid indices for a list of 4 elements.");

PyDoc_STRVAR(raw_input_doc, "raw_input([prompt]) -> string\n\
\n\
Read a string from standard input.  The trailing newline is stripped.\n\
If the user hits EOF (Unix: Ctl-D, Windows: Ctl-Z+Return), raise EOFError.\n\
On Unix, GNU readline is used if enabled.  The prompt string, if given,\n\
is printed without a trailing newline before reading.");

PyDoc_STRVAR(reduce_doc, "reduce(function, sequence[, initial]) -> value\n\
\n\
Apply a function of two arguments cumulatively to the items of a sequence,\n\
from left to right, so as to reduce the sequence to a single value.\n\
For example, reduce(lambda x, y: x+y, [1, 2, 3, 4, 5]) calculates\n\
((((1+2)+3)+4)+5).  If initial is present, it is placed before the items\n\
of the sequence in the calculation, and serves as a default when the\n\
sequence is empty.");

PyDoc_STRVAR(reload_doc, "reload(module) -> module\n\
\n\
Reload the module.  The module must have been successfully imported before.");

PyDoc_STRVAR(repr_doc, "repr(object) -> string\n\
\n\
Return the canonical string representation of the object.\n\
For most object types, eval(repr(object)) == object.");

PyDoc_STRVAR(round_doc, "round(number[, ndigits]) -> floating point number\n\
\n\
Round a number to a given precision in decimal digits (default 0 digits).\n\
This always returns a floating point number.  Precision may be negative.");

PyDoc_STRVAR(sorted_doc, "sorted(iterable, cmp=None, key=None, reverse=False) --> new sorted list");

PyDoc_STRVAR(vars_doc, "vars([object]) -> dictionary\n\
\n\
Without arguments, equivalent to locals().\n\
With an argument, equivalent to object.__dict__.");

PyDoc_STRVAR(sum_doc, "sum(sequence[, start]) -> value\n\
\n\
Return the sum of a sequence of numbers (NOT strings) plus the value\n\
of parameter 'start' (which defaults to 0).  When the sequence is\n\
empty, return start.");

PyDoc_STRVAR(isinstance_doc, "isinstance(object, class-or-type-or-tuple) -> bool\n\
\n\
Return whether an object is an instance of a class or of a subclass thereof.\n\
With a type as second argument, return whether that is the object's type.\n\
The form using a tuple, isinstance(x, (A, B, ...)), is a shortcut for\n\
isinstance(x, A) or isinstance(x, B) or ... (etc.).");

PyDoc_STRVAR(issubclass_doc, "issubclass(C, B) -> bool\n\
\n\
Return whether class C is a subclass (i.e., a derived class) of class B.\n\
When using a tuple as the second argument issubclass(X, (A, B, ...)),\n\
is a shortcut for issubclass(X, A) or issubclass(X, B) or ... (etc.).");

PyDoc_STRVAR(zip_doc, "zip(seq1 [, seq2 [...]]) -> [(seq1[0], seq2[0] ...), (...)]\n\
\n\
Return a list of tuples, where each tuple contains the i-th element\n\
from each of the argument sequences.  The returned list is truncated\n\
in length to the length of the shortest argument sequence.");

PyDoc_STRVAR(builtin_doc, "Built-in functions, exceptions, and other objects.\n\
\n\
Noteworthy: None is the `nil' object; Ellipsis represents `...' in slices.");

PyDoc_STRVAR(import_doc, "__import__(name, globals={}, locals={}, fromlist=[], level=-1) -> module\n\
\n\
Import a module. Because this function is meant for use by the Python\n\
interpreter and not for general use it is better to use\n\
importlib.import_module() to programmatically import a module.\n\
\n\
The globals argument is only used to determine the context;\n\
they are not modified.  The locals argument is unused.  The fromlist\n\
should be a list of names to emulate ``from name import ...'', or an\n\
empty list to emulate ``import name''.\n\
When importing a module from a package, note that __import__('A.B', ...)\n\
returns package A when fromlist is empty, but its submodule B when\n\
fromlist is not empty.  Level is used to determine whether to perform \n\
absolute or relative imports.  -1 is the original strategy of attempting\n\
both absolute and relative imports, 0 is absolute, a positive number\n\
is the number of parent directories to search relative to the current module.");

PyDoc_STRVAR(abs_doc, "abs(number) -> number\n\
\n\
Return the absolute value of the argument.");

PyDoc_STRVAR(all_doc, "all(iterable) -> bool\n\
\n\
Return True if bool(x) is True for all values x in the iterable.\n\
If the iterable is empty, Py_RETURN_TRUE.");

PyDoc_STRVAR(any_doc, "any(iterable) -> bool\n\
\n\
Return True if bool(x) is True for any x in the iterable.\n\
If the iterable is empty, Py_RETURN_FALSE.");

PyDoc_STRVAR(apply_doc, "apply(object[, args[, kwargs]]) -> value\n\
\n\
    Call a callable object with positional arguments taken from the tuple args,\n\
    and keyword arguments taken from the optional dictionary kwargs.\n\
    Note that classes are callable, as are instances with a __call__() method.\n\
\n\
    Deprecated since release 2.3. Instead, use the extended call syntax:\n\
        function(*args, **keywords).");

PyDoc_STRVAR(bin_doc, "bin(number) -> string\n\
\n\
Return the binary representation of an integer or long integer.");

PyDoc_STRVAR(callable_doc, "callable(object) -> bool\n\
\n\
Return whether the object is callable (i.e., some kind of function).\n\
Note that classes are callable, as are instances with a __call__() method.");

PyDoc_STRVAR(filter_doc, "filter(function or None, sequence) -> list, tuple, or string\n"
                         "\n"
                         "Return those items of sequence for which function(item) is true.  If\n"
                         "function is None, return the items that are true.  If sequence is a tuple\n"
                         "or string, return the same type, else return a list.");

PyDoc_STRVAR(format_doc, "format(value[, format_spec]) -> string\n\
\n\
Returns value.__format__(format_spec)\n\
format_spec defaults to \"\"");

PyDoc_STRVAR(chr_doc, "chr(i) -> character\n\
\n\
Return a string of one character with ordinal i; 0 <= i < 256.");

PyDoc_STRVAR(unichr_doc, "unichr(i) -> Unicode character\n\
\n\
Return a Unicode string of one character with ordinal i; 0 <= i <= 0x10ffff.");

PyDoc_STRVAR(cmp_doc, "cmp(x, y) -> integer\n\
\n\
Return negative if x<y, zero if x==y, positive if x>y.");

PyDoc_STRVAR(coerce_doc, "coerce(x, y) -> (x1, y1)\n\
\n\
Return a tuple consisting of the two numeric arguments converted to\n\
a common type, using the same rules as used by arithmetic operations.\n\
If coercion is not possible, raise TypeError.");

PyDoc_STRVAR(compile_doc, "compile(source, filename, mode[, flags[, dont_inherit]]) -> code object\n\
\n\
Compile the source string (a Python module, statement or expression)\n\
into a code object that can be executed by the exec statement or eval().\n\
The filename will be used for run-time error messages.\n\
The mode must be 'exec' to compile a module, 'single' to compile a\n\
single (interactive) statement, or 'eval' to compile an expression.\n\
The flags argument, if present, controls which future statements influence\n\
the compilation of the code.\n\
The dont_inherit argument, if non-zero, stops the compilation inheriting\n\
the effects of any future statements in effect in the code calling\n\
compile; if absent or zero these statements do influence the compilation,\n\
in addition to any features explicitly specified.");

PyDoc_STRVAR(dir_doc, "dir([object]) -> list of strings\n"
                      "\n"
                      "If called without an argument, return the names in the current scope.\n"
                      "Else, return an alphabetized list of names comprising (some of) the attributes\n"
                      "of the given object, and of attributes reachable from it.\n"
                      "If the object supplies a method named __dir__, it will be used; otherwise\n"
                      "the default dir() logic is used and returns:\n"
                      "  for a module object: the module's attributes.\n"
                      "  for a class object:  its attributes, and recursively the attributes\n"
                      "    of its bases.\n"
                      "  for any other object: its attributes, its class's attributes, and\n"
                      "    recursively the attributes of its class's base classes.");

PyDoc_STRVAR(divmod_doc, "divmod(x, y) -> (quotient, remainder)\n\
\n\
Return the tuple ((x-x%y)/y, x%y).  Invariant: div*y + mod == x.");

PyDoc_STRVAR(eval_doc, "eval(source[, globals[, locals]]) -> value\n\
\n\
Evaluate the source in the context of globals and locals.\n\
The source may be a string representing a Python expression\n\
or a code object as returned by compile().\n\
The globals must be a dictionary and locals can be any mapping,\n\
defaulting to the current globals and locals.\n\
If only globals is given, locals defaults to it.\n");

PyDoc_STRVAR(exec_doc, "exec(source, globals=None, locals=None, /)\n\
\n\
Execute the given source in the context of globals and locals.\n\
\n\
The source may be a string representing one or more Python statements\n\
or a code object as returned by compile().\n\
The globals must be a dictionary and locals can be any mapping,\n\
defaulting to the current globals and locals.\n\
If only globals is given, locals defaults to it.");

PyDoc_STRVAR(getattr_doc, "getattr(object, name[, default]) -> value\n\
\n\
Get a named attribute from an object; getattr(x, 'y') is equivalent to x.y.\n\
When a default argument is given, it is returned when the attribute doesn't\n\
exist; without it, an exception is raised in that case.");

PyDoc_STRVAR(globals_doc, "globals() -> dictionary\n\
\n\
Return the dictionary containing the current scope's global variables.");

PyDoc_STRVAR(hasattr_doc, "hasattr(object, name) -> bool\n\
\n\
Return whether the object has an attribute with the given name.\n\
(This is done by calling getattr(object, name) and catching exceptions.)");

PyDoc_STRVAR(id_doc, "id(object) -> integer\n\
\n\
Return the identity of an object.  This is guaranteed to be unique among\n\
simultaneously existing objects.  (Hint: it's the object's memory address.)");

PyDoc_STRVAR(map_doc, "map(function, sequence[, sequence, ...]) -> list\n\
\n\
Return a list of the results of applying the function to the items of\n\
the argument sequence(s).  If more than one sequence is given, the\n\
function is called with an argument list consisting of the corresponding\n\
item of each sequence, substituting None for missing values when not all\n\
sequences have the same length.  If the function is None, return a list of\n\
the items of the sequence (or a list of tuples if more than one sequence).");

PyDoc_STRVAR(next_doc, "next(iterator[, default])\n\
\n\
Return the next item from the iterator. If default is given and the iterator\n\
is exhausted, it is returned instead of raising StopIteration.");

PyDoc_STRVAR(setattr_doc, "setattr(object, name, value)\n\
\n\
Set a named attribute on an object; setattr(x, 'y', v) is equivalent to\n\
``x.y = v''.");

PyDoc_STRVAR(delattr_doc, "delattr(object, name)\n\
\n\
Delete a named attribute on an object; delattr(x, 'y') is equivalent to\n\
``del x.y''.");

PyDoc_STRVAR(hash_doc, "hash(object) -> integer\n\
\n\
Return a hash value for the object.  Two objects with the same value have\n\
the same hash value.  The reverse is not necessarily true, but likely.");

PyDoc_STRVAR(hex_doc, "hex(number) -> string\n\
\n\
Return the hexadecimal representation of an integer or long integer.");

PyDoc_STRVAR(input_doc, "input([prompt]) -> value\n\
\n\
Equivalent to eval(raw_input(prompt)).");

PyDoc_STRVAR(intern_doc, "intern(string) -> string\n\
\n\
``Intern'' the given string.  This enters the string in the (global)\n\
table of interned strings whose purpose is to speed up dictionary lookups.\n\
Return the string itself or the previously interned string object with the\n\
same value.");

PyDoc_STRVAR(iter_doc, "iter(collection) -> iterator\n\
iter(callable, sentinel) -> iterator\n\
\n\
Get an iterator from an object.  In the first form, the argument must\n\
supply its own iterator, or be a sequence.\n\
In the second form, the callable is called until it returns the sentinel.");

PyDoc_STRVAR(len_doc, "len(object) -> integer\n\
\n\
Return the number of items of a sequence or collection.");

PyDoc_STRVAR(locals_doc, "locals() -> dictionary\n\
\n\
Update and return a dictionary containing the current scope's local variables.");

PyDoc_STRVAR(min_doc, "min(iterable[, key=func]) -> value\n\
min(a, b, c, ...[, key=func]) -> value\n\
\n\
With a single iterable argument, return its smallest item.\n\
With two or more arguments, return the smallest argument.");

PyDoc_STRVAR(max_doc, "max(iterable[, key=func]) -> value\n\
max(a, b, c, ...[, key=func]) -> value\n\
\n\
With a single iterable argument, return its largest item.\n\
With two or more arguments, return the largest argument.");

PyDoc_STRVAR(oct_doc, "oct(number) -> string\n\
\n\
Return the octal representation of an integer or long integer.");

PyDoc_STRVAR(open_doc, "open(name[, mode[, buffering]]) -> file object\n\
\n\
Open a file using the file() type, returns a file object.  This is the\n\
preferred way to open a file.  See file.__doc__ for further information.");

PyDoc_STRVAR(ord_doc, "ord(c) -> integer\n\
\n\
Return the integer ordinal of a one-character string.");

PyDoc_STRVAR(pow_doc, "pow(x, y[, z]) -> number\n\
\n\
With two arguments, equivalent to x**y.  With three arguments,\n\
equivalent to (x**y) % z, but may be more efficient (e.g. for longs).");

static Box* lenCallInternalCapi(BoxedFunctionBase* func, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1,
                                Box* arg2, Box* arg3, Box** args,
                                const std::vector<BoxedString*>* keyword_names) noexcept {
    try {
        return lenCallInternal(func, NULL, argspec, arg1, arg2, arg3, args, keyword_names);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

void setupBuiltins() {
    builtins_module = createModule(autoDecref(boxString("builtins")), NULL,
                                   "Built-in functions, exceptions, and other objects.\n\nNoteworthy: None is "
                                   "the `nil' object; Ellipsis represents `...' in slices.");
    _PystonInterp_SetBuiltins(builtins_module->getAttrWrapper());

    PyType_Ready(&PyEllipsis_Type);
    builtins_module->giveAttrBorrowed("Ellipsis", Ellipsis);
    builtins_module->giveAttrBorrowed("None", Py_None);

    builtins_module->giveAttrBorrowed("__debug__", Py_True);

    BoxedClass::createStatic(notimplemented_cls, type_cls, object_cls, 0, 0, sizeof(Box), false, "NotImplementedType", false,
                                            NULL, NULL, false);
    notimplemented_cls->giveAttr(
        "__repr__", new BoxedFunction(BoxedCode::create((void*)notimplementedRepr, STR, 1, "NotImplementedType.__repr__")));
    notimplemented_cls->freeze();
    notimplemented_cls->instances_are_nonzero = true;
    NotImplemented = new (notimplemented_cls) Box();

    constants.push_back(NotImplemented);
    builtins_module->giveAttrBorrowed("NotImplemented", NotImplemented);

    builtins_module->giveAttr(
        "all", new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)all, BOXED_BOOL, 1, "all", all_doc)));
    builtins_module->giveAttr(
        "any", new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)any, BOXED_BOOL, 1, "any", any_doc)));


    repr_obj = new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)repr, UNKNOWN, 1, "repr", repr_doc));
    builtins_module->giveAttr("repr", repr_obj);

    auto len_func = BoxedCode::create((void*)len, UNKNOWN, 1, "len", len_doc);
    len_func->internal_callable.capi_val = lenCallInternalCapi;
    len_func->internal_callable.cxx_val = lenCallInternal;
    len_obj = new BoxedBuiltinFunctionOrMethod(len_func);
    builtins_module->giveAttr("len", len_obj);

    hash_obj = new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)hash, UNKNOWN, 1, "hash", hash_doc));
    builtins_module->giveAttr("hash", hash_obj);
    abs_obj = new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)abs_, UNKNOWN, 1, "abs", abs_doc));
    builtins_module->giveAttr("abs", abs_obj);


    builtins_module->giveAttr(
        "next", new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)next, UNKNOWN, 2, false, false, "next",
                                                                   next_doc, ParamNames::empty(), CAPI),
                                                 { NULL }, NULL));


    id_obj = new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)id, BOXED_INT, 1, "id", id_doc));
    builtins_module->giveAttr("id", id_obj);
    trap_obj = new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)trap, UNKNOWN, 0, "trap"));
    builtins_module->giveAttr("trap", trap_obj);
    builtins_module->giveAttr("dump",
                              new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)pydump, UNKNOWN, 2, "dump"),
                                                               { autoDecref(boxInt(0)) }));
    builtins_module->giveAttr(
        "dumpAddr", new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)pydumpAddr, UNKNOWN, 1, "dumpAddr")));

    builtins_module->giveAttr("delattr", new BoxedBuiltinFunctionOrMethod(
                                             BoxedCode::create((void*)delattrFunc, NONE, 2, "delattr", delattr_doc)));

    auto getattr_func = new BoxedCode(3, true, true, "getattr", getattr_doc, ParamNames::empty());
    getattr_func->internal_callable.capi_val = &getattrFuncInternal<CAPI>;
    getattr_func->internal_callable.cxx_val = &getattrFuncInternal<CXX>;
    builtins_module->giveAttr("getattr", new BoxedBuiltinFunctionOrMethod(getattr_func, { NULL }, NULL));

    builtins_module->giveAttr("setattr", new BoxedBuiltinFunctionOrMethod(BoxedCode::create(
                                             (void*)setattrFunc, UNKNOWN, 3, false, false, "setattr", setattr_doc)));

    auto hasattr_func = new BoxedCode(2, false, false, "hasattr", hasattr_doc);
    hasattr_func->internal_callable.capi_val = &hasattrFuncInternal<CAPI>;
    hasattr_func->internal_callable.cxx_val = &hasattrFuncInternal<CXX>;
    builtins_module->giveAttr("hasattr", new BoxedBuiltinFunctionOrMethod(hasattr_func));

    builtins_module->giveAttr(
        "pow", new BoxedBuiltinFunctionOrMethod(
                   BoxedCode::create((void*)powFunc, UNKNOWN, 3, false, false, "pow", pow_doc), { Py_None }, NULL));

    Box* isinstance_obj = new BoxedBuiltinFunctionOrMethod(
        BoxedCode::create((void*)isinstance_func, BOXED_BOOL, 2, "isinstance", isinstance_doc));
    builtins_module->giveAttr("isinstance", isinstance_obj);

    Box* issubclass_obj = new BoxedBuiltinFunctionOrMethod(
        BoxedCode::create((void*)issubclass_func, BOXED_BOOL, 2, "issubclass", issubclass_doc));
    builtins_module->giveAttr("issubclass", issubclass_obj);


    BoxedCode* import_func
        = BoxedCode::create((void*)bltinImport, UNKNOWN, 5, false, false, "__import__", import_doc,
                            ParamNames({ "name", "globals", "locals", "fromlist", "level" }, "", ""));
    builtins_module->giveAttr("__import__", new BoxedBuiltinFunctionOrMethod(
                                                import_func, { NULL, NULL, NULL, autoDecref(boxInt(0)) }, NULL));

    BoxedClass::createStatic(enumerate_cls, type_cls, object_cls, 0, 0, sizeof(BoxedEnumerate), false, "enumerate", true,
                                       BoxedEnumerate::dealloc, NULL, true, BoxedEnumerate::traverse, NOCLEAR);
    enumerate_cls->giveAttr(
        "__new__", new BoxedFunction(BoxedCode::create((void*)BoxedEnumerate::new_, UNKNOWN, 3, "enumerate.__new__", "",
                                                       ParamNames({ "", "sequence", "start" }, "", "")),
                                     { autoDecref(boxInt(0)) }));
    enumerate_cls->giveAttr(
        "__iter__", new BoxedFunction(BoxedCode::create((void*)BoxedEnumerate::iter, UNKNOWN, 1,
                                                        "enumerate.__iter__")));
    enumerate_cls->giveAttr("__next__", new BoxedFunction(BoxedCode::create((void*)BoxedEnumerate::next, BOXED_TUPLE,
                                                                             1, "enumerate.__next__")));
    enumerate_cls->giveAttr(
        "__hasnext__",
        new BoxedFunction(BoxedCode::create((void*)BoxedEnumerate::hasnext, BOXED_BOOL, 1, "enumerate.__hasnext__")));
    enumerate_cls->freeze();
    enumerate_cls->tp_iter = PyObject_SelfIter;
    builtins_module->giveAttrBorrowed("enumerate", enumerate_cls);

    builtins_module->giveAttrBorrowed("True", Py_True);
    builtins_module->giveAttrBorrowed("False", Py_False);



    builtins_module->giveAttr("globals", new BoxedBuiltinFunctionOrMethod(BoxedCode::create(
                                             (void*)globals, UNKNOWN, 0, false, false, "globals", globals_doc)));
    builtins_module->giveAttr("locals", new BoxedBuiltinFunctionOrMethod(BoxedCode::create(
                                            (void*)locals, UNKNOWN, 0, false, false, "locals", locals_doc)));

    builtins_module->giveAttr(
        "iter", new BoxedBuiltinFunctionOrMethod(
                    BoxedCode::create((void*)builtinIter, UNKNOWN, 2, false, false, "iter", iter_doc), { NULL }, NULL));
    builtins_module->giveAttr("reversed", new BoxedBuiltinFunctionOrMethod(BoxedCode::create(
                                              (void*)getreversed, UNKNOWN, 1, false, false, "reversed")));
    builtins_module->giveAttr(
        "divmod", new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)divmod, UNKNOWN, 2, "divmod", divmod_doc)));

    builtins_module->giveAttr(
        "dir", new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)dir, LIST, 1, false, false, "dir", dir_doc),
                                                { NULL }, NULL));
    builtins_module->giveAttr(
        "vars", new BoxedBuiltinFunctionOrMethod(
                    BoxedCode::create((void*)vars, UNKNOWN, 1, false, false, "vars", vars_doc), { NULL }, NULL));
    builtins_module->giveAttrBorrowed("object", object_cls);
    builtins_module->giveAttrBorrowed("str", str_cls);
    builtins_module->giveAttrBorrowed("bytes", bytes_cls);
    builtins_module->giveAttrBorrowed("int", int_cls);
    builtins_module->giveAttrBorrowed("float", float_cls);
    builtins_module->giveAttrBorrowed("list", list_cls);
    builtins_module->giveAttrBorrowed("slice", slice_cls);
    builtins_module->giveAttrBorrowed("type", type_cls);
    builtins_module->giveAttrBorrowed("range", range_cls);
    builtins_module->giveAttrBorrowed("bool", bool_cls);
    builtins_module->giveAttrBorrowed("dict", dict_cls);
    builtins_module->giveAttrBorrowed("set", set_cls);
    builtins_module->giveAttrBorrowed("frozenset", frozenset_cls);
    builtins_module->giveAttrBorrowed("tuple", tuple_cls);
    builtins_module->giveAttrBorrowed("complex", complex_cls);
    builtins_module->giveAttrBorrowed("super", super_cls);
    builtins_module->giveAttrBorrowed("property", property_cls);
    builtins_module->giveAttrBorrowed("staticmethod", staticmethod_cls);
    builtins_module->giveAttrBorrowed("classmethod", classmethod_cls);

    assert(memoryview_cls);
    Py_TYPE(&PyMemoryView_Type) = &PyType_Type;
    PyType_Ready(&PyMemoryView_Type);
    builtins_module->giveAttrBorrowed("memoryview", memoryview_cls);
    PyType_Ready(&PyByteArray_Type);
    builtins_module->giveAttrBorrowed("bytearray", &PyByteArray_Type);

    builtins_module->giveAttr("callable", new BoxedBuiltinFunctionOrMethod(BoxedCode::create(
                                              (void*)callable, UNKNOWN, 1, "callable", callable_doc)));



    static PyMethodDef builtin_methods[] = {
        { "eval", builtin_eval, METH_VARARGS, eval_doc },
        { "exec", (PyCFunction)builtin_exec, METH_VARARGS | METH_KEYWORDS, exec_doc },
    };
    for (auto& md : builtin_methods) {
        builtins_module->giveAttr(md.ml_name, new BoxedCApiFunction(&md, NULL, autoDecref(boxString("builtins"))));
    }

    // Everything that Pyston doesn't implement itself comes from CPython's Python/bltinmodule.c.
    PyMethodDef* cpython_methods = _PystonBuiltin_GetMethods();
    RELEASE_ASSERT(cpython_methods, "");
    for (PyMethodDef* md = cpython_methods; md->ml_name; md++) {
        if (builtins_module->getattr(autoDecref(internStringMortal(md->ml_name))))
            continue;
        builtins_module->giveAttr(md->ml_name, new BoxedCApiFunction(md, NULL, autoDecref(boxString("builtins"))));
    }
    builtins_module->giveAttrBorrowed("map", &PyMap_Type);
    builtins_module->giveAttrBorrowed("filter", &PyFilter_Type);
    builtins_module->giveAttrBorrowed("zip", &PyZip_Type);
}
}
