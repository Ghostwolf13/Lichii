// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The metadata readers that take a whole file: NACP, NPDM (main.npdm), CNMT, KIP1 and INI1.
// Run: build-fuzz/bin/fuzz_meta -max_len=32768 -rss_limit_mb=2048 -malloc_limit_mb=2048 corpus/meta
// A KIP header can still ask for a large zero-filled .text, .rodata or .data with no stored bytes;
// those show up as out-of-memory reports, not crashes.

#include <memory>
#include <vector>

#include "core/file_sys/control_metadata.h"
#include "core/file_sys/kernel_executable.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/program_metadata.h"
#include "fuzz/fuzz_common.h"

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const u8* data, std::size_t size) {
    auto file =
        std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>(data, data + size), "fuzz.bin");
    {
        const FileSys::NACP nacp{file};
        (void)nacp.GetApplicationName();
        (void)nacp.GetDeveloperName();
        (void)nacp.GetVersionString();
        (void)nacp.GetSupportedLanguages();
    }
    {
        FileSys::ProgramMetadata npdm;
        (void)npdm.Load(file);
    }
    {
        const FileSys::CNMT cnmt{file};
        (void)cnmt.GetTitleID();
    }
    { const FileSys::KIP kip{file}; }
    { const FileSys::INI ini{file}; }
    return 0;
}
