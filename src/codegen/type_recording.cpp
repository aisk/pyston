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

#include "codegen/type_recording.h"

#include "llvm/ADT/DenseMap.h"

#include "asm_writing/icinfo.h"
#include "core/options.h"
#include "core/types.h"

namespace pyston {

Box* recordType(TypeRecorder* self, Box* obj) {
    // The baseline JIT directly generates machine code for this function inside JitFragmentWriter::_emitRecordType.
    // When changing this function one has to also change the bjit code.

    if (!obj) {
        assert(PyErr_Occurred());
        return obj;
    }

    BoxedClass* cls = obj->cls;
    if (cls != self->last_seen) {
        self->last_seen = cls;
        self->last_count = 1;
        self->num_changes++;
    } else {
        self->last_count++;
    }

    // printf("Seen %s %ld times\n", getNameOfClass(cls)->c_str(), self->last_count);

    return obj;
}

static llvm::DenseMap<BST_stmt*, NodeTypeRecorder*> recorders_by_node;
static llvm::DenseMap<BST_stmt*, TypeRecorder> retired_recorders;

void retireTypeRecorder(BST_stmt* node, const TypeRecorder& recorder) {
    retired_recorders[node] = recorder;
}

NodeTypeRecorder::NodeTypeRecorder(BST_stmt* node) : node(node) {
    recorders_by_node[node] = this;
}

NodeTypeRecorder::~NodeTypeRecorder() {
    auto it = recorders_by_node.find(node);
    if (it != recorders_by_node.end() && it->second == this) {
        recorders_by_node.erase(it);
        retireTypeRecorder(node, *this);
    }
}

BoxedClass* predictClassFor(BST_stmt* node, int64_t monomorphic_threshold) {
    ICInfo* ic = ICInfo::getICInfoForNode(node);
    if (ic && ic->getTypeRecorder())
        return ic->getTypeRecorder()->predict(monomorphic_threshold);

    auto it = recorders_by_node.find(node);
    if (it != recorders_by_node.end())
        return it->second->predict(monomorphic_threshold);

    auto retired = retired_recorders.find(node);
    if (retired != retired_recorders.end())
        return retired->second.predict(monomorphic_threshold);
    return NULL;
}

BoxedClass* TypeRecorder::predict(int64_t monomorphic_threshold) {
    if (!ENABLE_TYPE_FEEDBACK)
        return NULL;

    if (last_count > SPECULATION_THRESHOLD)
        return last_seen;

    // Code in an outer loop can get compiled after only a few runs, when the inner loop triggers OSR.
    if (num_changes == 1 && last_count >= monomorphic_threshold)
        return last_seen;

    return NULL;
}
}
