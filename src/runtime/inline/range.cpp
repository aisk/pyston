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

// range and its iterators come from CPython's Objects/rangeobject.c. This file gives range_iterator
// (the iterator for ranges that fit in a C long) Pyston's iteration protocol, so that the JIT can
// inline loops over a range and keep the loop variable unboxed.

#include "core/types.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

// Same layout as rangeiterobject in rangeobject.c.
class BoxedRangeIterator : public Box {
public:
    long index;
    long start;
    long step;
    long len;
};

llvm_compat_bool rangeiterHasnextUnboxed(Box* s) {
    assert(s->cls == &PyRangeIter_Type);
    BoxedRangeIterator* self = static_cast<BoxedRangeIterator*>(s);
    return self->index < self->len;
}

Box* rangeiterHasnext(Box* s) {
    return boxBool(rangeiterHasnextUnboxed(s));
}

i64 rangeiterNextUnboxed(Box* s) {
    assert(s->cls == &PyRangeIter_Type);
    BoxedRangeIterator* self = static_cast<BoxedRangeIterator*>(s);
    if (self->index >= self->len)
        raiseExcHelper(StopIteration, "");
    // cast to unsigned to avoid possible signed overflow in intermediate calculations.
    return (long)(self->start + (unsigned long)(self->index++) * self->step);
}

Box* rangeiterNext(Box* s) {
    return boxInt(rangeiterNextUnboxed(s));
}

void setupRange() {
    RELEASE_ASSERT(PyRangeIter_Type.tp_basicsize == sizeof(BoxedRangeIterator), "rangeiterobject layout changed");

    BoxedCode* hasnext = BoxedCode::create((void*)rangeiterHasnextUnboxed, BOOL, 1, "range_iterator.__hasnext__");
    hasnext->addVersion((void*)rangeiterHasnext, BOXED_BOOL);
    PyRangeIter_Type.giveAttr("__hasnext__", new BoxedFunction(hasnext));

    // Replaces the slot wrapper that PyType_Ready made from tp_iternext.
    BoxedCode* next = BoxedCode::create((void*)rangeiterNextUnboxed, UNBOXED_INT, 1, "range_iterator.__next__");
    next->addVersion((void*)rangeiterNext, BOXED_INT);
    PyRangeIter_Type.setattr(autoDecref(internStringMortal("__next__")), autoDecref(new BoxedFunction(next)), NULL);

    PyRangeIter_Type.tpp_hasnext = rangeiterHasnextUnboxed;
    registerNoPythonCodeFunction((void*)rangeiterHasnextUnboxed);
    registerNoPythonCodeFunction((void*)rangeiterNextUnboxed);

    // Like the frozen C++ classes, so the JIT can speculate on them. freeze() itself would replace the C slots.
    PyRange_Type.is_constant = true;
    PyRangeIter_Type.is_constant = true;
}
}
