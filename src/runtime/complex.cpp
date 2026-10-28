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

#include "runtime/complex.h"

#include "capi/typeobject.h"
#include "core/types.h"
#include "runtime/float.h"
#include "runtime/inline/boxing.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

extern "C" PyObject* complex_pow(PyObject* v, PyObject* w, PyObject* z) noexcept;
extern "C" PyObject* _PystonComplex_New(PyTypeObject* type, PyObject* r, PyObject* i) noexcept;
extern "C" Py_complex PyComplex_AsCComplex(PyObject* op) noexcept;
extern "C" PyObject* complex_repr(PyComplexObject* v) noexcept;
extern "C" PyObject* complex_richcompare(PyObject* v, PyObject* w, int op) noexcept;
extern "C" PyMethodDef* _PystonComplex_GetMethods(void) noexcept;

namespace pyston {

static Box* toComplex(Box* self) noexcept {
    BoxedComplex* r;
    if (self == NULL) {
        return new BoxedComplex(0.0, 0.0);
    }

    if (PyComplex_Check(self)) {
        r = (BoxedComplex*)incref(self);
    } else if (PyInt_Check(self)) {
        r = new BoxedComplex(static_cast<BoxedInt*>(self)->n, 0.0);
    } else if (PyFloat_Check(self)) {
        r = new BoxedComplex((static_cast<BoxedFloat*>(autoDecref(PyNumber_Float(self))))->d, 0.0);
    } else if (PyLong_Check(self)) {
        double real = PyLong_AsDouble(self);
        if (real == -1 && PyErr_Occurred())
            throwCAPIException();
        r = new BoxedComplex(real, 0.0);
    } else {
        return incref(NotImplemented);
    }
    return r;
}

static inline void raiseDivZeroExc() {
    raiseExcHelper(ZeroDivisionError, "complex division by zero");
}

extern "C" Box* createPureImaginary(double i) {
    return new BoxedComplex(0.0, i);
}

// addition

extern "C" Box* complexAddComplex(BoxedComplex* lhs, BoxedComplex* rhs) {
    assert(PyComplex_Check(lhs));
    assert(PyComplex_Check(rhs));
    return boxComplex(lhs->real + rhs->real, lhs->imag + rhs->imag);
}

extern "C" Box* complexAddFloat(BoxedComplex* lhs, BoxedFloat* rhs) {
    assert(PyComplex_Check(lhs));
    assert(PyFloat_Check(rhs));
    return boxComplex(lhs->real + rhs->d, lhs->imag);
}

extern "C" Box* complexAddInt(BoxedComplex* lhs, BoxedInt* rhs) {
    assert(PyComplex_Check(lhs));
    assert(PyInt_Check(rhs));
    return boxComplex(lhs->real + (double)rhs->n, lhs->imag);
}

extern "C" Box* complexAdd(BoxedComplex* lhs, Box* _rhs) {
    if (!PyComplex_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__add__' requires a 'complex' object but received a '%s'",
                       getTypeName(lhs));

    Box* rhs = toComplex(_rhs);

    if (rhs == NotImplemented)
        return rhs;
    AUTO_DECREF(rhs);

    BoxedComplex* rhs_complex = static_cast<BoxedComplex*>(rhs);
    return complexAddComplex(lhs, rhs_complex);
}

// subtraction

extern "C" Box* complexSubComplex(BoxedComplex* lhs, BoxedComplex* rhs) {
    assert(PyComplex_Check(lhs));
    assert(PyComplex_Check(rhs));
    return boxComplex(lhs->real - rhs->real, lhs->imag - rhs->imag);
}

extern "C" Box* complexSubFloat(BoxedComplex* lhs, BoxedFloat* rhs) {
    assert(PyComplex_Check(lhs));
    assert(PyFloat_Check(rhs));
    return boxComplex(lhs->real - rhs->d, lhs->imag);
}

extern "C" Box* complexSubInt(BoxedComplex* lhs, BoxedInt* rhs) {
    assert(PyComplex_Check(lhs));
    assert(PyInt_Check(rhs));
    return boxComplex(lhs->real - (double)rhs->n, lhs->imag);
}

extern "C" Box* complexSub(BoxedComplex* lhs, Box* _rhs) {
    if (!PyComplex_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__sub__' requires a 'complex' object but received a '%s'",
                       getTypeName(lhs));

    Box* rhs = toComplex(_rhs);

    if (rhs == NotImplemented)
        return rhs;
    AUTO_DECREF(rhs);

    BoxedComplex* rhs_complex = static_cast<BoxedComplex*>(rhs);

    return complexSubComplex(lhs, rhs_complex);
}

extern "C" Box* complexRSub(BoxedComplex* lhs, Box* _rhs) {
    if (!PyComplex_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__rsub__' requires a 'complex' object but received a '%s'",
                       getTypeName(lhs));

    Box* rhs = toComplex(_rhs);

    if (rhs == NotImplemented)
        return rhs;
    AUTO_DECREF(rhs);

    BoxedComplex* rhs_complex = static_cast<BoxedComplex*>(rhs);

    return complexSubComplex(rhs_complex, lhs);
}

// multiplication

extern "C" Box* complexMulComplex(BoxedComplex* lhs, BoxedComplex* rhs) {
    assert(PyComplex_Check(lhs));
    assert(PyComplex_Check(rhs));
    return boxComplex(lhs->real * rhs->real - lhs->imag * rhs->imag, lhs->real * rhs->imag + lhs->imag * rhs->real);
}

extern "C" Box* complexMulFloat(BoxedComplex* lhs, BoxedFloat* rhs) {
    assert(PyComplex_Check(lhs));
    assert(PyFloat_Check(rhs));
    return boxComplex(lhs->real * rhs->d, lhs->imag * rhs->d);
}

extern "C" Box* complexMulInt(BoxedComplex* lhs, BoxedInt* rhs) {
    assert(PyComplex_Check(lhs));
    assert(PyInt_Check(rhs));
    return boxComplex(lhs->real * (double)rhs->n, lhs->imag * (double)rhs->n);
}

extern "C" Box* complexMul(BoxedComplex* lhs, Box* _rhs) {
    if (!PyComplex_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__mul__' requires a 'complex' object but received a '%s'",
                       getTypeName(lhs));

    Box* rhs = toComplex(_rhs);

    if (rhs == NotImplemented)
        return rhs;
    AUTO_DECREF(rhs);

    BoxedComplex* rhs_complex = static_cast<BoxedComplex*>(rhs);

    return complexMulComplex(lhs, rhs_complex);
}

// division
Box* complexDivComplex(BoxedComplex* lhs, BoxedComplex* rhs) {
    if (!PyComplex_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__div__' requires a 'complex' object but received a '%s'",
                       getTypeName(lhs));
    assert(PyComplex_Check(rhs));

    double real_f, imag_f;
    const double abs_breal = rhs->real < 0 ? -rhs->real : rhs->real;
    const double abs_bimag = rhs->imag < 0 ? -rhs->imag : rhs->imag;

    if (abs_breal >= abs_bimag) {
        /* divide tops and bottom by rhs.real */
        if (abs_breal == 0.0) {
            raiseDivZeroExc();
            real_f = imag_f = 0.0;
        } else {
            const double ratio = rhs->imag / rhs->real;
            const double denom = rhs->real + rhs->imag * ratio;
            real_f = (lhs->real + lhs->imag * ratio) / denom;
            imag_f = (lhs->imag - lhs->real * ratio) / denom;
        }
    } else {
        /* divide tops and bottom by rhs->imag */
        const double ratio = rhs->real / rhs->imag;
        const double denom = rhs->real * ratio + rhs->imag;
        real_f = (lhs->real * ratio + lhs->imag) / denom;
        imag_f = (lhs->imag * ratio - lhs->real) / denom;
    }
    return boxComplex(real_f, imag_f);
}

extern "C" Box* complexDivFloat(BoxedComplex* lhs, BoxedFloat* rhs) {
    if (!PyComplex_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__div__' requires a 'complex' object but received a '%s'",
                       getTypeName(lhs));
    assert(PyFloat_Check(rhs));
    if (rhs->d == 0.0) {
        raiseDivZeroExc();
    }
    return boxComplex(lhs->real / rhs->d, lhs->imag / rhs->d);
}

extern "C" Box* complexDivInt(BoxedComplex* lhs, BoxedInt* rhs) {
    if (!PyComplex_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__div__' requires a 'complex' object but received a '%s'",
                       getTypeName(lhs));
    assert(PyInt_Check(rhs));
    if (rhs->n == 0) {
        raiseDivZeroExc();
    }
    return boxComplex(lhs->real / (double)rhs->n, lhs->imag / (double)rhs->n);
}

extern "C" Box* complexDiv(BoxedComplex* lhs, Box* _rhs) {
    if (!PyComplex_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__div__' requires a 'complex' object but received a '%s'",
                       getTypeName(lhs));

    Box* rhs = toComplex(_rhs);

    if (rhs == NotImplemented)
        return rhs;
    AUTO_DECREF(rhs);

    BoxedComplex* rhs_complex = static_cast<BoxedComplex*>(rhs);

    return complexDivComplex(lhs, rhs_complex);
}

extern "C" Box* complexRDiv(BoxedComplex* lhs, Box* _rhs) {
    if (!PyComplex_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__rdiv__' requires a 'complex' object but received a '%s'",
                       getTypeName(lhs));

    Box* rhs = toComplex(_rhs);

    if (rhs == NotImplemented)
        return rhs;
    AUTO_DECREF(rhs);

    BoxedComplex* rhs_complex = static_cast<BoxedComplex*>(rhs);

    return complexDivComplex(rhs_complex, lhs);
}

// Python 3 has no floor division or modulo for complex numbers, but the methods exist and raise.
template <int Kind> static Box* complexNoFloorMod(BoxedComplex* lhs, Box* rhs) {
    if (!PyComplex_Check(rhs) && !PyFloat_Check(rhs) && !PyLong_Check(rhs))
        return incref(NotImplemented);
    if (Kind == 0)
        raiseExcHelper(TypeError, "can't take floor of complex number.");
    if (Kind == 1)
        raiseExcHelper(TypeError, "can't mod complex numbers.");
    raiseExcHelper(TypeError, "can't take floor or mod of complex number.");
}

Box* complexTruediv(BoxedComplex* lhs, Box* rhs) {
    if (!PyComplex_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__truediv__' requires a 'complex' object but received a '%s'",
                       getTypeName(lhs));

    if (PyInt_Check(rhs)) {
        return complexDivInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (PyFloat_Check(rhs)) {
        return complexDivFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (PyComplex_Check(rhs)) {
        return complexDivComplex(lhs, static_cast<BoxedComplex*>(rhs));
    } else if (PyLong_Check(rhs)) {
        double res = PyLong_AsDouble(rhs);
        if (res == -1 && PyErr_Occurred())
            throwCAPIException();
        return complexDivFloat(lhs, (BoxedFloat*)autoDecref(boxFloat(res)));
    } else {
        return incref(NotImplemented);
    }
}

Box* complexRTruediv(BoxedComplex* lhs, Box* _rhs) {
    if (!PyComplex_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__rtruediv__' requires a 'complex' object but received a '%s'",
                       getTypeName(lhs));
    if (_rhs == Py_None)
        return incref(NotImplemented);

    Box* rhs = toComplex(_rhs);

    if (rhs == NotImplemented)
        return rhs;
    AUTO_DECREF(rhs);

    BoxedComplex* rhs_complex = static_cast<BoxedComplex*>(rhs);

    return complexDivComplex(rhs_complex, lhs);
}

Box* complexPos(BoxedComplex* self) {
    if (!PyComplex_Check(self))
        raiseExcHelper(TypeError, "descriptor '__pos__' requires a 'complex' object but received a '%s'",
                       getTypeName(self));
    return PyComplex_FromDoubles(self->real, self->imag);
}

static void _addFunc(const char* name, ConcreteCompilerType* rtn_type, void* complex_func, void* float_func,
                     void* int_func, void* boxed_func) {
    BoxedCode* code = new BoxedCode(2, false, false, name);
    code->addVersion(complex_func, rtn_type, { BOXED_COMPLEX, BOXED_COMPLEX });
    code->addVersion(float_func, rtn_type, { BOXED_COMPLEX, BOXED_FLOAT });
    code->addVersion(int_func, rtn_type, { BOXED_COMPLEX, BOXED_INT });
    code->addVersion(boxed_func, UNKNOWN, { UNKNOWN, UNKNOWN });
    complex_cls->giveAttr(name, new BoxedFunction(code));
}

Box* complexPow(BoxedComplex* lhs, Box* _rhs, Box* mod) {
    if (!PyComplex_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__pow__' requires a 'complex' object but received a '%s'",
                       getTypeName(lhs));

    PyObject* res = complex_pow(lhs, _rhs, mod);
    if (res == NULL)
        throwCAPIException();
    return res;
}

Box* complexRPow(BoxedComplex* lhs, Box* _rhs) {
    if (!PyComplex_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__rpow__' requires a 'complex' object but received a '%s'",
                       getTypeName(lhs));

    Box* rhs = toComplex(_rhs);

    if (rhs == NotImplemented)
        return rhs;
    AUTO_DECREF(rhs);

    BoxedComplex* rhs_complex = static_cast<BoxedComplex*>(rhs);

    return complexPow(rhs_complex, lhs, Py_None);
}

Box* complexHash(BoxedComplex* self) {
    if (!PyComplex_Check(self))
        raiseExcHelper(TypeError, "descriptor '__hash__' requires a 'complex' object but received a '%s'",
                       getTypeName(self));
    long hashreal, hashimag, combined;

    hashreal = _Py_HashDouble(self->real);
    if (hashreal == -1) {
        throwCAPIException();
    }

    hashimag = _Py_HashDouble(self->imag);
    if (hashimag == -1) {
        throwCAPIException();
    }
    /* Note:  if the imaginary part is 0, hashimag is 0 now,
     * so the following returns hashreal unchanged.  This is
     * important because numbers of different types that
     * compare equal must have the same hash value, so that
     * hash(x + 0*j) must equal hash(x).
     */
    combined = hashreal + 1000003 * hashimag;
    if (combined == -1)
        combined = -2;
    return boxInt(combined);
}

Box* complexConjugate(BoxedComplex* self) {
    if (!PyComplex_Check(self))
        raiseExcHelper(TypeError, "descriptor 'conjuagte' requires a 'complex' object but received a '%s'",
                       getTypeName(self));
    return new BoxedComplex(self->real, -self->imag);
}

Box* complexAbs(BoxedComplex* _self) {
    if (!PyComplex_Check(_self))
        raiseExcHelper(TypeError, "descriptor '__abs__' requires a 'complex' object but received a '%s'",
                       getTypeName(_self));
    double result;
    Py_complex self = PyComplex_AsCComplex(_self);
    result = _Py_c_abs(self);

    if (errno == ERANGE) {
        raiseExcHelper(OverflowError, "absolute value too large");
    }

    return PyFloat_FromDouble(result);
}

Box* complexGetnewargs(BoxedComplex* self) {
    if (!PyComplex_Check(self))
        raiseExcHelper(TypeError, "descriptor '__getnewargs__' requires a 'complex' object but received a '%s'",
                       getTypeName(self));
    return BoxedTuple::create({ autoDecref(boxFloat(self->real)), autoDecref(boxFloat(self->imag)) });
}

Box* complexNonzero(BoxedComplex* self) {
    if (!PyComplex_Check(self))
        raiseExcHelper(TypeError, "descriptor '__bool__' requires a 'complex' object but received a '%s'",
                       getTypeName(self));
    bool res = self->real != 0.0 || self->imag != 0.0;
    return boxBool(res);
}


template <ExceptionStyle S> Box* complexStr(Box* self) noexcept(S == CAPI) {
    if (!PyComplex_Check(self))
        return setDescrTypeError<S>(self, "complex", "__str__");

    return callCAPIFromStyle<S>(complex_repr, (PyComplexObject*)self);
}

Box* complexInt(BoxedComplex* self) {
    if (!PyComplex_Check(self))
        raiseExcHelper(TypeError, "descriptor '__int__' requires a 'complex' object but received a '%s'",
                       getTypeName(self));

    raiseExcHelper(TypeError, "can't convert complex to int");
}

Box* complexFloat(BoxedComplex* self) {
    if (!PyComplex_Check(self))
        raiseExcHelper(TypeError, "descriptor '__float__' requires a 'complex' object but received a '%s'",
                       getTypeName(self));

    raiseExcHelper(TypeError, "can't convert complex to float");
}

template <ExceptionStyle S> Box* complexRepr(Box* self) noexcept(S == CAPI) {
    if (!PyComplex_Check(self))
        return setDescrTypeError<S>(self, "complex", "__repr__");

    return callCAPIFromStyle<S>(complex_repr, (PyComplexObject*)self);
}

template <ExceptionStyle S> Box* complexNew(BoxedClass* cls, Box* real, Box* imag) noexcept(S == CAPI) {
    if (real == NULL) {
        real = Py_False;
        imag = NULL;
    }
    Box* res = _PystonComplex_New(cls, real, imag);
    if (S == CXX && res == NULL)
        throwCAPIException();
    return res;
}

extern "C" Box* complexEq(BoxedComplex* lhs, Box* rhs) {
    if (lhs->cls != complex_cls) {
        raiseExcHelper(TypeError, "descriptor '__eq__' requires a 'complex' object but received a '%s'",
                       getTypeName(lhs));
    }
    Box* res = complex_richcompare(lhs, rhs, Py_EQ);

    if (!res) {
        throwCAPIException();
    }
    return res;
}

extern "C" Box* complexNe(BoxedComplex* lhs, Box* rhs) {
    if (lhs->cls != complex_cls) {
        raiseExcHelper(TypeError, "descriptor '__ne__' requires a 'complex' object but received a '%s'",
                       getTypeName(lhs));
    }
    Box* res = complex_richcompare(lhs, rhs, Py_NE);

    if (!res) {
        throwCAPIException();
    }
    return res;
}

extern "C" Box* complexLe(BoxedComplex* lhs, Box* rhs) {
    if (lhs->cls != complex_cls) {
        raiseExcHelper(TypeError, "descriptor '__le__' requires a 'complex' object but received a '%s'",
                       getTypeName(lhs));
    }
    Box* res = complex_richcompare(lhs, rhs, Py_LE);

    if (!res) {
        throwCAPIException();
    }
    return res;
}

extern "C" Box* complexLt(BoxedComplex* lhs, Box* rhs) {
    if (lhs->cls != complex_cls) {
        raiseExcHelper(TypeError, "descriptor '__lt__' requires a 'complex' object but received a '%s'",
                       getTypeName(lhs));
    }
    Box* res = complex_richcompare(lhs, rhs, Py_LT);
    if (!res) {
        throwCAPIException();
    }
    return res;
}

extern "C" Box* complexGe(BoxedComplex* lhs, Box* rhs) {
    if (lhs->cls != complex_cls) {
        raiseExcHelper(TypeError, "descriptor '__ge__' requires a 'complex' object but received a '%s'",
                       getTypeName(lhs));
    }
    Box* res = complex_richcompare(lhs, rhs, Py_GE);

    if (!res) {
        throwCAPIException();
    }
    return res;
}

extern "C" Box* complexGt(BoxedComplex* lhs, Box* rhs) {
    if (lhs->cls != complex_cls) {
        raiseExcHelper(TypeError, "descriptor '__gt__' requires a 'complex' object but received a '%s'",
                       getTypeName(lhs));
    }
    Box* res = complex_richcompare(lhs, rhs, Py_GT);

    if (!res) {
        throwCAPIException();
    }
    return res;
}

Box* complexNeg(Box* _self) {
    assert(_self->cls == complex_cls);
    BoxedComplex* self = (BoxedComplex*)_self;
    return PyComplex_FromDoubles(-self->real, -self->imag);
}

PyObject* complex_neg(PyComplexObject* v) {
    BoxedComplex* self = (BoxedComplex*)v;
    return PyComplex_FromDoubles(-self->real, -self->imag);
}

void setupComplex() {
    static PyNumberMethods complex_as_number;
    complex_cls->tp_as_number = &complex_as_number;

    auto complex_new_func = BoxedCode::create((void*)complexNew<CXX>, UNKNOWN, 3, false, false, "complex.__new__", "",
                                              ParamNames({ "", "real", "imag" }, "", ""), CXX);
    complex_new_func->addVersion((void*)complexNew<CAPI>, UNKNOWN, CAPI);
    complex_cls->giveAttr("__new__", new BoxedFunction(complex_new_func, { NULL, NULL }));

    _addFunc("__add__", BOXED_COMPLEX, (void*)complexAddComplex, (void*)complexAddFloat, (void*)complexAddInt,
             (void*)complexAdd);
    _addFunc("__radd__", BOXED_COMPLEX, (void*)complexAddComplex, (void*)complexAddFloat, (void*)complexAddInt,
             (void*)complexAdd);

    _addFunc("__sub__", BOXED_COMPLEX, (void*)complexSubComplex, (void*)complexSubFloat, (void*)complexSubInt,
             (void*)complexSub);

    _addFunc("__mul__", BOXED_COMPLEX, (void*)complexMulComplex, (void*)complexMulFloat, (void*)complexMulInt,
             (void*)complexMul);
    _addFunc("__rmul__", BOXED_COMPLEX, (void*)complexMulComplex, (void*)complexMulFloat, (void*)complexMulInt,
             (void*)complexMul);


    complex_cls->giveAttr("__rsub__",
                          new BoxedFunction(BoxedCode::create((void*)complexRSub, UNKNOWN, 2, "complex.__rsub__")));
    complex_cls->giveAttr(
        "__pow__", new BoxedFunction(BoxedCode::create((void*)complexPow, UNKNOWN, 3, false, false, "complex.__pow__"),
                                     { Py_None }));
    complex_cls->giveAttr("__rpow__",
                          new BoxedFunction(BoxedCode::create((void*)complexRPow, UNKNOWN, 2, "complex.__rpow__")));

    complex_cls->giveAttr(
        "__truediv__", new BoxedFunction(BoxedCode::create((void*)complexTruediv, UNKNOWN, 2, "complex.__truediv__")));
    complex_cls->giveAttr("__rtruediv__", new BoxedFunction(BoxedCode::create((void*)complexRTruediv, UNKNOWN, 2,
                                                                              "complex.__rtruediv__")));
    const char* no_floor_mod_names[][2] = { { "__floordiv__", "__rfloordiv__" },
                                            { "__mod__", "__rmod__" },
                                            { "__divmod__", "__rdivmod__" } };
    void* no_floor_mod_funcs[]
        = { (void*)complexNoFloorMod<0>, (void*)complexNoFloorMod<1>, (void*)complexNoFloorMod<2> };
    for (int i = 0; i < 3; i++) {
        for (const char* name : no_floor_mod_names[i])
            complex_cls->giveAttr(name, new BoxedFunction(BoxedCode::create(no_floor_mod_funcs[i], UNKNOWN, 2, name)));
    }
    complex_cls->giveAttr("conjugate", new BoxedFunction(BoxedCode::create((void*)complexConjugate, BOXED_COMPLEX, 1,
                                                                           "complex.conjugate")));
    complex_cls->giveAttr("__abs__",
                          new BoxedFunction(BoxedCode::create((void*)complexAbs, BOXED_FLOAT, 1, "complex.__abs__")));
    complex_cls->giveAttr("__bool__", new BoxedFunction(BoxedCode::create((void*)complexNonzero, BOXED_BOOL, 1,
                                                                          "complex.__bool__")));
    complex_cls->giveAttr("__eq__",
                          new BoxedFunction(BoxedCode::create((void*)complexEq, UNKNOWN, 2, "complex.__eq__")));
    complex_cls->giveAttr("__ne__",
                          new BoxedFunction(BoxedCode::create((void*)complexNe, UNKNOWN, 2, "complex.__ne__")));
    complex_cls->giveAttr("__le__",
                          new BoxedFunction(BoxedCode::create((void*)complexLe, UNKNOWN, 2, "complex.__le__")));
    complex_cls->giveAttr("__lt__",
                          new BoxedFunction(BoxedCode::create((void*)complexLt, UNKNOWN, 2, "complex.__lt__")));
    complex_cls->giveAttr("__ge__",
                          new BoxedFunction(BoxedCode::create((void*)complexGe, UNKNOWN, 2, "complex.__ge__")));
    complex_cls->giveAttr("__gt__",
                          new BoxedFunction(BoxedCode::create((void*)complexGt, UNKNOWN, 2, "complex.__gt__")));
    complex_cls->giveAttr("__neg__",
                          new BoxedFunction(BoxedCode::create((void*)complexNeg, BOXED_COMPLEX, 1, "complex.__neg__")));
    complex_cls->giveAttr("__pos__",
                          new BoxedFunction(BoxedCode::create((void*)complexPos, BOXED_COMPLEX, 1, "complex.__pos__")));
    complex_cls->giveAttr("__hash__",
                          new BoxedFunction(BoxedCode::create((void*)complexHash, BOXED_INT, 1, "complex.__hash__")));
    complex_cls->giveAttr("__str__",
                          new BoxedFunction(BoxedCode::create((void*)complexStr<CXX>, STR, 1, "complex.__str__")));
    complex_cls->giveAttr("__int__",
                          new BoxedFunction(BoxedCode::create((void*)complexInt, UNKNOWN, 1, "complex.__int__")));
    complex_cls->giveAttr("__float__",
                          new BoxedFunction(BoxedCode::create((void*)complexFloat, UNKNOWN, 1, "complex.__float__")));
    complex_cls->giveAttr("__repr__",
                          new BoxedFunction(BoxedCode::create((void*)complexRepr<CXX>, STR, 1, "complex.__repr__")));
    complex_cls->giveAttrMember("real", T_DOUBLE, offsetof(BoxedComplex, real), true);
    complex_cls->giveAttrMember("imag", T_DOUBLE, offsetof(BoxedComplex, imag), true);

    complex_cls->giveAttr("__doc__",
                          boxString("complex(real[, imag]) -> complex number\n"
                                    "\n"
                                    "Create a complex number from a real part and an optional imaginary part.\n"
                                    "This is equivalent to (real + imag*1j) where imag defaults to 0."));
    add_methods(complex_cls, _PystonComplex_GetMethods());

    add_operators(complex_cls);

    complex_cls->freeze();
    complex_cls->tp_str = complexStr<CAPI>;
    complex_cls->tp_repr = complexRepr<CAPI>;
    complex_cls->tp_as_number->nb_negative = (unaryfunc)complex_neg;
    complex_cls->tp_richcompare = complex_richcompare;
}
}
