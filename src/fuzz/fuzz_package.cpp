// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// One input as a whole NSP and as a whole XCI, the containers games ship in.
// Seed: FUZZ_PACKAGE_SEED=corpus/package build-fuzz/bin/fuzz_package writes a made-up NSP, the same
// NSP with its program as an NCZ, an extracted NSP and an XCI (no game data) into that directory
// and exits.

#include "core/file_sys/card_image.h"
#include "core/file_sys/submission_package.h"
#include "fuzz/fuzz_common.h"
#include "fuzz/fuzz_seeds.h"

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    using namespace Fuzz::Seeds;
    WriteIfAsked("FUZZ_PACKAGE_SEED",
                 {{"seed.nsp", Nsp()},
                  {"ncz.nsp", Nsp(true)},
                  {"extracted.nsp", ExtractedNsp()},
                  {"seed.xci", Xci()}});
    Fuzz::UseSyntheticKeys();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const u8* data, std::size_t size) {
    auto file = std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>(data, data + size),
                                                         "fuzz.bin");
    Fuzz::Budget budget;
    {
        FileSys::NSP nsp{file};
        if (nsp.GetStatus() == Loader::ResultStatus::Success) {
            (void)nsp.GetProgramTitleIDs();
            Fuzz::ReadRomFS(nsp.GetRomFS(), budget);
            Fuzz::Walk(nsp.GetExeFS(), budget);
        }
    }
    {
        FileSys::XCI xci{file};
        if (xci.GetStatus() == Loader::ResultStatus::Success) {
            for (const auto& partition : xci.GetPartitions()) {
                Fuzz::Walk(partition, budget);
            }
            if (const auto secure = xci.GetSecurePartitionNSP()) {
                Fuzz::ReadRomFS(secure->GetRomFS(), budget);
                Fuzz::Walk(secure->GetExeFS(), budget);
            }
        }
    }
    return 0;
}
