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

#ifndef PYSTON_CODEGEN_RUNTIMEHOOKS_H
#define PYSTON_CODEGEN_RUNTIMEHOOKS_H

// This file doesn't actually need to include core/types.h, but including it works around this clang bug:
// http://lists.cs.uiuc.edu/pipermail/cfe-dev/2014-June/037519.html
#include "core/types.h"

#include "llvm/IR/DerivedTypes.h"

namespace pyston {

struct GlobalFuncs {
    llvm::FunctionCallee allowGLReadPreemption;

    llvm::FunctionCallee softspace;

    llvm::FunctionCallee printf, my_assert, malloc, free;

    llvm::FunctionCallee boxInt, unboxInt, boxFloat, unboxFloat, createFunctionFromMetadata, boxInstanceMethod, boxBool,
        unboxBool, createTuple, createDict, createList, createSlice, createUserClass, createClassNamespace, createClosure,
        createGenerator, createSet, initFrame, deinitFrame, deinitFrameMaybe, makePendingCalls, setFrameExcInfo;
    llvm::FunctionCallee getattr, getattr_capi, setattr, delattr, delitem, delGlobal, nonzero, binop, compare,
        augbinop, unboxedLen, getitem, getitem_capi, getclsattr, getGlobal, setitem, unaryop, import,
        importFrom, importStar, repr, exceptionMatches, yield_capi, getiterHelper, hasnext, setGlobal,
        apply_slice, applySlice, assignSlice;

    llvm::FunctionCallee unpackIntoArray, raiseAttributeError, raiseAttributeErrorStr, raiseAttributeErrorCapi,
        raiseAttributeErrorStrCapi, raiseNotIterableError, raiseIndexErrorStr, raiseIndexErrorStrCapi,
        assertNameDefined, assertFail, assertFailDerefNameDefined, printExprHelper, printHelper, delClosureVar;
    llvm::FunctionCallee listAppendInternal, dictSetInternal;
    ExceptionSwitchable<llvm::FunctionCallee> runtimeCall0, runtimeCall1, runtimeCall2, runtimeCall3, runtimeCall, runtimeCallN;
    ExceptionSwitchable<llvm::FunctionCallee> callattr0, callattr1, callattr2, callattr3, callattr, callattrN;
    llvm::FunctionCallee reoptCompiledFunc, compilePartialFunc;
    llvm::FunctionCallee exec;
    llvm::FunctionCallee boxedLocalsSet, boxedLocalsGet, boxedLocalsDel;

    llvm::FunctionCallee __cxa_end_catch;
    llvm::FunctionCallee raise0, raise0_capi, raise3, raise3_capi, rawReraise;
    llvm::FunctionCallee PyErr_Fetch, PyErr_NormalizeException, PyErr_Restore, caughtCapiException, reraiseCapiExcAsCxx;
    llvm::FunctionCallee deopt;
    llvm::FunctionCallee checkRefs, xdecrefAndRethrow;

    llvm::FunctionCallee div_float_float, floordiv_float_float, mod_float_float;

    llvm::FunctionCallee dump;

#ifdef Py_TRACE_REFS
    llvm::FunctionCallee _Py_Dealloc;
#endif
};
}

#endif
