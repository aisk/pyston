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

#ifndef PYSTON_CODEGEN_CPYTHONAST_H
#define PYSTON_CODEGEN_CPYTHONAST_H

#include "Python.h"
#include "Python-ast.h"

// Python-ast.h defines a function-like macro for every AST constructor (Str,
// Tuple, ...), which breaks LLVM headers that use the same names.  We only read
// CPython ASTs, so drop them; the _Py_ prefixed functions are still available.
#undef Module
#undef Interactive
#undef Expression
#undef Suite
#undef FunctionDef
#undef ClassDef
#undef Return
#undef Delete
#undef Assign
#undef AugAssign
#undef Print
#undef For
#undef While
#undef If
#undef With
#undef Raise
#undef TryExcept
#undef TryFinally
#undef Assert
#undef Import
#undef ImportFrom
#undef Exec
#undef Global
#undef Expr
#undef Pass
#undef Break
#undef Continue
#undef BoolOp
#undef BinOp
#undef UnaryOp
#undef Lambda
#undef IfExp
#undef Dict
#undef Set
#undef ListComp
#undef SetComp
#undef DictComp
#undef GeneratorExp
#undef Yield
#undef Compare
#undef Call
#undef Repr
#undef Num
#undef Str
#undef Attribute
#undef Subscript
#undef Name
#undef List
#undef Tuple
#undef Ellipsis
#undef Slice
#undef ExtSlice
#undef Index
#undef ExceptHandler

#include "core/ast.h"

namespace pyston {

// Convert a CPython ast object to a Pyston ast object.
// This will also check for certain kinds of "syntax errors" (ex continue not in loop) and will
// throw them as C++ exceptions.
std::pair<AST*, std::unique_ptr<ASTAllocator>> cpythonToPystonAST(mod_ty mod, llvm::StringRef fn);
}

#endif
