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

#include "codegen/opt/inliner.h"

#include <unordered_map>

#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/InlineCost.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

#include "codegen/codegen.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/util.h"

//#undef VERBOSITY
//#define VERBOSITY(x) 2

namespace pyston {

// The functions we inline come from the stdlib module, so after inlining their bodies refer to functions and
// globals of that module.  This replaces those references with declarations in the module being compiled.
class MyMaterializer : public llvm::ValueMaterializer {
private:
    llvm::Module* new_module;

public:
    MyMaterializer(llvm::Module* new_module) : new_module(new_module) {}
    virtual ~MyMaterializer() {}

    llvm::Value* materialize(llvm::Value* v) override {
        if (llvm::Function* f = llvm::dyn_cast<llvm::Function>(v)) {
            if (f->getParent() == new_module)
                return nullptr;
            return new_module->getOrInsertFunction(f->getName(), f->getFunctionType()).getCallee();
        } else if (llvm::GlobalVariable* gv = llvm::dyn_cast<llvm::GlobalVariable>(v)) {
            if (gv->getParent() == new_module)
                return nullptr;
            assert(gv->getLinkage() != llvm::GlobalVariable::PrivateLinkage);
            llvm::GlobalVariable* new_gv
                = llvm::cast<llvm::GlobalVariable>(new_module->getOrInsertGlobal(gv->getName(), gv->getValueType()));
            RELEASE_ASSERT(!gv->isThreadLocal(), "I don't think MCJIT supports thread-local variables yet");
            new_gv->setThreadLocalMode(gv->getThreadLocalMode());
            return new_gv;
        } else if (llvm::GlobalAlias* alias = llvm::dyn_cast<llvm::GlobalAlias>(v)) {
            if (alias->getParent() == new_module)
                return nullptr;
            llvm::Value* addressee = materialize(alias->getAliasee());
            assert(addressee);
            return addressee;
        }
        return nullptr;
    }
};

static bool prepareInlining(llvm::CallBase* CS) {
    if (llvm::isa<llvm::IntrinsicInst>(CS))
        return false;

    llvm::Value* v = CS->getCalledOperand();
    llvm::ConstantExpr* ce = llvm::dyn_cast<llvm::ConstantExpr>(v);
    if (!ce)
        return false;

    assert(ce->isCast());
    llvm::ConstantInt* l_addr = llvm::cast<llvm::ConstantInt>(ce->getOperand(0));
    int64_t addr = l_addr->getSExtValue();

    if (addr == (int64_t)printf)
        return false;
    llvm::Function* f = g.func_addr_registry.getLLVMFuncAtAddress((void*)addr);
    if (f == NULL) {
        if (VERBOSITY() >= 3) {
            printf("Giving up on inlining %s:\n", g.func_addr_registry.getFuncNameAtAddress((void*)addr, true).c_str());
            CS->dump();
        }
        return false;
    }

    // We load the bitcode lazily, so check if we haven't yet fully loaded the function:
    if (f->isMaterializable()) {
        llvm::Error err = f->materialize();
        RELEASE_ASSERT(!err, "");

        // The stdlib is compiled for the generic target, while the functions we generate don't specify one.
        // Drop the target attributes so that the inline cost analysis considers them compatible.
        f->removeFnAttr("target-cpu");
        f->removeFnAttr("target-features");
        f->removeFnAttr("tune-cpu");
    }

    // It could still be a declaration, though I think the code won't generate this case any more:
    if (f->isDeclaration())
        return false;

    if (CS->getFunctionType() != f->getFunctionType())
        return false;

    if (f->getName() == "allowGLReadPreemption")
        return false;

    assert(!f->isDeclaration());
    CS->setCalledFunction(f);
    return true;
}

static bool _inlineRuntimeCalls(llvm::Function& f, int threshold) {
    Timer _t2("(sum)");
    Timer _t("collecting calls");

    llvm::Module* cur_module = f.getParent();

    // Calls whose callee we pointed at the stdlib function but then decided not to inline.  They have to be pointed
    // back at the original address, since the stdlib module is not part of the JIT.
    std::vector<std::pair<llvm::CallBase*, llvm::Value*>> calls;
    for (llvm::inst_iterator I = llvm::inst_begin(f), E = llvm::inst_end(f); I != E; ++I) {
        if (auto* call = llvm::dyn_cast<llvm::CallBase>(&(*I))) {
            llvm::Value* orig_callee = call->getCalledOperand();
            if (prepareInlining(call))
                calls.push_back(std::make_pair(call, orig_callee));
        }
    }

    std::unordered_map<llvm::Function*, std::unique_ptr<llvm::AssumptionCache>> assumption_caches;
    auto get_assumption_cache = [&](llvm::Function& func) -> llvm::AssumptionCache& {
        auto& ac = assumption_caches[&func];
        if (!ac)
            ac.reset(new llvm::AssumptionCache(func));
        return *ac;
    };
    llvm::TargetLibraryInfoImpl tlii(g.tm->getTargetTriple());
    std::unordered_map<llvm::Function*, std::unique_ptr<llvm::TargetLibraryInfo>> tlis;
    auto get_tli = [&](llvm::Function& func) -> const llvm::TargetLibraryInfo& {
        auto& tli = tlis[&func];
        if (!tli)
            tli.reset(new llvm::TargetLibraryInfo(tlii, &func));
        return *tli;
    };

    bool did_any_inlining = false;

    _t.split("doing inlining");
    while (calls.size()) {
        llvm::CallBase* cs = calls.back().first;
        llvm::Value* orig_callee = calls.back().second;
        calls.pop_back();

        llvm::Function* callee = cs->getCalledFunction();
        llvm::TargetTransformInfo tti = g.tm->getTargetTransformInfo(*callee);
        llvm::InlineCost IC = llvm::getInlineCost(*cs, llvm::getInlineParams(threshold), tti, get_assumption_cache,
                                                  get_tli);
        bool do_inline = (bool)IC;

        if (VERBOSITY("irgen.inlining") >= 2) {
            if (!do_inline)
                llvm::outs() << "not ";
            llvm::outs() << "inlining ";
            cs->dump();
        }

        bool inlined = false;
        if (do_inline) {
            static StatCounter num_inlines("num_inlines");
            num_inlines.log();

            llvm::InlineFunctionInfo InlineInfo;
            inlined = llvm::InlineFunction(*cs, InlineInfo).isSuccess();
            did_any_inlining = did_any_inlining || inlined;
        }

        if (!inlined)
            cs->setCalledOperand(orig_callee);
    }

    if (!did_any_inlining)
        return false;

    _t.split("remapping");

    llvm::ValueToValueMapTy VMap;
    MyMaterializer materializer(cur_module);
    for (llvm::inst_iterator I = llvm::inst_begin(f), E = llvm::inst_end(f); I != E; ++I) {
        RemapInstruction(&(*I), VMap, llvm::RF_IgnoreMissingLocals | llvm::RF_NoModuleLevelChanges, NULL,
                         &materializer);
    }

    _t.split("cleaning up");

    std::vector<llvm::GlobalValue*> to_remove;
    for (llvm::GlobalVariable& gv : cur_module->globals()) {
        if (gv.use_empty())
            to_remove.push_back(&gv);
    }

    for (int i = 0; i < to_remove.size(); i++) {
        to_remove[i]->eraseFromParent();
    }

    for (llvm::Module::iterator I = cur_module->begin(), E = cur_module->end(); I != E;) {
        if (!I->isDeclaration()) {
            ++I;
            continue;
        }

        if (I->use_empty()) {
            I = cur_module->getFunctionList().erase(I);
        } else {
            ++I;
        }
    }

    return did_any_inlining;
}

bool inlineRuntimeCalls(llvm::Function& f, int threshold) {
    Timer _t("inlining");

    bool rtn = _inlineRuntimeCalls(f, threshold);

    static StatCounter us_inlining("us_compiling_optimizing_inlining");
    long us = _t.end();
    us_inlining.log(us);

    return rtn;
}
}
