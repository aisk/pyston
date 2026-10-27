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

#include <cstdio>
#include <iostream>
#include <unordered_map>

#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/ToolOutputFile.h"

using namespace llvm;

static cl::opt<std::string>
InputFilename(cl::Positional, cl::desc("<input bitcode file>"),
        cl::init("-"), cl::value_desc("filename"));

static cl::opt<std::string>
OutputFilename("o", cl::desc("Specify output filename"),
        cl::value_desc("filename"), cl::init("-"));

static cl::opt<bool>
Force("f", cl::desc("Enable binary output on terminals"));

// TODO should restructure this as a set of passes rather than a monolithic binary


// This function was originally gotten from http://llvm.org/svn/llvm-project/vmkit/branches/mcjit/lib/vmkit-prepare-code/adapt-linkage.cc
// It was released under the license here http://llvm.org/svn/llvm-project/vmkit/trunk/LICENSE.TXT
bool makeVisible(llvm::GlobalValue* gv) {
    llvm::GlobalValue::LinkageTypes linkage = gv->getLinkage();
    bool changed = false;

    if (linkage == llvm::GlobalValue::LinkOnceODRLinkage) {
        gv->setLinkage(llvm::GlobalValue::WeakODRLinkage);
        changed = true;
    } else if (linkage == llvm::GlobalValue::LinkOnceAnyLinkage) {
        gv->setLinkage(llvm::GlobalValue::WeakAnyLinkage);
        changed = true;
    } else if (linkage == llvm::GlobalValue::PrivateLinkage) {
        gv->setName(gv->getParent()->getModuleIdentifier() + gv->getName());
        gv->setLinkage(llvm::GlobalValue::ExternalLinkage);
        changed = true;
    } else if (linkage == llvm::GlobalValue::InternalLinkage) {
        // Not sure if this is the right linkage here:
        gv->setLinkage(llvm::GlobalValue::WeakODRLinkage);
        changed = true;
    }

    // Hidden symbols won't end up as globals.
    // Worse, a hidden symbol, when linked with a default-visibility symbol,
    // will result in a non-visible symbol.
    // So it's not enough to just set the visibility here; instead we have to
    // set it to protected *and* change the name.
    // The only thing affected by this that I know about is __clang_call_terminate.
    llvm::GlobalValue::VisibilityTypes visibility = gv->getVisibility();
    if (visibility == llvm::GlobalValue::HiddenVisibility) {
        gv->setVisibility(llvm::GlobalValue::ProtectedVisibility);
        //gv->setDLLStorageClass(llvm::GlobalValue::DLLExportStorageClass);
        std::string old_name = gv->getName().str();
        std::string new_name = (gv->getName() + "_protected").str();
        gv->setName(new_name);

        // If this symbol has comdat set we have to remove the comdat for the old name
        // and create a new comdat with the new name
        if (gv->hasComdat() && isa<llvm::GlobalObject>(gv)) {
            llvm::Module* mod = gv->getParent();
            llvm::GlobalObject* go = cast<llvm::GlobalObject>(gv);
            llvm::Comdat* new_com = mod->getOrInsertComdat(new_name);
            new_com->setSelectionKind(go->getComdat()->getSelectionKind());
            go->setComdat(new_com);
            auto& Comdats = mod->getComdatSymbolTable();
            Comdats.erase(Comdats.find(old_name));
        }

        changed = true;
    }

    return changed;
}

int main(int argc, char **argv) {
    InitLLVM X(argc, argv);

    LLVMContext Context;
    cl::ParseCommandLineOptions(argc, argv, "mcjit pre-cacher");

    SMDiagnostic Err;

    std::unique_ptr<Module> M(parseIRFile(InputFilename, Err, Context));

    if (M.get() == 0) {
        Err.print(argv[0], errs());
        return 1;
    }

    for (Function& F : *M) {
        makeVisible(&F);
    }

    for (GlobalVariable& GV : M->globals()) {
        makeVisible(&GV);
    }

    if (OutputFilename.empty())
        OutputFilename = "-";

    std::error_code EC;
    ToolOutputFile out(OutputFilename, EC, sys::fs::OF_None);
    if (EC) {
        errs() << "error opening file for writing\n";
        return 1;
    }

    WriteBitcodeToFile(*M, out.os());

    out.keep();

    return 0;
}
