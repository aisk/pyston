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

// A small import system written in C++.  CPython 3.8 implements most of its import machinery
// in Python (importlib._bootstrap), and runs it frozen.  Pyston does not use importlib: this file
// handles sys.modules, the builtin modules in PyImport_Inittab, and source files, packages and
// C extensions found on sys.path or a package's __path__.  Import hooks (sys.meta_path,
// sys.path_hooks), namespace packages, .pyc files and module specs are not supported yet.

#include "runtime/import.h"

#include <dlfcn.h>
#include <limits.h>

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include "codegen/irgen/hooks.h"
#include "codegen/parser.h"
#include "codegen/unwinding.h"
#include "core/ast.h"
#include "runtime/inline/list.h"
#include "runtime/objmodel.h"
#include "runtime/util.h"

namespace pyston {

extern "C" {
extern struct _inittab _PyImport_Inittab[];
struct _inittab* PyImport_Inittab = _PyImport_Inittab;
}

#ifdef Py_REF_DEBUG
bool imported_foreign_cextension = false;
#endif

static void removeModule(BoxedString* name) {
    BoxedDict* d = getSysModulesDict();
    if (PyDict_DelItem(d, name) < 0)
        PyErr_Clear();
}

extern "C" BORROWED(PyObject*) PyImport_GetModuleDict(void) noexcept {
    return getSysModulesDict();
}

extern "C" PyObject* PyImport_GetModule(PyObject* name) noexcept {
    Box* m = getSysModulesDict()->getOrNull(name);
    return xincref(m);
}

extern "C" BORROWED(PyObject*) PyImport_AddModuleObject(PyObject* name) noexcept {
    BoxedDict* modules = getSysModulesDict();
    Box* m = modules->getOrNull(name);
    if (m && PyModule_Check(m))
        return m;

    m = PyModule_NewObject(name);
    if (!m)
        return NULL;
    if (PyDict_SetItem(modules, name, m) < 0) {
        Py_DECREF(m);
        return NULL;
    }
    Py_DECREF(m); // sys.modules holds the reference
    return m;
}

extern "C" BORROWED(PyObject*) PyImport_AddModule(const char* name) noexcept {
    Box* s = PyUnicode_FromString(name);
    if (!s)
        return NULL;
    AUTO_DECREF(s);
    return PyImport_AddModuleObject(s);
}

static BoxedString* getModuleName(Box* m) {
    static BoxedString* name_str = getStaticString("__name__");
    Box* name = getattrInternal<CXX>(m, name_str);
    if (!name || !PyUnicode_Check(name)) {
        Py_XDECREF(name);
        raiseExcHelper(ImportError, "module has no valid __name__");
    }
    return static_cast<BoxedString*>(name);
}

static void raiseModuleNotFound(BoxedString* name) {
    Box* msg = PyUnicode_FromFormat("No module named '%U'", name);
    if (!msg)
        throwCAPIException();
    AUTO_DECREF(msg);
    PyErr_SetImportErrorSubclass(PyExc_ModuleNotFoundError, msg, name, NULL);
    throwCAPIException();
}

// Single-phase init modules, indexed by their PyModuleDef's m_index.
static std::vector<Box*> modules_by_index;

extern "C" BORROWED(PyObject*) PyState_FindModule(struct PyModuleDef* module) noexcept {
    Py_ssize_t index = module->m_base.m_index;
    if (module->m_slots)
        return NULL;
    if (index == 0 || index >= (Py_ssize_t)modules_by_index.size())
        return NULL;
    return modules_by_index[index];
}

extern "C" int PyState_AddModule(PyObject* module, struct PyModuleDef* def) noexcept {
    if (!def) {
        Py_FatalError("PyState_AddModule: Module Definition is NULL");
        return -1;
    }
    if (def->m_slots) {
        PyErr_SetString(PyExc_SystemError, "PyState_AddModule called on module with slots");
        return -1;
    }
    if (def->m_base.m_index == 0) {
        // PyModuleDef_Init normally assigns the index; 0 means "not set".
        def->m_base.m_index = std::max((size_t)1, modules_by_index.size());
    }
    if (def->m_base.m_index >= (Py_ssize_t)modules_by_index.size())
        modules_by_index.resize(def->m_base.m_index + 1, NULL);
    Box*& slot = modules_by_index[def->m_base.m_index];
    Py_XDECREF(slot);
    Py_INCREF(module);
    slot = module;
    return 0;
}

extern "C" int PyState_RemoveModule(struct PyModuleDef* def) noexcept {
    Py_ssize_t index = def->m_base.m_index;
    if (index == 0 || index >= (Py_ssize_t)modules_by_index.size()) {
        PyErr_SetString(PyExc_SystemError, "PyState_RemoveModule: Module index out of bounds.");
        return -1;
    }
    Py_CLEAR(modules_by_index[index]);
    return 0;
}

// TODO: CPython clears out the modules at shutdown here.
extern "C" void PyImport_Cleanup(void) noexcept {
}

// Runs a module's PyInit function and returns the new module.  Handles both single-phase
// initialization (PyInit returns the module) and multi-phase initialization (PyInit returns a
// PyModuleDef).
static Box* initExtensionModule(BoxedString* name, PyObject* (*init)(void), const char* path) {
    const char* oldcontext = _Py_PackageContext;
    _Py_PackageContext = name->c_str();
    Box* m = init();
    _Py_PackageContext = oldcontext;

    if (!m) {
        if (!PyErr_Occurred())
            PyErr_Format(PyExc_SystemError, "initialization of %U failed without raising an exception", name);
        throwCAPIException();
    }

    if (PyObject_TypeCheck(m, &PyModuleDef_Type)) {
        PyModuleDef* def = (PyModuleDef*)m;
        // We don't have module specs; PyModule_FromDefAndSpec only looks at spec.name.
        static BoxedString* name_str = getStaticString("name");
        Box* ns = PyObject_CallFunctionObjArgs((PyObject*)&_PyNamespace_Type, NULL);
        if (!ns)
            throwCAPIException();
        AUTO_DECREF(ns);
        if (PyObject_SetAttr(ns, name_str, name) < 0)
            throwCAPIException();
        m = PyModule_FromDefAndSpec(def, ns);
        if (!m)
            throwCAPIException();
        if (PyModule_Check(m) && PyModule_ExecDef(m, def) < 0) {
            Py_DECREF(m);
            throwCAPIException();
        }
    } else if (PyModule_Check(m)) {
        PyModuleDef* def = PyModule_GetDef(m);
        if (def) {
            def->m_base.m_init = init;
            if (PyState_AddModule(m, def) < 0) {
                Py_DECREF(m);
                throwCAPIException();
            }
        }
    }

    if (path && PyModule_Check(m)) {
        static BoxedString* file_str = getStaticString("__file__");
        m->setattr(file_str, autoDecref(boxString(path)), NULL);
    }

    BoxedDict* modules = getSysModulesDict();
    if (PyDict_SetItem(modules, name, m) < 0) {
        Py_DECREF(m);
        throwCAPIException();
    }

    if (Py_VerboseFlag) {
        if (path)
            PySys_FormatStderr("import %U # dynamically loaded from %s\n", name, path);
        else
            PySys_FormatStderr("import %U # builtin\n", name);
    }
    return m;
}

static Box* importBuiltinModule(BoxedString* name) {
    for (struct _inittab* p = PyImport_Inittab; p->name != NULL; p++) {
        if (name->s() != p->name)
            continue;
        if (p->initfunc == NULL) {
            // These (sys, builtins, __main__) are created at startup and live in sys.modules.
            Box* m = getSysModulesDict()->getOrNull(name);
            if (!m)
                raiseExcHelper(ImportError, "Cannot re-init internal module %s", name->c_str());
            return incref(m);
        }
        return initExtensionModule(name, p->initfunc, NULL);
    }
    return NULL;
}

BoxedModule* importCExtension(BoxedString* full_name, const std::string& last_name, const std::string& path) {
    void* handle = dlopen(path.c_str(), RTLD_NOW);
    if (!handle)
        raiseExcHelper(ImportError, "%s", dlerror());

    std::string initname = "PyInit_" + last_name;
    PyObject* (*init)(void) = (PyObject * (*)(void))dlsym(handle, initname.c_str());
    if (!init)
        raiseExcHelper(ImportError, "dynamic module does not define module export function (%s)", initname.c_str());

#ifdef Py_REF_DEBUG
    // if we load a foreign C extension we can't check that _Py_RefTotal == 0
    imported_foreign_cextension = true;
#endif

    Box* m = initExtensionModule(full_name, init, path.c_str());
    RELEASE_ASSERT(PyModule_Check(m), "extension modules that aren't module objects are not supported yet");
    return static_cast<BoxedModule*>(m);
}

static Box* loadSourceModule(BoxedString* name, const std::string& path, const std::string* package_dir) {
    BoxedModule* module = createModule(name, path.c_str());
    try {
        static BoxedString* package_str = getStaticString("__package__");
        static BoxedString* path_str = getStaticString("__path__");
        static BoxedString* spec_str = getStaticString("__spec__");
        static BoxedString* loader_str = getStaticString("__loader__");
        if (package_dir) {
            module->setattr(package_str, name, NULL);
            BoxedList* path_list = new BoxedList();
            AUTO_DECREF(path_list);
            listAppendInternalStolen(path_list, boxString(*package_dir));
            module->setattr(path_str, path_list, NULL);
        } else {
            llvm::StringRef parent = name->s().rsplit('.').first;
            if (parent == name->s())
                parent = "";
            module->setattr(package_str, autoDecref(boxString(parent)), NULL);
        }
        module->setattr(spec_str, Py_None, NULL);
        module->setattr(loader_str, Py_None, NULL);

        std::unique_ptr<ASTAllocator> ast_allocator;
        AST_Module* ast;
        std::tie(ast, ast_allocator) = parse_file(path.c_str(), /* future_flags = */ 0);
        assert(ast);
        compileAndRunModule(ast, module);
    } catch (ExcInfo e) {
        removeModule(name);
        throw e;
    }

    // The module may have replaced itself in sys.modules.
    Box* r = getSysModulesDict()->getOrNull(name);
    if (!r)
        raiseExcHelper(ImportError, "Loaded module %s not found in sys.modules", name->c_str());
    if (Py_VerboseFlag)
        PySys_WriteStderr("import %s # from %s\n", name->c_str(), path.c_str());
    return incref(r);
}

static bool isFile(const llvm::Twine& path) {
    return llvm::sys::fs::is_regular_file(path);
}

static bool isDir(const llvm::Twine& path) {
    return llvm::sys::fs::is_directory(path);
}

// Looks for the module `last_name` in each directory of `search_path`, and loads it if found.
// Returns NULL if it wasn't found.
static Box* findAndLoadModule(BoxedString* full_name, llvm::StringRef last_name, Box* search_path) {
    Box* iter = PyObject_GetIter(search_path);
    if (!iter)
        throwCAPIException();
    AUTO_DECREF(iter);

    while (Box* entry = PyIter_Next(iter)) {
        AUTO_DECREF(entry);
        if (!PyUnicode_Check(entry))
            continue;
        std::string dir = static_cast<BoxedString*>(entry)->s().str();
        if (dir.empty())
            dir = ".";

        llvm::SmallString<128> pkg_dir(dir);
        llvm::sys::path::append(pkg_dir, last_name);
        if (isDir(pkg_dir)) {
            llvm::SmallString<128> init_fn(pkg_dir);
            llvm::sys::path::append(init_fn, "__init__.py");
            if (isFile(init_fn)) {
                std::string pkg_dir_str = pkg_dir.str().str();
                return loadSourceModule(full_name, init_fn.str().str(), &pkg_dir_str);
            }
        }

        llvm::SmallString<128> source_fn(dir);
        llvm::sys::path::append(source_fn, last_name + ".py");
        if (isFile(source_fn))
            return loadSourceModule(full_name, source_fn.str().str(), NULL);

        for (const char* suffix : { ".cpython-38-x86_64-linux-gnu.so", ".abi3.so", ".so" }) {
            llvm::SmallString<128> ext_fn(dir);
            llvm::sys::path::append(ext_fn, last_name + suffix);
            if (isFile(ext_fn))
                return importCExtension(full_name, last_name.str(), ext_fn.str().str());
        }
    }
    if (PyErr_Occurred())
        throwCAPIException();
    return NULL;
}

// Imports a module given its absolute dotted name, importing its parent packages first.
static Box* importFullName(BoxedString* name) {
    BoxedDict* modules = getSysModulesDict();
    Box* m = modules->getOrNull(name);
    if (m) {
        if (m == Py_None)
            raiseModuleNotFound(name);
        return incref(m);
    }

    llvm::StringRef s = name->s();
    size_t dot = s.rfind('.');
    if (dot == llvm::StringRef::npos) {
        Box* r = importBuiltinModule(name);
        if (r)
            return r;
        r = findAndLoadModule(name, s, getSysPath());
        if (!r)
            raiseModuleNotFound(name);
        return r;
    }

    BoxedString* parent_name = boxString(s.substr(0, dot));
    AUTO_DECREF(parent_name);
    Box* parent = importFullName(parent_name);
    AUTO_DECREF(parent);

    // Importing the parent may have imported us as a side effect.
    m = modules->getOrNull(name);
    if (m)
        return incref(m);

    static BoxedString* path_str = getStaticString("__path__");
    Box* parent_path = getattrInternal<CXX>(parent, path_str);
    if (!parent_path) {
        Box* msg = PyUnicode_FromFormat("No module named '%U'; '%U' is not a package", name, parent_name);
        if (!msg)
            throwCAPIException();
        AUTO_DECREF(msg);
        PyErr_SetImportErrorSubclass(PyExc_ModuleNotFoundError, msg, name, NULL);
        throwCAPIException();
    }
    AUTO_DECREF(parent_path);

    llvm::StringRef last_name = s.substr(dot + 1);
    Box* r = findAndLoadModule(name, last_name, parent_path);
    if (!r)
        raiseModuleNotFound(name);

    // Like importlib, bind the submodule as an attribute of its parent.
    parent->setattr(autoDecref(internStringMortal(last_name)), r, NULL);
    return r;
}

static Box* getFromGlobals(Box* globals, const char* name) {
    if (!globals)
        return NULL;
    if (PyModule_Check(globals))
        return xincref(globals->getattr(autoDecref(internStringMortal(name))));
    Box* r = PyMapping_GetItemString(globals, name);
    if (!r)
        PyErr_Clear();
    return r;
}

// Port of importlib._bootstrap._calc___package__
static BoxedString* calcPackage(Box* globals) {
    Box* package = getFromGlobals(globals, "__package__");
    if (package && package != Py_None) {
        if (!PyUnicode_Check(package)) {
            Py_DECREF(package);
            raiseExcHelper(TypeError, "package must be a string");
        }
        return static_cast<BoxedString*>(package);
    }
    Py_XDECREF(package);

    Box* name = getFromGlobals(globals, "__name__");
    if (!name)
        raiseExcHelper(KeyError, "'__name__' not in globals");
    AUTO_DECREF(name);
    if (!PyUnicode_Check(name))
        raiseExcHelper(TypeError, "__name__ must be a string");

    Box* path = getFromGlobals(globals, "__path__");
    if (path) {
        Py_DECREF(path);
        return static_cast<BoxedString*>(incref(name));
    }
    llvm::StringRef s = static_cast<BoxedString*>(name)->s();
    size_t dot = s.rfind('.');
    return boxString(dot == llvm::StringRef::npos ? "" : s.substr(0, dot));
}

// Port of importlib._bootstrap._resolve_name
static BoxedString* resolveName(BoxedString* name, Box* globals, int level) {
    BoxedString* package = calcPackage(globals);
    AUTO_DECREF(package);
    if (package->size() == 0)
        raiseExcHelper(ImportError, "attempted relative import with no known parent package");

    llvm::StringRef base = package->s();
    for (int i = 1; i < level; i++) {
        size_t dot = base.rfind('.');
        if (dot == llvm::StringRef::npos)
            raiseExcHelper(ImportError, "attempted relative import beyond top-level package");
        base = base.substr(0, dot);
    }
    if (name->size() == 0)
        return boxString(base);
    return boxStringTwine(base + "." + name->s());
}

// Port of importlib._bootstrap._handle_fromlist
static void handleFromlist(Box* module, Box* fromlist, bool recursive) {
    static BoxedString* path_str = getStaticString("__path__");
    static BoxedString* all_str = getStaticString("__all__");
    Box* path = getattrInternal<CXX>(module, path_str);
    if (!path)
        return;
    Py_DECREF(path);

    Box* iter = PyObject_GetIter(fromlist);
    if (!iter)
        throwCAPIException();
    AUTO_DECREF(iter);
    while (Box* x = PyIter_Next(iter)) {
        AUTO_DECREF(x);
        if (!PyUnicode_Check(x))
            raiseExcHelper(TypeError, "Item in %s must be str, not %s", recursive ? "``from list''" : "__all__",
                           getTypeName(x));
        BoxedString* xs = static_cast<BoxedString*>(x);
        if (xs->s() == "*") {
            if (!recursive) {
                Box* all = getattrInternal<CXX>(module, all_str);
                if (all) {
                    AUTO_DECREF(all);
                    handleFromlist(module, all, true);
                }
            }
            continue;
        }

        Box* existing = getattrInternal<CXX>(module, autoDecref(internStringMortal(xs->s())));
        if (existing) {
            Py_DECREF(existing);
            continue;
        }

        BoxedString* module_name = getModuleName(module);
        AUTO_DECREF(module_name);
        BoxedString* from_name = boxStringTwine(module_name->s() + "." + xs->s());
        AUTO_DECREF(from_name);
        try {
            autoDecref(importFullName(from_name));
        } catch (ExcInfo e) {
            // Backwards-compatibility dictates we ignore failed imports triggered by fromlist for
            // modules that don't exist.
            if (e.matches((BoxedClass*)PyExc_ModuleNotFoundError)) {
                Box* missing = PyObject_GetAttrString(e.value, "name");
                bool ignore = missing && PyUnicode_Check(missing) && PyUnicode_Compare(missing, from_name) == 0;
                Py_XDECREF(missing);
                if (!missing)
                    PyErr_Clear();
                if (ignore) {
                    e.clear();
                    continue;
                }
            }
            throw e;
        }
    }
    if (PyErr_Occurred())
        throwCAPIException();
}

static Box* importModuleLevel(BoxedString* name, Box* globals, Box* fromlist, int level) {
    if (level < 0)
        raiseExcHelper(ValueError, "level must be >= 0");

    BoxedString* abs_name;
    if (level > 0) {
        abs_name = resolveName(name, globals, level);
    } else {
        if (name->size() == 0)
            raiseExcHelper(ValueError, "Empty module name");
        abs_name = static_cast<BoxedString*>(incref(name));
    }
    AUTO_DECREF(abs_name);

    Box* module = importFullName(abs_name);

    bool has_from = fromlist && fromlist != Py_None && PyObject_IsTrue(fromlist);
    if (!has_from) {
        if (level == 0 || name->size() > 0) {
            llvm::StringRef s = name->s();
            size_t dot = s.find('.');
            if (dot == llvm::StringRef::npos)
                return module;
            Py_DECREF(module);
            if (level == 0) {
                BoxedString* front = boxString(s.substr(0, dot));
                AUTO_DECREF(front);
                return importFullName(front);
            }
            // Return the module that corresponds to the first component of `name`.
            size_t cut_off = s.size() - dot;
            BoxedString* to_return = boxString(abs_name->s().substr(0, abs_name->size() - cut_off));
            AUTO_DECREF(to_return);
            Box* r = getSysModulesDict()->getOrNull(to_return);
            if (!r)
                raiseExcHelper(KeyError, "%s not in sys.modules as expected", to_return->c_str());
            return incref(r);
        }
        return module;
    }

    try {
        handleFromlist(module, fromlist, false);
    } catch (ExcInfo e) {
        Py_DECREF(module);
        throw e;
    }
    return module;
}

extern "C" PyObject* PyImport_ImportModuleLevelObject(PyObject* name, PyObject* globals, PyObject* locals,
                                                      PyObject* fromlist, int level) noexcept {
    if (!name) {
        PyErr_SetString(PyExc_ValueError, "Empty module name");
        return NULL;
    }
    if (!PyUnicode_Check(name)) {
        PyErr_SetString(PyExc_TypeError, "module name must be a string");
        return NULL;
    }
    try {
        return importModuleLevel(static_cast<BoxedString*>(name), globals, fromlist, level);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" PyObject* PyImport_ImportModuleLevel(const char* name, PyObject* globals, PyObject* locals,
                                                PyObject* fromlist, int level) noexcept {
    Box* s = PyUnicode_FromString(name);
    if (!s)
        return NULL;
    AUTO_DECREF(s);
    return PyImport_ImportModuleLevelObject(s, globals, locals, fromlist, level);
}

extern "C" PyObject* PyImport_Import(PyObject* module_name) noexcept {
    // CPython calls builtins.__import__ here so that it can be overridden.  We don't support
    // overriding __import__ yet.
    PyObject* fromlist = PyList_New(0);
    if (!fromlist)
        return NULL;
    AUTO_DECREF(fromlist);
    Box* r = PyImport_ImportModuleLevelObject(module_name, NULL, NULL, fromlist, 0);
    if (!r)
        return NULL;
    Py_DECREF(r);
    // Like CPython, return the module from sys.modules.
    r = getSysModulesDict()->getOrNull(module_name);
    if (!r) {
        PyErr_SetObject(PyExc_KeyError, module_name);
        return NULL;
    }
    return incref(r);
}

extern "C" PyObject* PyImport_ImportModule(const char* name) noexcept {
    Box* s = PyUnicode_FromString(name);
    if (!s)
        return NULL;
    AUTO_DECREF(s);
    return PyImport_Import(s);
}

extern "C" PyObject* PyImport_ImportModuleNoBlock(const char* name) noexcept {
    return PyImport_ImportModule(name);
}

extern "C" PyObject* _PyImport_GetModuleId(struct _Py_Identifier* nameid) noexcept {
    PyObject* name = _PyUnicode_FromId(nameid);
    if (name == NULL)
        return NULL;
    return PyImport_GetModule(name);
}

extern "C" int _PyImport_SetModule(PyObject* name, PyObject* m) noexcept {
    return PyDict_SetItem(getSysModulesDict(), name, m);
}

extern "C" int _PyImport_SetModuleString(const char* name, PyObject* m) noexcept {
    return PyDict_SetItemString(getSysModulesDict(), name, m);
}

extern "C" PyObject* PyImport_ExecCodeModuleObject(PyObject* name, PyObject* co, PyObject* pathname,
                                                   PyObject* cpathname) noexcept {
    try {
        BoxedModule* module = (BoxedModule*)PyImport_AddModuleObject(name);
        if (module == NULL)
            return NULL;

        if (pathname) {
            static BoxedString* file_str = getStaticString("__file__");
            module->setattr(file_str, pathname, NULL);
        }
        Box* r = PyEval_EvalCode(co, module->getAttrWrapper(), module->getAttrWrapper());
        if (!r) {
            removeModule((BoxedString*)name);
            return NULL;
        }
        Py_DECREF(r);
        return xincref(getSysModulesDict()->getOrNull(name));
    } catch (ExcInfo e) {
        removeModule((BoxedString*)name);
        setCAPIException(e);
        return NULL;
    }
}

extern "C" PyObject* PyImport_ExecCodeModuleWithPathnames(const char* name, PyObject* co, const char* pathname,
                                                          const char* cpathname) noexcept {
    Box* name_obj = PyUnicode_FromString(name);
    if (!name_obj)
        return NULL;
    AUTO_DECREF(name_obj);
    Box* path_obj = NULL;
    if (pathname) {
        path_obj = PyUnicode_DecodeFSDefault(pathname);
        if (!path_obj)
            return NULL;
    }
    AUTO_XDECREF(path_obj);
    return PyImport_ExecCodeModuleObject(name_obj, co, path_obj, NULL);
}

extern "C" PyObject* PyImport_ExecCodeModuleEx(const char* name, PyObject* co, const char* pathname) noexcept {
    return PyImport_ExecCodeModuleWithPathnames(name, co, pathname, NULL);
}

extern "C" PyObject* PyImport_ExecCodeModule(const char* name, PyObject* co) noexcept {
    return PyImport_ExecCodeModuleWithPathnames(name, co, NULL, NULL);
}

extern "C" PyObject* PyImport_ReloadModule(PyObject* m) noexcept {
    PyErr_SetString(PyExc_NotImplementedError, "Pyston doesn't support reloading modules yet");
    return NULL;
}

extern "C" PyObject* PyImport_GetImporter(PyObject* path) noexcept {
    // sys.path_hooks are not supported.
    Py_RETURN_NONE;
}

extern "C" void _PyImport_AcquireLock(void) noexcept {
}

extern "C" int _PyImport_ReleaseLock(void) noexcept {
    return 1;
}

extern "C" void _PyImport_ReInitLock(void) noexcept {
}

extern "C" long PyImport_GetMagicNumber(void) noexcept {
    return 3413; // Python 3.8b4
}

extern "C" const char* PyImport_GetMagicTag(void) noexcept {
    return "cpython-38";
}

extern "C" Box* import(int level, Box* from_imports, BoxedString* module_name) {
    assert(PyUnicode_Check(module_name));
    Box* rtn = importModuleLevel(module_name, getGlobalsDict(), from_imports, level);
    return rtn;
}
}
