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

#ifndef PYSTON_CODEGEN_IRGEN_IRGENERATOR_H
#define PYSTON_CODEGEN_IRGEN_IRGENERATOR_H

#include <map>

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"

#include "core/cfg.h"
#include "core/stringpool.h"
#include "core/types.h"

namespace llvm {
class AllocaInst;
class BasicBlock;
class BranchInst;
// class Function;
class MDNode;
}

namespace pyston {

class CFGBlock;
class GCBuilder;
struct PatchpointInfo;
class ScopeInfo;
class TypeAnalysis;
class RefcountTracker;
class UnwindInfo;

typedef VRegMap<CompilerVariable*> SymbolTable;
typedef VRegMap<llvm::Value*> DefinednessTable;
typedef VRegMap<ConcreteCompilerVariable*> ConcreteSymbolTable;

extern const std::string CREATED_CLOSURE_NAME;
extern const std::string PASSED_CLOSURE_NAME;
extern const std::string PASSED_GENERATOR_NAME;
extern const std::string FRAME_INFO_PTR_NAME;


// Class that holds state of the current IR generation, that might not be local
// to the specific phase or pass we're in.
// TODO this probably shouldn't be here
class IRGenState;
class IRGenerator;

// A call to a Python function whose body gets emitted into the caller (see emitInlinedCall() in irgen.cpp).
// The inlined function gets its own FrameInfo, which it puts in the caller's frame_info_ptr slot (where the
// unwinder looks for the frame of the native frame) while it runs.  The code of the callee doesn't leave the
// inlined region by returning or throwing: it jumps to exits that the caller completes.
struct InlineInfo {
    IRGenState* caller;
    IRGenerator* caller_generator;
    llvm::Value* globals;
    const std::vector<CompilerVariable*>& args;
    // The caller block that jumps to the start of the callee.
    llvm::BasicBlock* entry_from;

    // The callee has set a CAPI exception (and exited its frame, if it still had it).
    llvm::BasicBlock* exc_exit = NULL;
    // The callee deopted.  The interpreter finished running it, and deopt_value is the value it returned.
    llvm::BasicBlock* deopt_exit = NULL;
    llvm::PHINode* deopt_value = NULL;
    // The blocks that return, and the values they return.  These blocks don't have a terminator yet.
    std::vector<std::pair<llvm::BasicBlock*, CompilerVariable*>> returns;

    InlineInfo(IRGenState* caller, IRGenerator* caller_generator, llvm::Value* globals,
               const std::vector<CompilerVariable*>& args, llvm::BasicBlock* entry_from)
        : caller(caller), caller_generator(caller_generator), globals(globals), args(args), entry_from(entry_from) {}
};

class IRGenState {
private:
    // Note: due to some not-yet-fixed behavior, cf->code_obj is NULL will only get set to point
    // to code_obj at the end of irgen.
    BoxedCode* code;
    CompiledFunction* cf;
    llvm::Function* func;
    SourceInfo* source_info;
    std::unique_ptr<PhiAnalysis> phis;
    const ParamNames* param_names;
    GCBuilder* gc;
    llvm::MDNode* func_dbg_info;
    RefcountTracker* refcount_tracker;

    InlineInfo* inline_info;

    llvm::AllocaInst* scratch_space;
    llvm::Value* frame_info;
    // Where the unwinder finds the FrameInfo of this native frame.
    llvm::Value* frame_info_ptr_slot;
    llvm::Value* boxed_locals;
    llvm::Value* globals;
    llvm::Value* vregs;
    llvm::Value* stmt;

    llvm::Value* passed_closure = NULL, * created_closure = NULL, * passed_generator = NULL;

    int scratch_size;

public:
    IRGenState(BoxedCode* code, CompiledFunction* cf, llvm::Function* func, SourceInfo* source_info,
               std::unique_ptr<PhiAnalysis> phis, const ParamNames* param_names, GCBuilder* gc,
               llvm::MDNode* func_dbg_info, RefcountTracker* refcount_tracker, InlineInfo* inline_info = NULL);
    ~IRGenState();

    CFG* getCFG() { return getSourceInfo()->cfg; }

    CompiledFunction* getCurFunction() { return cf; }
    BoxedCode* getCode() { return code; }
    const CodeConstants& getCodeConstants();

    // The exits of an inlined function work like the ones of a CAPI function.
    ExceptionStyle getExceptionStyle() { return inline_info ? CAPI : cf->exception_style; }

    InlineInfo* getInlineInfo() { return inline_info; }
    // OSR compiles keep using the frame of the interpreter, which also deinits it.
    bool ownsFrame() { return inline_info || !cf->entry_descriptor; }

    llvm::Function* getLLVMFunction() { return func; }

    EffortLevel getEffortLevel() { return cf->effort; }

    GCBuilder* getGC() { return gc; }

    void initNewFrameInfo(llvm::IRBuilder<>& alloca_builder, llvm::IRBuilder<>& builder, llvm::Value* passed_closure,
                          llvm::Value* passed_globals);
    void setupFrameInfoVar(llvm::Value* passed_closure, llvm::Value* passed_globals,
                           llvm::Value* frame_info_arg = NULL);
    void setupFrameInfoVarOSR(llvm::Value* frame_info_arg) { return setupFrameInfoVar(NULL, NULL, frame_info_arg); }
    // Creates the frame of an inlined function at the end of the given block, and makes it the frame of the native
    // frame.
    void setupInlinedFrameInfoVar(llvm::BasicBlock* bb);

    llvm::Value* getScratchSpace(int min_bytes);
    llvm::Value* getFrameInfoVar();
    llvm::Value* getFrameInfoPtrSlot();
    llvm::Value* getBoxedLocalsVar();
    llvm::Value* getVRegsVar();
    llvm::Value* getStmtVar();

    ConcreteCompilerType* getReturnType() { return cf->getReturnType(); }

    SourceInfo* getSourceInfo() { return source_info; }

    LivenessAnalysis* getLiveness() { return source_info->getLiveness(getCodeConstants()); }
    PhiAnalysis* getPhis() { return phis.get(); }

    const ScopingResults& getScopeInfo();

    llvm::MDNode* getFuncDbgInfo() { return func_dbg_info; }

    RefcountTracker* getRefcounts() { return refcount_tracker; }

    const ParamNames* getParamNames() { return param_names; }

    llvm::Value* getPassedClosure();
    llvm::Value* getCreatedClosure();
    llvm::Value* getPassedGenerator();

    void setPassedClosure(llvm::Value*);
    void setCreatedClosure(llvm::Value*);
    void setPassedGenerator(llvm::Value*);

    // Returns the custom globals, or the module if the globals come from the module.
    llvm::Value* getGlobals();
    // Returns the custom globals, or null if the globals come from the module.
    llvm::Value* getGlobalsIfCustom();
};

// turns CFGBlocks into LLVM IR
class IRGenerator {
private:
public:
    struct ExceptionState {
        llvm::BasicBlock* from_block;
        ConcreteCompilerVariable* exc_type, *exc_value, *exc_tb;
        ExceptionState(llvm::BasicBlock* from_block, ConcreteCompilerVariable* exc_type,
                       ConcreteCompilerVariable* exc_value, ConcreteCompilerVariable* exc_tb)
            : from_block(from_block), exc_type(exc_type), exc_value(exc_value), exc_tb(exc_tb) {}
    };
    struct EndingState {
        // symbol_table records which Python variables are bound to what CompilerVariables at the end of this block.
        // phi_symbol_table records the ones that will need to be `phi'd.
        // both only record non-globals.

        // TODO: switch these to unique_ptr's
        SymbolTable* symbol_table;
        ConcreteSymbolTable* phi_symbol_table;
        DefinednessTable* definedness_vars;
        llvm::BasicBlock* ending_block;
        llvm::SmallVector<ExceptionState, 2> exception_state;

        EndingState(SymbolTable* symbol_table, ConcreteSymbolTable* phi_symbol_table,
                    DefinednessTable* definedness_vars, llvm::BasicBlock* ending_block,
                    llvm::ArrayRef<ExceptionState> exception_state)
            : symbol_table(symbol_table),
              phi_symbol_table(phi_symbol_table),
              definedness_vars(definedness_vars),
              ending_block(ending_block),
              exception_state(exception_state.begin(), exception_state.end()) {}
    };

    virtual ~IRGenerator() {}

    virtual void doFunctionEntry(const ParamNames& param_names, const std::vector<ConcreteCompilerType*>& arg_types)
        = 0;
    // The entry of an inlined function (see InlineInfo).
    virtual void doInlinedFunctionEntry() = 0;

#ifndef NDEBUG
    virtual void giveLocalSymbol(InternedString name, CompilerVariable* var) = 0;
#endif
    virtual void giveLocalSymbol(int vreg, CompilerVariable* var) = 0;
    virtual void giveDefinednessVar(int vreg, llvm::Value* val) = 0;
    virtual void copySymbolsFrom(SymbolTable* st) = 0;
    virtual void run(const CFGBlock* block) = 0; // primary entry point
    virtual EndingState getEndingSymbolTable() = 0;
    virtual void doSafePoint(BST_stmt* next_statement) = 0;
    virtual void addFrameStackmapArgs(PatchpointInfo* pp, std::vector<llvm::Value*>& stackmap_args) = 0;
    virtual bool hasLazyLocals() = 0;
    virtual void flushLazyLocals() = 0;
    // For the caller of an inlined function: adds its lazy locals to a stackmap of the inlined function, or flushes
    // them at the end of a block of the inlined function (returning the block that the code continues in).
    virtual void addOuterLazyLocals(PatchpointInfo* pp, std::vector<llvm::Value*>& stackmap_args) = 0;
    virtual llvm::BasicBlock* flushLazyLocalsAt(llvm::BasicBlock* bb) = 0;
    virtual void addOutgoingExceptionState(ExceptionState exception_state) = 0;
    virtual void setIncomingExceptionState(llvm::SmallVector<ExceptionState, 2> exc_state) = 0;
    virtual llvm::BasicBlock* getCXXExcDest(const UnwindInfo&) = 0;
    virtual llvm::BasicBlock* getCAPIExcDest(llvm::BasicBlock* from_block, llvm::BasicBlock* final_dest,
                                             BST_stmt* current_stmt, bool is_after_deopt = false) = 0;
    virtual CFGBlock* getCFGBlock() = 0;
};

std::tuple<llvm::Value*, llvm::Value*, llvm::Value*> createLandingpad(llvm::BasicBlock*);

class IREmitter;
class BST_Call;
IREmitter* createIREmitter(IRGenState* irstate, llvm::BasicBlock*& curblock, IRGenerator* irgenerator = NULL);
// Emits the body of an inlined function (see InlineInfo).
void emitInlinedBody(BoxedCode* code, InlineInfo* inline_info);

IRGenerator* createIRGenerator(IRGenState* irstate, std::unordered_map<CFGBlock*, llvm::BasicBlock*>& entry_blocks,
                               CFGBlock* myblock, TypeAnalysis* types);
}

#endif
