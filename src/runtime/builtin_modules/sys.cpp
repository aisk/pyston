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
#include <cmath>
#include <dlfcn.h>
#include <fcntl.h>
#include <langinfo.h>
#include <sstream>

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "Python.h"
#include "structseq.h"

#include "capi/typeobject.h"
#include "capi/types.h"
#include "codegen/unwinding.h"
#include "core/types.h"
#include "runtime/inline/boxing.h"
#include "runtime/inline/list.h"
#include "runtime/int.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

BoxedModule* sys_module;
BoxedDict* sys_modules_dict;

extern "C" {
void _PystonInterp_SetModules(PyObject* modules, PyObject* sysdict);
PyStatus _PyErr_Init(void);
extern int _Py_int_max_str_digits, _Py_global_config_int_max_str_digits;

// supposed to be exposed through sys.flags
int Py_BytesWarningFlag = 0;
int Py_HashRandomizationFlag = 0;
int Py_DebugFlag = 0;
int Py_QuietFlag = 0;
int Py_IsolatedFlag = 0;
// Pyston change: set this by default to True
// We can support many cases of users wanting to write bytecode, but there are some
// that are much harder to support (py.test wants to write its own pyc files), and
// this flag is one of the few levers we have to avoid hitting those paths.
int Py_DontWriteBytecodeFlag = 1;
int Py_NoUserSiteDirectory = 0;
}

Box* sys_exc_info_func;

Box* sysExcInfo() {
    ExcInfo* exc = getFrameExcInfo();
    assert(exc->type);
    assert(exc->value);
    Box* tb = exc->traceback ? exc->traceback : Py_None;
    return BoxedTuple::create({ exc->type, exc->value, tb });
}

static Box* sysExit(Box* arg) {
    assert(arg);
    Box* exc = runtimeCall(SystemExit, ArgPassSpec(1), arg, NULL, NULL, NULL, NULL);
    raiseExc(exc);
}

BORROWED(BoxedDict*) getSysModulesDict() {
    // PyPy's behavior: fetch from sys.modules each time:
    // Box *_sys_modules = sys_module->getattr("modules");
    // assert(_sys_modules);
    // assert(_sys_modules->cls == dict_cls);
    // return static_cast<BoxedDict*>(_sys_modules);

    // CPython's behavior: return an internalized reference:
    return sys_modules_dict;
}

BORROWED(BoxedList*) getSysPath() {
    // Unlike sys.modules, CPython handles sys.path by fetching it each time:
    auto path_str = getStaticString("path");

    Box* _sys_path = sys_module->getattr(path_str);
    assert(_sys_path);

    if (_sys_path->cls != list_cls) {
        raiseExcHelper(RuntimeError, "sys.path must be a list of directory names");
    }

    assert(_sys_path->cls == list_cls);
    return static_cast<BoxedList*>(_sys_path);
}

Box* sysGetFrame(Box* val) {
    int depth = 0;
    if (val) {
        if (!PyInt_Check(val)) {
            raiseExcHelper(TypeError, "TypeError: an integer is required");
        }
        depth = static_cast<BoxedInt*>(val)->n;
    }

    Box* frame = getFrame(depth);
    if (!frame) {
        raiseExcHelper(ValueError, "call stack is not deep enough");
    }
    return incref(frame);
}

Box* sysCurrentFrames() {
    Box* rtn = _PyThread_CurrentFrames();
    if (!rtn)
        throwCAPIException();
    return rtn;
}

Box* sysGetDefaultEncoding() {
    return boxString(PyUnicode_GetDefaultEncoding());
}

Box* sysGetFilesystemEncoding() {
    if (Py_FileSystemDefaultEncoding)
        return boxString(Py_FileSystemDefaultEncoding);
    Py_RETURN_NONE;
}

Box* sysGetRecursionLimit() {
    return PyLong_FromLong(Py_GetRecursionLimit());
}

extern "C" int PySys_SetObject(const char* name, PyObject* v) noexcept {
    try {
        if (!v) {
            if (sys_module->getattr(autoDecref(internStringMortal(name))))
                sys_module->delattr(autoDecref(internStringMortal(name)), NULL);
        } else
            sys_module->setattr(autoDecref(internStringMortal(name)), v, NULL);
    } catch (ExcInfo e) {
        abort();
    }
    return 0;
}

extern "C" BORROWED(PyObject*) PySys_GetObject(const char* name) noexcept {
    return sys_module->getattr(autoDecref(internStringMortal(name)));
}

extern "C" BORROWED(PyObject*) _PySys_GetObjectId(_Py_Identifier* key) noexcept {
    return PySys_GetObject(key->string);
}

extern "C" int _PySys_SetObjectId(_Py_Identifier* key, PyObject* v) noexcept {
    return PySys_SetObject(key->string, v);
}

static void mywrite(const char* name, FILE* fp, const char* format, va_list va) noexcept {
    PyObject* file;
    PyObject* error_type, *error_value, *error_traceback;

    PyErr_Fetch(&error_type, &error_value, &error_traceback);
    file = PySys_GetObject(name);
    if (file == NULL || file == Py_None)
        vfprintf(fp, format, va);
    else {
        char buffer[1001];
        const int written = PyOS_vsnprintf(buffer, sizeof(buffer), format, va);
        if (PyFile_WriteString(buffer, file) != 0) {
            PyErr_Clear();
            fputs(buffer, fp);
        }
        if (written < 0 || (size_t)written >= sizeof(buffer)) {
            const char* truncated = "... truncated";
            if (PyFile_WriteString(truncated, file) != 0) {
                PyErr_Clear();
                fputs(truncated, fp);
            }
        }
    }
    PyErr_Restore(error_type, error_value, error_traceback);
}

extern "C" void PySys_WriteStdout(const char* format, ...) noexcept {
    va_list va;

    va_start(va, format);
    mywrite("stdout", stdout, format, va);
    va_end(va);
}

extern "C" void PySys_WriteStderr(const char* format, ...) noexcept {
    va_list va;

    va_start(va, format);
    mywrite("stderr", stderr, format, va);
    va_end(va);
}

extern "C" void PySys_FormatStderr(const char* format, ...) noexcept {
    PyObject* error_type, *error_value, *error_traceback;
    va_list va;

    PyErr_Fetch(&error_type, &error_value, &error_traceback);
    va_start(va, format);
    PyObject* msg = PyUnicode_FromFormatV(format, va);
    va_end(va);
    if (msg != NULL) {
        PyObject* file = PySys_GetObject("stderr");
        if (file == NULL || file == Py_None || PyFile_WriteObject(msg, file, Py_PRINT_RAW) != 0) {
            PyErr_Clear();
            fputs(PyUnicode_AsUTF8(msg), stderr);
        }
        Py_DECREF(msg);
    }
    PyErr_Restore(error_type, error_value, error_traceback);
}

// TODO: audit hooks (PEP 578) aren't supported.
extern "C" int PySys_Audit(const char* event, const char* argFormat, ...) noexcept {
    return 0;
}

extern "C" size_t _PySys_GetSizeOf(PyObject* o) noexcept {
    PyObject* res = NULL;
    PyObject* method;
    Py_ssize_t size;

    /* Make sure the type is initialized. float gets initialized late */
    if (PyType_Ready(Py_TYPE(o)) < 0)
        return (size_t)-1;

    method = _PyObject_LookupSpecial(o, "__sizeof__", NULL);
    if (method == NULL) {
        if (!PyErr_Occurred())
            PyErr_Format(PyExc_TypeError, "Type %.100s doesn't define __sizeof__", Py_TYPE(o)->tp_name);
    } else {
        res = _PyObject_CallNoArg(method);
        Py_DECREF(method);
    }

    if (res == NULL)
        return (size_t)-1;

    size = PyLong_AsSsize_t(res);
    Py_DECREF(res);
    if (size == -1 && PyErr_Occurred())
        return (size_t)-1;

    if (size < 0) {
        PyErr_SetString(PyExc_ValueError, "__sizeof__() should return >= 0");
        return (size_t)-1;
    }

    /* add gc_head size */
    if (PyObject_IS_GC(o))
        return ((size_t)size) + sizeof(PyGC_Head);
    return (size_t)size;
}

// Creates sys.stdin, sys.stdout and sys.stderr through the io module, like create_stdio() in
// CPython's pylifecycle.c, and sets builtins.open to io.open.
void setupStdio() {
    PyObject* io = PyImport_ImportModule("io");
    if (!io)
        throwCAPIException();
    AUTO_DECREF(io);
    PyObject* open = PyObject_GetAttrString(io, "open");
    if (!open)
        throwCAPIException();
    AUTO_DECREF(open);
    PyDict_SetItemString(builtins_module->getAttrWrapper(), "open", open);

    struct {
        int fd;
        const char* name;
        const char* mode;
        const char* errors;
    } streams[] = { { 0, "stdin", "r", "surrogateescape" },
                    { 1, "stdout", "w", "surrogateescape" },
                    { 2, "stderr", "w", "backslashreplace" } };
    for (auto&& s : streams) {
        PyObject* f = NULL;
        if (fcntl(s.fd, F_GETFD) >= 0) {
            f = PyObject_CallFunction(open, "isisssi", s.fd, s.mode, -1, "utf-8", s.errors, "\n", 0);
            if (!f)
                throwCAPIException();
        } else {
            f = incref(Py_None);
        }
        AUTO_DECREF(f);
        std::string dunder = std::string("__") + s.name + "__";
        if (PySys_SetObject(s.name, f) < 0 || PySys_SetObject(dunder.c_str(), f) < 0)
            throwCAPIException();
    }
}

void addToSysArgv(const char* str) {
    static BoxedString* argv_str = getStaticString("argv");
    Box* sys_argv = sys_module->getattr(argv_str);
    assert(sys_argv);
    assert(sys_argv->cls == list_cls);
    listAppendInternalStolen(sys_argv, boxString(str));
}

void appendToSysPath(llvm::StringRef path) {
    BoxedList* sys_path = getSysPath();
    listAppendInternalStolen(sys_path, boxString(path));
}

void prependToSysPath(llvm::StringRef path) {
    BoxedList* sys_path = getSysPath();
    static BoxedString* insert_str = getStaticString("insert");
    CallattrFlags callattr_flags{.cls_only = false, .null_on_nonexistent = false, .argspec = ArgPassSpec(2) };
    autoDecref(callattr(sys_path, insert_str, callattr_flags, autoDecref(boxInt(0)), autoDecref(boxString(path)), NULL,
                        NULL, NULL));
}

static std::string generateVersionString() {
    std::ostringstream oss;
    oss << PY_MAJOR_VERSION << '.' << PY_MINOR_VERSION << '.' << PY_MICRO_VERSION;
    // Same shape as CPython's "3.8.20 (default, Oct 28 2026, 12:00:00) \n[GCC ...]", which the
    // platform module parses.
    oss << " (default, " << __DATE__ << ", " << __TIME__ << ") \n";
    oss << "[Pyston " << PYSTON_VERSION_MAJOR << '.' << PYSTON_VERSION_MINOR << '.' << PYSTON_VERSION_MICRO << "]";
    return oss.str();
}

extern "C" const char* Py_GetVersion(void) noexcept {
    static std::string version = generateVersionString();
    return version.c_str();
}

static bool isLittleEndian() {
    unsigned long number = 1;
    char* s = (char*)&number;
    return s[0] != 0;
}

extern "C" const char* Py_GetPlatform() noexcept {
// cpython does this check in their configure script
#if defined(__linux__)
    return "linux";
#elif defined(__APPLE__) && defined(__MACH__)
    return "darwin";
#else
    // cpython also supports returning "atheos", "irix6", "win32", or
    // whatever the user can get PLATFORM to be #defined as at
    // build-time.
    return "unknown";
#endif
}

extern "C" BORROWED(PyObject*) PySys_GetModulesDict() noexcept {
    return getSysModulesDict();
}

static PyObject* sys_getsizeof(PyObject* self, PyObject* args, PyObject* kwds) noexcept {
    static const char* kwlist[] = { "object", "default", 0 };
    size_t size;
    PyObject* o, * dflt = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|O:getsizeof", const_cast<char**>(kwlist), &o, &dflt))
        return NULL;

    size = _PySys_GetSizeOf(o);

    if (size == (size_t)-1 && PyErr_Occurred()) {
        /* Has a default value been given */
        if (dflt != NULL && PyErr_ExceptionMatches(PyExc_TypeError)) {
            PyErr_Clear();
            Py_INCREF(dflt);
            return dflt;
        } else
            return NULL;
    }

    return PyLong_FromSize_t(size);
}

static PyObject* sys_excepthook(PyObject* self, PyObject* args) noexcept {
    PyObject* exc, *value, *tb;
    if (!PyArg_UnpackTuple(args, "excepthook", 3, 3, &exc, &value, &tb))
        return NULL;
    PyErr_Display(exc, value, tb);
    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject* sys_displayhook(PyObject* self, PyObject* o) noexcept {
    PyObject* outf;

    // Pyston change: we currently hardcode the builtins module
    /*
    PyInterpreterState* interp = PyThreadState_GET()->interp;
    PyObject* modules = interp->modules;
    PyObject* builtins = PyDict_GetItemString(modules, "__builtin__");

    if (builtins == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "lost __builtin__");
        return NULL;
    }
    */
    PyObject* builtins = builtins_module;

    /* Print value except if None */
    /* After printing, also assign to '_' */
    /* Before, set '_' to None to avoid recursion */
    if (o == Py_None) {
        Py_INCREF(Py_None);
        return Py_None;
    }
    if (PyObject_SetAttrString(builtins, "_", Py_None) != 0)
        return NULL;
    outf = PySys_GetObject("stdout");
    if (outf == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "lost sys.stdout");
        return NULL;
    }
    if (PyFile_WriteObject(o, outf, 0) != 0)
        return NULL;
    if (PyFile_WriteString("\n", outf) != 0)
        return NULL;
    if (PyObject_SetAttrString(builtins, "_", o) != 0)
        return NULL;
    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject* sys_clear_type_cache(PyObject* self, PyObject* args) {
    PyType_ClearCache();
    Py_RETURN_NONE;
}

PyDoc_STRVAR(sys_clear_type_cache__doc__, "_clear_type_cache() -> None\n\
        Clear the internal type lookup cache.");

static PyObject* sys_getrefcount(PyObject* self, PyObject* arg) {
    return PyLong_FromSsize_t(arg->ob_refcnt);
}

#ifdef Py_REF_DEBUG
static PyObject* sys_gettotalrefcount(PyObject* self) {
    return PyLong_FromSsize_t(_Py_GetRefTotal());
}
#endif /* Py_REF_DEBUG */

PyDoc_STRVAR(getrefcount_doc, "getrefcount(object) -> integer\n\
        \n\
        Return the reference count of object.  The count returned is generally\n\
        one higher than you might expect, because it includes the (temporary)\n\
        reference as an argument to getrefcount().");

PyDoc_STRVAR(excepthook_doc, "excepthook(exctype, value, traceback) -> None\n"
                             "\n"
                             "Handle an exception by displaying it with a traceback on sys.stderr.\n");

PyDoc_STRVAR(displayhook_doc, "displayhook(object) -> None\n"
                              "\n"
                              "Print an object to sys.stdout and also save it in __builtin__._\n");

PyDoc_STRVAR(getsizeof_doc, "getsizeof(object, default) -> int\n\
\n\
Return the size of object in bytes.");

static PyObject* sys_getfilesystemencodeerrors(PyObject* self, PyObject* args) noexcept {
    return PyUnicode_FromString("surrogateescape");
}

static PyObject* sys_intern(PyObject* self, PyObject* s) noexcept {
    if (!PyUnicode_CheckExact(s)) {
        PyErr_Format(PyExc_TypeError, "can't intern %.400s", Py_TYPE(s)->tp_name);
        return NULL;
    }
    Py_INCREF(s);
    PyUnicode_InternInPlace(&s);
    return s;
}

static PyObject* sys_setrecursionlimit(PyObject* self, PyObject* arg) noexcept {
    int new_limit = _PyLong_AsInt(arg);
    if (new_limit == -1 && PyErr_Occurred())
        return NULL;
    if (new_limit < 1) {
        PyErr_SetString(PyExc_ValueError, "recursion limit must be greater or equal than 1");
        return NULL;
    }
    Py_SetRecursionLimit(new_limit);
    Py_RETURN_NONE;
}

// TODO: tracing and profiling aren't implemented.  The hooks are stored so that they can be read
// back, but they are never called.
static PyObject* trace_func, *profile_func;

static PyObject* sys_settrace(PyObject* self, PyObject* func) noexcept {
    Py_XSETREF(trace_func, func == Py_None ? NULL : incref(func));
    Py_RETURN_NONE;
}

static PyObject* sys_gettrace(PyObject* self, PyObject* args) noexcept {
    return incref(trace_func ? trace_func : Py_None);
}

static PyObject* sys_setprofile(PyObject* self, PyObject* func) noexcept {
    Py_XSETREF(profile_func, func == Py_None ? NULL : incref(func));
    Py_RETURN_NONE;
}

static PyObject* sys_getprofile(PyObject* self, PyObject* args) noexcept {
    return incref(profile_func ? profile_func : Py_None);
}

static double switch_interval = 0.005;
static int check_interval = 100;
static int dlopen_flags = RTLD_NOW;

static PyObject* sys_setswitchinterval(PyObject* self, PyObject* arg) noexcept {
    double d = PyFloat_AsDouble(arg);
    if (d == -1.0 && PyErr_Occurred())
        return NULL;
    if (d <= 0.0) {
        PyErr_SetString(PyExc_ValueError, "switch interval must be strictly positive");
        return NULL;
    }
    switch_interval = d;
    Py_RETURN_NONE;
}

static PyObject* sys_getswitchinterval(PyObject* self, PyObject* args) noexcept {
    return PyFloat_FromDouble(switch_interval);
}

static PyObject* sys_setcheckinterval(PyObject* self, PyObject* args) noexcept {
    if (!PyArg_ParseTuple(args, "i:setcheckinterval", &check_interval))
        return NULL;
    Py_RETURN_NONE;
}

static PyObject* sys_getcheckinterval(PyObject* self, PyObject* args) noexcept {
    return PyLong_FromLong(check_interval);
}

static PyObject* sys_setdlopenflags(PyObject* self, PyObject* args) noexcept {
    if (!PyArg_ParseTuple(args, "i:setdlopenflags", &dlopen_flags))
        return NULL;
    Py_RETURN_NONE;
}

static PyObject* sys_getdlopenflags(PyObject* self, PyObject* args) noexcept {
    return PyLong_FromLong(dlopen_flags);
}

static PyObject* sys_is_finalizing(PyObject* self, PyObject* args) noexcept {
    return PyBool_FromLong(_Py_IsFinalizing());
}

static PyObject* sys_call_tracing(PyObject* self, PyObject* args) noexcept {
    PyObject* func, *funcargs;
    if (!PyArg_ParseTuple(args, "OO!:call_tracing", &func, &PyTuple_Type, &funcargs))
        return NULL;
    return PyObject_Call(func, funcargs, NULL);
}

static PyObject* sys_getallocatedblocks(PyObject* self, PyObject* args) noexcept {
    return PyLong_FromLong(0);
}

// TODO: audit hooks (PEP 578) aren't implemented.
static PyObject* sys_audit(PyObject* self, PyObject* args) noexcept {
    Py_RETURN_NONE;
}

static PyObject* sys_addaudithook(PyObject* self, PyObject* hook) noexcept {
    Py_RETURN_NONE;
}

static PyObject* sys_get_int_max_str_digits(PyObject* self, PyObject* args) noexcept {
    return PyLong_FromLong(_Py_int_max_str_digits);
}

static PyObject* sys_set_int_max_str_digits(PyObject* self, PyObject* args, PyObject* kwargs) noexcept {
    static char maxdigits_str[] = "maxdigits";
    static char* kwlist[] = { maxdigits_str, NULL };
    int maxdigits;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "i:set_int_max_str_digits", kwlist, &maxdigits))
        return NULL;
    if (maxdigits != 0 && maxdigits < 640) {
        PyErr_Format(PyExc_ValueError, "maxdigits must be 0 or larger than %d", 640);
        return NULL;
    }
    _Py_int_max_str_digits = maxdigits;
    Py_RETURN_NONE;
}

static PyObject* sys_unraisablehook(PyObject* self, PyObject* unraisable) noexcept {
    PyObject* exc_type = PyObject_GetAttrString(unraisable, "exc_type");
    PyObject* exc_value = exc_type ? PyObject_GetAttrString(unraisable, "exc_value") : NULL;
    PyObject* exc_tb = exc_value ? PyObject_GetAttrString(unraisable, "exc_traceback") : NULL;
    if (!exc_tb) {
        Py_XDECREF(exc_type);
        Py_XDECREF(exc_value);
        return NULL;
    }
    PyErr_Display(exc_type, exc_value, exc_tb);
    Py_DECREF(exc_type);
    Py_DECREF(exc_value);
    Py_DECREF(exc_tb);
    Py_RETURN_NONE;
}

static PyMethodDef sys_methods[] = {
    { "getfilesystemencodeerrors", sys_getfilesystemencodeerrors, METH_NOARGS, NULL },
    { "intern", sys_intern, METH_O, NULL },
    { "setrecursionlimit", sys_setrecursionlimit, METH_O, NULL },
    { "settrace", sys_settrace, METH_O, NULL },
    { "gettrace", sys_gettrace, METH_NOARGS, NULL },
    { "setprofile", sys_setprofile, METH_O, NULL },
    { "getprofile", sys_getprofile, METH_NOARGS, NULL },
    { "setswitchinterval", sys_setswitchinterval, METH_O, NULL },
    { "getswitchinterval", sys_getswitchinterval, METH_NOARGS, NULL },
    { "setcheckinterval", sys_setcheckinterval, METH_VARARGS, NULL },
    { "getcheckinterval", sys_getcheckinterval, METH_NOARGS, NULL },
    { "setdlopenflags", sys_setdlopenflags, METH_VARARGS, NULL },
    { "getdlopenflags", sys_getdlopenflags, METH_NOARGS, NULL },
    { "is_finalizing", sys_is_finalizing, METH_NOARGS, NULL },
    { "call_tracing", sys_call_tracing, METH_VARARGS, NULL },
    { "getallocatedblocks", sys_getallocatedblocks, METH_NOARGS, NULL },
    { "audit", sys_audit, METH_VARARGS, NULL },
    { "addaudithook", sys_addaudithook, METH_O, NULL },
    { "unraisablehook", sys_unraisablehook, METH_O, NULL },
    { "get_int_max_str_digits", sys_get_int_max_str_digits, METH_NOARGS, NULL },
    { "set_int_max_str_digits", (PyCFunction)sys_set_int_max_str_digits, METH_VARARGS | METH_KEYWORDS, NULL },
    { "excepthook", sys_excepthook, METH_VARARGS, excepthook_doc },
    { "displayhook", sys_displayhook, METH_O, displayhook_doc },
    { "_clear_type_cache", sys_clear_type_cache, METH_NOARGS, sys_clear_type_cache__doc__ },
    { "getrefcount", (PyCFunction)sys_getrefcount, METH_O, getrefcount_doc },
    { "getsizeof", (PyCFunction)sys_getsizeof, METH_VARARGS | METH_KEYWORDS, getsizeof_doc },
};

PyDoc_STRVAR(flags__doc__, "sys.flags\n\
\n\
Flags provided through command line arguments or environment vars.");

static struct _typeobject FlagsType;

static PyStructSequence_Field flags_fields[] = {
    { "debug", "-d" },
    { "inspect", "-i" },
    { "interactive", "-i" },
    { "optimize", "-O or -OO" },
    { "dont_write_bytecode", "-B" },
    { "no_user_site", "-s" },
    { "no_site", "-S" },
    { "ignore_environment", "-E" },
    { "verbose", "-v" },
    { "bytes_warning", "-b" },
    { "quiet", "-q" },
    { "hash_randomization", "-R" },
    { "isolated", "-I" },
    { "dev_mode", "-X dev" },
    { "utf8_mode", "-X utf8" },
    { "int_max_str_digits", "-X int_max_str_digits" },
    { 0, 0 },
};

static PyStructSequence_Desc flags_desc = { "sys.flags",  /* name */
                                            flags__doc__, /* doc */
                                            flags_fields, /* fields */
                                            16 };

static PyObject* make_flags(void) {
    int pos = 0;
    PyObject* seq;

    seq = PyStructSequence_New((PyTypeObject*)&FlagsType);
    if (seq == NULL)
        return NULL;

#define SetFlag(flag) PyStructSequence_SET_ITEM(seq, pos++, PyLong_FromLong(flag))

    SetFlag(Py_DebugFlag);
    SetFlag(Py_InspectFlag);
    SetFlag(Py_InteractiveFlag);
    SetFlag(Py_OptimizeFlag);
    SetFlag(Py_DontWriteBytecodeFlag);
    SetFlag(Py_NoUserSiteDirectory);
    SetFlag(Py_NoSiteFlag);
    SetFlag(Py_IgnoreEnvironmentFlag);
    SetFlag(Py_VerboseFlag);
    SetFlag(Py_BytesWarningFlag);
    SetFlag(Py_QuietFlag);
    SetFlag(Py_HashRandomizationFlag);
    SetFlag(Py_IsolatedFlag);
    PyStructSequence_SET_ITEM(seq, pos++, incref(Py_False));
    SetFlag(0);
    SetFlag(_Py_global_config_int_max_str_digits);
#undef SetFlag

    if (PyErr_Occurred()) {
        Py_DECREF(seq);
        return NULL;
    }
    return seq;
}

PyDoc_STRVAR(version_info__doc__, "sys.version_info\n\
        \n\
        Version information as a named tuple.");

static struct _typeobject VersionInfoType;

static PyStructSequence_Field version_info_fields[]
    = { { "major", "Major release number" },
        { "minor", "Minor release number" },
        { "micro", "Patch release number" },
        { "releaselevel", "'alpha', 'beta', 'candidate', or 'release'" },
        { "serial", "Serial release number" },
        { 0, 0 } };

static PyStructSequence_Desc version_info_desc = { "sys.version_info",  /* name */
                                                   version_info__doc__, /* doc */
                                                   version_info_fields, /* fields */
                                                   5 };

static PyObject* make_version_info(void) noexcept {
    PyObject* version_info;
    const char* s;
    int pos = 0;
    version_info = PyStructSequence_New((PyTypeObject*)&VersionInfoType);
    if (version_info == NULL) {
        return NULL;
    }

/*
 * These release level checks are mutually exclusive and cover
 * the field, so don't get too fancy with the pre-processor!
 */
#if PY_RELEASE_LEVEL == PY_RELEASE_LEVEL_ALPHA
    s = "alpha";
#elif PY_RELEASE_LEVEL == PY_RELEASE_LEVEL_BETA
    s = "beta";
#elif PY_RELEASE_LEVEL == PY_RELEASE_LEVEL_GAMMA
    s = "candidate";
#elif PY_RELEASE_LEVEL == PY_RELEASE_LEVEL_FINAL
    s = "final";
#endif

#define SetIntItem(flag) PyStructSequence_SET_ITEM(version_info, pos++, PyLong_FromLong(flag))
#define SetStrItem(flag) PyStructSequence_SET_ITEM(version_info, pos++, PyUnicode_FromString(flag))

    SetIntItem(PY_MAJOR_VERSION);
    SetIntItem(PY_MINOR_VERSION);
    SetIntItem(PY_MICRO_VERSION);
    SetStrItem(s);
    SetIntItem(PY_RELEASE_SERIAL);
#undef SetIntItem
#undef SetStrItem

    if (PyErr_Occurred()) {
        Py_CLEAR(version_info);
        return NULL;
    }
    return version_info;
}

static struct _typeobject FloatInfoType;

PyDoc_STRVAR(floatinfo__doc__, "sys.float_info\n\
\n\
A structseq holding information about the float type. It contains low level\n\
information about the precision and internal representation. Please study\n\
your system's :file:`float.h` for more information.");

static PyStructSequence_Field floatinfo_fields[]
    = { { "max", "DBL_MAX -- maximum representable finite float" },
        { "max_exp", "DBL_MAX_EXP -- maximum int e such that radix**(e-1) "
                     "is representable" },
        { "max_10_exp", "DBL_MAX_10_EXP -- maximum int e such that 10**e "
                        "is representable" },
        { "min", "DBL_MIN -- Minimum positive normalizer float" },
        { "min_exp", "DBL_MIN_EXP -- minimum int e such that radix**(e-1) "
                     "is a normalized float" },
        { "min_10_exp", "DBL_MIN_10_EXP -- minimum int e such that 10**e is "
                        "a normalized" },
        { "dig", "DBL_DIG -- digits" },
        { "mant_dig", "DBL_MANT_DIG -- mantissa digits" },
        { "epsilon", "DBL_EPSILON -- Difference between 1 and the next "
                     "representable float" },
        { "radix", "FLT_RADIX -- radix of exponent" },
        { "rounds", "FLT_ROUNDS -- addition rounds" },
        { NULL, NULL } };

static PyStructSequence_Desc float_info_desc = { "sys.float_info", /* name */
                                                 floatinfo__doc__, /* doc */
                                                 floatinfo_fields, /* fields */
                                                 11 };

PyObject* PyFloat_GetInfo(void) {
    PyObject* floatinfo;
    int pos = 0;

    floatinfo = PyStructSequence_New((PyTypeObject*)&FloatInfoType);
    if (floatinfo == NULL) {
        return NULL;
    }

#define SetIntFlag(flag) PyStructSequence_SET_ITEM(floatinfo, pos++, PyLong_FromLong(flag))
#define SetDblFlag(flag) PyStructSequence_SET_ITEM(floatinfo, pos++, PyFloat_FromDouble(flag))

    SetDblFlag(DBL_MAX);
    SetIntFlag(DBL_MAX_EXP);
    SetIntFlag(DBL_MAX_10_EXP);
    SetDblFlag(DBL_MIN);
    SetIntFlag(DBL_MIN_EXP);
    SetIntFlag(DBL_MIN_10_EXP);
    SetIntFlag(DBL_DIG);
    SetIntFlag(DBL_MANT_DIG);
    SetDblFlag(DBL_EPSILON);
    SetIntFlag(FLT_RADIX);
    SetIntFlag(FLT_ROUNDS);
#undef SetIntFlag
#undef SetDblFlag

    if (PyErr_Occurred()) {
        Py_CLEAR(floatinfo);
        return NULL;
    }
    return floatinfo;
}

static struct _typeobject HashInfoType;

static PyStructSequence_Field hash_info_fields[] = { { "width", "width of the type used for hashing, in bits" },
                                                     { "modulus", "prime number giving the modulus on which the hash "
                                                                  "function is based" },
                                                     { "inf", "value to be used for hash of a positive infinity" },
                                                     { "nan", "value to be used for hash of a nan" },
                                                     { "imag", "multiplier used for the imaginary part of a complex "
                                                               "number" },
                                                     { "algorithm", "name of the algorithm for hashing of str, bytes "
                                                                    "and memoryviews" },
                                                     { "hash_bits", "internal output size of hash algorithm" },
                                                     { "seed_bits", "seed size of hash algorithm" },
                                                     { "cutoff", "small string optimization cutoff" },
                                                     { NULL, NULL } };

static PyStructSequence_Desc hash_info_desc = { "sys.hash_info", NULL, hash_info_fields, 9 };

static PyObject* make_hash_info(void) noexcept {
    PyObject* hash_info = PyStructSequence_New((PyTypeObject*)&HashInfoType);
    if (hash_info == NULL)
        return NULL;
    PyHash_FuncDef* hashfunc = PyHash_GetFuncDef();
    int field = 0;
    PyStructSequence_SET_ITEM(hash_info, field++, PyLong_FromLong(8 * sizeof(Py_hash_t)));
    PyStructSequence_SET_ITEM(hash_info, field++, PyLong_FromSsize_t(_PyHASH_MODULUS));
    PyStructSequence_SET_ITEM(hash_info, field++, PyLong_FromLong(_PyHASH_INF));
    PyStructSequence_SET_ITEM(hash_info, field++, PyLong_FromLong(_PyHASH_NAN));
    PyStructSequence_SET_ITEM(hash_info, field++, PyLong_FromLong(_PyHASH_IMAG));
    PyStructSequence_SET_ITEM(hash_info, field++, PyUnicode_FromString(hashfunc->name));
    PyStructSequence_SET_ITEM(hash_info, field++, PyLong_FromLong(hashfunc->hash_bits));
    PyStructSequence_SET_ITEM(hash_info, field++, PyLong_FromLong(hashfunc->seed_bits));
    PyStructSequence_SET_ITEM(hash_info, field++, PyLong_FromLong(Py_HASH_CUTOFF));
    if (PyErr_Occurred()) {
        Py_CLEAR(hash_info);
        return NULL;
    }
    return hash_info;
}

static struct _typeobject IntInfoType;

static PyStructSequence_Field int_info_fields[] = { { "bits_per_digit", "size of a digit in bits" },
                                                    { "sizeof_digit", "size in bytes of the C type used to represent a "
                                                                      "digit" },
                                                    { "default_max_str_digits", "maximum string conversion digits limitation" },
                                                    { "str_digits_check_threshold", "minimum positive value for int_max_str_digits" },
                                                    { NULL, NULL } };

static PyStructSequence_Desc int_info_desc = { "sys.int_info", NULL, int_info_fields, 4 };

// Pyston stores big ints with GMP rather than in 30-bit digits; report CPython's values.
static PyObject* make_int_info(void) noexcept {
    PyObject* int_info = PyStructSequence_New((PyTypeObject*)&IntInfoType);
    if (int_info == NULL)
        return NULL;
    PyStructSequence_SET_ITEM(int_info, 0, PyLong_FromLong(30));
    PyStructSequence_SET_ITEM(int_info, 1, PyLong_FromLong(4));
    PyStructSequence_SET_ITEM(int_info, 2, PyLong_FromLong(4300));
    PyStructSequence_SET_ITEM(int_info, 3, PyLong_FromLong(640));
    if (PyErr_Occurred()) {
        Py_CLEAR(int_info);
        return NULL;
    }
    return int_info;
}

static PyObject* make_impl_info(void) noexcept {
    PyObject* ns = _PyNamespace_New(NULL);
    if (!ns)
        return NULL;
    PyObject* version = make_version_info();
    int failed = !version || PyObject_SetAttrString(ns, "name", autoDecref(PyUnicode_FromString("pyston")))
                 || PyObject_SetAttrString(ns, "cache_tag", Py_None)
                 || PyObject_SetAttrString(ns, "version", version)
                 || PyObject_SetAttrString(ns, "hexversion", autoDecref(PyLong_FromLong(PY_VERSION_HEX)))
                 || PyObject_SetAttrString(ns, "_multiarch", autoDecref(PyUnicode_FromString("x86_64-linux-gnu")));
    Py_XDECREF(version);
    if (failed) {
        Py_DECREF(ns);
        return NULL;
    }
    return ns;
}

PyDoc_STRVAR(exc_info_doc, "exc_info() -> (type, value, traceback)\n\
\n\
Return information about the most recent exception caught by an except\n\
clause in the current stack frame or in an older stack frame.");

PyDoc_STRVAR(exit_doc, "exit([status])\n\
\n\
Exit the interpreter by raising SystemExit(status).\n\
If the status is omitted or None, it defaults to zero (i.e., success).\n\
If the status is an integer, it will be used as the system exit status.\n\
If it is another kind of object, it will be printed and the system\n\
exit status will be one (i.e., failure).");

PyDoc_STRVAR(getdefaultencoding_doc, "getdefaultencoding() -> string\n\
\n\
Return the current default string encoding used by the Unicode \n\
implementation.");

PyDoc_STRVAR(getfilesystemencoding_doc, "getfilesystemencoding() -> string\n\
\n\
Return the encoding used to convert Unicode filenames in\n\
operating system filenames.");

PyDoc_STRVAR(getrecursionlimit_doc, "getrecursionlimit()\n\
\n\
Return the current value of the recursion limit, the maximum depth\n\
of the Python interpreter stack.  This limit prevents infinite\n\
recursion from causing an overflow of the C stack and crashing Python.");

static int _check_and_flush(FILE* stream) {
    int prev_fail = ferror(stream);
    return fflush(stream) || prev_fail ? EOF : 0;
}

void setupSys() {
    sys_modules_dict = new BoxedDict();
    constants.push_back(sys_modules_dict);

    // This is ok to call here because we've already created the sys_modules_dict
    sys_module = createModule(autoDecref(boxString("sys")));
    _PystonInterp_SetModules(sys_modules_dict, sys_module->getAttrWrapper());

    // sys_module is what holds on to all of the other modules:
    Py_INCREF(sys_module);
    late_constants.push_back(sys_module);

    sys_module->giveAttrBorrowed("modules", sys_modules_dict);

    BoxedList* sys_path = new BoxedList();
    constants.push_back(sys_path);
    sys_module->giveAttrBorrowed("path", sys_path);

    sys_module->giveAttr("argv", new BoxedList());

    sys_exc_info_func = new BoxedBuiltinFunctionOrMethod(
        BoxedCode::create((void*)sysExcInfo, BOXED_TUPLE, 0, "exc_info", exc_info_doc));
    PyGC_RegisterStaticConstant(sys_exc_info_func);
    sys_module->giveAttrBorrowed("exc_info", sys_exc_info_func);
    sys_module->giveAttr(
        "exit", new BoxedBuiltinFunctionOrMethod(
                    BoxedCode::create((void*)sysExit, NONE, 1, false, false, "exit", exit_doc), { Py_None }, NULL));

    sys_module->giveAttr("warnoptions", new BoxedList());
    sys_module->giveAttr("_xoptions", new BoxedDict());
    sys_module->giveAttr("byteorder", boxString(isLittleEndian() ? "little" : "big"));

    sys_module->giveAttr("platform", boxString(Py_GetPlatform()));

    sys_module->giveAttr("executable", PyUnicode_FromWideChar(Py_GetProgramFullPath(), -1));

    sys_module->giveAttr(
        "_getframe",
        new BoxedFunction(BoxedCode::create((void*)sysGetFrame, UNKNOWN, 1, false, false, "_getframe"), { NULL }));
    sys_module->giveAttr("_current_frames",
                         new BoxedFunction(BoxedCode::create((void*)sysCurrentFrames, UNKNOWN, 0, "_current_frames")));
    sys_module->giveAttr("getdefaultencoding",
                         new BoxedBuiltinFunctionOrMethod(BoxedCode::create(
                             (void*)sysGetDefaultEncoding, STR, 0, "getdefaultencoding", getdefaultencoding_doc)));

    sys_module->giveAttr("getfilesystemencoding", new BoxedBuiltinFunctionOrMethod(BoxedCode::create(
                                                      (void*)sysGetFilesystemEncoding, STR, 0, "getfilesystemencoding",
                                                      getfilesystemencoding_doc)));

    sys_module->giveAttr("getrecursionlimit",
                         new BoxedBuiltinFunctionOrMethod(BoxedCode::create(
                             (void*)sysGetRecursionLimit, UNKNOWN, 0, "getrecursionlimit", getrecursionlimit_doc)));

    sys_module->giveAttr("dont_write_bytecode", boxBool(Py_DontWriteBytecodeFlag));

    sys_module->giveAttr("prefix", PyUnicode_FromWideChar(Py_GetPrefix(), -1));
    sys_module->giveAttr("exec_prefix", PyUnicode_FromWideChar(Py_GetExecPrefix(), -1));
    sys_module->giveAttr("base_prefix", PyUnicode_FromWideChar(Py_GetPrefix(), -1));
    sys_module->giveAttr("base_exec_prefix", PyUnicode_FromWideChar(Py_GetExecPrefix(), -1));

    sys_module->giveAttr("copyright",
                         boxString("Copyright 2014-2016 Dropbox.\nAll Rights Reserved.\n\nCopyright (c) 2001-2014 "
                                   "Python Software Foundation.\nAll Rights Reserved.\n\nCopyright (c) 2000 "
                                   "BeOpen.com.\nAll Rights Reserved.\n\nCopyright (c) 1995-2001 Corporation for "
                                   "National Research Initiatives.\nAll Rights Reserved.\n\nCopyright (c) "
                                   "1991-1995 Stichting Mathematisch Centrum, Amsterdam.\nAll Rights Reserved."));

    sys_module->giveAttr("version", boxString(generateVersionString()));
    sys_module->giveAttr("hexversion", boxInt(PY_VERSION_HEX));
    sys_module->giveAttr("maxsize", boxInt(PY_SSIZE_T_MAX));

#define SET_SYS_FROM_STRING(key, value) sys_module->giveAttr((key), (value))
    SET_SYS_FROM_STRING("maxunicode", PyLong_FromLong(0x10FFFF));

/* float repr style: 0.03 (short) vs 0.029999999999999999 (legacy) */
#ifndef PY_NO_SHORT_FLOAT_REPR
    SET_SYS_FROM_STRING("float_repr_style", PyUnicode_FromString("short"));
#else
    SET_SYS_FROM_STRING("float_repr_style", PyUnicode_FromString("legacy"));
#endif


    auto sys_str = getStaticString("sys");
    for (auto& md : sys_methods) {
        sys_module->giveAttr(md.ml_name, new BoxedCApiFunction(&md, NULL, sys_str));
    }

    sys_module->giveAttrBorrowed("__displayhook__", sys_module->getattr(autoDecref(internStringMortal("displayhook"))));
    sys_module->giveAttrBorrowed("__excepthook__", sys_module->getattr(autoDecref(internStringMortal("excepthook"))));
    sys_module->giveAttrBorrowed("__unraisablehook__",
                                 sys_module->getattr(autoDecref(internStringMortal("unraisablehook"))));

    SET_SYS_FROM_STRING("abiflags", PyUnicode_FromString(""));
    SET_SYS_FROM_STRING("api_version", PyLong_FromLong(PYTHON_API_VERSION));
    SET_SYS_FROM_STRING("pycache_prefix", incref(Py_None));
    // Pyston has its own import system, so these are only here for code that inspects them.
    SET_SYS_FROM_STRING("meta_path", PyList_New(0));
    SET_SYS_FROM_STRING("path_hooks", PyList_New(0));
    SET_SYS_FROM_STRING("path_importer_cache", PyDict_New());
}

void setupSysEnd() {
    std::vector<Box*> builtin_module_names;
    for (int i = 0; PyImport_Inittab[i].name != NULL; i++)
        builtin_module_names.push_back(boxString(PyImport_Inittab[i].name));

    std::sort<decltype(builtin_module_names)::iterator, PyLt>(builtin_module_names.begin(), builtin_module_names.end(),
                                                              PyLt());

    sys_module->giveAttr("builtin_module_names",
                         BoxedTuple::create(builtin_module_names.size(), &builtin_module_names[0]));

    for (Box* b : builtin_module_names)
        Py_DECREF(b);

#ifndef NDEBUG
    for (const auto& p : *sys_modules_dict) {
        assert(PyUnicode_Check(p.first));

        bool found = false;
        for (int i = 0; PyImport_Inittab[i].name != NULL; i++) {
            if (((BoxedString*)p.first)->s() == PyImport_Inittab[i].name) {
                found = true;
            }
        }
        if (!found)
            assert(0 && "found a module which is inside sys.modules but not listed inside PyImport_Inittab!");
    }
#endif

    /* version_info */
    if (VersionInfoType.tp_name == 0)
        PyStructSequence_InitType((PyTypeObject*)&VersionInfoType, &version_info_desc);
    /* prevent user from creating new instances */
    VersionInfoType.tp_init = NULL;
    VersionInfoType.tp_new = NULL;

    SET_SYS_FROM_STRING("version_info", make_version_info());

    /* float_info */
    if (FloatInfoType.tp_name == 0)
        PyStructSequence_InitType((PyTypeObject*)&FloatInfoType, &float_info_desc);
    /* flags */
    if (FlagsType.tp_name == 0)
        PyStructSequence_InitType((PyTypeObject*)&FlagsType, &flags_desc);
    SET_SYS_FROM_STRING("flags", make_flags());
    /* prevent user from creating new instances */
    FlagsType.tp_init = NULL;
    FlagsType.tp_new = NULL;
    /* prevent user from creating new instances */
    FloatInfoType.tp_init = NULL;
    FloatInfoType.tp_new = NULL;

    SET_SYS_FROM_STRING("float_info", PyFloat_GetInfo());

    if (HashInfoType.tp_name == 0)
        PyStructSequence_InitType((PyTypeObject*)&HashInfoType, &hash_info_desc);
    SET_SYS_FROM_STRING("hash_info", make_hash_info());
    if (IntInfoType.tp_name == 0)
        PyStructSequence_InitType((PyTypeObject*)&IntInfoType, &int_info_desc);
    SET_SYS_FROM_STRING("int_info", make_int_info());
    SET_SYS_FROM_STRING("thread_info", PyThread_GetInfo());
    SET_SYS_FROM_STRING("implementation", make_impl_info());

    // The type of the argument passed to sys.unraisablehook.
    _PyErr_Init();
}
}
