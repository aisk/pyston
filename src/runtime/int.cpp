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

#include "runtime/int.h"

#include <cmath>
#include <sstream>

#include "capi/typeobject.h"
#include "core/common.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/types.h"
#include "runtime/float.h"
#include "runtime/inline/boxing.h"
#include "runtime/long.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

extern "C" PyObject* float_pow(PyObject* v, PyObject* w, PyObject* z) noexcept;

namespace pyston {

extern "C" long PyInt_GetMax() noexcept {
    return LONG_MAX; /* To initialize sys.maxint */
}

/* Integers are quite normal objects, to make object handling uniform.
   (Using odd pointers to represent integers would save much space
   but require extra checks for this special case throughout the code.)
   Since a typical Python program spends much of its time allocating
   and deallocating integers, these operations should be very fast.
   Therefore we use a dedicated allocation scheme with a much lower
   overhead (in space and time) than straight malloc(): a simple
   dedicated free list, filled when necessary with memory from malloc().

   block_list is a singly-linked list of all PyIntBlocks ever allocated,
   linked via their next members.  PyIntBlocks are never returned to the
   system before shutdown (PyInt_Fini).

   free_list is a singly-linked list of available PyIntObjects, linked
   via abuse of their ob_type members.
*/

#define BLOCK_SIZE 1000 /* 1K less typical malloc overhead */
#define BHEAD_SIZE 8    /* Enough for a 64-bit pointer */
#define N_INTOBJECTS ((BLOCK_SIZE - BHEAD_SIZE) / sizeof(PyIntObject))

struct _intblock {
    struct _intblock* next;
    PyIntObject objects[N_INTOBJECTS];
};

typedef struct _intblock PyIntBlock;

static PyIntBlock* block_list = NULL;
PyIntObject* BoxedInt::free_list = NULL;

PyIntObject* BoxedInt::fill_free_list(void) noexcept {
    PyIntObject* p, *q;
    /* Python's object allocator isn't appropriate for large blocks. */
    p = (PyIntObject*)PyMem_MALLOC(sizeof(PyIntBlock));
    if (p == NULL)
        return (PyIntObject*)PyErr_NoMemory();
    ((PyIntBlock*)p)->next = block_list;
    block_list = (PyIntBlock*)p;
    /* Link the int objects together, from rear to front, then return
       the address of the last int object in the block. */
    p = &((PyIntBlock*)p)->objects[0];
    q = p + N_INTOBJECTS;
    while (--q > p)
        Py_TYPE(q) = (PyTypeObject*)(q - 1);
    Py_TYPE(q) = NULL;
    return p + N_INTOBJECTS - 1;
}

void BoxedInt::tp_dealloc(Box* b) noexcept {
#ifdef DISABLE_INT_FREELIST
    b->cls->tp_free(b);
#else
    if (PyInt_CheckExact(b)) {
        PyIntObject* v = (PyIntObject*)(b);
        Py_TYPE(v) = (PyTypeObject*)free_list;
        free_list = v;
    } else {
        b->cls->tp_free(b);
    }
#endif
}

extern "C" unsigned long PyInt_AsUnsignedLongMask(PyObject* op) noexcept {
    if (op && PyInt_Check(op))
        return PyInt_AS_LONG((PyIntObject*)op);
    if (op && PyLong_Check(op))
        return PyLong_AsUnsignedLongMask(op);
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyLong_FromSize_t(size_t ival) noexcept {
    if (ival <= LONG_MAX)
        return boxInt(ival);
    return PyLong_FromUnsignedLong(ival);
}

extern "C" PyObject* PyLong_FromSsize_t(Py_ssize_t ival) noexcept {
    return boxInt(ival);
}

extern "C" PyObject* PyLong_FromLong(long n) noexcept {
    return boxInt(n);
}

/* Convert an integer to a decimal string.  On many platforms, this
   will be significantly faster than the general arbitrary-base
   conversion machinery in _PyInt_Format, thanks to optimization
   opportunities offered by division by a compile-time constant. */
static Box* int_to_decimal_string(BoxedInt* v) noexcept {
    // C code calls PyLong_Type.tp_repr directly, also on our big ints.
    if (PyLongObject_Check(v))
        return long_cls->tp_repr(v);
    char buf[sizeof(long) * CHAR_BIT / 3 + 6], *p, *bufend;
    long n = v->n;
    unsigned long absn;
    p = bufend = buf + sizeof(buf);
    absn = n < 0 ? 0UL - n : n;
    do {
        *--p = '0' + (char)(absn % 10);
        absn /= 10;
    } while (absn);
    if (n < 0)
        *--p = '-';
    return PyUnicode_FromStringAndSize(p, bufend - p);
}

extern "C" PyAPI_FUNC(PyObject*) _PyInt_Format(PyIntObject* v, int base, int newstyle) noexcept {
    BoxedInt* bint = reinterpret_cast<BoxedInt*>(v);
    RELEASE_ASSERT(PyInt_Check(bint), "");

    /* There are no doubt many, many ways to optimize this, using code
       similar to _PyLong_Format */
    long n = bint->n;
    int negative = n < 0;
    int is_zero = n == 0;

    /* For the reasoning behind this size, see
       http://c-faq.com/misc/hexio.html. Then, add a few bytes for
       the possible sign and prefix "0[box]" */
    char buf[sizeof(n) * CHAR_BIT + 6];

    /* Start by pointing to the end of the buffer.  We fill in from
       the back forward. */
    char* p = &buf[sizeof(buf)];

    assert(base >= 2 && base <= 36);

    /* Special case base 10, for speed */
    if (base == 10)
        return int_to_decimal_string(bint);

    do {
        /* I'd use i_divmod, except it doesn't produce the results
           I want when n is negative.  So just duplicate the salient
           part here. */
        long div = n / base;
        long mod = n - div * base;

        /* convert abs(mod) to the right character in [0-9, a-z] */
        char cdigit = (char)(mod < 0 ? -mod : mod);
        cdigit += (cdigit < 10) ? '0' : 'a' - 10;
        *--p = cdigit;

        n = div;
    } while (n);

    if (base == 2) {
        *--p = 'b';
        *--p = '0';
    } else if (base == 8) {
        if (newstyle) {
            *--p = 'o';
            *--p = '0';
        } else if (!is_zero)
            *--p = '0';
    } else if (base == 16) {
        *--p = 'x';
        *--p = '0';
    } else {
        *--p = '#';
        *--p = '0' + base % 10;
        if (base > 10)
            *--p = '0' + base / 10;
    }
    if (negative)
        *--p = '-';

    return PyUnicode_FromStringAndSize(p, &buf[sizeof(buf)] - p);
}

extern "C" int _PyInt_AsInt(PyObject* obj) noexcept {
    long result = PyLong_AsLong(obj);
    if (result == -1 && PyErr_Occurred())
        return -1;
    if (result > INT_MAX || result < INT_MIN) {
        PyErr_SetString(PyExc_OverflowError, "Python int too large to convert to C int");
        return -1;
    }
    return (int)result;
}

#ifdef HAVE_LONG_LONG
extern "C" unsigned PY_LONG_LONG PyInt_AsUnsignedLongLongMask(register PyObject* op) noexcept {
    Py_FatalError("unimplemented");

    unsigned PY_LONG_LONG val = 0;

    return val;
}
#endif

extern "C" PyObject* PyInt_FromString(const char* s, char** pend, int base) noexcept {
    return PyLong_FromString(s, pend, base);
}

#ifdef Py_USING_UNICODE
extern "C" PyObject* PyInt_FromUnicode(Py_UNICODE* s, Py_ssize_t length, int base) noexcept {
    PyObject* result;
    char* buffer = (char*)PyMem_MALLOC(length + 1);

    if (buffer == NULL)
        return PyErr_NoMemory();

    if (PyUnicode_EncodeDecimal(s, length, buffer, NULL)) {
        PyMem_FREE(buffer);
        return NULL;
    }
    result = PyInt_FromString(buffer, NULL, base);
    PyMem_FREE(buffer);
    return result;
}
#endif

BoxedInt* interned_ints[NUM_INTERNED_INTS];

// If we don't have fast overflow-checking builtins, provide some slow variants:
#if !__has_builtin(__builtin_saddl_overflow)

#ifdef __clang__
#error "shouldn't be defining the slow versions of these for clang"
#endif

bool __builtin_saddl_overflow(i64 lhs, i64 rhs, i64* result) {
    __int128 r = (__int128)lhs + (__int128)rhs;
    if (r > (__int128)PYSTON_INT_MAX)
        return true;
    if (r < (__int128)PYSTON_INT_MIN)
        return true;
    *result = (i64)r;
    return false;
}
bool __builtin_ssubl_overflow(i64 lhs, i64 rhs, i64* result) {
    __int128 r = (__int128)lhs - (__int128)rhs;
    if (r > (__int128)PYSTON_INT_MAX)
        return true;
    if (r < (__int128)PYSTON_INT_MIN)
        return true;
    *result = (i64)r;
    return false;
}
bool __builtin_smull_overflow(i64 lhs, i64 rhs, i64* result) {
    __int128 r = (__int128)lhs * (__int128)rhs;
    if (r > (__int128)PYSTON_INT_MAX)
        return true;
    if (r < (__int128)PYSTON_INT_MIN)
        return true;
    *result = (i64)r;
    return false;
}

#endif

extern "C" Box* add_i64_i64(i64 lhs, i64 rhs) {
    i64 result;
    if (!__builtin_saddl_overflow(lhs, rhs, &result))
        return boxInt(result);
    return longAdd(autoDecref(boxLong(lhs)), autoDecref(boxLong(rhs)));
}

extern "C" Box* sub_i64_i64(i64 lhs, i64 rhs) {
    i64 result;
    if (!__builtin_ssubl_overflow(lhs, rhs, &result))
        return boxInt(result);
    return longSub(autoDecref(boxLong(lhs)), autoDecref(boxLong(rhs)));
}

extern "C" Box* div_i64_i64(i64 lhs, i64 rhs) {
    if (rhs == 0) {
        raiseExcHelper(ZeroDivisionError, "integer division or modulo by zero");
    }

// It's possible for division to overflow:
#if PYSTON_INT_MIN < -PYSTON_INT_MAX
    static_assert(PYSTON_INT_MIN == -PYSTON_INT_MAX - 1, "");

    if (lhs == PYSTON_INT_MIN && rhs == -1) {
        return longDiv(autoDecref(boxLong(lhs)), autoDecref(boxLong(rhs)));
    }
#endif

    i64 div_result, mod_result;
    div_result = lhs / rhs;
    /* div_result * rhs can overflow on platforms where lhs/rhs gives floor(lhs/rhs)
     * for lhs and rhs with differing signs. (This is unusual
     * behaviour, and C99 prohibits it, but it's allowed by C89;
     * for an example of overflow, take lhs = LONG_MIN, rhs = 5 or lhs =
     * LONG_MAX, rhs = -5.)  However, lhs - div_result*rhs is always
     * representable as a long, since it lies strictly between
     * -abs(rhs) and abs(rhs).  We add casts to avoid intermediate
     * overflow.
     */
    mod_result = (i64)(lhs - (unsigned long)div_result * rhs);
    /* If the signs of lhs and rhs differ, and the remainder is non-0,
     * C89 doesn't define whether div_result is now the floor or the
     * ceiling of the infinitely precise quotient.  We want the floor,
     * and we have it iff the remainder's sign matches rhs's.
     */
    if (mod_result && ((rhs ^ mod_result) < 0) /* i.e. and signs differ */) {
        mod_result += rhs;
        --div_result;
        assert(mod_result && ((rhs ^ mod_result) >= 0));
    }

    return boxInt(div_result);
}

extern "C" Box* mod_i64_i64(i64 lhs, i64 rhs) {
    if (rhs == 0) {
        raiseExcHelper(ZeroDivisionError, "integer division or modulo by zero");
    }

    // this would overflow:
    if (lhs == PYSTON_INT_MIN && rhs == -1)
        return boxLong(0); // long because pypy and cpython both return a long

    if (lhs < 0 && rhs > 0)
        return boxInt(((lhs + 1) % rhs) + (rhs - 1));
    if (lhs > 0 && rhs < 0)
        return boxInt(((lhs - 1) % rhs) + (rhs + 1));
    return boxInt(lhs % rhs);
}

extern "C" Box* pow_i64_i64(i64 lhs, i64 rhs, Box* mod) {
    if (mod != Py_None) {
        if (!PyInt_Check(mod)) {
            return incref(NotImplemented);
        }
        BoxedInt* mod_int = static_cast<BoxedInt*>(mod);
        if (mod_int->n == 0) {
            raiseExcHelper(ValueError, "pow() 3rd argument cannot be 0");
        }
    }

    i64 orig_rhs = rhs;
    i64 rtn = 1, curpow = lhs;

    if (rhs < 0)
        // already checked, rhs is a integer,
        // and mod will be None in this case.
        return powFloatFloat(lhs, rhs);

    // let longPow do the checks.
    return longPow(autoDecref(boxLong(lhs)), autoDecref(boxLong(rhs)), mod);
}

extern "C" Box* mul_i64_i64(i64 lhs, i64 rhs) {
    i64 result;
    if (!__builtin_smull_overflow(lhs, rhs, &result))
        return boxInt(result);
    return longMul(autoDecref(boxLong(lhs)), autoDecref(boxLong(rhs)));
}

// The GMP ints are instances of a hidden subclass of int, so they can reach int's methods
// through explicit calls like int.__add__(2**70, 1). Hand those to the GMP implementation.
static Box* forwardToLong(const char* name, Box* self, Box* a = NULL, Box* b = NULL) {
    Box* r = PyObject_CallMethodObjArgs(self, autoDecref(internStringMortal(name)), a, b, NULL);
    if (!r)
        throwCAPIException();
    return r;
}

extern "C" Box* intAddInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(PyInt_Check(lhs));
    assert(PyInt_Check(rhs));
    return add_i64_i64(lhs->n, rhs->n);
}

extern "C" Box* intAddFloat(BoxedInt* lhs, BoxedFloat* rhs) {
    assert(PyInt_Check(lhs));
    assert(PyFloat_Check(rhs));
    return boxFloat(lhs->n + rhs->d);
}

extern "C" Box* intAdd(BoxedInt* lhs, Box* rhs) {
    if (unlikely(PyLongObject_Check(lhs)))
        return forwardToLong("__add__", lhs, rhs);
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__add__' requires a 'int' object but received a '%s'", getTypeName(lhs));
    if (unlikely(PyLongObject_Check(rhs)))
        return forwardToLong("__add__", autoDecref(boxLong(lhs->n)), rhs);

    if (PyInt_Check(rhs)) {
        BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
        return add_i64_i64(lhs->n, rhs_int->n);
    } else if (PyFloat_CheckExact(rhs)) {
        BoxedFloat* rhs_float = static_cast<BoxedFloat*>(rhs);
        return boxFloat(lhs->n + rhs_float->d);
    } else {
        return incref(NotImplemented);
    }
}

Box* intRAdd(BoxedInt* lhs, Box* rhs) {
    if (unlikely(PyLongObject_Check(lhs)))
        return forwardToLong("__radd__", lhs, rhs);
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__radd__' requires a 'int' object but received a '%s'",
                       getTypeName(lhs));
    if (unlikely(PyLongObject_Check(rhs)))
        return forwardToLong("__radd__", autoDecref(boxLong(lhs->n)), rhs);

    if (PyInt_Check(rhs)) {
        BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
        return add_i64_i64(lhs->n, rhs_int->n);
    } else {
        return incref(NotImplemented);
    }
}

extern "C" Box* intAndInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(PyInt_Check(lhs));
    assert(PyInt_Check(rhs));
    return boxInt(lhs->n & rhs->n);
}

extern "C" Box* intAnd(BoxedInt* lhs, Box* rhs) {
    if (unlikely(PyLongObject_Check(lhs)))
        return forwardToLong("__and__", lhs, rhs);
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__and__' requires a 'int' object but received a '%s'", getTypeName(lhs));
    if (unlikely(PyLongObject_Check(rhs)))
        return forwardToLong("__and__", autoDecref(boxLong(lhs->n)), rhs);

    if (!PyInt_Check(rhs)) {
        return incref(NotImplemented);
    }
    BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
    return boxInt(lhs->n & rhs_int->n);
}

Box* intRAnd(BoxedInt* lhs, Box* rhs) {
    if (unlikely(PyLongObject_Check(lhs)))
        return forwardToLong("__rand__", lhs, rhs);
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__rand__' requires a 'int' object but received a '%s'",
                       getTypeName(lhs));
    if (unlikely(PyLongObject_Check(rhs)))
        return forwardToLong("__rand__", autoDecref(boxLong(lhs->n)), rhs);

    if (!PyInt_Check(rhs)) {
        return incref(NotImplemented);
    }
    BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
    return boxInt(lhs->n & rhs_int->n);
}

extern "C" Box* intOrInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(PyInt_Check(lhs));
    assert(PyInt_Check(rhs));
    return boxInt(lhs->n | rhs->n);
}

extern "C" Box* intOr(BoxedInt* lhs, Box* rhs) {
    if (unlikely(PyLongObject_Check(lhs)))
        return forwardToLong("__or__", lhs, rhs);
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__or__' requires a 'int' object but received a '%s'", getTypeName(lhs));
    if (unlikely(PyLongObject_Check(rhs)))
        return forwardToLong("__or__", autoDecref(boxLong(lhs->n)), rhs);

    if (!PyInt_Check(rhs)) {
        return incref(NotImplemented);
    }
    BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
    return boxInt(lhs->n | rhs_int->n);
}

Box* intROr(BoxedInt* lhs, Box* rhs) {
    if (unlikely(PyLongObject_Check(lhs)))
        return forwardToLong("__ror__", lhs, rhs);
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__ror__' requires a 'int' object but received a '%s'", getTypeName(lhs));
    if (unlikely(PyLongObject_Check(rhs)))
        return forwardToLong("__ror__", autoDecref(boxLong(lhs->n)), rhs);

    if (!PyInt_Check(rhs)) {
        return incref(NotImplemented);
    }
    BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
    return boxInt(lhs->n | rhs_int->n);
}

extern "C" Box* intXorInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(PyInt_Check(lhs));
    assert(PyInt_Check(rhs));
    return boxInt(lhs->n ^ rhs->n);
}

extern "C" Box* intXor(BoxedInt* lhs, Box* rhs) {
    if (unlikely(PyLongObject_Check(lhs)))
        return forwardToLong("__xor__", lhs, rhs);
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__xor__' requires a 'int' object but received a '%s'", getTypeName(lhs));
    if (unlikely(PyLongObject_Check(rhs)))
        return forwardToLong("__xor__", autoDecref(boxLong(lhs->n)), rhs);

    if (!PyInt_Check(rhs)) {
        return incref(NotImplemented);
    }
    BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
    return boxInt(lhs->n ^ rhs_int->n);
}

Box* intRXor(BoxedInt* lhs, Box* rhs) {
    if (unlikely(PyLongObject_Check(lhs)))
        return forwardToLong("__rxor__", lhs, rhs);
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__rxor__' requires a 'int' object but received a '%s'",
                       getTypeName(lhs));
    if (unlikely(PyLongObject_Check(rhs)))
        return forwardToLong("__rxor__", autoDecref(boxLong(lhs->n)), rhs);

    if (!PyInt_Check(rhs)) {
        return incref(NotImplemented);
    }
    BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
    return boxInt(lhs->n ^ rhs_int->n);
}

extern "C" Box* intDivInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(PyInt_Check(lhs));
    assert(PyInt_Check(rhs));
    return div_i64_i64(lhs->n, rhs->n);
}

extern "C" Box* intDivFloat(BoxedInt* lhs, BoxedFloat* rhs) {
    assert(PyInt_Check(lhs));
    assert(PyFloat_Check(rhs));

    if (rhs->d == 0) {
        raiseExcHelper(ZeroDivisionError, "float division by zero");
    }
    return boxFloat(lhs->n / rhs->d);
}

extern "C" Box* intDiv(BoxedInt* lhs, Box* rhs) {
    if (unlikely(PyLongObject_Check(lhs)))
        return forwardToLong("__div__", lhs, rhs);
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__div__' requires a 'int' object but received a '%s'", getTypeName(lhs));
    if (unlikely(PyLongObject_Check(rhs)))
        return forwardToLong("__div__", autoDecref(boxLong(lhs->n)), rhs);

    if (PyInt_Check(rhs)) {
        return intDivInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (PyFloat_CheckExact(rhs)) {
        return intDivFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else {
        return incref(NotImplemented);
    }
}

Box* intRDiv(BoxedInt* lhs, Box* rhs) {
    if (unlikely(PyLongObject_Check(lhs)))
        return forwardToLong("__rdiv__", lhs, rhs);
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__rdiv__' requires a 'int' object but received a '%s'",
                       getTypeName(lhs));
    if (unlikely(PyLongObject_Check(rhs)))
        return forwardToLong("__rdiv__", autoDecref(boxLong(lhs->n)), rhs);

    if (PyInt_Check(rhs)) {
        return div_i64_i64(static_cast<BoxedInt*>(rhs)->n, lhs->n);
    } else {
        return incref(NotImplemented);
    }
}

extern "C" Box* intFloordivInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(PyInt_Check(lhs));
    assert(PyInt_Check(rhs));
    return div_i64_i64(lhs->n, rhs->n);
}

extern "C" Box* intFloordivFloat(BoxedInt* lhs, BoxedFloat* rhs) {
    assert(PyInt_Check(lhs));
    assert(PyFloat_Check(rhs));

    if (rhs->d == 0) {
        raiseExcHelper(ZeroDivisionError, "float divmod()");
    }
    return boxFloat(floor(lhs->n / rhs->d));
}

extern "C" Box* intFloordiv(BoxedInt* lhs, Box* rhs) {
    if (unlikely(PyLongObject_Check(lhs)))
        return forwardToLong("__floordiv__", lhs, rhs);
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__floordiv__' requires a 'int' object but received a '%s'",
                       getTypeName(lhs));
    if (unlikely(PyLongObject_Check(rhs)))
        return forwardToLong("__floordiv__", autoDecref(boxLong(lhs->n)), rhs);

    if (PyInt_Check(rhs)) {
        return intFloordivInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (PyFloat_CheckExact(rhs)) {
        return intFloordivFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else {
        return incref(NotImplemented);
    }
}

Box* intRFloordiv(BoxedInt* lhs, Box* rhs) {
    if (unlikely(PyLongObject_Check(lhs)))
        return forwardToLong("__rfloordiv__", lhs, rhs);
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__rfloordiv__' requires a 'int' object but received a '%s'",
                       getTypeName(lhs));
    if (unlikely(PyLongObject_Check(rhs)))
        return forwardToLong("__rfloordiv__", autoDecref(boxLong(lhs->n)), rhs);

    if (!PyInt_Check(rhs)) {
        return incref(NotImplemented);
    }

    return div_i64_i64(static_cast<BoxedInt*>(rhs)->n, lhs->n);
}

extern "C" Box* intTruedivInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(PyInt_Check(lhs));
    assert(PyInt_Check(rhs));

    if (rhs->n == 0) {
        raiseExcHelper(ZeroDivisionError, "division by zero");
    }
    return boxFloat(lhs->n / (double)rhs->n);
}

extern "C" Box* intTruedivFloat(BoxedInt* lhs, BoxedFloat* rhs) {
    assert(PyInt_Check(lhs));
    assert(PyFloat_Check(rhs));

    if (rhs->d == 0) {
        raiseExcHelper(ZeroDivisionError, "float division by zero");
    }
    return boxFloat(lhs->n / rhs->d);
}

extern "C" Box* intTruediv(BoxedInt* lhs, Box* rhs) {
    if (unlikely(PyLongObject_Check(lhs)))
        return forwardToLong("__truediv__", lhs, rhs);
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__truediv__' requires a 'int' object but received a '%s'",
                       getTypeName(lhs));
    if (unlikely(PyLongObject_Check(rhs)))
        return forwardToLong("__truediv__", autoDecref(boxLong(lhs->n)), rhs);

    if (PyInt_Check(rhs)) {
        return intTruedivInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (PyFloat_CheckExact(rhs)) {
        return intTruedivFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else {
        return incref(NotImplemented);
    }
}

Box* intRTruediv(BoxedInt* lhs, Box* rhs) {
    if (unlikely(PyLongObject_Check(lhs)))
        return forwardToLong("__rtruediv__", lhs, rhs);
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__rtruediv__' requires a 'int' object but received a '%s'",
                       getTypeName(lhs));
    if (unlikely(PyLongObject_Check(rhs)))
        return forwardToLong("__rtruediv__", autoDecref(boxLong(lhs->n)), rhs);

    if (PyInt_Check(rhs)) {
        return intTruedivInt(static_cast<BoxedInt*>(rhs), lhs);
    } else {
        return incref(NotImplemented);
    }
}

extern "C" Box* intLShiftInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(PyInt_Check(lhs));
    assert(PyInt_Check(rhs));

    if (rhs->n < 0)
        raiseExcHelper(ValueError, "negative shift count");

    bool undefined = rhs->n >= sizeof(rhs->n) * 8;
    if (!undefined) {
        int64_t res = lhs->n << rhs->n;
        if ((res >> rhs->n) == lhs->n)
            return boxInt(lhs->n << rhs->n);
    }
    return longLShiftLong(autoDecref(boxLong(lhs->n)), rhs);
}

extern "C" Box* intLShift(BoxedInt* lhs, Box* rhs) {
    if (unlikely(PyLongObject_Check(lhs)))
        return forwardToLong("__lshift__", lhs, rhs);
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__lshift__' requires a 'int' object but received a '%s'",
                       getTypeName(lhs));
    if (unlikely(PyLongObject_Check(rhs)))
        return forwardToLong("__lshift__", autoDecref(boxLong(lhs->n)), rhs);

    if (PyLongObject_Check(rhs))
        return longLShiftLong(autoDecref(boxLong(lhs->n)), rhs);

    if (!PyInt_Check(rhs)) {
        return incref(NotImplemented);
    }
    BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
    return intLShiftInt(lhs, rhs_int);
}

Box* intRLShift(BoxedInt* lhs, Box* rhs) {
    if (unlikely(PyLongObject_Check(lhs)))
        return forwardToLong("__rlshift__", lhs, rhs);
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__rlshift__' requires a 'int' object but received a '%s'",
                       getTypeName(lhs));
    if (unlikely(PyLongObject_Check(rhs)))
        return forwardToLong("__rlshift__", autoDecref(boxLong(lhs->n)), rhs);


    if (!PyInt_Check(rhs)) {
        return incref(NotImplemented);
    }
    BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
    return intLShiftInt(rhs_int, lhs);
}

extern "C" Box* intModInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(PyInt_Check(lhs));
    assert(PyInt_Check(rhs));
    return mod_i64_i64(lhs->n, rhs->n);
}

extern "C" Box* intMod(BoxedInt* lhs, Box* rhs) {
    if (unlikely(PyLongObject_Check(lhs)))
        return forwardToLong("__mod__", lhs, rhs);
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__mod__' requires a 'int' object but received a '%s'", getTypeName(lhs));
    if (unlikely(PyLongObject_Check(rhs)))
        return forwardToLong("__mod__", autoDecref(boxLong(lhs->n)), rhs);

    if (!PyInt_Check(rhs)) {
        return incref(NotImplemented);
    }
    BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
    return mod_i64_i64(lhs->n, rhs_int->n);
}

Box* intRMod(BoxedInt* lhs, Box* rhs) {
    if (unlikely(PyLongObject_Check(lhs)))
        return forwardToLong("__rmod__", lhs, rhs);
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__rmod__' requires a 'int' object but received a '%s'",
                       getTypeName(lhs));
    if (unlikely(PyLongObject_Check(rhs)))
        return forwardToLong("__rmod__", autoDecref(boxLong(lhs->n)), rhs);

    if (!PyInt_Check(rhs)) {
        return incref(NotImplemented);
    }
    BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
    return mod_i64_i64(rhs_int->n, lhs->n);
}

extern "C" Box* intDivmod(BoxedInt* lhs, Box* rhs) {
    if (unlikely(PyLongObject_Check(lhs)))
        return forwardToLong("__divmod__", lhs, rhs);
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__divmod__' requires a 'int' object but received a '%s'",
                       getTypeName(lhs));
    if (unlikely(PyLongObject_Check(rhs)))
        return forwardToLong("__divmod__", autoDecref(boxLong(lhs->n)), rhs);
    // Like CPython, leave floats to float.__rdivmod__.
    if (!PyInt_Check(rhs))
        return incref(NotImplemented);

    Box* divResult = intDiv(lhs, rhs);

    if (divResult == NotImplemented) {
        return divResult;
    }

    AUTO_DECREF(divResult);

    Box* modResult = intMod(lhs, rhs);

    if (modResult == NotImplemented) {
        return modResult;
    }

    AUTO_DECREF(modResult);

    Box* arg[2] = { divResult, modResult };
    return createTuple(2, arg);
}

Box* intRDivmod(BoxedInt* lhs, Box* rhs) {
    if (unlikely(PyLongObject_Check(lhs)))
        return forwardToLong("__rdivmod__", lhs, rhs);
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__rdivmod__' requires a 'int' object but received a '%s'",
                       getTypeName(lhs));
    if (unlikely(PyLongObject_Check(rhs)))
        return forwardToLong("__rdivmod__", autoDecref(boxLong(lhs->n)), rhs);

    if (!PyInt_Check(rhs)) {
        return incref(NotImplemented);
    }
    return intDivmod(static_cast<BoxedInt*>(rhs), lhs);
}

extern "C" Box* intMulInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(PyInt_Check(lhs));
    assert(PyInt_Check(rhs));
    return mul_i64_i64(lhs->n, rhs->n);
}

extern "C" Box* intMulFloat(BoxedInt* lhs, BoxedFloat* rhs) {
    assert(PyInt_Check(lhs));
    assert(PyFloat_Check(rhs));
    return boxFloat(lhs->n * rhs->d);
}

extern "C" Box* intMul(BoxedInt* lhs, Box* rhs) {
    if (unlikely(PyLongObject_Check(lhs)))
        return forwardToLong("__mul__", lhs, rhs);
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__mul__' requires a 'int' object but received a '%s'", getTypeName(lhs));
    if (unlikely(PyLongObject_Check(rhs)))
        return forwardToLong("__mul__", autoDecref(boxLong(lhs->n)), rhs);

    if (PyInt_Check(rhs)) {
        BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
        return intMulInt(lhs, rhs_int);
    } else if (PyFloat_CheckExact(rhs)) {
        BoxedFloat* rhs_float = static_cast<BoxedFloat*>(rhs);
        return intMulFloat(lhs, rhs_float);
    } else {
        return incref(NotImplemented);
    }
}

Box* intRMul(BoxedInt* lhs, Box* rhs) {
    if (unlikely(PyLongObject_Check(lhs)))
        return forwardToLong("__rmul__", lhs, rhs);
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__rmul__' requires a 'int' object but received a '%s'",
                       getTypeName(lhs));
    if (unlikely(PyLongObject_Check(rhs)))
        return forwardToLong("__rmul__", autoDecref(boxLong(lhs->n)), rhs);

    if (!PyInt_Check(rhs))
        return incref(NotImplemented);

    return intMul(lhs, rhs);
}

static void _addFuncPow(const char* name, ConcreteCompilerType* rtn_type, void* float_func, void* int_func) {
    std::vector<ConcreteCompilerType*> v_ifu{ BOXED_INT, BOXED_FLOAT, UNKNOWN };
    std::vector<ConcreteCompilerType*> v_uuu{ UNKNOWN, UNKNOWN, UNKNOWN };

    BoxedCode* code = new BoxedCode(3, false, false, name);
    code->addVersion(float_func, UNKNOWN, v_ifu);
    code->addVersion(int_func, UNKNOWN, v_uuu);
    int_cls->giveAttr(name, new BoxedFunction(code, { Py_None }));
}

extern "C" Box* intPowLong(BoxedInt* lhs, BoxedLong* rhs, Box* mod) {
    assert(PyInt_Check(lhs));
    assert(PyLongObject_Check(rhs));
    BoxedLong* lhs_long = boxLong(lhs->n);
    return longPow(autoDecref(lhs_long), rhs, mod);
}

extern "C" Box* intPowFloat(BoxedInt* lhs, BoxedFloat* rhs, Box* mod) {
    assert(PyInt_Check(lhs));
    assert(PyFloat_Check(rhs));

    if (mod != Py_None) {
        raiseExcHelper(TypeError, "pow() 3rd argument not allowed unless all arguments are integers");
    }
    return powFloatFloat(lhs->n, rhs->d);
}

extern "C" Box* intPow(BoxedInt* lhs, Box* rhs, Box* mod) {
    if (unlikely(PyLongObject_Check(lhs)))
        return forwardToLong("__pow__", lhs, rhs, mod);
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__pow__' requires a 'int' object but received a '%s'", getTypeName(lhs));
    if (unlikely(PyLongObject_Check(rhs)))
        return forwardToLong("__pow__", autoDecref(boxLong(lhs->n)), rhs, mod);

    if (PyLongObject_Check(rhs))
        return intPowLong(lhs, static_cast<BoxedLong*>(rhs), mod);
    else if (PyFloat_CheckExact(rhs))
        return intPowFloat(lhs, static_cast<BoxedFloat*>(rhs), mod);
    else if (!PyInt_Check(rhs))
        return incref(NotImplemented);

    BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
    // A negative exponent with a modulus needs the modular inverse, which the GMP version does.
    if (mod != Py_None && rhs_int->n < 0)
        return forwardToLong("__pow__", autoDecref(boxLong(lhs->n)), rhs, mod);

    Box* rtn = pow_i64_i64(lhs->n, rhs_int->n, mod);
    if (PyLongObject_Check(rtn))
        return longInt(autoDecref(rtn));
    return rtn;
}

Box* intRPow(BoxedInt* lhs, Box* rhs, Box* mod) {
    if (unlikely(PyLongObject_Check(lhs)))
        return forwardToLong("__rpow__", lhs, rhs, mod);
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__rpow__' requires a 'int' object but received a '%s'",
                       getTypeName(lhs));
    if (unlikely(PyLongObject_Check(rhs)))
        return forwardToLong("__rpow__", autoDecref(boxLong(lhs->n)), rhs, mod);

    if (!PyInt_Check(rhs))
        return incref(NotImplemented);
    BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
    if (mod != Py_None && lhs->n < 0)
        return forwardToLong("__pow__", autoDecref(boxLong(rhs_int->n)), lhs, mod);
    Box* rtn = pow_i64_i64(rhs_int->n, lhs->n, mod);
    if (PyLongObject_Check(rtn))
        return longInt(autoDecref(rtn));
    return rtn;
}

Box* intRShiftInt(BoxedInt* lhs, BoxedInt* rhs) {
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__rshift__' requires a 'int' object but received a '%s'",
                       getTypeName(lhs));
    assert(PyInt_Check(rhs));

    if (rhs->n < 0)
        raiseExcHelper(ValueError, "negative shift count");

    return boxInt(lhs->n >> rhs->n);
}

extern "C" Box* intRShift(BoxedInt* lhs, Box* rhs) {
    if (unlikely(PyLongObject_Check(lhs)))
        return forwardToLong("__rshift__", lhs, rhs);
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__rshift__' requires a 'int' object but received a '%s'",
                       getTypeName(lhs));
    if (unlikely(PyLongObject_Check(rhs)))
        return forwardToLong("__rshift__", autoDecref(boxLong(lhs->n)), rhs);

    if (PyLongObject_Check(rhs))
        return longRShiftLong(autoDecref(boxLong(lhs->n)), rhs);

    if (!PyInt_Check(rhs)) {
        return incref(NotImplemented);
    }
    BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
    return intRShiftInt(lhs, rhs_int);
}

Box* intRRShift(BoxedInt* lhs, Box* rhs) {
    if (unlikely(PyLongObject_Check(lhs)))
        return forwardToLong("__rrshift__", lhs, rhs);
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__rrshift__' requires a 'int' object but received a '%s'",
                       getTypeName(lhs));
    if (unlikely(PyLongObject_Check(rhs)))
        return forwardToLong("__rrshift__", autoDecref(boxLong(lhs->n)), rhs);

    if (!PyInt_Check(rhs)) {
        return incref(NotImplemented);
    }
    BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
    return intRShiftInt(rhs_int, lhs);
}

extern "C" Box* intSubInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(PyInt_Check(lhs));
    assert(PyInt_Check(rhs));
    return sub_i64_i64(lhs->n, rhs->n);
}

extern "C" Box* intSubFloat(BoxedInt* lhs, BoxedFloat* rhs) {
    assert(PyInt_Check(lhs));
    assert(PyFloat_Check(rhs));
    return boxFloat(lhs->n - rhs->d);
}

extern "C" Box* intSub(BoxedInt* lhs, Box* rhs) {
    if (unlikely(PyLongObject_Check(lhs)))
        return forwardToLong("__sub__", lhs, rhs);
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__sub__' requires a 'int' object but received a '%s'", getTypeName(lhs));
    if (unlikely(PyLongObject_Check(rhs)))
        return forwardToLong("__sub__", autoDecref(boxLong(lhs->n)), rhs);

    if (PyInt_Check(rhs)) {
        BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
        return intSubInt(lhs, rhs_int);
    } else if (PyFloat_CheckExact(rhs)) {
        BoxedFloat* rhs_float = static_cast<BoxedFloat*>(rhs);
        return intSubFloat(lhs, rhs_float);
    } else {
        return incref(NotImplemented);
    }
}

Box* intRSub(BoxedInt* lhs, Box* rhs) {
    if (unlikely(PyLongObject_Check(lhs)))
        return forwardToLong("__rsub__", lhs, rhs);
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__rsub__' requires a 'int' object but received a '%s'",
                       getTypeName(lhs));
    if (unlikely(PyLongObject_Check(rhs)))
        return forwardToLong("__rsub__", autoDecref(boxLong(lhs->n)), rhs);

    if (PyInt_Check(rhs)) {
        BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
        return sub_i64_i64(rhs_int->n, lhs->n);
    } else {
        return incref(NotImplemented);
    }
}

extern "C" Box* intInvert(BoxedInt* v) {
    if (unlikely(PyLongObject_Check(v)))
        return forwardToLong("__invert__", v);
    if (!PyInt_Check(v))
        raiseExcHelper(TypeError, "descriptor '__invert__' requires a 'int' object but received a '%s'",
                       getTypeName(v));

    return boxInt(~v->n);
}

extern "C" Box* intPos(BoxedInt* v) {
    if (unlikely(PyLongObject_Check(v)))
        return forwardToLong("__pos__", v);
    if (!PyInt_Check(v))
        raiseExcHelper(TypeError, "descriptor '__pos__' requires a 'int' object but received a '%s'", getTypeName(v));

    if (v->cls == int_cls)
        return incref(v);
    return boxInt(v->n);
}

extern "C" Box* intNeg(BoxedInt* v) {
    if (unlikely(PyLongObject_Check(v)))
        return forwardToLong("__neg__", v);
    if (!PyInt_Check(v))
        raiseExcHelper(TypeError, "descriptor '__neg__' requires a 'int' object but received a '%s'", getTypeName(v));


// It's possible for this to overflow:
#if PYSTON_INT_MIN < -PYSTON_INT_MAX
    static_assert(PYSTON_INT_MIN == -PYSTON_INT_MAX - 1, "");

    if (v->n == PYSTON_INT_MIN) {
        return longNeg(autoDecref(boxLong(v->n)));
    }
#endif

    return boxInt(-v->n);
}

extern "C" Box* intNonzero(BoxedInt* v) {
    if (unlikely(PyLongObject_Check(v)))
        return forwardToLong("__bool__", v);
    if (!PyInt_Check(v))
        raiseExcHelper(TypeError, "descriptor '__bool__' requires a 'int' object but received a '%s'",
                       getTypeName(v));

    return boxBool(v->n != 0);
}

extern "C" BoxedString* intRepr(BoxedInt* v) {
    if (PyLongObject_Check(v)) {
        Box* r = long_cls->tp_repr(v);
        if (!r)
            throwCAPIException();
        return static_cast<BoxedString*>(r);
    }
    if (!PyInt_Check(v))
        raiseExcHelper(TypeError, "descriptor '__repr__' requires a 'int' object but received a '%s'", getTypeName(v));

    char buf[80];
    int len = snprintf(buf, 80, "%ld", v->n);
    return static_cast<BoxedString*>(boxString(llvm::StringRef(buf, len)));
}

// Python 3 hashes ints modulo _PyHASH_MODULUS so that equal ints and floats hash the same.
static int64_t hashInt64(int64_t n) {
    uint64_t a = n < 0 ? -(uint64_t)n : (uint64_t)n;
    int64_t x = (int64_t)(a % _PyHASH_MODULUS);
    if (n < 0)
        x = -x;
    return x == -1 ? -2 : x;
}

extern "C" Box* intHash(BoxedInt* self) {
    if (unlikely(PyLongObject_Check(self)))
        return forwardToLong("__hash__", self);
    if (!PyInt_Check(self))
        raiseExcHelper(TypeError, "descriptor '__hash__' requires a 'int' object but received a '%s'",
                       getTypeName(self));

    int64_t h = hashInt64(self->n);
    if (h == self->n && self->cls == int_cls)
        return incref(self);
    return boxInt(h);
}

Box* intAbs(BoxedInt* v) {
    if (unlikely(PyLongObject_Check(v)))
        return forwardToLong("__abs__", v);
    if (!PyInt_Check(v))
        raiseExcHelper(TypeError, "descriptor '__abs__' requires a 'int' object but received a '%s'", getTypeName(v));

    if (v->n == PYSTON_INT_MIN) {
        return longNeg(autoDecref(boxLong(v->n)));
    }
    return boxInt(std::abs(v->n));
}

extern "C" Box* intTrunc(BoxedInt* self) {
    if (unlikely(PyLongObject_Check(self)))
        return forwardToLong("__trunc__", self);
    if (!PyInt_Check(self))
        raiseExcHelper(TypeError, "descriptor '__trunc__' requires a 'int' object but received a '%s'",
                       getTypeName(self));

    if (self->cls == int_cls)
        return incref(self);
    return boxInt(self->n);
}

static PyObject* int_int(BoxedInt* v) noexcept {
    if (v->cls == int_cls)
        return incref(v);
    return boxInt(v->n);
}

static PyObject* int_long(BoxedInt* v) noexcept {
    return boxLong(v->n);
}

extern "C" Box* intInt(BoxedInt* self) {
    if (unlikely(PyLongObject_Check(self)))
        return forwardToLong("__int__", self);
    if (!PyInt_Check(self))
        raiseExcHelper(TypeError, "descriptor '__int__' requires a 'int' object but received a '%s'",
                       getTypeName(self));

    return int_int(self);
}

Box* intFloat(BoxedInt* self) {
    if (unlikely(PyLongObject_Check(self)))
        return forwardToLong("__float__", self);
    if (!PyInt_Check(self))
        raiseExcHelper(TypeError, "descriptor '__float__' requires a 'int' object but received a '%s'",
                       getTypeName(self));

    return boxFloat(self->n);
}

Box* intLong(BoxedInt* self) {
    if (unlikely(PyLongObject_Check(self)))
        return forwardToLong("__long__", self);
    if (!PyInt_Check(self))
        raiseExcHelper(TypeError, "descriptor '__long__' requires a 'int' object but received a '%s'",
                       getTypeName(self));

    return int_long(self);
}

extern "C" Box* intIndex(BoxedInt* v) {
    if (unlikely(PyLongObject_Check(v)))
        return forwardToLong("__index__", v);
    if (PyInt_CheckExact(v))
        return incref(v);
    return boxInt(v->n);
}

static Box* intNewWithBase(Box* val, Box* _base) noexcept {
    Py_ssize_t base = PyNumber_AsSsize_t(_base, NULL);
    if (base == -1 && PyErr_Occurred())
        return NULL;
    if ((base != 0 && base < 2) || base > 36) {
        PyErr_SetString(PyExc_ValueError, "int() base must be >= 2 and <= 36, or 0");
        return NULL;
    }
    if (val == NULL) {
        PyErr_SetString(PyExc_TypeError, "int() missing string argument");
        return NULL;
    }

    if (PyUnicode_Check(val))
        return PyLong_FromUnicodeObject(val, base);
    if (PyBytes_Check(val))
        return _PyLong_FromBytes(PyBytes_AS_STRING(val), PyBytes_GET_SIZE(val), base);
    if (PyByteArray_Check(val))
        return _PyLong_FromBytes(PyByteArray_AS_STRING(val), PyByteArray_GET_SIZE(val), base);
    PyErr_SetString(PyExc_TypeError, "int() can't convert non-string with explicit base");
    return NULL;
}

template <ExceptionStyle S> static Box* _intNew(Box* val, Box* _base) noexcept(S == CAPI) {
    Box* r;
    if (_base)
        r = intNewWithBase(val, _base);
    else if (val == NULL)
        r = PyLong_FromLong(0L);
    else
        r = PyNumber_Long(val);
    if (!r && S == CXX)
        throwCAPIException();
    return r;
}

template <ExceptionStyle S> Box* intNew(Box* _cls, Box* val, Box* base) noexcept(S == CAPI) {
    if (!PyType_Check(_cls)) {
        if (S == CAPI) {
            PyErr_Format(TypeError, "int.__new__(X): X is not a type object (%s)", getTypeName(_cls));
            return NULL;
        } else
            raiseExcHelper(TypeError, "int.__new__(X): X is not a type object (%s)", getTypeName(_cls));
    }

    BoxedClass* cls = static_cast<BoxedClass*>(_cls);
    if (!isSubclass(cls, int_cls)) {
        if (S == CAPI) {
            PyErr_Format(TypeError, "int.__new__(%s): %s is not a subtype of int", getNameOfClass(cls),
                         getNameOfClass(cls));
            return NULL;
        } else
            raiseExcHelper(TypeError, "int.__new__(%s): %s is not a subtype of int", getNameOfClass(cls),
                           getNameOfClass(cls));
    }

    if (cls == int_cls)
        return _intNew<S>(val, base);

    if (cls == bool_cls) {
        if (S == CAPI) {
            PyErr_SetString(TypeError, "int.__new__(bool) is not safe, use bool.__new__()");
            return NULL;
        } else
            raiseExcHelper(TypeError, "int.__new__(bool) is not safe, use bool.__new__()");
    }

    BoxedInt* n = (BoxedInt*)_intNew<S>(val, base);
    if (!n) {
        assert(S == CAPI);
        return NULL;
    }

    if (n->cls == long_cls) {
        if (cls == int_cls)
            return n;

        Py_DECREF(n);

        if (S == CAPI) {
            PyErr_SetString(OverflowError, "Python int too large to convert to C long");
            return NULL;
        } else
            raiseExcHelper(OverflowError, "Python int too large to convert to C long");
    }

    AUTO_DECREF(n);
    return new (cls) BoxedInt(n->n);
}

// Roughly analogous to CPython's int_new.
// The arguments need to be unpacked from args and kwds.
static Box* intNewPacked(BoxedClass* type, Box* args, Box* kwds) noexcept {
    PyObject* x = NULL;
    PyObject* base = NULL;
    static const char* const kwlist[] = { "", "base", NULL };

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|OO:int", const_cast<char**>(kwlist), &x, &base))
        return NULL;
    return intNew<CAPI>(type, x, base);
}

static const unsigned char BitLengthTable[32]
    = { 0, 1, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5 };

static int bits_in_ulong(unsigned long d) noexcept {
    int d_bits = 0;
    while (d >= 32) {
        d_bits += 6;
        d >>= 6;
    }
    d_bits += (int)BitLengthTable[d];
    return d_bits;
}

extern "C" Box* intBitLength(BoxedInt* v) {
    if (unlikely(PyLongObject_Check(v)))
        return forwardToLong("bit_length", v);
    if (!PyInt_Check(v))
        raiseExcHelper(TypeError, "descriptor 'bit_length' requires a 'int' object but received a '%s'",
                       getTypeName(v));

    unsigned long n;
    if (v->n < 0)
        /* avoid undefined behaviour when v->n == -LONG_MAX-1 */
        n = 0U - (unsigned long)v->n;
    else
        n = (unsigned long)v->n;

    return PyLong_FromLong(bits_in_ulong(n));
}

static void _addFuncIntFloatUnknown(const char* name, void* int_func, void* float_func, void* boxed_func) {
    std::vector<ConcreteCompilerType*> v_ii, v_if, v_iu;
    assert(BOXED_INT);
    v_ii.push_back(BOXED_INT);
    v_ii.push_back(BOXED_INT);
    v_if.push_back(BOXED_INT);
    v_if.push_back(BOXED_FLOAT);
    // Only the unknown version can accept non-ints (ex if you access the function directly ex via int.__add__)
    v_iu.push_back(UNKNOWN);
    v_iu.push_back(UNKNOWN);

    BoxedCode* code = new BoxedCode(2, false, false, name);
    code->addVersion(int_func, UNKNOWN, v_ii);
    code->addVersion(float_func, BOXED_FLOAT, v_if);
    registerNoPythonCodeFunction(int_func);
    registerNoPythonCodeFunction(float_func);
    code->addVersion(boxed_func, UNKNOWN, v_iu);
    int_cls->giveAttr(name, new BoxedFunction(code));
}

static void _addFuncIntUnknown(const char* name, ConcreteCompilerType* rtn_type, void* int_func, void* boxed_func) {
    std::vector<ConcreteCompilerType*> v_ii, v_iu;
    assert(BOXED_INT);
    v_ii.push_back(BOXED_INT);
    v_ii.push_back(BOXED_INT);
    v_iu.push_back(UNKNOWN);
    v_iu.push_back(UNKNOWN);

    BoxedCode* code = new BoxedCode(2, false, false, name);
    code->addVersion(int_func, rtn_type, v_ii);
    code->addVersion(boxed_func, UNKNOWN, v_iu);
    registerNoPythonCodeFunction(int_func);
    int_cls->giveAttr(name, new BoxedFunction(code));
}

static Box* int_int_getset(Box* b, void*) noexcept {
    if (b->cls == int_cls || b->cls == long_cls) {
        return incref(b);
    } else {
        assert(PyInt_Check(b));
        return boxInt(static_cast<BoxedInt*>(b)->n);
    }
}

static Box* int0(Box*, void*) noexcept {
    return boxInt(0);
}

static Box* int1(Box*, void*) noexcept {
    return boxInt(1);
}

static int64_t int_hash(BoxedInt* o) noexcept {
    if (PyLongObject_Check(o))
        return long_cls->tp_hash(o);
    return hashInt64(o->n);
}

static PyObject* int_richcompare(PyObject* v, PyObject* w, int op) noexcept {
    if (PyLongObject_Check(v) || PyLongObject_Check(w)) {
        static const int swapped_op[] = { Py_GT, Py_GE, Py_EQ, Py_NE, Py_LT, Py_LE };
        if (PyLongObject_Check(v) && PyLong_Check(w))
            return long_cls->tp_richcompare(v, w, op);
        if (PyInt_Check(v) && PyLongObject_Check(w))
            return long_cls->tp_richcompare(w, v, swapped_op[op]);
    }
    if (!PyInt_Check(v) || !PyInt_Check(w)) {
        Py_INCREF(Py_NotImplemented);
        return Py_NotImplemented;
    }

    int64_t lhs = static_cast<BoxedInt*>(v)->n;
    int64_t rhs = static_cast<BoxedInt*>(w)->n;

    switch (op) {
        case Py_EQ:
            return boxBool(lhs == rhs);
        case Py_NE:
            return boxBool(lhs != rhs);
        case Py_LT:
            return boxBool(lhs < rhs);
        case Py_LE:
            return boxBool(lhs <= rhs);
        case Py_GT:
            return boxBool(lhs > rhs);
        case Py_GE:
            return boxBool(lhs >= rhs);
        default:
            RELEASE_ASSERT(0, "%d", op);
    }
}

static PyObject* int_getnewargs(BoxedInt* v) noexcept {
    return Py_BuildValue("(l)", v->n);
}

extern "C" int PyInt_ClearFreeList() noexcept {
    PyIntObject* p;
    PyIntBlock* list, *next;
    int i;
    int u; /* remaining unfreed ints per block */
    int freelist_size = 0;

    list = block_list;
    block_list = NULL;
    BoxedInt::free_list = NULL;
    while (list != NULL) {
        u = 0;
        for (i = 0, p = &list->objects[0]; i < N_INTOBJECTS; i++, p++) {
            if (PyInt_CheckExact((BoxedInt*)p) && Py_REFCNT(p) != 0)
                u++;
        }
        next = list->next;
        if (u) {
            list->next = block_list;
            block_list = list;
            for (i = 0, p = &list->objects[0]; i < N_INTOBJECTS; i++, p++) {
                if (!PyInt_CheckExact((BoxedInt*)p) || Py_REFCNT(p) == 0) {
                    Py_TYPE(p) = (PyTypeObject*)BoxedInt::free_list;
                    BoxedInt::free_list = p;
                }
#if NSMALLNEGINTS + NSMALLPOSINTS > 0
                else if (-NSMALLNEGINTS <= p->ob_ival && p->ob_ival < NSMALLPOSINTS
                         && small_ints[p->ob_ival + NSMALLNEGINTS] == NULL) {
                    Py_INCREF(p);
                    small_ints[p->ob_ival + NSMALLNEGINTS] = p;
                }
#endif
            }
        } else {
            PyMem_FREE(list);
        }
        freelist_size += u;
        list = next;
    }

    return freelist_size;
}

static Box* intFormat(PyObject* self, Box* format_spec) {
    if (!PyUnicode_Check(format_spec))
        raiseExcHelper(TypeError, "format() argument 2 must be str, not %s", getTypeName(format_spec));

    _PyUnicodeWriter writer;
    _PyUnicodeWriter_Init(&writer);
    if (_PyLong_FormatAdvancedWriter(&writer, self, format_spec, 0, PyUnicode_GET_LENGTH(format_spec)) == -1) {
        _PyUnicodeWriter_Dealloc(&writer);
        throwCAPIException();
    }
    Box* rtn = _PyUnicodeWriter_Finish(&writer);
    if (!rtn)
        throwCAPIException();
    return rtn;
}

// A C-level number slot for int that calls the implementation directly instead of going through the
// slot_nb_* wrappers, which look up and call int.__op__ like a Python method.
template <Box* (*F)(BoxedInt*, Box*), Box* (*RF)(BoxedInt*, Box*)>
static PyObject* intNbBinop(PyObject* a, PyObject* b) noexcept {
    try {
        if (PyLong_Check(a))
            return F(static_cast<BoxedInt*>(a), b);
        return RF(static_cast<BoxedInt*>(b), a);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

void setupInt() {
    static PyNumberMethods int_as_number;
    int_cls->tp_as_number = &int_as_number;

    for (int i = MIN_INTERNED_INT; i <= MAX_INTERNED_INT; i++) {
        interned_ints[-MIN_INTERNED_INT + i] = new BoxedInt(i);
    }

    int_cls->giveAttr("__getnewargs__",
                      new BoxedFunction(BoxedCode::create((void*)int_getnewargs, UNKNOWN, 1, "int.__getnewargs__", "",
                                                          ParamNames::empty(), CAPI)));
    _addFuncIntFloatUnknown("__add__", (void*)intAddInt, (void*)intAddFloat, (void*)intAdd);
    _addFuncIntUnknown("__and__", BOXED_INT, (void*)intAndInt, (void*)intAnd);
    _addFuncIntUnknown("__or__", BOXED_INT, (void*)intOrInt, (void*)intOr);
    _addFuncIntUnknown("__xor__", BOXED_INT, (void*)intXorInt, (void*)intXor);
    _addFuncIntFloatUnknown("__sub__", (void*)intSubInt, (void*)intSubFloat, (void*)intSub);
    _addFuncIntFloatUnknown("__floordiv__", (void*)intFloordivInt, (void*)intFloordivFloat, (void*)intFloordiv);
    _addFuncIntFloatUnknown("__truediv__", (void*)intTruedivInt, (void*)intTruedivFloat, (void*)intTruediv);
    _addFuncIntFloatUnknown("__mul__", (void*)intMulInt, (void*)intMulFloat, (void*)intMul);
    _addFuncIntUnknown("__mod__", UNKNOWN, (void*)intModInt, (void*)intMod);
    _addFuncPow("__pow__", BOXED_INT, (void*)intPowFloat, (void*)intPow);

    int_cls->giveAttr("__radd__", new BoxedFunction(BoxedCode::create((void*)intRAdd, UNKNOWN, 2, "int.__radd__")));
    int_cls->giveAttr("__rand__", new BoxedFunction(BoxedCode::create((void*)intRAnd, UNKNOWN, 2, "int.__rand__")));
    int_cls->giveAttr("__ror__", new BoxedFunction(BoxedCode::create((void*)intROr, UNKNOWN, 2, "int.__ror__")));
    int_cls->giveAttr("__rxor__", new BoxedFunction(BoxedCode::create((void*)intRXor, UNKNOWN, 2, "int.__rxor__")));

    int_cls->giveAttr("__rsub__", new BoxedFunction(BoxedCode::create((void*)intRSub, UNKNOWN, 2, "int.__rsub__")));
    int_cls->giveAttr("__rmul__", new BoxedFunction(BoxedCode::create((void*)intRMul, UNKNOWN, 2, "int.__rmul__")));
    int_cls->giveAttr("__rfloordiv__",
                      new BoxedFunction(BoxedCode::create((void*)intRFloordiv, UNKNOWN, 2, "int.__rfloordiv__")));
    int_cls->giveAttr("__rtruediv__",
                      new BoxedFunction(BoxedCode::create((void*)intRTruediv, UNKNOWN, 2, "int.__rtruediv__")));
    int_cls->giveAttr("__rmod__", new BoxedFunction(BoxedCode::create((void*)intRMod, UNKNOWN, 2, "int.__rmod__")));
    int_cls->giveAttr("__rdivmod__",
                      new BoxedFunction(BoxedCode::create((void*)intRDivmod, UNKNOWN, 2, "int.__rdivmod__")));
    int_cls->giveAttr(
        "__rpow__",
        new BoxedFunction(BoxedCode::create((void*)intRPow, UNKNOWN, 3, false, false, "int.__rpow__"), { Py_None }));
    int_cls->giveAttr("__rrshift__",
                      new BoxedFunction(BoxedCode::create((void*)intRRShift, UNKNOWN, 2, "int.__rrshift__")));
    int_cls->giveAttr("__rlshift__",
                      new BoxedFunction(BoxedCode::create((void*)intRLShift, UNKNOWN, 2, "int.__rlshift__")));
    // Note: CPython implements int comparisons using tp_compare
    int_cls->tp_richcompare = int_richcompare;

    _addFuncIntUnknown("__lshift__", UNKNOWN, (void*)intLShiftInt, (void*)intLShift);
    _addFuncIntUnknown("__rshift__", UNKNOWN, (void*)intRShiftInt, (void*)intRShift);

    int_cls->giveAttr("__invert__",
                      new BoxedFunction(BoxedCode::create((void*)intInvert, UNKNOWN, 1, "int.__invert__")));
    int_cls->giveAttr("__pos__", new BoxedFunction(BoxedCode::create((void*)intPos, UNKNOWN, 1, "int.__pos__")));
    int_cls->giveAttr("__neg__", new BoxedFunction(BoxedCode::create((void*)intNeg, UNKNOWN, 1, "int.__neg__")));
    int_cls->giveAttr("__bool__",
                      new BoxedFunction(BoxedCode::create((void*)intNonzero, BOXED_BOOL, 1, "int.__bool__")));
    int_cls->giveAttr("__repr__", new BoxedFunction(BoxedCode::create((void*)intRepr, STR, 1, "int.__repr__")));
    int_cls->tp_hash = (hashfunc)int_hash;
    int_cls->giveAttr("__divmod__",
                      new BoxedFunction(BoxedCode::create((void*)intDivmod, UNKNOWN, 2, "int.__divmod__")));
    int_cls->giveAttr("__abs__", new BoxedFunction(BoxedCode::create((void*)intAbs, UNKNOWN, 1, "int.__abs__")));

    int_cls->giveAttr("__trunc__",
                      new BoxedFunction(BoxedCode::create((void*)intTrunc, UNKNOWN, 1, "int.__trunc__")));
    int_cls->giveAttr("__index__",
                      new BoxedFunction(BoxedCode::create((void*)intIndex, UNKNOWN, 1, "int.__index__")));
    int_cls->giveAttr("__int__", new BoxedFunction(BoxedCode::create((void*)intInt, UNKNOWN, 1, "int.__int__")));
    int_cls->giveAttr("__float__",
                      new BoxedFunction(BoxedCode::create((void*)intFloat, BOXED_FLOAT, 1, "int.__float__")));

    int_cls->giveAttr("__format__", new BoxedFunction(BoxedCode::create((void*)intFormat, STR, 2, "int.__format__")));

    int_cls->giveAttr("__doc__",
                      boxString("int(x=0) -> int or long\n"
                                "int(x, base=10) -> int or long\n"
                                "\n"
                                "Convert a number or string to an integer, or return 0 if no arguments\n"
                                "are given.  If x is floating point, the conversion truncates towards zero.\n"
                                "If x is outside the integer range, the function returns a long instead.\n"
                                "\n"
                                "If x is not a number or if base is given, then x must be a string or\n"
                                "Unicode object representing an integer literal in the given base.  The\n"
                                "literal can be preceded by '+' or '-' and be surrounded by whitespace.\n"
                                "The base defaults to 10.  Valid bases are 0 and 2-36.  Base 0 means to\n"
                                "interpret the base from the string as an integer literal.\n"
                                ">>> int('0b100', base=0)\n"
                                "4"));

    auto int_new = BoxedCode::create((void*)intNew<CXX>, UNKNOWN, 3, false, false, "int.__new__", "",
                                     ParamNames({ "", "x", "base" }, "", "", 2), CXX);
    int_new->addVersion((void*)intNew<CAPI>, UNKNOWN, CAPI);
    int_cls->giveAttr("__new__", new BoxedFunction(int_new, { NULL, NULL }));


    int_cls->giveAttr("bit_length",
                      new BoxedFunction(BoxedCode::create((void*)intBitLength, UNKNOWN, 1, "int.bit_length")));

    // int_int_getset doesn't throw at all, so we can cheat and use it as both a CAPI and CXX style function.
    int_cls->giveAttrDescriptor("real", int_int_getset, NULL);
    int_cls->giveAttrDescriptor("imag", int0, NULL);
    int_cls->giveAttr("conjugate",
                      new BoxedFunction(BoxedCode::create((void*)int_int_getset, UNKNOWN, 1, "int.conjugate")));
    int_cls->giveAttrDescriptor("numerator", int_int_getset, NULL);
    int_cls->giveAttrDescriptor("denominator", int1, NULL);

    add_operators(int_cls);
    int_cls->freeze();

    int_cls->tp_repr = (reprfunc)int_to_decimal_string;
    int_cls->tp_new = (newfunc)intNewPacked;
    int_as_number.nb_int = (unaryfunc)int_int;
    int_as_number.nb_add = intNbBinop<intAdd, intRAdd>;
    int_as_number.nb_subtract = intNbBinop<intSub, intRSub>;
    int_as_number.nb_multiply = intNbBinop<intMul, intRMul>;
    int_as_number.nb_floor_divide = intNbBinop<intFloordiv, intRFloordiv>;
    int_as_number.nb_true_divide = intNbBinop<intTruediv, intRTruediv>;
    int_as_number.nb_remainder = intNbBinop<intMod, intRMod>;
    int_as_number.nb_and = intNbBinop<intAnd, intRAnd>;
    int_as_number.nb_or = intNbBinop<intOr, intROr>;
    int_as_number.nb_xor = intNbBinop<intXor, intRXor>;
}
}
