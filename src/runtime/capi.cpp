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

#include <fcntl.h>
#include <stdarg.h>
#include <string.h>

#include "Python.h"
#include "pythread.h"

#include "codegen/cpython_ast.h"
#include "grammar.h"
#include "node.h"
#include "token.h"
#include "parsetok.h"
#include "errcode.h"
#include "ast.h"
#undef BYTE
#undef STRING

#include "llvm/Support/ErrorHandling.h" // For llvm_unreachable
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include "capi/typeobject.h"
#include "capi/types.h"
#include "codegen/irgen/hooks.h"
#include "codegen/unwinding.h"
#include "core/ast.h"
#include "core/threading.h"
#include "core/types.h"
#include "runtime/import.h"
#include "runtime/objmodel.h"
#include "runtime/rewrite_args.h"
#include "runtime/types.h"

namespace pyston {

extern "C" PyObject* builtinFunctionReduce(PyObject* self, PyObject*) noexcept;

extern "C" int _PyObject_CheckBuffer(PyObject* obj) noexcept {
    return ((Py_TYPE(obj)->tp_as_buffer != NULL) && (Py_TYPE(obj)->tp_as_buffer->bf_getbuffer != NULL));
}

extern "C" PyObject* PyObject_GetAttr(PyObject* o, PyObject* attr) noexcept {
    if (!PyUnicode_Check(attr)) {
        PyErr_Format(TypeError, "attribute name must be string, not '%.200s'", Py_TYPE(attr)->tp_name);
        return NULL;
    }

    BoxedString* s = static_cast<BoxedString*>(attr);
    Py_INCREF(s);
    internStringMortalInplace(s);
    AUTO_DECREF(s);

    Box* r = getattrInternal<ExceptionStyle::CAPI>(o, s);

    if (!r && !PyErr_Occurred()) {
        PyErr_Format(PyExc_AttributeError, "'%.50s' object has no attribute '%.400s'", o->cls->tp_name,
                     PyUnicode_AsUTF8(attr));
    }

    return r;
}

extern "C" PyObject* PyObject_GenericGetAttr(PyObject* o, PyObject* name) noexcept {
    try {
        BoxedString* s = static_cast<BoxedString*>(name);
        Py_INCREF(s);
        internStringMortalInplace(s);
        AUTO_DECREF(s);
        Box* r = getattrInternalGeneric<false, NOT_REWRITABLE>(o, s, NULL, false, false, NULL, NULL);
        if (!r)
            PyErr_Format(PyExc_AttributeError, "'%.50s' object has no attribute '%.400s'", o->cls->tp_name,
                         PyUnicode_AsUTF8(name));
        return r;
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

// Note (kmod): I don't feel great about including an alternate code-path for lookups.  I also, however, don't feel
// great about modifying our code paths to take a custom dict, and since this code is just copied from CPython
// I feel like the risk is pretty low.
extern "C" PyObject* _PyObject_GenericGetAttrWithDict(PyObject* obj, PyObject* name, PyObject* dict,
                                                      int suppress) noexcept {
    PyTypeObject* tp = Py_TYPE(obj);
    PyObject* descr = NULL;
    PyObject* res = NULL;
    descrgetfunc f;
    Py_ssize_t dictoffset;
    PyObject** dictptr;

    if (!PyUnicode_Check(name)) {
        PyErr_Format(PyExc_TypeError, "attribute name must be string, not '%.200s'", Py_TYPE(name)->tp_name);
        return NULL;
    }
    Py_INCREF(name);

    if (tp->tp_dict == NULL) {
        if (PyType_Ready(tp) < 0)
            goto done;
    }

    descr = _PyType_Lookup(tp, name);

    Py_XINCREF(descr);

    f = NULL;
    if (descr != NULL) {
        f = descr->cls->tp_descr_get;
        if (f != NULL && PyDescr_IsData(descr)) {
            res = f(descr, obj, (PyObject*)obj->cls);
            Py_DECREF(descr);
            goto done;
        }
    }

    if (dict == NULL) {
        /* Inline _PyObject_GetDictPtr */
        dictoffset = tp->tp_dictoffset;
        if (dictoffset != 0) {
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
            dictptr = (PyObject**)((char*)obj + dictoffset);
            dict = *dictptr;
        }
    }
    if (dict != NULL) {
        Py_INCREF(dict);
        res = PyDict_GetItem(dict, name);
        if (res != NULL) {
            Py_INCREF(res);
            Py_XDECREF(descr);
            Py_DECREF(dict);
            goto done;
        }
        Py_DECREF(dict);
    }

    if (f != NULL) {
        res = f(descr, obj, (PyObject*)Py_TYPE(obj));
        Py_DECREF(descr);
        if (res == NULL && suppress && PyErr_ExceptionMatches(PyExc_AttributeError))
            PyErr_Clear();
        goto done;
    }

    if (descr != NULL) {
        res = descr;
        /* descr was already increfed above */
        goto done;
    }

    if (!suppress)
        PyErr_Format(PyExc_AttributeError, "'%.50s' object has no attribute '%.400s'", tp->tp_name,
                     PyUnicode_AsUTF8(name));
done:
    Py_DECREF(name);
    return res;
}

// (see note for _PyObject_GenericGetAttrWithDict)
extern "C" int _PyObject_GenericSetAttrWithDict(PyObject* obj, PyObject* name, PyObject* value,
                                                PyObject* dict) noexcept {
    PyTypeObject* tp = Py_TYPE(obj);
    PyObject* descr;
    descrsetfunc f;
    PyObject** dictptr;
    int res = -1;

    if (!PyUnicode_Check(name)) {
#ifdef Py_USING_UNICODE
        /* The Unicode to string conversion is done here because the
           existing tp_setattro slots expect a string object as name
           and we wouldn't want to break those. */
        if (PyUnicode_Check(name)) {
            name = PyUnicode_AsEncodedString(name, NULL, NULL);
            if (name == NULL)
                return -1;
        } else
#endif
        {
            PyErr_Format(PyExc_TypeError, "attribute name must be string, not '%.200s'", Py_TYPE(name)->tp_name);
            return -1;
        }
    } else
        Py_INCREF(name);

    if (tp->tp_dict == NULL) {
        if (PyType_Ready(tp) < 0)
            goto done;
    }

    descr = _PyType_Lookup(tp, name);
    f = NULL;
    if (descr != NULL) {
        f = descr->cls->tp_descr_set;
        if (f != NULL && PyDescr_IsData(descr)) {
            res = f(descr, obj, value);
            goto done;
        }
    }

    if (dict == NULL) {
        dictptr = _PyObject_GetDictPtr(obj);
        if (dictptr != NULL) {
            dict = *dictptr;
            if (dict == NULL && value != NULL) {
                dict = PyDict_New();
                if (dict == NULL)
                    goto done;
                *dictptr = dict;
            }
        }
    }
    if (dict != NULL) {
        Py_INCREF(dict);
        if (value == NULL)
            res = PyDict_DelItem(dict, name);
        else
            res = PyDict_SetItem(dict, name, value);
        if (res < 0 && PyErr_ExceptionMatches(PyExc_KeyError))
            PyErr_SetObject(PyExc_AttributeError, name);
        Py_DECREF(dict);
        goto done;
    }

    if (f != NULL) {
        res = f(descr, obj, value);
        goto done;
    }

    if (descr == NULL) {
        PyErr_Format(PyExc_AttributeError, "'%.100s' object has no attribute '%.200s'", tp->tp_name,
                     PyUnicode_AsUTF8(name));
        goto done;
    }

    PyErr_Format(PyExc_AttributeError, "'%.50s' object attribute '%.400s' is read-only", tp->tp_name,
                 PyUnicode_AsUTF8(name));
done:
    Py_DECREF(name);
    return res;
}


extern "C" PyObject* PyObject_GetItem(PyObject* o, PyObject* key) noexcept {
    return getitemInternal<ExceptionStyle::CAPI>(o, key);
}

extern "C" int PyObject_SetItem(PyObject* o, PyObject* key, PyObject* v) noexcept {
    try {
        setitem(o, key, v);
        return 0;
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
}

extern "C" int PyObject_DelItem(PyObject* o, PyObject* key) noexcept {
    try {
        delitem(o, key);
        return 0;
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
}

extern "C" long PyObject_HashNotImplemented(PyObject* self) noexcept {
    PyErr_Format(PyExc_TypeError, "unhashable type: '%.200s'", Py_TYPE(self)->tp_name);
    return -1;
}

extern "C" PyObject* _PyObject_NextNotImplemented(PyObject* self) noexcept {
    PyErr_Format(PyExc_TypeError, "'%.200s' object is not iterable", Py_TYPE(self)->tp_name);
    return NULL;
}

extern "C" int PyObject_IsTrue(PyObject* o) noexcept {
    if (o->cls == bool_cls)
        return o == Py_True;

    try {
        return o->nonzeroIC();
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
}


extern "C" int PyObject_Not(PyObject* o) noexcept {
    int res;
    res = PyObject_IsTrue(o);
    if (res < 0)
        return res;
    return res == 0;
}

extern "C" PyObject* PyObject_Call(PyObject* callable_object, PyObject* args, PyObject* kw) noexcept {
    if (kw)
        return runtimeCallInternal<ExceptionStyle::CAPI, NOT_REWRITABLE>(
            callable_object, NULL, ArgPassSpec(0, 0, true, true), args, kw, NULL, NULL, NULL);
    else
        return runtimeCallInternal<ExceptionStyle::CAPI, NOT_REWRITABLE>(
            callable_object, NULL, ArgPassSpec(0, 0, true, false), args, NULL, NULL, NULL, NULL);
}

extern "C" int PyObject_GetBuffer(PyObject* obj, Py_buffer* view, int flags) noexcept {
    if (!PyObject_CheckBuffer(obj)) {
        PyErr_Format(PyExc_TypeError, "'%100s' does not have the buffer interface", Py_TYPE(obj)->tp_name);
        return -1;
    }
    return (*(obj->cls->tp_as_buffer->bf_getbuffer))(obj, view, flags);
}

/* Implementation of PyObject_Print with recursion checking */
static int internal_print(PyObject* op, FILE* fp, int flags, int nesting) noexcept {
    int ret = 0;
    if (nesting > 10) {
        PyErr_SetString(PyExc_RuntimeError, "print recursion");
        return -1;
    }
    if (PyErr_CheckSignals())
        return -1;
#ifdef USE_STACKCHECK
    if (PyOS_CheckStack()) {
        PyErr_SetString(PyExc_MemoryError, "stack overflow");
        return -1;
    }
#endif
    clearerr(fp); /* Clear any previous error condition */
    if (op == NULL) {
        Py_BEGIN_ALLOW_THREADS fprintf(fp, "<nil>");
        Py_END_ALLOW_THREADS
    } else {
        if (Py_TYPE(op)->tp_print == NULL) {
            PyObject* s;
            if (flags & Py_PRINT_RAW)
                s = PyObject_Str(op);
            else
                s = PyObject_Repr(op);
            if (s == NULL)
                ret = -1;
            else {
                ret = internal_print(s, fp, Py_PRINT_RAW, nesting + 1);
            }
            Py_XDECREF(s);
        } else
            ret = (*Py_TYPE(op)->tp_print)(op, fp, flags);
    }
    if (ret == 0) {
        if (ferror(fp)) {
            PyErr_SetFromErrno(PyExc_IOError);
            clearerr(fp);
            ret = -1;
        }
    }
    return ret;
}

extern "C" int PyObject_Print(PyObject* obj, FILE* fp, int flags) noexcept {
    return internal_print(obj, fp, flags, 0);
};

extern "C" int PyCallable_Check(PyObject* x) noexcept {
    if (x == NULL)
        return 0;
    return x->cls->tp_call != NULL;
}

void setCAPIException(STOLEN(const ExcInfo&) e) noexcept {
    PyErr_Restore(e.type, e.value, e.traceback);
}

void ensureCAPIExceptionSet() {
    if (!cur_thread_state.curexc_type)
        PyErr_SetString(SystemError, "error return without exception set");
}

void throwCAPIException() {
    checkAndThrowCAPIException();
    raiseExcHelper(SystemError, "error return without exception set");
}

void checkAndThrowCAPIException() {
    // Log these since these are expensive and usually avoidable:
    static StatCounter num_checkAndThrowCAPIException("num_checkAndThrowCAPIException");
    num_checkAndThrowCAPIException.log();

    Box* _type, *value, *tb;
    PyErr_Fetch(&_type, &value, &tb);

    if (!_type)
        assert(!value);

    if (_type) {
        BoxedClass* type = static_cast<BoxedClass*>(_type);
        assert(PyType_Check(_type) && isSubclass(static_cast<BoxedClass*>(type), BaseException)
               && "Only support throwing subclass of BaseException for now");

        if (!value)
            value = incref(Py_None);

        // This is similar to PyErr_NormalizeException:
        if (!isSubclass(value->cls, type)) {
            if (value->cls == tuple_cls) {
                value = runtimeCall(type, ArgPassSpec(0, 0, true, false), autoDecref(value), NULL, NULL, NULL, NULL);
            } else if (value == Py_None) {
                value = runtimeCall(type, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
                Py_DECREF(Py_None);
            } else {
                value = runtimeCall(type, ArgPassSpec(1), autoDecref(value), NULL, NULL, NULL, NULL);
            }
        }

        RELEASE_ASSERT(value->cls == type, "unsupported");

        if (tb)
            throw ExcInfo(value->cls, value, tb);
        Py_DECREF(type);
        Py_XDECREF(tb);
        raiseExc(value);
    }
}

extern "C" void _PystonCallPyExitFuncs(void);
extern "C" int _PystonFlushStdFiles(void);

extern "C" void Py_Exit(int sts) noexcept {
    // A full Py_Finalize() is not safe from here yet, but atexit handlers have to run and
    // sys.stdout/sys.stderr have to be flushed like CPython does.
    _PystonCallPyExitFuncs();
    _PystonFlushStdFiles();

    Stats::dump(false);
    exit(sts);
}

extern "C" void PyErr_GetExcInfo(PyObject** ptype, PyObject** pvalue, PyObject** ptraceback) noexcept {
    ExcInfo* exc = getFrameExcInfo();
    *ptype = xincref(exc->type);
    *pvalue = xincref(exc->value);
    *ptraceback = xincref(exc->traceback);
}

extern "C" void PyErr_SetExcInfo(PyObject* type, PyObject* value, PyObject* traceback) noexcept {
    ExcInfo* exc = getFrameExcInfo();
    AUTO_XDECREF(exc->type);
    AUTO_XDECREF(exc->value);
    AUTO_XDECREF(exc->traceback);

    exc->type = type ? type : incref(Py_None);
    exc->value = value ? value : incref(Py_None);
    exc->traceback = traceback;
}

// With optimizations turned off, we can handle much fewer Python stack frames
// in the same amount of C-stack space.
// Use NDEBUG as a proxy for whether optimizations are turned on.
#ifndef NDEBUG
#define Py_DEFAULT_RECURSION_LIMIT 300
#else
#define Py_DEFAULT_RECURSION_LIMIT 1000
#endif

static int recursion_limit = Py_DEFAULT_RECURSION_LIMIT;
extern "C" {
int _Py_CheckRecursionLimit = Py_DEFAULT_RECURSION_LIMIT;
}

/* the macro Py_EnterRecursiveCall() only calls _Py_CheckRecursiveCall()
   if the recursion_depth reaches _Py_CheckRecursionLimit.
   If USE_STACKCHECK, the macro decrements _Py_CheckRecursionLimit
   to guarantee that _Py_CheckRecursiveCall() is regularly called.
   Without USE_STACKCHECK, there is no need for this. */
extern "C" int _Py_CheckRecursiveCall(const char* where) noexcept {
    PyThreadState* tstate = PyThreadState_GET();

#ifdef USE_STACKCHECK
    if (PyOS_CheckStack()) {
        --tstate->recursion_depth;
        PyErr_SetString(PyExc_MemoryError, "Stack overflow");
        return -1;
    }
#endif
    if (tstate->recursion_depth > recursion_limit) {
        --tstate->recursion_depth;
        PyErr_Format(PyExc_RecursionError, "maximum recursion depth exceeded%s", where);
        return -1;
    }
    _Py_CheckRecursionLimit = recursion_limit;
    return 0;
}

extern "C" int Py_GetRecursionLimit(void) noexcept {
    return recursion_limit;
}

extern "C" void Py_SetRecursionLimit(int new_limit) noexcept {
    recursion_limit = new_limit;
    _Py_CheckRecursionLimit = recursion_limit;
}

extern "C" int PyOS_snprintf(char* str, size_t size, const char* format, ...) noexcept {
    int rc;
    va_list va;

    va_start(va, format);
    rc = PyOS_vsnprintf(str, size, format, va);
    va_end(va);
    return rc;
}

extern "C" int PyOS_vsnprintf(char* str, size_t size, const char* format, va_list va) noexcept {
    int len; /* # bytes written, excluding \0 */
#ifdef HAVE_SNPRINTF
#define _PyOS_vsnprintf_EXTRA_SPACE 1
#else
#define _PyOS_vsnprintf_EXTRA_SPACE 512
    char* buffer;
#endif
    assert(str != NULL);
    assert(size > 0);
    assert(format != NULL);
    /* We take a size_t as input but return an int.  Sanity check
     * our input so that it won't cause an overflow in the
     * vsnprintf return value or the buffer malloc size.  */
    if (size > INT_MAX - _PyOS_vsnprintf_EXTRA_SPACE) {
        len = -666;
        goto Done;
    }

#ifdef HAVE_SNPRINTF
    len = vsnprintf(str, size, format, va);
#else
    /* Emulate it. */
    buffer = (char*)PyMem_MALLOC(size + _PyOS_vsnprintf_EXTRA_SPACE);
    if (buffer == NULL) {
        len = -666;
        goto Done;
    }

    len = vsprintf(buffer, format, va);
    if (len < 0)
        /* ignore the error */;

    else if ((size_t)len >= size + _PyOS_vsnprintf_EXTRA_SPACE)
        Py_FatalError("Buffer overflow in PyOS_snprintf/PyOS_vsnprintf");

    else {
        const size_t to_copy = (size_t)len < size ? (size_t)len : size - 1;
        assert(to_copy < size);
        memcpy(str, buffer, to_copy);
        str[to_copy] = '\0';
    }
    PyMem_FREE(buffer);
#endif
Done:
    if (size > 0)
        str[size - 1] = '\0';
    return len;
#undef _PyOS_vsnprintf_EXTRA_SPACE
}

extern "C" int Py_FdIsInteractive(FILE* fp, const char* filename) noexcept {
    if (isatty((int)fileno(fp)))
        return 1;
    if (!Py_InteractiveFlag)
        return 0;
    return (filename == NULL) || (strcmp(filename, "<stdin>") == 0) || (strcmp(filename, "???") == 0);
}

static const wchar_t* progname = L"pyston";
extern "C" void Py_SetProgramName(const wchar_t* pn) noexcept {
    if (pn && *pn)
        progname = pn;
}

extern "C" wchar_t* Py_GetProgramName(void) noexcept {
    return const_cast<wchar_t*>(progname);
}

static wchar_t* default_home = NULL;
extern "C" void Py_SetPythonHome(const wchar_t* home) noexcept {
    default_home = const_cast<wchar_t*>(home);
}

extern "C" wchar_t* Py_GetPythonHome(void) noexcept {
    return default_home;
}

// A minimal replacement for Modules/getpath.c.  The prefix is PYTHONHOME if it is set, and
// otherwise the directory that holds the executable; the stdlib lives in <prefix>/lib/python3.8.
static std::wstring toWide(llvm::StringRef s) {
    std::string tmp = s.str();
    wchar_t* w = Py_DecodeLocale(tmp.c_str(), NULL);
    RELEASE_ASSERT(w, "can't decode path '%s'", tmp.c_str());
    std::wstring rtn(w);
    PyMem_RawFree(w);
    return rtn;
}

static std::string getProgramFullPathUTF8() {
    static std::string full_path = llvm::sys::fs::getMainExecutable(NULL, (void*)&getProgramFullPathUTF8);
    return full_path;
}

static std::string getPrefixUTF8() {
    const char* home = Py_GETENV("PYTHONHOME");
    if (home && *home)
        return home;
    return llvm::sys::path::parent_path(getProgramFullPathUTF8()).str();
}

static std::string& path_override() {
    static std::string s;
    return s;
}

extern "C" void Py_SetPath(const wchar_t* path) noexcept {
    char* s = Py_EncodeLocale(path, NULL);
    RELEASE_ASSERT(s, "");
    path_override() = s;
    PyMem_Free(s);
}

extern "C" wchar_t* Py_GetProgramFullPath(void) noexcept {
    static std::wstring w = toWide(getProgramFullPathUTF8());
    return const_cast<wchar_t*>(w.c_str());
}

extern "C" wchar_t* Py_GetPrefix(void) noexcept {
    static std::wstring w = toWide(getPrefixUTF8());
    return const_cast<wchar_t*>(w.c_str());
}

extern "C" wchar_t* Py_GetExecPrefix(void) noexcept {
    return Py_GetPrefix();
}

std::vector<std::string> getModuleSearchPath() {
    std::vector<std::string> rtn;
    llvm::StringRef path = path_override();
    std::string env;
    if (path.empty()) {
        const char* pythonpath = Py_GETENV("PYTHONPATH");
        if (pythonpath && *pythonpath)
            env = std::string(pythonpath) + ':';
        llvm::SmallString<128> stdlib(getPrefixUTF8());
        llvm::sys::path::append(stdlib, "lib", "python3.8");
        env += stdlib.str();
        path = env;
    }

    while (!path.empty()) {
        auto p = path.split(':');
        if (!p.first.empty())
            rtn.push_back(p.first.str());
        path = p.second;
    }
    return rtn;
}

extern "C" wchar_t* Py_GetPath(void) noexcept {
    static std::wstring w;
    std::string joined;
    for (auto& p : getModuleSearchPath()) {
        if (!joined.empty())
            joined += ':';
        joined += p;
    }
    w = toWide(joined);
    return const_cast<wchar_t*>(w.c_str());
}

extern "C" BORROWED(PyObject*) PyThreadState_GetDict(void) noexcept {
    Box* dict = cur_thread_state.dict;
    if (!dict) {
        dict = cur_thread_state.dict = new BoxedDict();
    }
    return dict;
}

extern "C" PyOS_sighandler_t PyOS_getsig(int sig) noexcept {
#ifdef HAVE_SIGACTION
    struct sigaction context;
    if (sigaction(sig, NULL, &context) == -1)
        return SIG_ERR;
    return context.sa_handler;
#else
    PyOS_sighandler_t handler;
/* Special signal handling for the secure CRT in Visual Studio 2005 */
#if defined(_MSC_VER) && _MSC_VER >= 1400
    switch (sig) {
        /* Only these signals are valid */
        case SIGINT:
        case SIGILL:
        case SIGFPE:
        case SIGSEGV:
        case SIGTERM:
        case SIGBREAK:
        case SIGABRT:
            break;
        /* Don't call signal() with other values or it will assert */
        default:
            return SIG_ERR;
    }
#endif /* _MSC_VER && _MSC_VER >= 1400 */
    handler = signal(sig, SIG_IGN);
    if (handler != SIG_ERR)
        signal(sig, handler);
    return handler;
#endif
}

extern "C" PyOS_sighandler_t PyOS_setsig(int sig, PyOS_sighandler_t handler) noexcept {
#ifdef HAVE_SIGACTION
    /* Some code in Modules/signalmodule.c depends on sigaction() being
     * used here if HAVE_SIGACTION is defined.  Fix that if this code
     * changes to invalidate that assumption.
     */
    struct sigaction context, ocontext;
    context.sa_handler = handler;
    sigemptyset(&context.sa_mask);
    context.sa_flags = 0;
    if (sigaction(sig, &context, &ocontext) == -1)
        return SIG_ERR;
    return ocontext.sa_handler;
#else
    PyOS_sighandler_t oldhandler;
    oldhandler = signal(sig, handler);
#ifdef HAVE_SIGINTERRUPT
    siginterrupt(sig, 1);
#endif
    return oldhandler;
#endif
}

extern "C" PyObject* PyCFunction_NewEx(PyMethodDef* ml, PyObject* self, PyObject* module) noexcept {
    return new BoxedCApiFunction(ml, self, module);
}

extern "C" PyCFunction PyCFunction_GetFunction(PyObject* op) noexcept {
    if (!PyCFunction_Check(op)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return static_cast<BoxedCApiFunction*>(op)->getFunction();
}

extern "C" PyObject* PyCFunction_GetSelf(BORROWED(PyObject*) op) noexcept {
    if (!PyCFunction_Check(op)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return static_cast<BoxedCApiFunction*>(op)->passthrough;
}

extern "C" int PyCFunction_GetFlags(PyObject* op) noexcept {
    if (!PyCFunction_Check(op)) {
        PyErr_BadInternalCall();
        return -1;
    }
    return static_cast<BoxedCApiFunction*>(op)->method_def->ml_flags;
}

extern "C" PyObject* PyCFunction_Call(PyObject* func, PyObject* arg, PyObject* kw) noexcept {
    assert(arg->cls == tuple_cls);
    assert(!kw || kw->cls == dict_cls);
    return BoxedCApiFunction::tppCall<CAPI>(func, NULL, ArgPassSpec(0, 0, true, true), arg, kw, NULL, NULL, NULL);
}

extern "C" int PyEval_GetRestricted(void) noexcept {
    return 0; // We don't support restricted mode
}

extern "C" void PyEval_InitThreads(void) noexcept {
    // nothing to do here
}

extern "C" BORROWED(struct _frame*) PyEval_GetFrame(void) noexcept {
    Box* frame = NULL;
    try {
        frame = getFrame(0);
    } catch (ExcInfo e) {
        e.clear();
        RELEASE_ASSERT(0, "untested");
    }
    return (struct _frame*)frame;
}

Box* BoxedCApiFunction::__call__(BoxedCApiFunction* self, BoxedTuple* varargs, BoxedDict* kwargs) {
    STAT_TIMER(t0, "us_timer_boxedcapifunction__call__", (self->cls->is_user_defined ? 10 : 20));
    assert(self->cls == capifunc_cls);
    assert(varargs->cls == tuple_cls);
    assert(!kwargs || kwargs->cls == dict_cls);

    // Kind of silly to have asked callFunc to rearrange the arguments for us, just to pass things
    // off to tppCall, but this case should be very uncommon (people explicitly asking for __call__)

    return BoxedCApiFunction::tppCall<CXX>(self, NULL, ArgPassSpec(0, 0, true, true), varargs, kwargs, NULL, NULL,
                                           NULL);
}

// METH_FASTCALL functions receive their arguments as a C array.  We collect them into a tuple
// (and a dict for keywords) like for METH_VARARGS, and unpack them here.
Box* capiCallFastcall(Box* passthrough, PyCFunction func, Box* args) noexcept {
    assert(PyTuple_CheckExact(args));
    return ((_PyCFunctionFast)func)(passthrough, &PyTuple_GET_ITEM(args, 0), PyTuple_GET_SIZE(args));
}

Box* capiCallFastcallKeywords(Box* passthrough, PyCFunction func, Box* args, Box* kwargs) noexcept {
    assert(PyTuple_CheckExact(args));
    Py_ssize_t nargs = PyTuple_GET_SIZE(args);
    Py_ssize_t nkw = kwargs ? PyDict_GET_SIZE(kwargs) : 0;
    if (nkw == 0)
        return ((_PyCFunctionFastWithKeywords)func)(passthrough, &PyTuple_GET_ITEM(args, 0), nargs, NULL);

    llvm::SmallVector<Box*, 8> stack;
    for (Py_ssize_t i = 0; i < nargs; i++)
        stack.push_back(PyTuple_GET_ITEM(args, i));

    Box* kwnames = PyTuple_New(nkw);
    if (!kwnames)
        return NULL;
    AUTO_DECREF(kwnames);
    Py_ssize_t pos = 0, i = 0;
    PyObject *key, *value;
    while (PyDict_Next(kwargs, &pos, &key, &value)) {
        PyTuple_SET_ITEM(kwnames, i++, incref(key));
        stack.push_back(value);
    }
    return ((_PyCFunctionFastWithKeywords)func)(passthrough, stack.data(), nargs, kwnames);
}

template <ExceptionStyle S>
Box* BoxedCApiFunction::tppCall(Box* _self, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2,
                                Box* arg3, Box** args,
                                const std::vector<BoxedString*>* keyword_names) noexcept(S == CAPI) {
    STAT_TIMER(t0, "us_timer_boxedcapifunction__call__", 10);

    BoxedCApiFunction* self = static_cast<BoxedCApiFunction*>(_self);

    if (rewrite_args) {
        rewrite_args->obj->addGuard((intptr_t)self);
        rewrite_args->rewriter->addGCReference(self);
    }

    int flags = self->method_def->ml_flags;
    auto func = self->method_def->ml_meth;

    flags &= ~(METH_CLASS | METH_STATIC | METH_COEXIST);

    ParamReceiveSpec paramspec(0, 0, true, false);
    Box** defaults = NULL;
    if (flags == METH_VARARGS) {
        paramspec = ParamReceiveSpec(0, 0, true, false);
    } else if (flags == (METH_VARARGS | METH_KEYWORDS)) {
        paramspec = ParamReceiveSpec(0, 0, true, true);
    } else if (flags == METH_NOARGS) {
        paramspec = ParamReceiveSpec(0, 0, false, false);
    } else if (flags == METH_O) {
        paramspec = ParamReceiveSpec(1, 0, false, false);
    } else if (flags == METH_FASTCALL) {
        paramspec = ParamReceiveSpec(0, 0, true, false);
    } else if (flags == (METH_FASTCALL | METH_KEYWORDS)) {
        paramspec = ParamReceiveSpec(0, 0, true, true);
    } else if ((flags & ~(METH_O3 | METH_D3)) == 0) {
        int num_args = 0;
        if (flags & METH_O)
            num_args++;
        if (flags & METH_O2)
            num_args += 2;

        int num_defaults = 0;
        if (flags & METH_D1)
            num_defaults++;
        if (flags & METH_D2)
            num_defaults += 2;

        paramspec = ParamReceiveSpec(num_args, num_defaults, false, false);
        if (num_defaults) {
            static Box* _defaults[] = { NULL, NULL, NULL };
            assert(num_defaults <= 3);
            defaults = _defaults;
        }

        assert(paramspec.totalReceived() <= 3); // would need to allocate oargs
    } else {
        RELEASE_ASSERT(0, "0x%x", flags);
    }

    Box** oargs = NULL;

    if (func == (void*)tp_new_wrapper) {
        assert(PyType_Check(self->passthrough));
        BoxedClass* passthrough_cls = static_cast<BoxedClass*>(self->passthrough);
        if (passthrough_cls->tp_new == BaseException->tp_new && argspec.num_args >= 1) {
            // Optimization: BaseException->tp_new doesn't look at its arguments.
            // Don't bother allocating any
            assert(paramspec == ParamReceiveSpec(0, 0, true, true));

            assert(PyType_Check(arg1));
            Box* rtn = BaseException->tp_new(static_cast<BoxedClass*>(arg1), NULL, NULL);
            if (rewrite_args) {
                rewrite_args->out_rtn
                    = rewrite_args->rewriter->call(true, (void*)BaseException->tp_new, rewrite_args->arg1,
                                                   rewrite_args->rewriter->loadConst(0, Location::forArg(1)),
                                                   rewrite_args->rewriter->loadConst(0, Location::forArg(2)));
                rewrite_args->out_rtn->setType(RefType::OWNED);
                rewrite_args->out_success = true;
            }
            assert(rtn); // This shouldn't throw, otherwise we would need to check the return value
            return rtn;
        }
        // TODO rewrite these cases specially; tp_new_wrapper just slices the args array,
        // so we could just rearrangeArguments to the form that it wants and then call tp_new directly.
    }

    auto continuation = [=](CallRewriteArgs* rewrite_args, Box* arg1, Box* arg2, Box* arg3, Box** args) {
        RewriterVar* r_passthrough = NULL;
        if (rewrite_args)
            r_passthrough = rewrite_args->rewriter->loadConst((intptr_t)self->passthrough, Location::forArg(0));

        Box* rtn;
        if (flags == METH_VARARGS) {
            rtn = (Box*)func(self->passthrough, arg1);
            if (rewrite_args)
                rewrite_args->out_rtn = rewrite_args->rewriter->call(true, (void*)func, r_passthrough,
                                                                     rewrite_args->arg1)->setType(RefType::OWNED);
        } else if (flags == (METH_VARARGS | METH_KEYWORDS)) {
            rtn = (Box*)((PyCFunctionWithKeywords)func)(self->passthrough, arg1, arg2);
            if (rewrite_args)
                rewrite_args->out_rtn
                    = rewrite_args->rewriter->call(true, (void*)func, r_passthrough, rewrite_args->arg1,
                                                   rewrite_args->arg2)->setType(RefType::OWNED);
        } else if (flags == METH_NOARGS) {
            rtn = (Box*)func(self->passthrough, NULL);
            if (rewrite_args)
                rewrite_args->out_rtn
                    = rewrite_args->rewriter->call(true, (void*)func, r_passthrough,
                                                   rewrite_args->rewriter->loadConst(0, Location::forArg(1)))
                          ->setType(RefType::OWNED);
        } else if (flags == METH_O) {
            rtn = (Box*)func(self->passthrough, arg1);
            if (rewrite_args)
                rewrite_args->out_rtn = rewrite_args->rewriter->call(true, (void*)func, r_passthrough,
                                                                     rewrite_args->arg1)->setType(RefType::OWNED);
        } else if ((flags & ~(METH_O3 | METH_D3)) == 0) {
            assert(paramspec.totalReceived() <= 3); // would need to pass through oargs
            rtn = ((Box * (*)(Box*, Box*, Box*, Box**))func)(self->passthrough, arg1, arg2, &arg3);
            if (rewrite_args) {
                if (paramspec.totalReceived() == 1)
                    rewrite_args->out_rtn = rewrite_args->rewriter->call(true, (void*)func, r_passthrough,
                                                                         rewrite_args->arg1)->setType(RefType::OWNED);
                else if (paramspec.totalReceived() == 2)
                    rewrite_args->out_rtn
                        = rewrite_args->rewriter->call(true, (void*)func, r_passthrough, rewrite_args->arg1,
                                                       rewrite_args->arg2)->setType(RefType::OWNED);
                else if (paramspec.totalReceived() == 3) {
                    auto args = rewrite_args->rewriter->allocate(1);
                    args->setAttr(0, rewrite_args->arg3);
                    rewrite_args->out_rtn
                        = rewrite_args->rewriter->call(true, (void*)func, r_passthrough, rewrite_args->arg1,
                                                       rewrite_args->arg2, args)->setType(RefType::OWNED);
                } else
                    abort();
            }
        } else if (flags == METH_FASTCALL) {
            rtn = capiCallFastcall(self->passthrough, func, arg1);
            if (rewrite_args)
                rewrite_args->out_rtn
                    = rewrite_args->rewriter->call(true, (void*)capiCallFastcall, r_passthrough,
                                                   rewrite_args->rewriter->loadConst((intptr_t)func, Location::forArg(1)),
                                                   rewrite_args->arg1)->setType(RefType::OWNED);
        } else if (flags == (METH_FASTCALL | METH_KEYWORDS)) {
            rtn = capiCallFastcallKeywords(self->passthrough, func, arg1, arg2);
            if (rewrite_args)
                rewrite_args->out_rtn
                    = rewrite_args->rewriter->call(true, (void*)capiCallFastcallKeywords, r_passthrough,
                                                   rewrite_args->rewriter->loadConst((intptr_t)func, Location::forArg(1)),
                                                   rewrite_args->arg1, rewrite_args->arg2)->setType(RefType::OWNED);
        } else {
            RELEASE_ASSERT(0, "0x%x", flags);
        }

        if (rewrite_args) {
            if (S == CXX)
                rewrite_args->rewriter->checkAndThrowCAPIException(rewrite_args->out_rtn);
            rewrite_args->out_success = true;
        }

        if (S == CXX && !rtn)
            throwCAPIException();
        return rtn;
    };

    return callCXXFromStyle<S>([=]() {
        return rearrangeArgumentsAndCall(paramspec, NULL, self->method_def->ml_name, defaults, rewrite_args, argspec,
                                         arg1, arg2, arg3, args, keyword_names, continuation);
    });
}

/* extension modules might be compiled with GC support so these
   functions must always be available */

#undef PyObject_GC_Track
#undef PyObject_GC_UnTrack
#undef PyObject_GC_Del
#undef _PyObject_GC_Malloc

extern "C" void _Py_FatalError(const char* fmt, const char* function, const char* message) {
    fprintf(stderr, fmt, function, message);
    fflush(stderr); /* it helps in Windows debug build */
    abort();
}

extern "C" void Py_FatalError(const char* msg) noexcept {
    fprintf(stderr, "Fatal Python error: %s\n", msg);
    fflush(stderr);
    abort();
}

extern "C" int Py_IsInitialized(void) noexcept {
    return 1;
}

extern "C" int PyStatus_Exception(PyStatus status) noexcept {
    return status._type != PyStatus::_PyStatus_TYPE_OK;
}

// Set by the signal module when a KeyboardInterrupt isn't handled.
extern "C" {
int _Py_UnhandledKeyboardInterrupt = 0;
}

extern "C" int _Py_IsFinalizing(void) noexcept {
    // TODO: Pyston doesn't track this yet.
    return 0;
}

extern "C" void _Py_RestoreSignals(void) noexcept {
#ifdef SIGPIPE
    PyOS_setsig(SIGPIPE, SIG_DFL);
#endif
#ifdef SIGXFZ
    PyOS_setsig(SIGXFZ, SIG_DFL);
#endif
#ifdef SIGXFSZ
    PyOS_setsig(SIGXFSZ, SIG_DFL);
#endif
}

extern "C" void _Py_DumpPathConfig(PyThreadState* tstate) noexcept {
}

extern "C" int _Py_SetFileSystemEncoding(const char* encoding, const char* errors) noexcept {
    // Pyston always uses utf-8 with surrogateescape, see builtins.cpp.
    return 0;
}

extern "C" void _PyMem_DumpTraceback(int fd, const void* ptr) noexcept {
}

extern "C" int PyContext_ClearFreeList(void) noexcept {
    return 0;
}

extern "C" int PyAsyncGen_ClearFreeLists(void) noexcept {
    return 0;
}

extern "C" int PyMethod_ClearFreeList(void) noexcept {
    return 0;
}

extern "C" PyObject* PyMethod_New(PyObject* func, PyObject* self) noexcept {
    if (self == NULL) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return new BoxedInstanceMethod(self, func);
}

extern "C" BORROWED(PyObject*) PyMethod_Function(PyObject* im) noexcept {
    if (!PyMethod_Check(im)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return static_cast<BoxedInstanceMethod*>(im)->func;
}

extern "C" BORROWED(PyObject*) PyMethod_Self(PyObject* im) noexcept {
    if (!PyMethod_Check(im)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return static_cast<BoxedInstanceMethod*>(im)->obj;
}

extern "C" int PyCFunction_ClearFreeList() noexcept {
    return 0; // number of entries cleared
}

void setupCAPI() {
    capifunc_cls->giveAttr("__repr__", new BoxedFunction(BoxedCode::create((void*)BoxedCApiFunction::__repr__<CXX>,
                                                                           UNKNOWN, 1, "capifunc.__repr__")));


    auto capi_call
        = new BoxedFunction(BoxedCode::create((void*)BoxedCApiFunction::__call__, UNKNOWN, 1, true, true,
                                              "capifunc.__call__", "", ParamNames({ "self" }, "args", "kw", 1)));
    capifunc_cls->giveAttr("__call__", capi_call);
    capifunc_cls->tpp_call.capi_val = BoxedCApiFunction::tppCall<CAPI>;
    capifunc_cls->tpp_call.cxx_val = BoxedCApiFunction::tppCall<CXX>;
    capifunc_cls->giveAttrDescriptor("__name__", BoxedCApiFunction::getname, NULL);
    capifunc_cls->giveAttrDescriptor("__doc__", BoxedCApiFunction::doc, NULL);
    capifunc_cls->giveAttrMember("__module__", T_OBJECT, offsetof(BoxedCApiFunction, module));

    {
        static PyMethodDef capifunc_methods[] = {
            { "__reduce__", (PyCFunction)builtinFunctionReduce, METH_NOARGS, NULL },
            { NULL, NULL, 0, NULL },
        };
        add_methods(capifunc_cls, capifunc_methods);
    }
    capifunc_cls->freeze();
    capifunc_cls->tp_repr = BoxedCApiFunction::__repr__<CAPI>;
}
}
