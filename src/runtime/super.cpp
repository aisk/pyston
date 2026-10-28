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

#include "runtime/super.h"

#include <sstream>

#include "capi/types.h"
#include "codegen/unwinding.h"
#include "core/cfg.h"
#include "core/types.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {


class BoxedSuper : public Box {
public:
    // These definitions+names are taken from CPython; not sure I understand it well enough yet
    BoxedClass* type;     // "the class invoking super()"
    Box* obj;             // "the instance invoking super(); make be None"
    BoxedClass* obj_type; // "the type of the instance invoking super(); may be None"

    BoxedSuper(BoxedClass* type, Box* obj, BoxedClass* obj_type) : type(type), obj(obj), obj_type(obj_type) {
        Py_INCREF(type);
        Py_INCREF(obj);
        Py_INCREF(obj_type);
    }

    DEFAULT_CLASS_SIMPLE(super_cls, true);

    static void dealloc(Box* b) noexcept {
        BoxedSuper* self = (BoxedSuper*)b;

        _PyObject_GC_UNTRACK(self);
        Py_XDECREF(self->obj);
        Py_XDECREF(self->type);
        Py_XDECREF(self->obj_type);

        Py_TYPE(self)->tp_free(self);
    }
    static int traverse(Box* self, visitproc visit, void* arg) noexcept {
        BoxedSuper* su = (BoxedSuper*)self;

        Py_VISIT(su->obj);
        Py_VISIT(su->type);
        Py_VISIT(su->obj_type);

        return 0;
    }
};

static const char* class_str = "__class__";

Box* superGetattribute(Box* _s, Box* _attr) {
    RELEASE_ASSERT(PyObject_TypeCheck(_s, super_cls), "");
    BoxedSuper* s = static_cast<BoxedSuper*>(_s);

    RELEASE_ASSERT(_attr->cls == str_cls, "");
    BoxedString* attr = static_cast<BoxedString*>(_attr);

    bool skip = s->obj_type == NULL;

    if (!skip) {
        // Looks like __class__ is supposed to be "super", not the class of the the proxied object.
        skip = (attr->s() == class_str);
    }

    if (!skip) {
        PyObject* mro, *res, *tmp, *dict;
        PyTypeObject* starttype;
        descrgetfunc f;
        Py_ssize_t i, n;

        starttype = s->obj_type;
        mro = starttype->tp_mro;

        if (mro == NULL)
            n = 0;
        else {
            assert(PyTuple_Check(mro));
            n = PyTuple_GET_SIZE(mro);
        }
        for (i = 0; i < n; i++) {
            if ((PyObject*)(s->type) == PyTuple_GET_ITEM(mro, i))
                break;
        }
        i++;
        res = NULL;
        for (; i < n; i++) {
            tmp = PyTuple_GET_ITEM(mro, i);

// Pyston change:
#if 0
            if (PyType_Check(tmp))
                dict = ((PyTypeObject *)tmp)->tp_dict;
            else if (PyClass_Check(tmp))
                dict = ((PyClassObject *)tmp)->cl_dict;
            else
                continue;
            res = PyDict_GetItem(dict, name);
#endif
            res = tmp->getattr(attr);

            if (res != NULL) {
// Pyston change:
#if 0
                Py_INCREF(res);
                f = Py_TYPE(res)->tp_descr_get;
                if (f != NULL) {
                    tmp = f(res,
                        /* Only pass 'obj' param if
                           this is instance-mode sper
                           (See SF ID #743627)
                        */
                        (s->obj == (PyObject *)
                                    s->obj_type
                            ? (PyObject *)NULL
                            : s->obj),
                        (PyObject *)starttype);
                    Py_DECREF(res);
                    res = tmp;
                }
#endif
                return processDescriptor(res, (s->obj == s->obj_type ? Py_None : s->obj), s->obj_type);
            }
        }
    }

    Box* rtn = PyObject_GenericGetAttr(s, attr);
    if (!rtn)
        throwCAPIException();
    return rtn;
}

Box* super_getattro(Box* _s, Box* _attr) noexcept {
    try {
        return superGetattribute(_s, _attr);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

Box* superRepr(Box* _s) {
    RELEASE_ASSERT(PyObject_TypeCheck(_s, super_cls), "");
    BoxedSuper* s = static_cast<BoxedSuper*>(_s);

    if (s->obj_type) {
        return boxStringTwine(llvm::Twine("<super: <class '") + (s->type ? getNameOfClass(s->type) : "NULL") + "'>, <"
                              + getNameOfClass(s->obj_type) + " object>>");
    } else {
        return boxStringTwine(llvm::Twine("<super: <class '") + (s->type ? getNameOfClass(s->type) : "NULL")
                              + "'>, <NULL>>");
    }
}


// Ported from the CPython version:
// TODO: this returns an owned ref in cpython
template <ExceptionStyle S> BORROWED(BoxedClass*) superCheck(BoxedClass* type, Box* obj) noexcept(S == CAPI) {
    if (PyType_Check(obj) && isSubclass(static_cast<BoxedClass*>(obj), type))
        return static_cast<BoxedClass*>(obj);

    if (isSubclass(obj->cls, type)) {
        return obj->cls;
    }

    static BoxedString* class_str = getStaticString("__class__");
    Box* class_attr = obj->getattr(class_str);
    if (class_attr && PyType_Check(class_attr) && class_attr != obj->cls) {
        Py_FatalError("warning: this path never tested"); // blindly copied from CPython
        return static_cast<BoxedClass*>(class_attr);
    }

    if (S == CXX)
        raiseExcHelper(TypeError, "super(type, obj): obj must be an instance or subtype of type");
    else
        PyErr_SetString(TypeError, "super(type, obj): obj must be an instance or subtype of type");
    return NULL;
}

template <ExceptionStyle S>
static PyObject* superGet(PyObject* _self, PyObject* obj, PyObject* type) noexcept(S == CAPI) {
    BoxedSuper* self = static_cast<BoxedSuper*>(_self);

    if (obj == NULL || obj == Py_None || self->obj != NULL) {
        /* Not binding to an object, or already bound */
        return incref(self);
    }
    if (self->cls != super_cls) {
        /* If self is an instance of a (strict) subclass of super,
           call its type */
        return runtimeCallInternal<S, NOT_REWRITABLE>(self->cls, NULL, ArgPassSpec(2), self->type, obj, NULL, NULL,
                                                      NULL);
    } else {
        /* Inline the common case */
        BoxedClass* obj_type = superCheck<S>(self->type, obj);
        if (obj_type == NULL)
            return NULL;
        return new BoxedSuper(self->type, obj, obj_type);
    }
}

// Does the class attribute `attr` wrap a function whose code is `code`?
static bool attrHasCode(Box* attr, BoxedCode* code) {
    if (!attr)
        return false;
    if (attr->cls == staticmethod_cls)
        attr = static_cast<BoxedStaticmethod*>(attr)->sm_callable;
    else if (attr->cls == classmethod_cls)
        attr = static_cast<BoxedClassmethod*>(attr)->cm_callable;
    else if (attr->cls == property_cls) {
        BoxedProperty* prop = static_cast<BoxedProperty*>(attr);
        return attrHasCode(prop->prop_get, code) || attrHasCode(prop->prop_set, code)
               || attrHasCode(prop->prop_del, code);
    }
    if (!attr || !PyFunction_Check(attr))
        return false;
    return static_cast<BoxedFunctionBase*>(attr)->code == code;
}

static BoxedClass* findDefiningClass(BoxedClass* start, BoxedCode* code) {
    Box* mro = start->tp_mro;
    if (!mro)
        return NULL;
    // Methods are nearly always stored under their own name, so try that before scanning
    // every class dict.
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < PyTuple_GET_SIZE(mro); i++) {
            BoxedClass* cls = static_cast<BoxedClass*>(PyTuple_GET_ITEM(mro, i));
            if (pass == 0) {
                if (code->name && attrHasCode(cls->getattr(code->name), code))
                    return cls;
            } else {
                BoxedDict* d = static_cast<BoxedDict*>(PyObject_GetAttrString(cls, "__dict__"));
                if (!d)
                    throwCAPIException();
                AUTO_DECREF(d);
                Box* values = PyMapping_Values(d);
                if (!values)
                    throwCAPIException();
                AUTO_DECREF(values);
                for (int j = 0; j < PyList_GET_SIZE(values); j++) {
                    if (attrHasCode(PyList_GET_ITEM(values, j), code))
                        return cls;
                }
            }
        }
    }
    return NULL;
}

// Zero-argument super().  CPython finds the class through the __class__ cell that the compiler
// creates for every method that mentions super or __class__.  We don't have that cell yet, so
// instead take the first argument of the calling frame and search its MRO for the class whose
// dict holds the function being run.
// TODO: implement the __class__ cell; this guess fails for functions that are nested inside a
// method or that are attached to a class after it was created.
static void superFromFrame(Box** type_out, Box** obj_out) {
    FrameInfo* frame_info = getPythonFrameInfo(0);
    if (!frame_info || !frame_info->code || !frame_info->code->source)
        raiseExcHelper(RuntimeError, "super(): no current frame");
    BoxedCode* code = frame_info->code;
    const ParamNames& params = code->param_names;
    if (params.numNormalArgs() == 0 || !params.all_args_contains_names)
        raiseExcHelper(RuntimeError, "super(): no arguments");

    BST_Name* first = params.all_args[0].name;
    int vreg = first->vreg;
    if (vreg == VREG_UNDEFINED) {
        // getVReg() only exists in debug builds, so look the name up in the user visible vregs.
        auto names = code->source->cfg->getVRegInfo().getVRegSymUserVisibleMap();
        for (int i = 0; i < names.size(); i++) {
            if (names[i] == first->id) {
                vreg = i;
                break;
            }
        }
    }
    Box* obj = (vreg >= 0 && vreg < frame_info->num_vregs) ? frame_info->vregs[vreg] : NULL;
    if (!obj)
        raiseExcHelper(RuntimeError, "super(): arg[0] deleted");

    BoxedClass* type = NULL;
    if (PyType_Check(obj))
        type = findDefiningClass(static_cast<BoxedClass*>(obj), code);
    if (!type)
        type = findDefiningClass(obj->cls, code);
    if (!type)
        raiseExcHelper(RuntimeError, "super(): __class__ cell not found");

    *type_out = type;
    *obj_out = obj;
}

Box* superInit(Box* _self, Box* _type, Box* obj) {
    RELEASE_ASSERT(PyObject_TypeCheck(_self, super_cls), "");
    BoxedSuper* self = static_cast<BoxedSuper*>(_self);

    if (!_type) {
        if (obj)
            raiseExcHelper(TypeError, "super() argument 1 must be type, not None");
        superFromFrame(&_type, &obj);
    }

    if (!PyType_Check(_type))
        raiseExcHelper(TypeError, "must be type, not %s", getTypeName(_type));
    BoxedClass* type = static_cast<BoxedClass*>(_type);

    BoxedClass* obj_type = NULL;
    if (obj == Py_None)
        obj = NULL;
    if (obj != NULL) {
        obj_type = superCheck<CXX>(type, obj);

        if (!obj_type)
            throwCAPIException();

        incref(obj);
        incref(obj_type);
    }

    Box* old_type = self->type;
    Box* old_obj = self->obj;
    Box* old_obj_type = self->obj_type;
    self->type = incref(type);
    self->obj = obj;
    self->obj_type = obj_type;
    Py_XDECREF(old_type);
    Py_XDECREF(old_obj);
    Py_XDECREF(old_obj_type);

    return incref(Py_None);
}

void setupSuper() {
    BoxedClass::createStatic(super_cls, type_cls, object_cls, 0, 0, sizeof(BoxedSuper), false, "super", true,
                             (destructor)BoxedSuper::dealloc, NULL, true, (traverseproc)BoxedSuper::traverse, NOCLEAR);

    // super_cls->giveAttr("__getattribute__", new BoxedFunction(BoxedCode::create((void*)superGetattribute,
    // UNKNOWN, 2)));
    super_cls->giveAttr("__repr__", new BoxedFunction(BoxedCode::create((void*)superRepr, STR, 1, "super.__repr__")));

    super_cls->giveAttr(
        "__init__",
        new BoxedFunction(BoxedCode::create((void*)superInit, UNKNOWN, 3, false, false, "super.__init__"), { NULL, NULL }));
    super_cls->giveAttr("__get__",
                        new BoxedFunction(BoxedCode::create((void*)superGet<CXX>, UNKNOWN, 3, "super.__get__")));

    super_cls->giveAttrMember("__thisclass__", T_OBJECT, offsetof(BoxedSuper, type));
    super_cls->giveAttrMember("__self__", T_OBJECT, offsetof(BoxedSuper, obj));
    super_cls->giveAttrMember("__self_class__", T_OBJECT, offsetof(BoxedSuper, obj_type));

    super_cls->freeze();
    super_cls->tp_getattro = super_getattro;
    super_cls->tp_descr_get = superGet<CAPI>;
}
}
