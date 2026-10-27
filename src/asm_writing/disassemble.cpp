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

#include "asm_writing/disassemble.h"

#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/IR/Mangler.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnon-virtual-dtor"
#endif
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif
#include "llvm/MC/MCInstPrinter.h"
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#ifdef GCC
#pragma GCC diagnostic pop
#endif

#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"

#include "codegen/codegen.h"

void LLVMInitializeX86Disassembler();

namespace pyston {
namespace assembler {

void disassemblyInitialize() {
    LLVMInitializeX86Disassembler();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();
}

void AssemblyLogger::log_comment(const llvm::Twine& comment, size_t offset) {
    comments[offset].push_back(comment.str());
}

void AssemblyLogger::append_comments(llvm::raw_string_ostream& stream, size_t pos) const {
    if (comments.count(pos) > 0) {
        for (const std::string& comment : comments.at(pos)) {
            stream << "; " << comment << "\n";
        }
    }
}

std::string AssemblyLogger::finalize_log(uint8_t const* start_addr, uint8_t const* end_addr) const {
    static __thread llvm::MCDisassembler* DisAsm = NULL;
    static __thread llvm::MCInstPrinter* IP = NULL;
    static __thread const llvm::MCSubtargetInfo* STI_for_printing = NULL;

    if (!DisAsm) {
        const llvm::Triple& triple = g.tm->getTargetTriple();
        std::string err;
        const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, err);
        assert(target);
        const llvm::MCRegisterInfo* MRI = target->createMCRegInfo(triple.str());
        assert(MRI);
        const llvm::MCAsmInfo* MAI = target->createMCAsmInfo(*MRI, triple.str(), llvm::MCTargetOptions());
        assert(MAI);
        const llvm::MCInstrInfo* MII = target->createMCInstrInfo();
        assert(MII);
        std::string FeaturesStr;
        const llvm::StringRef CPU = "";
        const llvm::MCSubtargetInfo* STI = target->createMCSubtargetInfo(triple.str(), CPU, FeaturesStr);
        assert(STI);
        STI_for_printing = STI;
        int AsmPrinterVariant = MAI->getAssemblerDialect(); // 0 is ATT, 1 is Intel
        IP = target->createMCInstPrinter(triple, AsmPrinterVariant, *MAI, *MII, *MRI);
        assert(IP);
        IP->setPrintImmHex(true);
        llvm::MCContext* Ctx = new llvm::MCContext(triple, MAI, MRI, STI);
        assert(Ctx);
        DisAsm = target->createMCDisassembler(*STI, *Ctx);
        assert(DisAsm);
    }

    std::string result = "";
    llvm::raw_string_ostream stream(result);

    size_t pos = 0;
    append_comments(stream, pos);

    int nnops = 0;
    auto print_nops = [&]() {
        if (nnops == 1)
            stream << start_addr + pos - nnops << "        nop\n";
        else if (nnops)
            stream << start_addr + pos - nnops << "        nop * " << nnops << '\n';
        nnops = 0;
    };

    while (pos < (end_addr - start_addr)) {
        llvm::MCInst inst;
        uint64_t size;
        if (*(start_addr + pos) == 0x90) {
            nnops++;
            pos++;
            append_comments(stream, pos);
            continue;
        } else {
            print_nops();
        }
        llvm::MCDisassembler::DecodeStatus s = DisAsm->getInstruction(
            inst /* out */, size /* out */, llvm::ArrayRef<uint8_t>(start_addr + pos, end_addr), 0, llvm::nulls());
        assert(s == llvm::MCDisassembler::Success);

        stream << start_addr + pos;
        IP->printInst(&inst, 0, "", *STI_for_printing, stream);
        stream << "\n";

        pos += size;
        append_comments(stream, pos);
    }
    print_nops();

    return stream.str();
}
}
}
