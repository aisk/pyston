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

#include "codegen/parser.h"

#include "codegen/cpython_ast.h"

#include "core/ast.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/types.h"
#include "core/util.h"
#include "runtime/types.h"

namespace pyston {

// Python code is parsed with CPython's parser into a CPython AST, which then gets converted into
// Pyston's AST.  Pyston used to cache its own serialized ASTs in .pyc files; that went away with
// the Python 3 port.

static PyCompilerFlags makeCompilerFlags(FutureFlags inherited_flags) {
    PyCompilerFlags cf = _PyCompilerFlags_INIT;
    cf.cf_flags = inherited_flags & PyCF_MASK;
    return cf;
}

std::pair<AST_Module*, std::unique_ptr<ASTAllocator>> parse_string(const char* code, FutureFlags inherited_flags) {
    PyCompilerFlags cf = makeCompilerFlags(inherited_flags);
    ArenaWrapper arena;
    assert(arena);
    const char* fn = "<string>";
    mod_ty mod = PyParser_ASTFromString(code, fn, Py_file_input, &cf, arena);
    if (!mod)
        throwCAPIException();
    assert(mod->kind != Interactive_kind);
    auto t = cpythonToPystonAST(mod, fn);
    return std::make_pair((AST_Module*)t.first, std::move(t.second));
}

std::pair<AST_Module*, std::unique_ptr<ASTAllocator>> parse_file(const char* fn, FutureFlags inherited_flags) {
    UNAVOIDABLE_STAT_TIMER(t0, "us_timer_parse_file");
    static StatCounter us_parsing("us_parsing");
    Timer _t("parsing");
    _t.setExitCallback([](uint64_t t) { us_parsing.log(t); });

    FileHandle fp(fn, "rb");
    if (!fp) {
        PyErr_SetFromErrnoWithFilename(PyExc_OSError, fn);
        throwCAPIException();
    }
    PyCompilerFlags cf = makeCompilerFlags(inherited_flags);
    ArenaWrapper arena;
    assert(arena);
    mod_ty mod = PyParser_ASTFromFile(fp, fn, NULL, Py_file_input, NULL, NULL, &cf, NULL, arena);
    if (!mod)
        throwCAPIException();
    assert(mod->kind != Interactive_kind);
    auto t = cpythonToPystonAST(mod, fn);
    return std::make_pair((AST_Module*)t.first, std::move(t.second));
}
}
