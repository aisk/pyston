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

// str is implemented by CPython's Objects/unicodeobject.c.  This file only has the helpers that
// the rest of Pyston uses to create strings and to look inside them.

#include "llvm/ADT/SmallString.h"

#include "Python.h"

#include "core/common.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

BoxedString* EmptyString;

llvm::StringRef BoxedString::sSlow() const {
    Py_ssize_t size;
    BoxedString* self = const_cast<BoxedString*>(this);
    const char* data = PyUnicode_AsUTF8AndSize(self, &size);
    if (data)
        return llvm::StringRef(data, size);

    // Strings with lone surrogates can't be encoded as strict UTF-8.  Use surrogatepass (which is
    // what boxString() decodes) and keep the result in the object's UTF-8 cache, so the StringRef
    // stays valid as long as the string.
    // TODO: with the cache filled, str.encode() no longer rejects the surrogates.
    PyErr_Clear();
    assert(!PyUnicode_IS_COMPACT_ASCII(self));
    Box* bytes = PyUnicode_AsEncodedString(self, "utf-8", "surrogatepass");
    RELEASE_ASSERT(bytes, "string can't be encoded as UTF-8");
    AUTO_DECREF(bytes);
    size = PyBytes_GET_SIZE(bytes);
    char* buf = (char*)PyObject_MALLOC(size + 1);
    RELEASE_ASSERT(buf, "");
    memcpy(buf, PyBytes_AS_STRING(bytes), size + 1);
    PyCompactUnicodeObject* compact = (PyCompactUnicodeObject*)self;
    compact->utf8 = buf;
    compact->utf8_length = size;
    return llvm::StringRef(buf, size);
}

BoxedString* BoxedString::createUninitializedString(ssize_t n) {
    BoxedString* r = (BoxedString*)PyUnicode_New(n, 127);
    if (!r)
        throwCAPIException();
    return r;
}

static bool isAscii(llvm::StringRef s) {
    for (char c : s) {
        if ((unsigned char)c >= 128)
            return false;
    }
    return true;
}

BoxedString* boxString(llvm::StringRef s) {
    if (s.size() == 0) {
        Py_INCREF(EmptyString);
        return EmptyString;
    }

    if (isAscii(s)) {
        BoxedString* r = BoxedString::createUninitializedString(s.size());
        memcpy(r->getWriteableStringContents(), s.data(), s.size());
        return r;
    }

    // The front end encodes lone surrogates in literals with surrogatepass, so decode them back the same way.
    Box* r = PyUnicode_DecodeUTF8(s.data(), s.size(), "surrogatepass");
    if (r)
        return (BoxedString*)r;
    PyErr_Clear();

    // Bytes that aren't valid UTF-8 get mapped to lone surrogates, so that this never fails.
    r = PyUnicode_DecodeUTF8(s.data(), s.size(), "surrogateescape");
    if (!r)
        throwCAPIException();
    return (BoxedString*)r;
}

extern "C" Box* decodeUTF8StringPtr(llvm::StringRef s) {
    Box* rtn = PyUnicode_DecodeUTF8(s.data(), s.size(), "strict");
    if (!rtn)
        throwCAPIException();
    return rtn;
}

// These have to produce the same values as str.__hash__, since the interning table looks
// strings up by their UTF-8 contents.
size_t strHashUnboxedStrRef(llvm::StringRef s) {
    if (isAscii(s))
        return _Py_HashBytes(s.data(), s.size());

    BoxedString* tmp = boxString(s);
    AUTO_DECREF(tmp);
    return strHashUnboxed(tmp);
}

extern "C" size_t strHashUnboxed(BoxedString* self) {
    assert(PyUnicode_Check(self));
    Py_hash_t r = PyObject_Hash(self);
    assert(r != -1);
    return r;
}

void setupStr() {
    EmptyString = (BoxedString*)PyUnicode_New(0, 0);
    PyGC_RegisterStaticConstant(EmptyString);
}

} // namespace pyston
