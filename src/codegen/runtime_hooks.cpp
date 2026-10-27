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

#include "codegen/runtime_hooks.h"

#include <cstdio>
#include <unordered_map>

#include "llvm/Analysis/Passes.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Transforms/Scalar.h"

#include "codegen/ast_interpreter.h"
#include "codegen/codegen.h"
#include "codegen/irgen.h"
#include "codegen/irgen/hooks.h"
#include "codegen/irgen/util.h"
#include "core/threading.h"
#include "core/types.h"
#include "runtime/complex.h"
#include "runtime/float.h"
#include "runtime/generator.h"
#include "runtime/import.h"
#include "runtime/inline/boxing.h"
#include "runtime/inline/list.h"
#include "runtime/int.h"
#include "runtime/long.h"
#include "runtime/objmodel.h"
#include "runtime/set.h"
#include "runtime/types.h"
#include "runtime/util.h"

extern "C" void* __cxa_begin_catch(void*);
extern "C" void __cxa_end_catch();

namespace pyston {

static llvm::Function* lookupFunction(const std::string& name) {
    llvm::Function* r = g.stdlib_module->getFunction(name);
    ASSERT(r, "Couldn't find '%s'", name.c_str());
    return r;
}

static llvm::FunctionCallee getFunc(void* func, const char* name) {
    llvm::Function* f = lookupFunction(name);
    ASSERT(f, "%s", name);
    g.func_addr_registry.registerFunction(name, func, 0, f);
    llvm::Constant* callee = embedConstantPtr(func, g.i8_ptr);
    registerRuntimeFunction(callee, f);
    return llvm::FunctionCallee(f->getFunctionType(), callee);
}

static llvm::FunctionCallee addFunc(void* func, llvm::Type* rtn_type, llvm::ArrayRef<llvm::Type*> arg_types,
                                    bool varargs = false) {
    llvm::FunctionType* ft = llvm::FunctionType::get(rtn_type, arg_types, varargs);
    return llvm::FunctionCallee(ft, embedConstantPtr(func, g.i8_ptr));
}

static llvm::FunctionCallee addFunc(void* func, llvm::Type* rtn_type, bool varargs = false) {
    return addFunc(func, rtn_type, llvm::ArrayRef<llvm::Type*>(), varargs);
}

static llvm::FunctionCallee addFunc(void* func, llvm::Type* rtn_type, llvm::Type* arg1, bool varargs = false) {
    llvm::Type* array[] = { arg1 };
    return addFunc(func, rtn_type, array, varargs);
}

static llvm::FunctionCallee addFunc(void* func, llvm::Type* rtn_type, llvm::Type* arg1, llvm::Type* arg2,
                            bool varargs = false) {
    llvm::Type* array[] = { arg1, arg2 };
    return addFunc(func, rtn_type, array, varargs);
}

static llvm::FunctionCallee addFunc(void* func, llvm::Type* rtn_type, llvm::Type* arg1, llvm::Type* arg2, llvm::Type* arg3,
                            bool varargs = false) {
    llvm::Type* array[] = { arg1, arg2, arg3 };
    return addFunc(func, rtn_type, array, varargs);
}

static llvm::FunctionCallee addFunc(void* func, llvm::Type* rtn_type, llvm::Type* arg1, llvm::Type* arg2, llvm::Type* arg3,
                                    llvm::Type* arg4, bool varargs = false) {
    llvm::Type* array[] = { arg1, arg2, arg3, arg4 };
    return addFunc(func, rtn_type, array, varargs);
}

static llvm::FunctionCallee addFunc(void* func, llvm::Type* rtn_type, llvm::Type* arg1, llvm::Type* arg2, llvm::Type* arg3,
                                    llvm::Type* arg4, llvm::Type* arg5, bool varargs = false) {
    llvm::Type* array[] = { arg1, arg2, arg3, arg4, arg5 };
    return addFunc(func, rtn_type, array, varargs);
}

static llvm::FunctionCallee addFunc(void* func, llvm::Type* rtn_type, llvm::Type* arg1, llvm::Type* arg2, llvm::Type* arg3,
                                    llvm::Type* arg4, llvm::Type* arg5, llvm::Type* arg6, bool varargs = false) {
    llvm::Type* array[] = { arg1, arg2, arg3, arg4, arg5, arg6 };
    return addFunc(func, rtn_type, array, varargs);
}

static llvm::FunctionCallee addFunc(void* func, llvm::Type* rtn_type, llvm::Type* arg1, llvm::Type* arg2, llvm::Type* arg3,
                                    llvm::Type* arg4, llvm::Type* arg5, llvm::Type* arg6, llvm::Type* arg7,
                            bool varargs = false) {
    llvm::Type* array[] = { arg1, arg2, arg3, arg4, arg5, arg6, arg7 };
    return addFunc(func, rtn_type, array, varargs);
}

// Builds a struct type with the given fields, and checks that LLVM lays it out with the given offsets.
static llvm::StructType* createCheckedStructType(const char* name, llvm::ArrayRef<llvm::Type*> fields,
                                                 llvm::ArrayRef<size_t> offsets, size_t size) {
    llvm::StructType* t = llvm::StructType::create(g.context, fields, name);
    const llvm::StructLayout* layout = g.tm->createDataLayout().getStructLayout(t);
    RELEASE_ASSERT(layout->getSizeInBytes() == size, "%s", name);
    for (int i = 0; i < offsets.size(); i++) {
        RELEASE_ASSERT(layout->getElementOffset(i) == offsets[i], "%s field %d", name, i);
    }
    return t;
}

void initGlobalFuncs(GlobalState& g) {
    llvm::Type* ptr = llvm::PointerType::getUnqual(g.context);

    g.llvm_value_type_ptr = ptr;
    g.llvm_value_type_ptr_ptr = ptr;
    g.llvm_class_type_ptr = ptr;
    g.llvm_boxedstring_type_ptr = ptr;
    g.llvm_dict_type_ptr = ptr;
    g.llvm_code_type_ptr = ptr;
    g.llvm_closure_type_ptr = ptr;
    g.llvm_generator_type_ptr = ptr;
    g.llvm_module_type_ptr = ptr;
    g.llvm_bool_type_ptr = ptr;
    g.vector_ptr = ptr;

    g.llvm_excinfo_type = createCheckedStructType(
        "ExcInfo", { ptr, ptr, ptr }, { offsetof(ExcInfo, type), offsetof(ExcInfo, value), offsetof(ExcInfo, traceback) },
        sizeof(ExcInfo));

    // The field indices are used by the getXxxGEP() functions in irgenerator.cpp
    g.llvm_frame_info_type = createCheckedStructType(
        "FrameInfo", { g.llvm_excinfo_type, ptr, ptr, ptr, ptr, g.i32, g.i32, ptr, ptr, ptr },
        { offsetof(FrameInfo, exc), offsetof(FrameInfo, boxedLocals), offsetof(FrameInfo, frame_obj),
          offsetof(FrameInfo, passed_closure), offsetof(FrameInfo, vregs), offsetof(FrameInfo, num_vregs),
          offsetof(FrameInfo, stmt_offset), offsetof(FrameInfo, globals), offsetof(FrameInfo, back),
          offsetof(FrameInfo, code) },
        sizeof(FrameInfo));

#define GET(N) g.funcs.N = getFunc((void*)N, STRINGIFY(N))

    g.funcs.printf = addFunc((void*)printf, g.i8_ptr, true);
    g.funcs.my_assert = getFunc((void*)my_assert, "my_assert");
    g.funcs.malloc = addFunc((void*)malloc, g.i8_ptr, g.i64);
    g.funcs.free = addFunc((void*)free, g.void_, g.i8_ptr);

    g.funcs.allowGLReadPreemption = getFunc((void*)threading::allowGLReadPreemption, "allowGLReadPreemption");

    GET(createFunctionFromMetadata);
    GET(createUserClass);
    GET(boxInt);
    GET(unboxInt);
    GET(boxFloat);
    GET(unboxFloat);
    GET(boxInstanceMethod);
    GET(boxBool);
    GET(unboxBool);
    GET(createTuple);
    GET(createList);
    GET(createDict);
    GET(createSlice);
    GET(createClosure);
    GET(createGenerator);
    GET(createSet);
    GET(initFrame);
    GET(deinitFrame);
    GET(deinitFrameMaybe);
    GET(makePendingCalls);
    GET(setFrameExcInfo);

    GET(getattr);
    GET(getattr_capi);
    GET(setattr);
    GET(delattr);
    GET(getitem);
    GET(getitem_capi);
    GET(setitem);
    GET(delitem);
    GET(getGlobal);
    GET(delGlobal);
    GET(setGlobal);
    GET(binop);
    GET(compare);
    GET(augbinop);
    GET(nonzero);
    GET(unboxedLen);
    GET(getclsattr);
    GET(unaryop);
    GET(import);
    GET(importFrom);
    GET(importStar);
    GET(repr);
    GET(exceptionMatches);
    GET(yield_capi);
    GET(getiterHelper);
    GET(hasnext);
    GET(apply_slice);
    GET(applySlice);
    GET(assignSlice);

    GET(unpackIntoArray);
    GET(raiseAttributeError);
    GET(raiseAttributeErrorStr);
    GET(raiseAttributeErrorCapi);
    GET(raiseAttributeErrorStrCapi);
    GET(raiseIndexErrorStr);
    GET(raiseIndexErrorStrCapi);
    GET(raiseNotIterableError);
    GET(assertNameDefined);
    GET(assertFailDerefNameDefined);
    GET(assertFail);
    GET(printExprHelper);
    GET(printHelper);

    GET(listAppendInternal);

    GET(exec);
    GET(boxedLocalsSet);
    GET(boxedLocalsGet);
    GET(boxedLocalsDel);

    g.funcs.runtimeCall.cxx_val = getFunc((void*)runtimeCall, "runtimeCall");
    g.funcs.runtimeCall0.cxx_val = addFunc((void*)runtimeCall, g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.i32);
    g.funcs.runtimeCall1.cxx_val
        = addFunc((void*)runtimeCall, g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.i32, g.llvm_value_type_ptr);
    g.funcs.runtimeCall2.cxx_val = addFunc((void*)runtimeCall, g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.i32,
                                           g.llvm_value_type_ptr, g.llvm_value_type_ptr);
    g.funcs.runtimeCall3.cxx_val = addFunc((void*)runtimeCall, g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.i32,
                                           g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.llvm_value_type_ptr);
    g.funcs.runtimeCallN.cxx_val
        = addFunc((void*)runtimeCall, g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.i32, g.llvm_value_type_ptr,
                  g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.llvm_value_type_ptr_ptr);

    g.funcs.runtimeCall.capi_val = getFunc((void*)runtimeCallCapi, "runtimeCallCapi");
    g.funcs.runtimeCall0.capi_val
        = addFunc((void*)runtimeCallCapi, g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.i32);
    g.funcs.runtimeCall1.capi_val
        = addFunc((void*)runtimeCallCapi, g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.i32, g.llvm_value_type_ptr);
    g.funcs.runtimeCall2.capi_val = addFunc((void*)runtimeCallCapi, g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.i32,
                                            g.llvm_value_type_ptr, g.llvm_value_type_ptr);
    g.funcs.runtimeCall3.capi_val = addFunc((void*)runtimeCallCapi, g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.i32,
                                            g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.llvm_value_type_ptr);
    g.funcs.runtimeCallN.capi_val
        = addFunc((void*)runtimeCallCapi, g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.i32, g.llvm_value_type_ptr,
                  g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.llvm_value_type_ptr_ptr);


    g.funcs.callattr.cxx_val = getFunc((void*)callattr, "callattr");
    g.funcs.callattr0.cxx_val
        = addFunc((void*)callattr, g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.llvm_boxedstring_type_ptr, g.i64);
    g.funcs.callattr1.cxx_val = addFunc((void*)callattr, g.llvm_value_type_ptr, g.llvm_value_type_ptr,
                                        g.llvm_boxedstring_type_ptr, g.i64, g.llvm_value_type_ptr);
    g.funcs.callattr2.cxx_val
        = addFunc((void*)callattr, g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.llvm_boxedstring_type_ptr, g.i64,
                  g.llvm_value_type_ptr, g.llvm_value_type_ptr);
    g.funcs.callattr3.cxx_val
        = addFunc((void*)callattr, g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.llvm_boxedstring_type_ptr, g.i64,
                  g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.llvm_value_type_ptr);
    g.funcs.callattrN.cxx_val = addFunc(
        (void*)callattr, g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.llvm_boxedstring_type_ptr, g.i64,
        g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.llvm_value_type_ptr_ptr);


    g.funcs.callattr.capi_val = getFunc((void*)callattrCapi, "callattrCapi");
    g.funcs.callattr0.capi_val = addFunc((void*)callattrCapi, g.llvm_value_type_ptr, g.llvm_value_type_ptr,
                                         g.llvm_boxedstring_type_ptr, g.i64);
    g.funcs.callattr1.capi_val = addFunc((void*)callattrCapi, g.llvm_value_type_ptr, g.llvm_value_type_ptr,
                                         g.llvm_boxedstring_type_ptr, g.i64, g.llvm_value_type_ptr);
    g.funcs.callattr2.capi_val
        = addFunc((void*)callattrCapi, g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.llvm_boxedstring_type_ptr, g.i64,
                  g.llvm_value_type_ptr, g.llvm_value_type_ptr);
    g.funcs.callattr3.capi_val
        = addFunc((void*)callattrCapi, g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.llvm_boxedstring_type_ptr, g.i64,
                  g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.llvm_value_type_ptr);
    g.funcs.callattrN.capi_val = addFunc(
        (void*)callattrCapi, g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.llvm_boxedstring_type_ptr, g.i64,
        g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.llvm_value_type_ptr_ptr);

    g.funcs.reoptCompiledFunc = addFunc((void*)reoptCompiledFunc, g.i8_ptr, g.i8_ptr);
    g.funcs.compilePartialFunc = addFunc((void*)compilePartialFunc, g.i8_ptr, g.i8_ptr);

    g.funcs.__cxa_end_catch = addFunc((void*)__cxa_end_catch, g.void_);
    GET(raise0);
    GET(raise0_capi);
    GET(raise3);
    GET(raise3_capi);
    GET(rawReraise);
    GET(PyErr_Fetch);
    GET(PyErr_NormalizeException);
    GET(PyErr_Restore);
    GET(caughtCapiException);
    GET(reraiseCapiExcAsCxx);
    GET(deopt);
    GET(checkRefs);
    GET(xdecrefAndRethrow);

    GET(div_float_float);
    GET(floordiv_float_float);
    GET(mod_float_float);
    GET(pow_float_float);

    GET(dump);

#ifdef Py_TRACE_REFS
    GET(_Py_Dealloc);
#endif
}
}
