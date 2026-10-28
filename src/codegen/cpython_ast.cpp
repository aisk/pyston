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

#include "codegen/cpython_ast.h"

#include <cstdarg>
#include <set>

#include "llvm/ADT/STLExtras.h"

#include "core/types.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

#if 0
#Helpful script:
with open("Include/Python-ast.h") as f:
    lines = f.readlines()

#type, lines = "stmt", lines[72 : 181]
#type, lines = "expr", lines[196 : 309]
type, lines = "slice", lines[319:332]

names = []
while lines:
    l = lines.pop(0).strip()
    if l.startswith("struct {"):
        continue
    elif l.startswith("}"):
        name = l[2:-1]
        print "case %s_kind: {" % name
        print "auto r = new (getAlloc()) AST_%s();" % name
        print "auto v = %s->v.%s;" % (type, name)
        for n in names:
            print "r->%s = convert(v.%s);" % (n, n)
        print "return r;"
        print "}"
        names = []
        continue
    elif l.endswith(';'):
        n = l.split()[1][:-1]
        if n.startswith('*'):
            n = n[1:]
        names.append(n)
#endif

class Converter {
private:
    InternedStringPool* pool = NULL;
    int loop_depth = 0;
    int in_finally = 0;
    int interactive = 0;
    int nestlevel = 0;
    llvm::StringRef fn;
    std::unique_ptr<ASTAllocator> ast_allocator;

    ASTAllocator& getAlloc() { return *ast_allocator; }

    // Emits a SyntaxWarning like CPython's compiler_warn(); an error filter turns it into a SyntaxError.
    void warn(int lineno, int col_offset, const char* fmt, ...) {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);

        PyObject* filename = PyUnicode_DecodeFSDefaultAndSize(fn.data(), fn.size());
        if (!filename)
            throwCAPIException();
        AUTO_DECREF(filename);
        PyObject* msg = PyUnicode_FromString(buf);
        if (!msg)
            throwCAPIException();
        AUTO_DECREF(msg);
        if (PyErr_WarnExplicitObject(PyExc_SyntaxWarning, msg, filename, lineno, NULL, NULL) < 0) {
            if (PyErr_ExceptionMatches(PyExc_SyntaxWarning)) {
                PyErr_Clear();
                raiseSyntaxError(buf, lineno, col_offset, fn, "", true);
            }
            throwCAPIException();
        }
    }

    // The type of a literal expression, or NULL (CPython's infer_type()).
    static const char* inferTypeName(expr_ty e) {
        switch (e->kind) {
            case Tuple_kind:
                return "tuple";
            case List_kind:
            case ListComp_kind:
                return "list";
            case Dict_kind:
            case DictComp_kind:
                return "dict";
            case Set_kind:
            case SetComp_kind:
                return "set";
            case GeneratorExp_kind:
                return "generator";
            case Lambda_kind:
                return "function";
            case JoinedStr_kind:
            case FormattedValue_kind:
                return "str";
            case Constant_kind:
                return PyLong_Check(e->v.Constant.value) && !PyBool_Check(e->v.Constant.value)
                           ? "int"
                           : Py_TYPE(e->v.Constant.value)->tp_name;
            default:
                return NULL;
        }
    }

    static bool isNamedSingletonOrNotConstant(expr_ty e) {
        if (e->kind != Constant_kind)
            return true;
        PyObject* value = e->v.Constant.value;
        return value == Py_None || value == Py_False || value == Py_True || value == Py_Ellipsis;
    }

    void checkCompare(expr_ty e) {
        bool left = isNamedSingletonOrNotConstant(e->v.Compare.left);
        for (int i = 0; i < asdl_seq_LEN(e->v.Compare.ops); i++) {
            cmpop_ty op = (cmpop_ty)asdl_seq_GET(e->v.Compare.ops, i);
            bool right = isNamedSingletonOrNotConstant((expr_ty)asdl_seq_GET(e->v.Compare.comparators, i));
            if ((op == Is || op == IsNot) && (!left || !right)) {
                warn(e->lineno, e->col_offset, op == Is ? "\"is\" with a literal. Did you mean \"==\"?"
                                                        : "\"is not\" with a literal. Did you mean \"!=\"?");
                return;
            }
            left = right;
        }
    }

    void checkCaller(expr_ty e) {
        switch (e->kind) {
            case Constant_kind:
            case Tuple_kind:
            case List_kind:
            case ListComp_kind:
            case Dict_kind:
            case DictComp_kind:
            case Set_kind:
            case SetComp_kind:
            case GeneratorExp_kind:
            case JoinedStr_kind:
            case FormattedValue_kind:
                warn(e->lineno, e->col_offset, "'%.200s' object is not callable; perhaps you missed a comma?",
                     inferTypeName(e));
            default:
                break;
        }
    }

    void checkSubscript(expr_ty value, slice_ty slice) {
        switch (value->kind) {
            case Constant_kind: {
                PyObject* v = value->v.Constant.value;
                if (!(v == Py_None || v == Py_Ellipsis || PyLong_Check(v) || PyFloat_Check(v) || PyComplex_Check(v)
                      || PyAnySet_Check(v)))
                    break;
            }
            // fallthrough
            case Set_kind:
            case SetComp_kind:
            case GeneratorExp_kind:
            case Lambda_kind:
                warn(value->lineno, value->col_offset,
                     "'%.200s' object is not subscriptable; perhaps you missed a comma?", inferTypeName(value));
                return;
            default:
                break;
        }

        if (slice->kind != Index_kind)
            return;
        expr_ty index = slice->v.Index.value;
        const char* index_type = inferTypeName(index);
        if (!index_type || (index->kind == Constant_kind && PyLong_Check(index->v.Constant.value)))
            return;
        switch (value->kind) {
            case Constant_kind: {
                PyObject* v = value->v.Constant.value;
                if (!(PyUnicode_Check(v) || PyBytes_Check(v) || PyTuple_Check(v)))
                    break;
            }
            // fallthrough
            case Tuple_kind:
            case List_kind:
            case ListComp_kind:
            case JoinedStr_kind:
            case FormattedValue_kind:
                warn(value->lineno, value->col_offset,
                     "%.200s indices must be integers or slices, not %.200s; perhaps you missed a comma?",
                     inferTypeName(value), index_type);
            default:
                break;
        }
    }

public:
    Converter(llvm::StringRef fn) : fn(fn) {}

    template <typename T, typename P> std::vector<P> convert(asdl_seq* seq) {
        std::vector<P> rtn;
        if (!seq)
            return rtn;
        for (int i = 0; i < seq->size; i++) {
            auto converted = convert((T)seq->elements[i]);
            if constexpr (std::is_same<T, stmt_ty>::value) {
                for (AST_stmt* extra : stmts_before)
                    rtn.push_back(extra);
                stmts_before.clear();
            }
            rtn.push_back(converted);
            if constexpr (std::is_same<T, stmt_ty>::value) {
                for (AST_stmt* extra : stmts_after)
                    rtn.push_back(extra);
                stmts_after.clear();
            }
        }
        return rtn;
    }

    // Statements that a converted statement expands to, emitted right before and after it.
    std::vector<AST_stmt*> stmts_before, stmts_after;
    int num_temp_names = 0;

    // What kind of scope the statements being converted are in, for annotations.
    enum class ScopeKind { Module, Class, Function };
    std::vector<ScopeKind> scope_kinds{ ScopeKind::Module };
    // Name of the innermost enclosing class, which private names in annotation keys get mangled with.
    std::vector<PyObject*> class_names{ NULL };

    AST_Str* makeMangledStr(PyObject* name) {
        PyObject* mangled = _Py_Mangle(class_names.back(), name);
        if (!mangled)
            throwCAPIException();
        AUTO_DECREF(mangled);
        return new (getAlloc()) AST_Str(PyUnicode_AsUTF8(mangled));
    }

    // Whether a module or class body has annotated assignments, which means it needs an
    // `__annotations__` dict (SETUP_ANNOTATIONS in CPython).
    static bool hasAnnotations(asdl_seq* body) {
        for (int i = 0; i < asdl_seq_LEN(body); i++) {
            stmt_ty st = (stmt_ty)asdl_seq_GET(body, i);
            switch (st->kind) {
                case AnnAssign_kind:
                    return true;
                case For_kind:
                    if (hasAnnotations(st->v.For.body) || hasAnnotations(st->v.For.orelse))
                        return true;
                    break;
                case While_kind:
                    if (hasAnnotations(st->v.While.body) || hasAnnotations(st->v.While.orelse))
                        return true;
                    break;
                case If_kind:
                    if (hasAnnotations(st->v.If.body) || hasAnnotations(st->v.If.orelse))
                        return true;
                    break;
                case With_kind:
                    if (hasAnnotations(st->v.With.body))
                        return true;
                    break;
                case Try_kind:
                    if (hasAnnotations(st->v.Try.body) || hasAnnotations(st->v.Try.orelse)
                        || hasAnnotations(st->v.Try.finalbody))
                        return true;
                    for (int j = 0; j < asdl_seq_LEN(st->v.Try.handlers); j++) {
                        excepthandler_ty h = (excepthandler_ty)asdl_seq_GET(st->v.Try.handlers, j);
                        if (hasAnnotations(h->v.ExceptHandler.body))
                            return true;
                    }
                    break;
                default:
                    break;
            }
        }
        return false;
    }

    AST_Name* makeName(const char* name, AST_TYPE::AST_TYPE ctx, int lineno) {
        return new (getAlloc()) AST_Name(pool->get(name), ctx, lineno);
    }

    AST_Assign* makeAssign(AST_expr* target, AST_expr* value, int lineno) {
        auto r = new (getAlloc()) AST_Assign();
        r->targets.push_back(target);
        r->value = value;
        r->lineno = lineno;
        return r;
    }

    // Converts a module or class body, starting it with `__annotations__ = {}` if needed.
    std::vector<AST_stmt*> convertScopeBody(asdl_seq* body, ScopeKind kind, int lineno) {
        scope_kinds.push_back(kind);
        std::vector<AST_stmt*> rtn;
        if (kind != ScopeKind::Function && hasAnnotations(body)) {
            auto setup = new (getAlloc()) AST_LangPrimitive(AST_LangPrimitive::SETUP_ANNOTATIONS);
            setup->lineno = lineno;
            auto e = new (getAlloc()) AST_Expr(setup);
            e->lineno = lineno;
            rtn.push_back(e);
        }
        for (AST_stmt* st : convert<stmt_ty, AST_stmt*>(body))
            rtn.push_back(st);
        scope_kinds.pop_back();
        return rtn;
    }

    // Collects the annotations of a def into a dict display, or returns NULL if there are none.
    AST_Dict* convertFunctionAnnotations(arguments_ty args, expr_ty returns) {
        auto r = new (getAlloc()) AST_Dict();
        auto add = [&](arg_ty a) {
            if (a && a->annotation) {
                r->keys.push_back(makeMangledStr(a->arg));
                r->values.push_back(convert(a->annotation));
            }
        };
        for (auto seq : { args->posonlyargs, args->args }) {
            for (int i = 0; i < asdl_seq_LEN(seq); i++)
                add((arg_ty)asdl_seq_GET(seq, i));
        }
        add(args->vararg);
        for (int i = 0; i < asdl_seq_LEN(args->kwonlyargs); i++)
            add((arg_ty)asdl_seq_GET(args->kwonlyargs, i));
        add(args->kwarg);
        if (returns) {
            r->keys.push_back(new (getAlloc()) AST_Str("return"));
            r->values.push_back(convert(returns));
        }
        return r->keys.empty() ? NULL : r;
    }

    template <typename T, typename P> std::vector<P> convert(asdl_int_seq* seq) {
        std::vector<P> rtn;
        if (!seq)
            return rtn;
        for (int i = 0; i < seq->size; i++) {
            rtn.push_back(convert((T)seq->elements[i]));
        }
        return rtn;
    }

    template <typename T, typename P> void convertAll(asdl_seq* seq, std::vector<P>& vec) { vec = convert<T, P>(seq); }

    InternedString convert(identifier ident) {
        assert(pool);
        if (!ident)
            return pool->get("");
        return pool->get(static_cast<BoxedString*>(ident)->s());
    }

    AST_Name* convertToName(identifier ident) {
        if (!ident)
            return NULL;
        return new (getAlloc()) AST_Name(convert(ident), AST_TYPE::Store, -1, -1);
    }

    // Raises a SyntaxError for Python 3 features that Pyston can parse but doesn't support yet.
    void __attribute__((__noreturn__)) unsupported(const char* what, int lineno, int col_offset) {
        char buf[1024];
        snprintf(buf, sizeof(buf), "%s %s not supported by Pyston yet", what,
                 llvm::StringRef(what).ends_with("s") ? "are" : "is");
        raiseSyntaxError(buf, lineno, col_offset, fn, "", true);
        __builtin_unreachable();
    }

    AST_Name* convertArg(arg_ty arg) {
        return new (getAlloc()) AST_Name(convert(arg->arg), AST_TYPE::Param, arg->lineno, arg->col_offset);
    }

    AST_arguments* convert(arguments_ty ident, AST* parent) {
        auto r = new (getAlloc()) AST_arguments();

        r->num_posonly = asdl_seq_LEN(ident->posonlyargs);
        for (auto seq : { ident->posonlyargs, ident->args }) {
            for (int i = 0; i < asdl_seq_LEN(seq); i++)
                r->args.push_back(convertArg((arg_ty)asdl_seq_GET(seq, i)));
        }
        convertAll<expr_ty>(ident->defaults, r->defaults);
        for (int i = 0; i < asdl_seq_LEN(ident->kwonlyargs); i++) {
            r->kwonlyargs.push_back(convertArg((arg_ty)asdl_seq_GET(ident->kwonlyargs, i)));
            expr_ty d = (expr_ty)asdl_seq_GET(ident->kw_defaults, i);
            r->kw_defaults.push_back(d ? convert(d) : NULL);
        }
        if (ident->vararg)
            r->vararg = convertToName(ident->vararg->arg);
        if (ident->kwarg)
            r->kwarg = convertToName(ident->kwarg->arg);

        if (r->vararg && r->kwarg && (r->vararg->id == r->kwarg->id)) {
            char buf[1024];
            snprintf(buf, sizeof(buf), "duplicate argument '%s' in function definition", r->vararg->id.c_str());
            raiseSyntaxError(buf, parent->lineno, parent->col_offset, fn, "", true);
        }

        std::set<InternedString> seen;
        if (r->vararg) {
            seen.insert(r->vararg->id);
        }
        if (r->kwarg) {
            seen.insert(r->kwarg->id);
        }
        checkDuplicateArgs(parent, r->args, &seen);
        checkDuplicateArgs(parent, r->kwonlyargs, &seen);

        return r;
    }

    // Elements of a list, tuple or set display (or assignment target).  `*x` elements are stored
    // unwrapped, with a flag in `starred`.
    void convertStarredElts(asdl_seq* seq, std::vector<AST_expr*>& elts, std::vector<bool>& starred) {
        bool any = false;
        for (int i = 0; i < asdl_seq_LEN(seq); i++) {
            expr_ty e = (expr_ty)asdl_seq_GET(seq, i);
            bool is_starred = e->kind == Starred_kind;
            elts.push_back(convert(is_starred ? e->v.Starred.value : e));
            starred.push_back(is_starred);
            any = any || is_starred;
        }
        if (!any)
            starred.clear();
    }

    void checkDuplicateArgs(AST* parent, std::vector<AST_expr*> args, std::set<InternedString>* seen) {
        for (auto arg : args) {
            if (arg->type == AST_TYPE::Name) {
                auto name_node = static_cast<AST_Name*>(arg);
                if (seen->find(name_node->id) != seen->end()) {
                    char buf[1024];
                    snprintf(buf, sizeof(buf), "duplicate argument '%s' in function definition", name_node->id.c_str());
                    raiseSyntaxError(buf, parent->lineno, parent->col_offset, fn, "", true);
                }
                seen->insert(name_node->id);
            } else if (arg->type == AST_TYPE::Tuple) {
                auto slice_node = static_cast<AST_Tuple*>(arg);
                checkDuplicateArgs(parent, slice_node->elts, seen);
            } else {
                RELEASE_ASSERT(0, "");
            }
        }
    }

#define CASE(N)                                                                                                        \
    case N:                                                                                                            \
        return AST_TYPE::N

    AST_TYPE::AST_TYPE convert(expr_context_ty context) {
        switch (context) {
            CASE(Load);
            CASE(Store);
            CASE(Del);
            CASE(Param);
            default:
                RELEASE_ASSERT(0, "unhandled context type: %d", context);
        }
    }

    AST_TYPE::AST_TYPE convert(operator_ty op) {
        switch (op) {
            CASE(Add);
            CASE(Sub);
            CASE(Mult);
            // In Python 3, / is always true division.
            case Div:
                return AST_TYPE::TrueDiv;
            CASE(MatMult);
            CASE(Mod);
            CASE(Pow);
            CASE(LShift);
            CASE(RShift);
            CASE(BitOr);
            CASE(BitXor);
            CASE(BitAnd);
            CASE(FloorDiv);
        }
        // GCC wants this:
        RELEASE_ASSERT(0, "invalid operator: %d", op);
    }

    AST_TYPE::AST_TYPE convert(boolop_ty op) {
        switch (op) {
            CASE(And);
            CASE(Or);
        }
        // GCC wants this:
        RELEASE_ASSERT(0, "invalid operator: %d", op);
    }

    AST_TYPE::AST_TYPE convert(unaryop_ty op) {
        switch (op) {
            CASE(Invert);
            CASE(Not);
            CASE(UAdd);
            CASE(USub);
        }
        // GCC wants this:
        RELEASE_ASSERT(0, "invalid operator: %d", op);
    }

    AST_TYPE::AST_TYPE convert(cmpop_ty op) {
        switch (op) {
            CASE(Eq);
            CASE(NotEq);
            CASE(Lt);
            CASE(LtE);
            CASE(Gt);
            CASE(GtE);
            CASE(Is);
            CASE(IsNot);
            CASE(In);
            CASE(NotIn);
        }
        // GCC wants this:
        RELEASE_ASSERT(0, "invalid operator: %d", op);
    }
#undef CASE

    AST_keyword* convert(keyword_ty keyword) {
        auto r = new (getAlloc()) AST_keyword();
        r->arg = convert(keyword->arg);
        r->value = convert(keyword->value);
        return r;
    }

    AST_comprehension* convert(comprehension_ty comprehension) {
        auto r = new (getAlloc()) AST_comprehension();
        r->target = convert(comprehension->target);
        r->iter = convert(comprehension->iter);
        r->ifs = convert<expr_ty, AST_expr*>(comprehension->ifs);
        return r;
    }

    AST_slice* convert(slice_ty slice) {
        switch (slice->kind) {
            case Slice_kind: {
                auto r = new (getAlloc()) AST_Slice();
                auto v = slice->v.Slice;
                r->lower = convert(v.lower);
                r->upper = convert(v.upper);
                r->step = convert(v.step);
                return r;
            }
            case ExtSlice_kind: {
                auto r = new (getAlloc()) AST_ExtSlice();
                auto v = slice->v.ExtSlice;
                r->dims = convert<slice_ty, AST_slice*>(v.dims);
                return r;
            }
            case Index_kind: {
                auto r = new (getAlloc()) AST_Index();
                auto v = slice->v.Index;
                r->value = convert(v.value);
                return r;
            }
        }
        RELEASE_ASSERT(0, "invalid slice type: %d", slice->kind);
    }

    AST_expr* _convert(expr_ty expr) {
        switch (expr->kind) {
            case BoolOp_kind: {
                auto r = new (getAlloc()) AST_BoolOp();
                auto v = expr->v.BoolOp;
                r->op_type = convert(v.op);
                r->values = convert<expr_ty, AST_expr*>(v.values);
                return r;
            }
            case BinOp_kind: {
                auto r = new (getAlloc()) AST_BinOp();
                auto v = expr->v.BinOp;
                r->left = convert(v.left);
                r->op_type = convert(v.op);
                r->right = convert(v.right);
                return r;
            }
            case UnaryOp_kind: {
                auto r = new (getAlloc()) AST_UnaryOp();
                auto v = expr->v.UnaryOp;
                r->op_type = convert(v.op);
                r->operand = convert(v.operand);
                return r;
            }
            case Lambda_kind: {
                auto r = new (getAlloc()) AST_Lambda();
                r->lineno = expr->lineno;
                r->col_offset = expr->col_offset;
                auto v = expr->v.Lambda;
                r->args = convert(v.args, r);
                r->body = convert(v.body);
                return r;
            }
            case IfExp_kind: {
                auto r = new (getAlloc()) AST_IfExp();
                auto v = expr->v.IfExp;
                r->test = convert(v.test);
                r->body = convert(v.body);
                r->orelse = convert(v.orelse);
                return r;
            }
            case Dict_kind: {
                auto r = new (getAlloc()) AST_Dict();
                auto v = expr->v.Dict;
                // A NULL key means `**value`.
                r->keys = convert<expr_ty, AST_expr*>(v.keys);
                r->values = convert<expr_ty, AST_expr*>(v.values);
                return r;
            }
            case Set_kind: {
                auto r = new (getAlloc()) AST_Set();
                auto v = expr->v.Set;
                convertStarredElts(v.elts, r->elts, r->elt_starred);
                return r;
            }
            case ListComp_kind: {
                auto r = new (getAlloc()) AST_ListComp();
                auto v = expr->v.ListComp;
                r->elt = convert(v.elt);
                r->generators = convert<comprehension_ty, AST_comprehension*>(v.generators);
                return r;
            }
            case SetComp_kind: {
                auto r = new (getAlloc()) AST_SetComp();
                auto v = expr->v.SetComp;
                r->elt = convert(v.elt);
                r->generators = convert<comprehension_ty, AST_comprehension*>(v.generators);
                return r;
            }
            case DictComp_kind: {
                auto r = new (getAlloc()) AST_DictComp();
                auto v = expr->v.DictComp;
                r->key = convert(v.key);
                r->value = convert(v.value);
                r->generators = convert<comprehension_ty, AST_comprehension*>(v.generators);
                return r;
            }
            case GeneratorExp_kind: {
                auto r = new (getAlloc()) AST_GeneratorExp();
                auto v = expr->v.GeneratorExp;
                r->elt = convert(v.elt);
                r->generators = convert<comprehension_ty, AST_comprehension*>(v.generators);
                return r;
            }
            case Yield_kind: {
                auto r = new (getAlloc()) AST_Yield();
                auto v = expr->v.Yield;
                r->value = convert(v.value);
                return r;
            }
            case Compare_kind: {
                checkCompare(expr);
                auto r = new (getAlloc()) AST_Compare();
                auto v = expr->v.Compare;
                r->left = convert(v.left);
                r->ops = convert<cmpop_ty, AST_TYPE::AST_TYPE>(v.ops);
                r->comparators = convert<expr_ty, AST_expr*>(v.comparators);
                return r;
            }
            case Call_kind: {
                checkCaller(expr->v.Call.func);
                auto r = new (getAlloc()) AST_Call();
                auto v = expr->v.Call;
                r->func = convert(v.func);
                // Pyston's AST only has the Python 2 form f(a, b, *args, k=v, **kwargs).  Other forms are
                // rewritten to it: positional arguments from the first non-final *arg on are collected with a
                // starred tuple display, and keywords from the first non-final **kwargs on with a dict display.
                // TODO: CPython raises on duplicate keys between several **kwargs, the dict display merges them.
                int nargs = asdl_seq_LEN(v.args);
                int first_star = nargs;
                for (int i = 0; i < nargs; i++) {
                    if (((expr_ty)asdl_seq_GET(v.args, i))->kind == Starred_kind) {
                        first_star = i;
                        break;
                    }
                }
                for (int i = 0; i < first_star; i++)
                    r->args.push_back(convert((expr_ty)asdl_seq_GET(v.args, i)));
                if (first_star == nargs - 1) {
                    r->starargs = convert(((expr_ty)asdl_seq_GET(v.args, first_star))->v.Starred.value);
                } else if (first_star < nargs) {
                    auto t = new (getAlloc()) AST_Tuple();
                    t->lineno = expr->lineno;
                    t->col_offset = expr->col_offset;
                    t->ctx_type = AST_TYPE::Load;
                    for (int i = first_star; i < nargs; i++) {
                        expr_ty arg = (expr_ty)asdl_seq_GET(v.args, i);
                        bool is_starred = arg->kind == Starred_kind;
                        t->elts.push_back(convert(is_starred ? arg->v.Starred.value : arg));
                        t->elt_starred.push_back(is_starred);
                    }
                    r->starargs = t;
                }

                int nkeywords = asdl_seq_LEN(v.keywords);
                int first_dstar = nkeywords;
                for (int i = 0; i < nkeywords; i++) {
                    if (((keyword_ty)asdl_seq_GET(v.keywords, i))->arg == NULL) {
                        first_dstar = i;
                        break;
                    }
                }
                for (int i = 0; i < first_dstar; i++)
                    r->keywords.push_back(convert((keyword_ty)asdl_seq_GET(v.keywords, i)));
                if (first_dstar == nkeywords - 1) {
                    r->kwargs = convert(((keyword_ty)asdl_seq_GET(v.keywords, first_dstar))->value);
                } else if (first_dstar < nkeywords) {
                    auto d = new (getAlloc()) AST_Dict();
                    d->lineno = expr->lineno;
                    d->col_offset = expr->col_offset;
                    for (int i = first_dstar; i < nkeywords; i++) {
                        keyword_ty kw = (keyword_ty)asdl_seq_GET(v.keywords, i);
                        AST_Str* key = NULL;
                        if (kw->arg) {
                            key = new (getAlloc()) AST_Str(PyUnicode_AsUTF8(kw->arg));
                            key->lineno = kw->value->lineno;
                            key->col_offset = kw->value->col_offset;
                        }
                        d->keys.push_back(key);
                        d->values.push_back(convert(kw->value));
                    }
                    r->kwargs = d;
                }
                return r;
            }
            case Attribute_kind: {
                auto r = new (getAlloc()) AST_Attribute();
                auto v = expr->v.Attribute;
                r->value = convert(v.value);
                r->attr = convert(v.attr);
                r->ctx_type = convert(v.ctx);
                return r;
            }
            case Subscript_kind: {
                if (expr->v.Subscript.ctx == Load)
                    checkSubscript(expr->v.Subscript.value, expr->v.Subscript.slice);
                auto r = new (getAlloc()) AST_Subscript();
                auto v = expr->v.Subscript;
                r->value = convert(v.value);
                r->slice = convert(v.slice);
                r->ctx_type = convert(v.ctx);
                return r;
            }
            case Name_kind: {
                auto v = expr->v.Name;
                auto r = new (getAlloc()) AST_Name(convert(v.id), convert(v.ctx), 0);
                return r;
            }
            case List_kind: {
                auto r = new (getAlloc()) AST_List();
                auto v = expr->v.List;
                convertStarredElts(v.elts, r->elts, r->elt_starred);
                r->ctx_type = convert(v.ctx);
                return r;
            }
            case Tuple_kind: {
                auto r = new (getAlloc()) AST_Tuple();
                auto v = expr->v.Tuple;
                convertStarredElts(v.elts, r->elts, r->elt_starred);
                r->ctx_type = convert(v.ctx);
                return r;
            }
            case Constant_kind: {
                PyObject* o = expr->v.Constant.value;
                // None, True, False and Ellipsis become names; the CFG turns True, False and "..." into constants.
                // TODO: None is still looked up by name.
                if (o == Py_None || o == Py_True || o == Py_False || o == Py_Ellipsis) {
                    const char* name = o == Py_None ? "None" : o == Py_True ? "True" : o == Py_False ? "False"
                                                                                                      : "...";
                    return new (getAlloc()) AST_Name(pool->get(name), AST_TYPE::Load, expr->lineno, expr->col_offset);
                }
                if (PyInt_Check(o)) {
                    auto r = new (getAlloc()) AST_Num();
                    r->num_type = AST_Num::INT;
                    r->n_int = PyInt_AS_LONG(o);
                    return r;
                }
                if (PyLong_Check(o)) {
                    auto r = new (getAlloc()) AST_Num();
                    r->num_type = AST_Num::LONG;
                    auto s = _PyLong_Format(o, 10);
                    RELEASE_ASSERT(s, "");
                    AUTO_DECREF(s);
                    r->n_long = PyUnicode_AsUTF8(s);
                    return r;
                }
                if (PyFloat_Check(o)) {
                    auto r = new (getAlloc()) AST_Num();
                    r->num_type = AST_Num::FLOAT;
                    r->n_float = PyFloat_AS_DOUBLE(o);
                    return r;
                }
                if (PyComplex_Check(o)) {
                    auto r = new (getAlloc()) AST_Num();
                    r->num_type = AST_Num::COMPLEX;

                    double real = PyComplex_RealAsDouble(o);
                    double imag = PyComplex_ImagAsDouble(o);
                    RELEASE_ASSERT(real != -1.0 || !PyErr_Occurred(), "");
                    RELEASE_ASSERT(imag != -1.0 || !PyErr_Occurred(), "");

                    r->n_float = imag;

                    if (real == 0.0)
                        return r;

                    // TODO very silly:
                    auto freal = new (getAlloc()) AST_Num();
                    freal->n_float = real;
                    freal->num_type = AST_Num::FLOAT;

                    auto binop = new (getAlloc()) AST_BinOp();
                    binop->op_type = AST_TYPE::Add;
                    binop->left = freal;
                    binop->right = r;

                    return binop;
                }
                if (PyUnicode_Check(o)) {
                    // surrogatepass, since string literals can contain lone surrogates.
                    PyObject* utf8 = PyUnicode_AsEncodedString(o, "utf-8", "surrogatepass");
                    RELEASE_ASSERT(utf8, "");
                    AUTO_DECREF(utf8);
                    auto r = new (getAlloc()) AST_Str();
                    r->str_data = std::string(PyBytes_AS_STRING(utf8), PyBytes_GET_SIZE(utf8));
                    r->str_type = AST_Str::STR;
                    return r;
                }
                if (PyBytes_Check(o)) {
                    auto r = new (getAlloc()) AST_Str();
                    r->str_data = std::string(PyBytes_AS_STRING(o), PyBytes_GET_SIZE(o));
                    r->str_type = AST_Str::BYTES;
                    return r;
                }
                RELEASE_ASSERT(0, "unhandled constant type: %s\n", o->cls->tp_name);
            }
            case JoinedStr_kind:
            case FormattedValue_kind: {
                std::string fmt;
                std::vector<AST_expr*> args;
                appendFString(expr, fmt, args);
                auto str = new (getAlloc()) AST_Str(std::move(fmt));
                str->lineno = expr->lineno;
                str->col_offset = expr->col_offset;
                auto r = new (getAlloc()) AST_Call();
                r->func = new (getAlloc()) AST_Attribute(str, AST_TYPE::Load, pool->get("format"));
                r->func->lineno = expr->lineno;
                r->func->col_offset = expr->col_offset;
                r->args = std::move(args);
                return r;
            }
            case Starred_kind:
                unsupported("starred expressions", expr->lineno, expr->col_offset);
            case NamedExpr_kind: {
                auto r = new (getAlloc()) AST_NamedExpr();
                auto v = expr->v.NamedExpr;
                r->value = convert(v.value);
                r->target = ast_cast<AST_Name>(convert(v.target));
                return r;
            }
            case YieldFrom_kind: {
                auto r = new (getAlloc()) AST_Yield();
                r->value = convert(expr->v.YieldFrom.value);
                r->yield_from = true;
                return r;
            }
            case Await_kind:
                unsupported("'await' expressions", expr->lineno, expr->col_offset);
            default:
                RELEASE_ASSERT(0, "unhandled kind: %d\n", expr->kind);
        };
        Py_FatalError("unimplemented");
    }

    // f-strings are converted to a str.format() call: f"a{x!r:>{w}}" becomes "a{!r:>{}}".format(x, w).
    // TODO: CPython calls format() directly, so this differs if the format spec contains '{' or '}'
    // after the nested fields are substituted.
    void appendFString(expr_ty e, std::string& fmt, std::vector<AST_expr*>& args) {
        if (e->kind == Constant_kind) {
            PyObject* utf8 = PyUnicode_AsEncodedString(e->v.Constant.value, "utf-8", "surrogatepass");
            RELEASE_ASSERT(utf8, "");
            AUTO_DECREF(utf8);
            for (char c : llvm::StringRef(PyBytes_AS_STRING(utf8), PyBytes_GET_SIZE(utf8))) {
                fmt += c;
                if (c == '{' || c == '}')
                    fmt += c;
            }
        } else if (e->kind == JoinedStr_kind) {
            for (int i = 0; i < asdl_seq_LEN(e->v.JoinedStr.values); i++)
                appendFString((expr_ty)asdl_seq_GET(e->v.JoinedStr.values, i), fmt, args);
        } else {
            assert(e->kind == FormattedValue_kind);
            auto v = e->v.FormattedValue;
            fmt += '{';
            if (v.conversion != -1) {
                fmt += '!';
                fmt += (char)v.conversion;
            }
            args.push_back(convert(v.value));
            if (v.format_spec) {
                fmt += ':';
                appendFString(v.format_spec, fmt, args);
            }
            fmt += '}';
        }
    }

    AST_expr* convert(expr_ty expr) {
        if (!expr)
            return NULL;

        auto r = _convert(expr);
        r->lineno = expr->lineno;
        r->col_offset = expr->col_offset;
        return r;
    }

    AST_ExceptHandler* convert(excepthandler_ty eh) {
        assert(eh->kind == ExceptHandler_kind);

        auto r = new (getAlloc()) AST_ExceptHandler();
        auto v = eh->v.ExceptHandler;
        r->type = convert(v.type);
        r->name = convertToName(v.name);
        r->body = convert<stmt_ty, AST_stmt*>(v.body);
        r->lineno = eh->lineno;
        r->col_offset = eh->col_offset;
        return r;
    }

    AST_alias* convert(alias_ty alias) {
        return new (getAlloc()) AST_alias(convert(alias->name), convert(alias->asname));
    }

    AST_stmt* _convert(stmt_ty stmt) {
        switch (stmt->kind) {
            case FunctionDef_kind: {
                auto r = new (getAlloc()) AST_FunctionDef();
                r->lineno = stmt->lineno;
                r->col_offset = stmt->col_offset;
                auto v = stmt->v.FunctionDef;
                r->name = convert(v.name);
                r->args = convert(v.args, r);
                nestlevel++;
                r->body = convertScopeBody(v.body, ScopeKind::Function, stmt->lineno);
                nestlevel--;
                r->decorator_list = convert<expr_ty, AST_expr*>(v.decorator_list);

                // Pyston doesn't pass annotations to the function object, so they are set on it
                // right after the def: `f.__annotations__ = {...}`. With decorators that has to
                // happen before they are applied, so `@d def f` becomes `def f; ...; f = d(f)`.
                if (AST_Dict* annotations = convertFunctionAnnotations(v.args, v.returns)) {
                    int lineno = stmt->lineno;
                    auto target = new (getAlloc()) AST_Attribute(
                        new (getAlloc()) AST_Name(r->name, AST_TYPE::Load, lineno), AST_TYPE::Store,
                        pool->get("__annotations__"));
                    stmts_after.push_back(makeAssign(target, annotations, lineno));
                    if (!r->decorator_list.empty()) {
                        // The decorator expressions are still evaluated before the def, e.g.
                        // `@x.setter` has to see the old x.
                        std::vector<InternedString> decorators;
                        for (AST_expr* d : r->decorator_list) {
                            InternedString tmp = pool->get("#decorator" + std::to_string(num_temp_names++));
                            stmts_before.push_back(
                                makeAssign(new (getAlloc()) AST_Name(tmp, AST_TYPE::Store, lineno), d, lineno));
                            decorators.push_back(tmp);
                        }
                        AST_expr* value = new (getAlloc()) AST_Name(r->name, AST_TYPE::Load, lineno);
                        for (int i = r->decorator_list.size() - 1; i >= 0; i--) {
                            auto call = new (getAlloc()) AST_Call();
                            call->func = new (getAlloc()) AST_Name(decorators[i], AST_TYPE::Load, lineno);
                            call->args.push_back(value);
                            call->lineno = lineno;
                            value = call;
                        }
                        r->decorator_list.clear();
                        stmts_after.push_back(
                            makeAssign(new (getAlloc()) AST_Name(r->name, AST_TYPE::Store, lineno), value, lineno));
                    }
                }
                return r;
            }
            case ClassDef_kind: {
                auto r = new (getAlloc()) AST_ClassDef();
                auto v = stmt->v.ClassDef;
                r->name = convert(v.name);
                for (int i = 0; i < asdl_seq_LEN(v.bases); i++) {
                    expr_ty base = (expr_ty)asdl_seq_GET(v.bases, i);
                    if (base->kind == Starred_kind)
                        unsupported("starred class bases", stmt->lineno, stmt->col_offset);
                    r->bases.push_back(convert(base));
                }
                for (int i = 0; i < asdl_seq_LEN(v.keywords); i++) {
                    keyword_ty kw = (keyword_ty)asdl_seq_GET(v.keywords, i);
                    if (kw->arg == NULL)
                        unsupported("**kwargs in class definitions", stmt->lineno, stmt->col_offset);
                    r->keywords.push_back(convert(kw));
                }
                nestlevel++;
                class_names.push_back(v.name);
                r->body = convertScopeBody(v.body, ScopeKind::Class, stmt->lineno);
                class_names.pop_back();
                nestlevel--;
                r->decorator_list = convert<expr_ty, AST_expr*>(v.decorator_list);
                return r;
            }
            case Return_kind: {
                auto r = new (getAlloc()) AST_Return();
                auto v = stmt->v.Return;
                r->value = convert(v.value);
                return r;
            }
            case Delete_kind: {
                auto r = new (getAlloc()) AST_Delete();
                auto v = stmt->v.Delete;
                r->targets = convert<expr_ty, AST_expr*>(v.targets);
                return r;
            }
            case Assign_kind: {
                auto r = new (getAlloc()) AST_Assign();
                auto v = stmt->v.Assign;
                r->targets = convert<expr_ty, AST_expr*>(v.targets);
                r->value = convert(v.value);
                return r;
            }
            case AugAssign_kind: {
                auto r = new (getAlloc()) AST_AugAssign();
                auto v = stmt->v.AugAssign;
                r->target = convert(v.target);
                r->op_type = convert(v.op);
                r->value = convert(v.value);
                return r;
            }
            case AnnAssign_kind: {
                // In a module or class body the annotation is evaluated and, for a plain name,
                // stored in __annotations__. Annotations of locals are not evaluated.
                auto v = stmt->v.AnnAssign;
                std::vector<AST_stmt*> stmts;
                if (v.value)
                    stmts.push_back(makeAssign(convert(v.target), convert(v.value), stmt->lineno));
                if (scope_kinds.back() != ScopeKind::Function) {
                    AST_expr* annotation = convert(v.annotation);
                    if (v.simple && v.target->kind == Name_kind) {
                        auto index = new (getAlloc()) AST_Index();
                        index->value = makeMangledStr(v.target->v.Name.id);
                        auto sub = new (getAlloc()) AST_Subscript();
                        sub->value = makeName("__annotations__", AST_TYPE::Load, stmt->lineno);
                        sub->slice = index;
                        sub->ctx_type = AST_TYPE::Store;
                        stmts.push_back(makeAssign(sub, annotation, stmt->lineno));
                    } else {
                        auto e = new (getAlloc()) AST_Expr(annotation);
                        e->lineno = stmt->lineno;
                        stmts.push_back(e);
                    }
                } else if (!v.value && v.simple && v.target->kind == Name_kind) {
                    // `x: int` still makes x a local: `if 0: x = None`.
                    auto zero = new (getAlloc()) AST_Num();
                    zero->num_type = AST_Num::INT;
                    zero->n_int = 0;
                    auto r = new (getAlloc()) AST_If();
                    r->lineno = stmt->lineno;
                    r->col_offset = stmt->col_offset;
                    r->test = zero;
                    r->body.push_back(makeAssign(convert(v.target), makeName("None", AST_TYPE::Load, stmt->lineno),
                                                 stmt->lineno));
                    stmts.push_back(r);
                }
                if (stmts.empty())
                    return new (getAlloc()) AST_Pass();
                for (int i = 1; i < stmts.size(); i++)
                    stmts_after.push_back(stmts[i]);
                return stmts[0];
            }
            case For_kind: {
                auto r = new (getAlloc()) AST_For();
                auto v = stmt->v.For;
                r->target = convert(v.target);
                r->iter = convert(v.iter);
                auto fin = in_finally;
                in_finally = 0;
                loop_depth++;
                r->body = convert<stmt_ty, AST_stmt*>(v.body);
                loop_depth--;
                in_finally = fin;
                r->orelse = convert<stmt_ty, AST_stmt*>(v.orelse);
                return r;
            }
            case While_kind: {
                auto r = new (getAlloc()) AST_While();
                auto v = stmt->v.While;
                r->test = convert(v.test);
                auto fin = in_finally;
                in_finally = 0;
                loop_depth++;
                r->body = convert<stmt_ty, AST_stmt*>(v.body);
                loop_depth--;
                in_finally = fin;
                r->orelse = convert<stmt_ty, AST_stmt*>(v.orelse);
                return r;
            }
            case If_kind: {
                auto r = new (getAlloc()) AST_If();
                auto v = stmt->v.If;
                r->test = convert(v.test);
                r->body = convert<stmt_ty, AST_stmt*>(v.body);
                r->orelse = convert<stmt_ty, AST_stmt*>(v.orelse);
                return r;
            }
            case With_kind: {
                // Python 3 allows several context managers in one with statement; nest them.
                auto v = stmt->v.With;
                std::vector<AST_stmt*> body = convert<stmt_ty, AST_stmt*>(v.body);
                for (int i = asdl_seq_LEN(v.items) - 1; i >= 0; i--) {
                    withitem_ty item = (withitem_ty)asdl_seq_GET(v.items, i);
                    auto r = new (getAlloc()) AST_With();
                    r->lineno = stmt->lineno;
                    r->col_offset = stmt->col_offset;
                    r->context_expr = convert(item->context_expr);
                    r->optional_vars = convert(item->optional_vars);
                    r->body = std::move(body);
                    body = { r };
                }
                return body[0];
            }
            case Raise_kind: {
                auto r = new (getAlloc()) AST_Raise();
                auto v = stmt->v.Raise;
                r->lineno = stmt->lineno;
                r->col_offset = stmt->col_offset;
                r->arg0 = convert(v.exc);
                // The CFG turns 'raise X from Y' into a call that sets the cause.
                r->arg1 = convert(v.cause);
                return r;
            }
            case Try_kind: {
                auto v = stmt->v.Try;
                std::vector<AST_stmt*> body = convert<stmt_ty, AST_stmt*>(v.body);
                if (asdl_seq_LEN(v.handlers)) {
                    auto r = new (getAlloc()) AST_TryExcept();
                    r->lineno = stmt->lineno;
                    r->col_offset = stmt->col_offset;
                    r->body = std::move(body);
                    r->handlers = convert<excepthandler_ty, AST_ExceptHandler*>(v.handlers);
                    r->orelse = convert<stmt_ty, AST_stmt*>(v.orelse);
                    if (!asdl_seq_LEN(v.finalbody))
                        return r;
                    body = { r };
                }
                auto r = new (getAlloc()) AST_TryFinally();
                r->body = std::move(body);
                in_finally++;
                r->finalbody = convert<stmt_ty, AST_stmt*>(v.finalbody);
                in_finally--;
                return r;
            }
            case Assert_kind: {
                if (stmt->v.Assert.test->kind == Tuple_kind && asdl_seq_LEN(stmt->v.Assert.test->v.Tuple.elts) > 0)
                    warn(stmt->lineno, stmt->col_offset, "assertion is always true, perhaps remove parentheses?");
                auto r = new (getAlloc()) AST_Assert();
                auto v = stmt->v.Assert;
                r->test = convert(v.test);
                r->msg = convert(v.msg);
                return r;
            }
            case Import_kind: {
                auto r = new (getAlloc()) AST_Import();
                auto v = stmt->v.Import;
                r->names = convert<alias_ty, AST_alias*>(v.names);
                return r;
            }
            case ImportFrom_kind: {
                auto r = new (getAlloc()) AST_ImportFrom();
                auto v = stmt->v.ImportFrom;
                r->module = convert(v.module);
                r->names = convert<alias_ty, AST_alias*>(v.names);
                r->level = v.level;
                return r;
            }
            case Global_kind: {
                auto r = new (getAlloc()) AST_Global();
                auto v = stmt->v.Global;
                r->names = convert<identifier, InternedString>(v.names);
                return r;
            }
            case Expr_kind: {
                auto r = new (getAlloc()) AST_Expr();
                auto v = stmt->v.Expr;
                r->value = convert(v.value);
                if (interactive && nestlevel <= 0) {
                    auto print_expr = new (getAlloc()) AST_LangPrimitive(AST_LangPrimitive::PRINT_EXPR);
                    print_expr->args.push_back(r->value);
                    r->value = print_expr;
                }
                return r;
            }
            case Nonlocal_kind: {
                if (nestlevel == 0)
                    raiseSyntaxError("nonlocal declaration not allowed at module level", stmt->lineno,
                                     stmt->col_offset, fn, "", true);
                auto r = new (getAlloc()) AST_Global();
                r->names = convert<identifier, InternedString>(stmt->v.Nonlocal.names);
                r->is_nonlocal = true;
                return r;
            }
            case AsyncFunctionDef_kind: {
                // TODO: coroutines.  For now an async function is defined as a plain function that raises
                // when called, so that modules which define (but don't use) coroutines can be imported.
                auto r = new (getAlloc()) AST_FunctionDef();
                r->lineno = stmt->lineno;
                r->col_offset = stmt->col_offset;
                auto v = stmt->v.AsyncFunctionDef;
                r->name = convert(v.name);
                r->args = convert(v.args, r);
                r->decorator_list = convert<expr_ty, AST_expr*>(v.decorator_list);

                auto call = new (getAlloc()) AST_Call();
                call->lineno = stmt->lineno;
                call->func = new (getAlloc()) AST_Name(pool->get("SystemError"), AST_TYPE::Load, stmt->lineno);
                auto msg = new (getAlloc()) AST_Str("coroutines are not supported by Pyston yet");
                msg->lineno = stmt->lineno;
                call->args.push_back(msg);
                auto raise = new (getAlloc()) AST_Raise();
                raise->lineno = stmt->lineno;
                raise->arg0 = call;
                r->body.push_back(raise);
                return r;
            }
            case AsyncFor_kind:
            case AsyncWith_kind:
                unsupported("coroutines", stmt->lineno, stmt->col_offset);
            case Pass_kind:
                return new (getAlloc()) AST_Pass();
            case Break_kind:
                // This is not really the right place to be handling this, but this whole thing is temporary anyway.
                if (loop_depth == 0)
                    raiseSyntaxError("'break' outside loop", stmt->lineno, stmt->col_offset, fn, "", true);
                return new (getAlloc()) AST_Break();
            case Continue_kind:
                if (loop_depth == 0)
                    raiseSyntaxError("'continue' not properly in loop", stmt->lineno, stmt->col_offset, fn, "", true);
                return new (getAlloc()) AST_Continue();
        };
        // GCC wants this:
        RELEASE_ASSERT(0, "invalid statement type: %d", stmt->kind);
    }

    AST_stmt* convert(stmt_ty stmt) {
        auto r = _convert(stmt);
        r->lineno = stmt->lineno;
        r->col_offset = stmt->col_offset;

        if (stmt->lineno == 0) {
            // So far it looks like these can only be generated for empty lines on the repl
            assert(stmt->kind == Pass_kind);
            r->lineno = 1;
        }

        return r;
    }

    std::pair<AST*, std::unique_ptr<ASTAllocator>> convert(mod_ty mod) {
        ast_allocator.reset(new ASTAllocator);
        switch (mod->kind) {
            case Module_kind: {
                AST_Module* rtn = new (getAlloc()) AST_Module(std::make_unique<InternedStringPool>());
                rtn->lineno = 1;
                assert(!this->pool);
                this->pool = rtn->interned_strings.get();
                rtn->body = convertScopeBody(mod->v.Module.body, ScopeKind::Module, 1);
                return std::make_pair(rtn, std::move(ast_allocator));
            }
            case Interactive_kind: {
                this->interactive = 1;
                AST_Module* rtn = new (getAlloc()) AST_Module(std::make_unique<InternedStringPool>());
                rtn->lineno = 1;
                assert(!this->pool);
                this->pool = rtn->interned_strings.get();
                convertAll<stmt_ty>(mod->v.Interactive.body, rtn->body);
                return std::make_pair(rtn, std::move(ast_allocator));
            }
            case Expression_kind: {
                AST_Expression* rtn = new (getAlloc()) AST_Expression(std::make_unique<InternedStringPool>());
                rtn->lineno = 1;
                this->pool = rtn->interned_strings.get();

                // instead of storing the expression inside the AST node we convert it directly to a return statement
                AST_expr* expr = this->convert(mod->v.Expression.body);
                auto rtn_stmt = new (getAlloc()) AST_Return;
                rtn_stmt->lineno = expr->lineno;
                rtn_stmt->col_offset = expr->col_offset;
                rtn_stmt->value = expr;

                rtn->body = rtn_stmt;
                return std::make_pair(rtn, std::move(ast_allocator));
            }
            default:
                RELEASE_ASSERT(0, "unhandled kind: %d\n", mod->kind);
        }
    }
};

extern "C" int _PystonCheckSymtable(mod_ty mod, PyObject* filename, int future_features);

// Runs CPython's symbol table pass, which reports scoping errors such as an assignment expression
// rebinding a comprehension iteration variable.
static void checkSymtable(mod_ty mod, llvm::StringRef fn) {
    int future_features = 0;
    if (mod->kind == Module_kind) {
        for (int i = 0; i < asdl_seq_LEN(mod->v.Module.body); i++) {
            stmt_ty stmt = (stmt_ty)asdl_seq_GET(mod->v.Module.body, i);
            if (stmt->kind != ImportFrom_kind || !stmt->v.ImportFrom.module
                || !_PyUnicode_EqualToASCIIString(stmt->v.ImportFrom.module, "__future__"))
                continue;
            for (int j = 0; j < asdl_seq_LEN(stmt->v.ImportFrom.names); j++) {
                alias_ty alias = (alias_ty)asdl_seq_GET(stmt->v.ImportFrom.names, j);
                if (_PyUnicode_EqualToASCIIString(alias->name, "annotations"))
                    future_features |= CO_FUTURE_ANNOTATIONS;
            }
        }
    }

    PyObject* filename = PyUnicode_DecodeFSDefaultAndSize(fn.data(), fn.size());
    if (!filename)
        throwCAPIException();
    AUTO_DECREF(filename);
    if (_PystonCheckSymtable(mod, filename, future_features) < 0)
        throwCAPIException();
}

std::pair<AST*, std::unique_ptr<ASTAllocator>> cpythonToPystonAST(mod_ty mod, llvm::StringRef fn) {
    checkSymtable(mod, fn);
    Converter c(fn);
    return c.convert(mod);
}
}
