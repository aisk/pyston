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
#include <cstdarg>
#include <dlfcn.h>

#include "codegen/unwinding.h"
#include "core/ast.h"
#include "core/options.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

void raiseExc(STOLEN(Box*) exc_obj) {
    assert(!PyErr_Occurred());
    throw ExcInfo(incref(exc_obj->cls), exc_obj, NULL);
}

// Have a special helper function for syntax errors, since we want to include the location
// of the syntax error in the traceback, even though it is not part of the execution:
void raiseSyntaxError(const char* msg, int lineno, int col_offset, llvm::StringRef file, llvm::StringRef func,
                      bool compiler_error) {
    if (compiler_error) {
        // This is how CPython's compiler_error() works:
        assert(file.data()[file.size()] == '\0');
        Box* loc = PyErr_ProgramText(file.data(), lineno);
        if (!loc) {
            Py_INCREF(Py_None);
            loc = Py_None;
        }
        AUTO_DECREF(loc);

        auto args = BoxedTuple::create(
            { autoDecref(boxString(file)), autoDecref(boxInt(lineno)), autoDecref(boxInt(col_offset + 1)), loc });
        AUTO_DECREF(args);
        Box* exc = runtimeCall(SyntaxError, ArgPassSpec(2), autoDecref(boxString(msg)), args, NULL, NULL, NULL);
        assert(!PyErr_Occurred());
        throw ExcInfo(incref(exc->cls), exc, NULL);
    } else {
        PyErr_SetString(SyntaxError, msg);
        PyErr_SyntaxLocation(file.str().c_str(), lineno);
        throwCAPIException();
    }
}

void raiseSyntaxErrorHelper(llvm::StringRef file, llvm::StringRef func, AST* node_at, const char* msg, ...) {
    va_list ap;
    va_start(ap, msg);

    char buf[1024];
    vsnprintf(buf, sizeof(buf), msg, ap);


    // TODO I'm not sure that it's safe to raise an exception here, since I think
    // there will be things that end up not getting cleaned up.
    // Then again, there are a huge number of things that don't get cleaned up even
    // if an exception doesn't get thrown...

    // TODO output is still a little wrong, should be, for example
    //
    //  File "../test/tests/future_non_existent.py", line 1
    //    from __future__ import rvalue_references # should cause syntax error
    //
    // but instead it is
    //
    // Traceback (most recent call last):
    //  File "../test/tests/future_non_existent.py", line -1, in :
    //    from __future__ import rvalue_references # should cause syntax error
    raiseSyntaxError(buf, node_at->lineno, node_at->col_offset, file, "");
}

void ExcInfo::printExcAndTraceback() const {
    PyErr_Display(type, value, traceback);
}

bool ExcInfo::matches(BoxedClass* cls) const {
    assert(this->type);
    RELEASE_ASSERT(PyType_Check(this->type), "throwing old-style objects not supported yet (%s)",
                   getTypeName(this->type));
    return isSubclass(static_cast<BoxedClass*>(this->type), cls);
}

// takes the three arguments of a `raise' and produces the ExcInfo to throw
ExcInfo excInfoForRaise(STOLEN(Box*) type, STOLEN(Box*) value, STOLEN(Box*) tb) {
    assert(type && value && tb); // use None for default behavior, not nullptr
    // TODO switch this to PyErr_Normalize

    if (tb == Py_None) {
        Py_CLEAR(tb);
    } else if (tb != NULL && !PyTraceBack_Check(tb)) {
        Py_DECREF(type);
        Py_DECREF(value);
        Py_XDECREF(tb);
        raiseExcHelper(TypeError, "raise: arg 3 must be a traceback or None");
    }


    if (PyExceptionClass_Check(type)) {
        PyErr_NormalizeException(&type, &value, &tb);

        if (!PyExceptionInstance_Check(value)) {
            Py_DECREF(type);
            Py_DECREF(value);
            Py_XDECREF(tb);
            raiseExcHelper(TypeError, "calling %s() should have returned an instance of "
                                      "BaseException, not '%s'",
                           ((PyTypeObject*)type)->tp_name, Py_TYPE(value)->tp_name);
        }
    } else if (PyExceptionInstance_Check(type)) {
        /* Raising an instance.  The value should be a dummy. */
        if (value != Py_None) {
            Py_DECREF(type);
            Py_DECREF(value);
            Py_XDECREF(tb);
            raiseExcHelper(TypeError, "instance exception may not have a separate value");
        } else {
            /* Normalize to raise <class>, <instance> */
            Py_DECREF(value);
            value = type;
            type = PyExceptionInstance_Class(type);
            Py_INCREF(type);
        }
    } else {
        Py_DECREF(type);
        Py_DECREF(value);
        Py_XDECREF(tb);
        /* Not something you can raise.  You get an exception
           anyway, just not what you specified :-) */
        raiseExcHelper(TypeError, "exceptions must derive from BaseException");
    }

    assert(PyExceptionClass_Check(type));

    return ExcInfo(type, value, tb);
}

// Sets value.__context__ to the exception currently being handled, like CPython's _PyErr_SetObject.
void chainExceptionContext(Box* value) {
    if (!cur_thread_state.frame_info || !value || !PyExceptionInstance_Check(value))
        return;
    Box* handled = getFrameExcInfo()->value;
    if (!handled || handled == Py_None || handled == value || !PyExceptionInstance_Check(handled))
        return;

    // Avoid creating a cycle in the context chain.
    Box* o = handled;
    Box* context;
    while ((context = PyException_GetContext(o))) {
        Py_DECREF(context);
        if (context == value) {
            PyException_SetContext(o, NULL);
            break;
        }
        o = context;
    }
    Py_INCREF(handled);
    PyException_SetContext(value, handled);
}

// Implements 'raise exc from cause': returns the exception instance with its __cause__ set.
static Box* raiseFrom(Box* exc, Box* cause) {
    Box* value;
    if (PyExceptionClass_Check(exc)) {
        value = runtimeCall(exc, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
        if (!PyExceptionInstance_Check(value)) {
            AUTO_DECREF(value);
            raiseExcHelper(TypeError, "calling %s should have returned an instance of BaseException, not %s",
                           ((PyTypeObject*)exc)->tp_name, Py_TYPE(value)->tp_name);
        }
    } else if (PyExceptionInstance_Check(exc)) {
        value = incref(exc);
    } else {
        raiseExcHelper(TypeError, "exceptions must derive from BaseException");
    }
    AUTO_DECREF(value);

    Box* fixed_cause;
    if (PyExceptionClass_Check(cause)) {
        fixed_cause = runtimeCall(cause, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
        if (!PyExceptionInstance_Check(fixed_cause)) {
            AUTO_DECREF(fixed_cause);
            raiseExcHelper(TypeError, "calling %s should have returned an instance of BaseException, not %s",
                           ((PyTypeObject*)cause)->tp_name, Py_TYPE(fixed_cause)->tp_name);
        }
    } else if (PyExceptionInstance_Check(cause)) {
        fixed_cause = incref(cause);
    } else if (cause == Py_None) {
        fixed_cause = NULL;
    } else {
        raiseExcHelper(TypeError, "exception causes must derive from BaseException");
    }
    // PyException_SetCause steals the cause and sets __suppress_context__.
    PyException_SetCause(value, fixed_cause);
    return incref(value);
}

// The CFG saves the exception being handled with this before a try statement and puts it back with
// restoreExcInfo when a handler is done. NotImplemented, which is never an exception value, means the
// frame had no state of its own and inherits it from its caller.
static Box* getExcValue() {
    FrameInfo* frame_info = (FrameInfo*)cur_thread_state.frame_info;
    if (!frame_info->exc.type)
        return incref(Py_NotImplemented);
    return incref(frame_info->exc.value);
}

static Box* restoreExcInfo(Box* value) {
    FrameInfo* frame_info = (FrameInfo*)cur_thread_state.frame_info;
    if (value == Py_NotImplemented)
        setFrameExcInfo(frame_info, NULL, NULL, NULL);
    else if (value == Py_None)
        setFrameExcInfo(frame_info, incref(Py_None), incref(Py_None), NULL);
    else
        setFrameExcInfo(frame_info, incref(Py_TYPE(value)), incref(value), PyException_GetTraceback(value));
    Py_RETURN_NONE;
}

// CPython's SETUP_ANNOTATIONS: a module or class body with annotations gets an __annotations__ dict,
// unless its namespace already has one.
static Box* setupAnnotations() {
    static BoxedString* annotations_str = getStaticString("__annotations__");
    FrameInfo* frame_info = (FrameInfo*)cur_thread_state.frame_info;
    Box* ns = frame_info->boxedLocals;
    if (!ns) {
        ns = frame_info->globals;
        if (PyModule_Check(ns))
            ns = ns->getAttrWrapper();
    }
    if (PyDict_CheckExact(ns)) {
        if (PyDict_GetItem(ns, annotations_str))
            Py_RETURN_NONE;
    } else {
        Box* v = PyObject_GetItem(ns, annotations_str);
        if (v) {
            Py_DECREF(v);
            Py_RETURN_NONE;
        }
        if (!PyErr_ExceptionMatches(PyExc_KeyError))
            throwCAPIException();
        PyErr_Clear();
    }
    Box* d = PyDict_New();
    AUTO_DECREF(d);
    if (PyObject_SetItem(ns, annotations_str, d) < 0)
        throwCAPIException();
    Py_RETURN_NONE;
}

Box* raise_from_func, *get_exc_value_func, *restore_exc_info_func, *setup_annotations_func;

void setupRaiseFrom() {
    raise_from_func = new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)raiseFrom, UNKNOWN, 2, "raise_from"));
    PyGC_RegisterStaticConstant(raise_from_func);
    get_exc_value_func
        = new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)getExcValue, UNKNOWN, 0, "get_exc_value"));
    PyGC_RegisterStaticConstant(get_exc_value_func);
    restore_exc_info_func
        = new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)restoreExcInfo, NONE, 1, "restore_exc_info"));
    PyGC_RegisterStaticConstant(restore_exc_info_func);
    setup_annotations_func
        = new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)setupAnnotations, NONE, 0, "setup_annotations"));
    PyGC_RegisterStaticConstant(setup_annotations_func);
}

extern "C" void raise0(ExcInfo* frame_exc_info) {
    updateFrameExcInfoIfNeeded(frame_exc_info);
    assert(frame_exc_info->type);

    // TODO need to clean up when we call normalize, do_raise, etc
    if (frame_exc_info->type == Py_None)
        raiseExcHelper(RuntimeError, "No active exception to reraise");

    startReraise();
    assert(!PyErr_Occurred());

    Py_INCREF(frame_exc_info->type);
    Py_INCREF(frame_exc_info->value);
    Py_INCREF(frame_exc_info->traceback);
    throw * frame_exc_info;
}

extern "C" void raise0_capi(ExcInfo* frame_exc_info) noexcept {
    updateFrameExcInfoIfNeeded(frame_exc_info);
    assert(frame_exc_info->type);

    // TODO need to clean up when we call normalize, do_raise, etc
    if (frame_exc_info->type == Py_None) {
        PyErr_SetString(RuntimeError, "No active exception to reraise");
        return;
    }

    startReraise();
    assert(!PyErr_Occurred());

    Py_INCREF(frame_exc_info->type);
    Py_INCREF(frame_exc_info->value);
    Py_INCREF(frame_exc_info->traceback);

    PyErr_Restore(frame_exc_info->type, frame_exc_info->value, frame_exc_info->traceback);
}

extern "C" void raise3(STOLEN(Box*) arg0, STOLEN(Box*) arg1, STOLEN(Box*) arg2) {
    bool reraise = arg2 != NULL && arg2 != Py_None;
    auto exc_info = excInfoForRaise(arg0, arg1, arg2);

    if (reraise) {
        startReraise();
    } else {
        chainExceptionContext(exc_info.value);
        // Raising an instance continues the traceback it already has.
        if (!exc_info.traceback)
            exc_info.traceback = PyException_GetTraceback(exc_info.value);
    }

    if (PyErr_Occurred())
        PyErr_Clear();
    throw exc_info;
}

extern "C" void raise3_capi(STOLEN(Box*) arg0, STOLEN(Box*) arg1, STOLEN(Box*) arg2) noexcept {
    bool reraise = arg2 != NULL && arg2 != Py_None;

    ExcInfo exc_info(NULL, NULL, NULL);
    try {
        exc_info = excInfoForRaise(arg0, arg1, arg2);
    } catch (ExcInfo e) {
        exc_info = e;
        reraise = false;
    }

    if (reraise) {
        startReraise();
    } else {
        chainExceptionContext(exc_info.value);
        if (!exc_info.traceback)
            exc_info.traceback = PyException_GetTraceback(exc_info.value);
    }

    PyErr_Restore(exc_info.type, exc_info.value, exc_info.traceback);
}

void raiseExcHelper(BoxedClass* cls, Box* arg) {
    assert(!PyErr_Occurred());
    Box* exc_obj = runtimeCall(cls, ArgPassSpec(1), arg, NULL, NULL, NULL, NULL);
    raiseExc(exc_obj);
}

void raiseExcHelper(BoxedClass* cls, const char* msg, ...) {
    assert(!PyErr_Occurred());
    if (msg != NULL) {
        va_list ap;
        va_start(ap, msg);

        // printf("Raising: ");
        // vprintf(msg, ap);
        // printf("\n");
        // va_start(ap, msg);

        char buf[1024];
        vsnprintf(buf, sizeof(buf), msg, ap);

        va_end(ap);

        BoxedString* message = boxString(buf);
        Box* exc_obj = runtimeCall(cls, ArgPassSpec(1), autoDecref(message), NULL, NULL, NULL, NULL);
        raiseExc(exc_obj);
    } else {
        Box* exc_obj = runtimeCall(cls, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
        raiseExc(exc_obj);
    }
}


void logException(ExcInfo* exc_info) {
#if STAT_EXCEPTIONS
    static StatCounter num_exceptions("num_exceptions");
    num_exceptions.log();

    std::string stat_name;
    if (PyType_Check(exc_info->type))
        stat_name = "num_exceptions_" + std::string(static_cast<BoxedClass*>(exc_info->type)->tp_name);
    else
        stat_name = "num_exceptions_" + std::string(exc_info->value->cls->tp_name);
    Stats::log(Stats::getStatCounter(stat_name));
#if STAT_EXCEPTIONS_LOCATION
    logByCurrentPythonLine(stat_name);
#endif
#endif
}

extern "C" void caughtCapiException() {
    PyThreadState* tstate = PyThreadState_GET();
    // An exception raised by C code reaches its first Python frame without a traceback.
    if (tstate->curexc_type && !tstate->curexc_traceback && !getIsReraiseFlag() && cur_thread_state.frame_info) {
        Box* handled = getFrameExcInfo()->value;
        if (handled && handled != Py_None) {
            Box* type, *value, *tb;
            PyErr_Fetch(&type, &value, &tb);
            PyErr_NormalizeException(&type, &value, &tb);
            chainExceptionContext(value);
            PyErr_Restore(type, value, tb);
        }
    }
    exceptionAtLine(&tstate->curexc_traceback);
}

extern "C" void reraiseCapiExcAsCxx() {
    ensureCAPIExceptionSet();
    // TODO: we are normalizing to many times?
    Box* type, *value, *tb;
    PyErr_Fetch(&type, &value, &tb);
    ExcInfo e = excInfoForRaise(type, value, tb);
    startReraise();
    throw e;
}

extern "C" void rawReraise(Box* type, Box* value, Box* tb) {
    startReraise();
    throw ExcInfo(type, value, tb);
}

void caughtCxxException(ExcInfo* exc_info) {
    static StatCounter frames_unwound("num_frames_unwound_python");
    frames_unwound.log();

    if (!exc_info->traceback && !getIsReraiseFlag())
        chainExceptionContext(exc_info->value);
    exceptionAtLine(&exc_info->traceback);
}

struct ExcState {
    bool is_reraise;
    constexpr ExcState() : is_reraise(false) {}
} static __thread exc_state;

bool& getIsReraiseFlag() {
    return exc_state.is_reraise;
}

void exceptionAtLine(Box** traceback) {
    if (!getIsReraiseFlag())
        PyTraceBack_Here_Tb((struct _frame*)getFrame((FrameInfo*)cur_thread_state.frame_info),
                            (PyTracebackObject**)traceback);
    else
        getIsReraiseFlag() = false;
}
}
