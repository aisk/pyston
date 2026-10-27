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

#ifndef PYSTON_CODEGEN_CODEGEN_H
#define PYSTON_CODEGEN_CODEGEN_H

#include <unordered_map>

#include "codegen/runtime_hooks.h"
#include "core/threading.h"
#include "core/types.h"

namespace llvm {
class ExecutionEngine;
class JITEventListener;
class LLVMContext;
class Module;
class StructType;
class TargetMachine;
class Type;
}

namespace pyston {

class PystonObjectCache;

class FunctionAddressRegistry {
private:
    struct FuncInfo {
        std::string name;
        int length;
        llvm::Function* llvm_func;
        FuncInfo(const std::string& name, int length, llvm::Function* llvm_func)
            : name(name), length(length), llvm_func(llvm_func) {}
    };
    typedef std::unordered_map<void*, FuncInfo> FuncMap;
    FuncMap functions;
    std::unordered_set<void*> lookup_neg_cache;

public:
    std::string getFuncNameAtAddress(void* addr, bool demangle, bool* out_success = NULL);
    llvm::Function* getLLVMFuncAtAddress(void* addr);
    void registerFunction(const std::string& name, void* addr, int length, llvm::Function* llvm_func);
    void deregisterFunction(void* addr) { functions.erase(addr); }
    void dumpPerfMap();
};

llvm::JITEventListener* makeRegistryListener();
llvm::JITEventListener* makeTracebacksListener();

llvm::LLVMContext& getGlobalLLVMContext();

struct GlobalState {
    // Much of this section is not thread-safe:
    llvm::LLVMContext& context;
    llvm::Module* stdlib_module;
    llvm::Module* cur_module;
    CompiledFunction* cur_cf;
    llvm::TargetMachine* tm;
    llvm::ExecutionEngine* engine;
    PystonObjectCache* object_cache;

    std::vector<llvm::JITEventListener*> jit_listeners;

    FunctionAddressRegistry func_addr_registry;
    // LLVM pointers are opaque, so all of the *_ptr types are the same "ptr" type.  They are kept
    // separate to document what the values point to.
    llvm::Type* llvm_value_type_ptr, *llvm_value_type_ptr_ptr;
    llvm::Type* llvm_class_type_ptr;
    llvm::Type* llvm_boxedstring_type_ptr, *llvm_dict_type_ptr;
    llvm::Type* llvm_code_type_ptr, *llvm_closure_type_ptr, *llvm_generator_type_ptr;
    llvm::Type* llvm_module_type_ptr, *llvm_bool_type_ptr;
    // Struct types that mirror the C++ layout of FrameInfo and ExcInfo, checked in initGlobalFuncs.
    llvm::StructType* llvm_frame_info_type;
    llvm::StructType* llvm_excinfo_type;
    llvm::Type* i1, *i8, *i8_ptr, *i32, *i64, *void_, *double_;
    llvm::Type* vector_ptr;

    GlobalFuncs funcs;

    GlobalState();
};

extern GlobalState g;

// in runtime_hooks.cpp:
void initGlobalFuncs(GlobalState& g);
}

#endif
