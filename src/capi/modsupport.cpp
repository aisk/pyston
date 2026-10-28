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

// Hopefully soon we will be able to switch to CPython's modsupport.c, instead of having
// to reimplement it.  For now, it's easier to create simple versions of the functions
// instead of trying to support all of the internals of modsupport.c

#include <dlfcn.h>
#include <stdarg.h>
#include <string.h>
#include "Python.h"
#include "capi/types.h"
#include "core/threading.h"
#include "core/types.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

// Py_BuildValue and friends come from CPython's Python/modsupport.c.  This file has the
// module object API from Objects/moduleobject.c, adapted to BoxedModule.

static Py_ssize_t max_module_number;

extern "C" PyObject* PyModuleDef_Init(struct PyModuleDef* def) noexcept {
    if (PyType_Ready(&PyModuleDef_Type) < 0)
        return NULL;
    if (def->m_base.m_index == 0) {
        max_module_number++;
        Py_REFCNT(def) = 1;
        Py_TYPE(def) = &PyModuleDef_Type;
        def->m_base.m_index = max_module_number;
    }
    return (PyObject*)def;
}

extern "C" PyObject* PyModule_NewObject(PyObject* name) noexcept {
    BoxedModule* module = new BoxedModule();
    module->giveAttrBorrowed("__name__", name);
    module->giveAttrBorrowed("__doc__", Py_None);
    module->giveAttrBorrowed("__package__", Py_None);
    module->giveAttrBorrowed("__loader__", Py_None);
    module->giveAttrBorrowed("__spec__", Py_None);
    return module;
}

extern "C" PyObject* PyModule_New(const char* name) noexcept {
    PyObject* nameobj = PyUnicode_FromString(name);
    if (nameobj == NULL)
        return NULL;
    PyObject* module = PyModule_NewObject(nameobj);
    Py_DECREF(nameobj);
    return module;
}

static int check_api_version(const char* name, int module_api_version) noexcept {
    if (module_api_version != PYTHON_API_VERSION && module_api_version != PYTHON_ABI_VERSION) {
        int err;
        err = PyErr_WarnFormat(PyExc_RuntimeWarning, 1, "Python C API version mismatch for module %.100s: "
                                                        "This Python has API version %d, module %.100s has version %d.",
                               name, PYTHON_API_VERSION, name, module_api_version);
        if (err)
            return 0;
    }
    return 1;
}

static int add_methods_to_object(PyObject* module, PyObject* name, PyMethodDef* functions) noexcept {
    for (PyMethodDef* fdef = functions; fdef->ml_name != NULL; fdef++) {
        if ((fdef->ml_flags & METH_CLASS) || (fdef->ml_flags & METH_STATIC)) {
            PyErr_SetString(PyExc_ValueError, "module functions cannot set"
                                              " METH_CLASS or METH_STATIC");
            return -1;
        }
        PyObject* func = PyCFunction_NewEx(fdef, (PyObject*)module, name);
        if (func == NULL)
            return -1;
        if (PyObject_SetAttrString(module, fdef->ml_name, func) != 0) {
            Py_DECREF(func);
            return -1;
        }
        Py_DECREF(func);
    }
    return 0;
}

extern "C" PyObject* _PyModule_CreateInitialized(struct PyModuleDef* def, int module_api_version) noexcept {
    const char* name;

    if (!PyModuleDef_Init(def))
        return NULL;
    name = def->m_name;
    if (!check_api_version(name, module_api_version))
        return NULL;
    if (def->m_slots) {
        PyErr_Format(PyExc_SystemError, "module %s: PyModule_Create is incompatible with m_slots", name);
        return NULL;
    }
    // Comment from CPython:
    /* Make sure name is fully qualified.

       This is a bit of a hack: when the shared library is loaded,
       the module name is "package.module", but the module calls
       PyModule_Create*() with just "module" for the name.  The shared
       library loader squirrels away the true name of the module in
       _Py_PackageContext, and PyModule_Create*() will substitute this
       (if the name actually matches).
    */
    if (_Py_PackageContext != NULL) {
        const char* p = strrchr(_Py_PackageContext, '.');
        if (p != NULL && strcmp(def->m_name, p + 1) == 0) {
            name = _Py_PackageContext;
            _Py_PackageContext = NULL;
        }
    }

    BoxedModule* m = (BoxedModule*)PyModule_New(name);
    if (m == NULL)
        return NULL;

    if (def->m_size > 0) {
        m->md_state = PyMem_MALLOC(def->m_size);
        if (!m->md_state) {
            PyErr_NoMemory();
            Py_DECREF(m);
            return NULL;
        }
        memset(m->md_state, 0, def->m_size);
    }

    if (def->m_methods != NULL) {
        if (PyModule_AddFunctions(m, def->m_methods) != 0) {
            Py_DECREF(m);
            return NULL;
        }
    }
    if (def->m_doc != NULL) {
        if (PyModule_SetDocString(m, def->m_doc) != 0) {
            Py_DECREF(m);
            return NULL;
        }
    }
    m->md_def = def;
    return m;
}

extern "C" PyObject* PyModule_Create2(struct PyModuleDef* def, int module_api_version) noexcept {
    return _PyModule_CreateInitialized(def, module_api_version);
}

extern "C" PyObject* PyModule_FromDefAndSpec2(struct PyModuleDef* def, PyObject* spec,
                                             int module_api_version) noexcept {
    PyModuleDef_Slot* cur_slot;
    PyObject* (*create)(PyObject*, PyModuleDef*) = NULL;
    PyObject* nameobj;
    PyObject* m = NULL;
    int has_execution_slots = 0;
    const char* name;

    PyModuleDef_Init(def);

    nameobj = PyObject_GetAttrString(spec, "name");
    if (nameobj == NULL)
        return NULL;
    name = PyUnicode_AsUTF8(nameobj);
    if (name == NULL)
        goto error;

    if (!check_api_version(name, module_api_version))
        goto error;

    if (def->m_size < 0) {
        PyErr_Format(PyExc_SystemError, "module %s: m_size may not be negative for multi-phase initialization", name);
        goto error;
    }

    for (cur_slot = def->m_slots; cur_slot && cur_slot->slot; cur_slot++) {
        if (cur_slot->slot == Py_mod_create) {
            if (create) {
                PyErr_Format(PyExc_SystemError, "module %s has multiple create slots", name);
                goto error;
            }
            create = (PyObject * (*)(PyObject*, PyModuleDef*))cur_slot->value;
        } else if (cur_slot->slot < 0 || cur_slot->slot > _Py_mod_LAST_SLOT) {
            PyErr_Format(PyExc_SystemError, "module %s uses unknown slot ID %i", name, cur_slot->slot);
            goto error;
        } else {
            has_execution_slots = 1;
        }
    }

    if (create) {
        m = create(spec, def);
        if (m == NULL) {
            if (!PyErr_Occurred())
                PyErr_Format(PyExc_SystemError, "creation of module %s failed without setting an exception", name);
            goto error;
        } else if (PyErr_Occurred()) {
            PyErr_Format(PyExc_SystemError, "creation of module %s raised unreported exception", name);
            goto error;
        }
    } else {
        m = PyModule_NewObject(nameobj);
        if (m == NULL)
            goto error;
    }

    if (PyModule_Check(m)) {
        ((BoxedModule*)m)->md_state = NULL;
        ((BoxedModule*)m)->md_def = def;
    } else {
        if (def->m_size > 0 || def->m_traverse || def->m_clear || def->m_free) {
            PyErr_Format(PyExc_SystemError, "module %s is not a module object, but requests module state", name);
            goto error;
        }
        if (has_execution_slots) {
            PyErr_Format(PyExc_SystemError, "module %s specifies execution slots, but did not create "
                                            "a ModuleType instance",
                         name);
            goto error;
        }
    }

    if (def->m_methods != NULL) {
        if (add_methods_to_object(m, nameobj, def->m_methods) != 0)
            goto error;
    }

    if (def->m_doc != NULL) {
        if (PyModule_SetDocString(m, def->m_doc) != 0)
            goto error;
    }

    Py_DECREF(nameobj);
    return m;

error:
    Py_DECREF(nameobj);
    Py_XDECREF(m);
    return NULL;
}

extern "C" int PyModule_ExecDef(PyObject* module, PyModuleDef* def) noexcept {
    const char* name = PyModule_GetName(module);
    if (name == NULL)
        return -1;

    if (def->m_size >= 0) {
        BoxedModule* md = (BoxedModule*)module;
        if (md->md_state == NULL) {
            /* Always set a state pointer; this serves as a marker to skip
             * multiple initialization (importlib.reload() is no-op) */
            md->md_state = PyMem_MALLOC(def->m_size);
            if (!md->md_state) {
                PyErr_NoMemory();
                return -1;
            }
            memset(md->md_state, 0, def->m_size);
        }
    }

    if (def->m_slots == NULL)
        return 0;

    for (PyModuleDef_Slot* cur_slot = def->m_slots; cur_slot && cur_slot->slot; cur_slot++) {
        switch (cur_slot->slot) {
            case Py_mod_create:
                /* handled in PyModule_FromDefAndSpec2 */
                break;
            case Py_mod_exec: {
                int ret = ((int (*)(PyObject*))cur_slot->value)(module);
                if (ret != 0) {
                    if (!PyErr_Occurred())
                        PyErr_Format(PyExc_SystemError,
                                     "execution of module %s failed without setting an exception", name);
                    return -1;
                }
                if (PyErr_Occurred()) {
                    PyErr_Format(PyExc_SystemError, "execution of module %s raised unreported exception", name);
                    return -1;
                }
                break;
            }
            default:
                PyErr_Format(PyExc_SystemError, "module %s initialized with unknown slot %i", name, cur_slot->slot);
                return -1;
        }
    }
    return 0;
}

extern "C" int PyModule_AddFunctions(PyObject* m, PyMethodDef* functions) noexcept {
    PyObject* name = PyModule_GetNameObject(m);
    if (name == NULL)
        return -1;
    int res = add_methods_to_object(m, name, functions);
    Py_DECREF(name);
    return res;
}

extern "C" int PyModule_SetDocString(PyObject* m, const char* doc) noexcept {
    PyObject* v = PyUnicode_FromString(doc);
    if (v == NULL || PyObject_SetAttrString(m, "__doc__", v) != 0) {
        Py_XDECREF(v);
        return -1;
    }
    Py_DECREF(v);
    return 0;
}

extern "C" BORROWED(PyObject*) PyModule_GetDict(PyObject* _m) noexcept {
    if (!PyModule_Check(_m)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return static_cast<BoxedModule*>(_m)->getAttrWrapper();
}

extern "C" PyObject* PyModule_GetNameObject(PyObject* m) noexcept {
    if (!PyModule_Check(m)) {
        PyErr_BadArgument();
        return NULL;
    }
    Box* name = m->getattr(getStaticString("__name__"));
    if (name == NULL || !PyUnicode_Check(name)) {
        PyErr_SetString(PyExc_SystemError, "nameless module");
        return NULL;
    }
    return incref(name);
}

extern "C" const char* PyModule_GetName(PyObject* m) noexcept {
    PyObject* name = PyModule_GetNameObject(m);
    if (name == NULL)
        return NULL;
    Py_DECREF(name); /* module dict has still a reference */
    return PyUnicode_AsUTF8(name);
}

extern "C" PyObject* PyModule_GetFilenameObject(PyObject* m) noexcept {
    if (!PyModule_Check(m)) {
        PyErr_BadArgument();
        return NULL;
    }
    Box* fileobj = m->getattr(getStaticString("__file__"));
    if (fileobj == NULL || !PyUnicode_Check(fileobj)) {
        PyErr_SetString(PyExc_SystemError, "module filename missing");
        return NULL;
    }
    return incref(fileobj);
}

extern "C" const char* PyModule_GetFilename(PyObject* m) noexcept {
    PyObject* fileobj = PyModule_GetFilenameObject(m);
    if (fileobj == NULL)
        return NULL;
    const char* utf8 = PyUnicode_AsUTF8(fileobj);
    Py_DECREF(fileobj); /* module dict has still a reference */
    return utf8;
}

extern "C" PyModuleDef* PyModule_GetDef(PyObject* m) noexcept {
    if (!PyModule_Check(m)) {
        PyErr_BadArgument();
        return NULL;
    }
    return static_cast<BoxedModule*>(m)->md_def;
}

extern "C" void* PyModule_GetState(PyObject* m) noexcept {
    if (!PyModule_Check(m)) {
        PyErr_BadArgument();
        return NULL;
    }
    return static_cast<BoxedModule*>(m)->md_state;
}

} // namespace pyston

using namespace pyston;

extern "C" PyObject* PyEval_CallMethod(PyObject* obj, const char* methodname, const char* format, ...) noexcept {
    va_list vargs;
    PyObject* meth;
    PyObject* args;
    PyObject* res;

    meth = PyObject_GetAttrString(obj, methodname);
    if (meth == NULL)
        return NULL;

    va_start(vargs, format);

    args = Py_VaBuildValue(format, vargs);
    va_end(vargs);

    if (args == NULL) {
        Py_DECREF(meth);
        return NULL;
    }

    res = PyEval_CallObject(meth, args);
    Py_DECREF(meth);
    Py_DECREF(args);

    return res;
}

extern "C" PyObject* PyEval_CallObjectWithKeywords(PyObject* func, PyObject* arg, PyObject* kw) noexcept {
    PyObject* result;

    if (arg == NULL) {
        arg = PyTuple_New(0);
        if (arg == NULL)
            return NULL;
    } else if (!PyTuple_Check(arg)) {
        PyErr_SetString(PyExc_TypeError, "argument list must be a tuple");
        return NULL;
    } else
        Py_INCREF(arg);

    if (kw != NULL && !PyDict_Check(kw)) {
        PyErr_SetString(PyExc_TypeError, "keyword list must be a dictionary");
        Py_DECREF(arg);
        return NULL;
    }

    result = PyObject_Call(func, arg, kw);
    Py_DECREF(arg);
    return result;
}



