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

#include <cstring>

#include "runtime/dict.h"
#include "runtime/objmodel.h"

namespace pyston {

BoxedDictIterator::BoxedDictIterator(BoxedDict* d) : d(d), it(d->d.begin()), itEnd(d->d.end()), size(d->d.size()) {
    Py_INCREF(d);
}

Box* dict_iter(Box* s) noexcept {
    assert(PyDict_Check(s));
    BoxedDict* self = static_cast<BoxedDict*>(s);
    return new (&PyDictIterKey_Type, FAST_GC) BoxedDictIterator(self);
}

Box* dictIterKeys(Box* s) {
    return dict_iter(s);
}

Box* dictIterValues(Box* s) {
    assert(PyDict_Check(s));
    BoxedDict* self = static_cast<BoxedDict*>(s);
    return new (&PyDictIterValue_Type, FAST_GC) BoxedDictIterator(self);
}

Box* dictIterItems(Box* s) {
    assert(PyDict_Check(s));
    BoxedDict* self = static_cast<BoxedDict*>(s);
    return new (&PyDictIterItem_Type, FAST_GC) BoxedDictIterator(self);
}

Box* dictIterIter(Box* s) {
    return incref(s);
}

llvm_compat_bool dictIterHasnextUnboxed(Box* s) {
    BoxedDictIterator* self = static_cast<BoxedDictIterator*>(s);

    // A size change is reported by the next call to next().
    if (self->size != -1 && self->size != self->d->d.size())
        return true;
    return self->it != self->itEnd;
}

Box* dictIterHasnext(Box* s) {
    return boxBool(dictIterHasnextUnboxed(s));
}

Box* dictiter_next(Box* s) noexcept {
    BoxedDictIterator* self = static_cast<BoxedDictIterator*>(s);

    if (self->size == -1)
        return NULL;
    if (self->size != self->d->d.size()) {
        self->size = -1;
        PyErr_SetString(PyExc_RuntimeError, "dictionary changed size during iteration");
        return NULL;
    }
    if (self->it == self->itEnd)
        return NULL;

    Box* rtn = nullptr;
    if (self->cls == &PyDictIterKey_Type) {
        rtn = incref(self->it->first.value);
    } else if (self->cls == &PyDictIterValue_Type) {
        rtn = incref(self->it->second);
    } else if (self->cls == &PyDictIterItem_Type) {
        rtn = BoxedTuple::create({ self->it->first.value, self->it->second });
    } else {
        RELEASE_ASSERT(0, "");
    }
    ++self->it;
    return rtn;
}

Box* dictIterNext(Box* s) {
    auto* rtn = dictiter_next(s);
    if (!rtn) {
        if (PyErr_Occurred())
            throwCAPIException();
        raiseExcHelper(StopIteration, "");
    }
    return rtn;
}
}
