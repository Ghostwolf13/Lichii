// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// IPS and IPS32 patches, and IPSwitch (.pchtxt) text patches, applied to a small file. The first
// input byte is the size of the patched file in 16-byte units; the rest is the patch.
// Run: build-fuzz/bin/fuzz_ips -max_len=4096 corpus/ips

#include <memory>
#include <vector>

#include "core/file_sys/ips_layer.h"
#include "fuzz/fuzz_common.h"

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const u8* data, std::size_t size) {
    if (size < 1) {
        return 0;
    }
    const std::size_t base_size = std::size_t{data[0]} * 16;
    auto base = std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>(base_size, 0xAA), "main");
    auto patch =
        std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>(data + 1, data + size), "patch");
    Fuzz::Budget budget;
    Fuzz::ReadAll(FileSys::PatchIPS(base, patch), budget);
    const FileSys::IPSwitchCompiler compiler{patch};
    (void)compiler.GetBuildID();
    Fuzz::ReadAll(compiler.Apply(base), budget);
    return 0;
}
