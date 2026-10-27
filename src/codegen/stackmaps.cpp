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

#include "codegen/stackmaps.h"

#include <cstdio>
#include <iostream>
#include <unordered_map>

#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/JITEventListener.h"
#include "llvm/Object/ObjectFile.h"

#include "codegen/codegen.h"
#include "core/options.h"

//#undef VERBOSITY
//#define VERBOSITY() 2

namespace pyston {

// TODO shouldn't be recording this in a global variable
static uint64_t stackmap_address = 0;

std::unique_ptr<StackMap> parseStackMap() {
    if (!stackmap_address)
        return NULL;

    if (VERBOSITY() >= 3)
        printf("Found the stackmaps at stackmap_address 0x%lx\n", stackmap_address);

    std::unique_ptr<StackMap> cur_map = std::unique_ptr<StackMap>(new StackMap());

    union {
        const int8_t* i8;
        const int16_t* i16;
        const int32_t* i32;
        const int64_t* i64;
        const uint8_t* u8;
        const uint16_t* u16;
        const uint32_t* u32;
        const uint64_t* u64;
        const StackMap::Record::Location* record_loc;
        const StackMap::Record::LiveOut* record_liveout;
        const StackMap::StackSizeRecord* size_record;
    } ptr;
    const int8_t* start_ptr = ptr.i8 = (const int8_t*)stackmap_address;

    // This parses version 3 of the stack map format, see llvm/docs/StackMaps.rst
    cur_map->header = *ptr.u32++; // header
    RELEASE_ASSERT((cur_map->header & 0xff) == 3, "unsupported stackmap version %d", cur_map->header & 0xff);

    int nfunctions = *ptr.u32++;
    int nconstants = *ptr.u32++;
    int nrecords = *ptr.u32++;

    if (VERBOSITY() >= 3)
        printf("%d functions\n", nfunctions);
    cur_map->stack_size_records.reserve(nfunctions);
    for (int i = 0; i < nfunctions; i++) {
        StackMap::StackSizeRecord size_record;
        size_record.offset = *ptr.u64++;
        size_record.stack_size = *ptr.u64++;
        ptr.u64++; // record count
        cur_map->stack_size_records.push_back(size_record);
        if (VERBOSITY() >= 3)
            printf("function %d: offset 0x%lx, stack size 0x%lx\n", i, size_record.offset, size_record.stack_size);
    }

    if (VERBOSITY() >= 3)
        printf("%d constants\n", nconstants);
    cur_map->constants.reserve(nconstants);

    for (int i = 0; i < nconstants; i++) {
        uint64_t constant = *ptr.u64++;
        if (VERBOSITY() >= 3)
            printf("Constant %d: %ld\n", i, constant);
        cur_map->constants.push_back(constant);
    }

    if (VERBOSITY() >= 3)
        printf("%d records\n", nrecords);
    cur_map->records.reserve(nrecords);

    for (int i = 0; i < nrecords; i++) {
        cur_map->records.emplace_back();
        StackMap::Record* record = &cur_map->records.back();

        record->id = *ptr.u64++;
        record->offset = *ptr.u32++;
        record->flags = *ptr.u16++; // reserved (record flags)

        int numlocations = *ptr.u16++;
        record->locations.reserve(numlocations);

        if (VERBOSITY() >= 3)
            printf("Stackmap record %ld at 0x%x has %d locations:\n", record->id, record->offset, numlocations);
        for (int j = 0; j < numlocations; j++) {
            StackMap::Record::Location r;
            r.type = (StackMap::Record::Location::LocationType)*ptr.u8++;
            ptr.u8++; // reserved
            int size = *ptr.u16++;
            r.regnum = *ptr.u16++;
            ptr.u16++; // reserved
            r.offset = *ptr.i32++;
            // Pyston's Location keeps the size in what used to be the flags byte.
            assert(size < 256);
            r.flags = size;
            record->locations.push_back(r);

            // from http://lxr.free-electrons.com/source/tools/perf/arch/x86/util/dwarf-regs.c
            // TODO this probably can be fetched more portably from the llvm target files
            const char* dwarf_reg_names[] = {
                "%rax", "%rdx", "%rcx", "%rbx", "%rsi", "%rdi", "%rbp", "%rsp",
                "%r8",  "%r9",  "%r10", "%r11", "%r12", "%r13", "%r14", "%r15",
            };
            if (VERBOSITY() >= 3) {
                if (r.type == 1) {
                    printf("Location %d: type %d (reg), reg %d (%s), offset %d\n", j, r.type, r.regnum,
                           dwarf_reg_names[r.regnum], r.offset);
                } else {
                    printf("Location %d: type %d, reg %d, offset %d\n", j, r.type, r.regnum, r.offset);
                }
            }
        }

        // pad to 8-byte boundary
        if ((ptr.i8 - start_ptr) % 8)
            ptr.u32++;

        ptr.u16++; // padding
        int num_live_outs = *ptr.u16++;
        record->live_outs.reserve(num_live_outs);
        for (int i = 0; i < num_live_outs; i++) {
            const StackMap::Record::LiveOut& r = *ptr.record_liveout++;
            record->live_outs.push_back(r);

            if (VERBOSITY() >= 3) {
                printf("Live out %d: reg #%d (?), size %d\n", i, r.regnum, r.size);
            }
        }
        // pad to 8-byte boundary
        if ((ptr.i8 - start_ptr) % 8)
            ptr.u32++;
    }

    stackmap_address = 0;
    return cur_map;
}

class StackmapJITEventListener : public llvm::JITEventListener {
public:
    void notifyObjectLoaded(ObjectKey key, const llvm::object::ObjectFile& Obj,
                            const llvm::RuntimeDyld::LoadedObjectInfo& L) override {
        for (const auto& sec : Obj.sections()) {
            llvm::Expected<llvm::StringRef> name = sec.getName();
            RELEASE_ASSERT(name, "");

            if (*name == ".llvm_stackmaps") {
                assert(stackmap_address == 0);
                stackmap_address = L.getSectionLoadAddress(sec);
                assert(stackmap_address > 0);
            }
        }
    }
};

// LLVM will silently not register the eh frames with libgcc if these functions don't exist;
// make sure that these functions exist.
// TODO I think this breaks it for windows, which apparently loads these dynamically?
// see llvm/lib/ExecutionEngine/RTDyldMemoryManager.cpp
extern "C" void __register_frame(void*);
extern "C" void __deregister_frame(void*);
extern void _force_link() {
    __register_frame(nullptr);
    __deregister_frame(nullptr);
}

llvm::JITEventListener* makeStackMapListener() {
    return new StackmapJITEventListener();
}
}
