// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// One input as an NCA: on its own, as an update NCA whose base is missing (as the patch manager
// opens it), and inside an NSP. Plaintext NCA headers and sections need no keys, so the input
// reaches the FS header, integrity, patch and RomFS parsers directly.
// Seed: FUZZ_NCA_SEED=corpus/nca build-fuzz/bin/fuzz_nca writes made-up NCAs (no game data) into
// that directory and exits.

#include "core/file_sys/content_archive.h"
#include "core/file_sys/submission_package.h"
#include "fuzz/fuzz_common.h"
#include "fuzz/fuzz_seeds.h"

namespace {

void Exercise(FileSys::NCA& nca, Fuzz::Budget& budget) {
    if (nca.GetStatus() != Loader::ResultStatus::Success) {
        return;
    }
    (void)nca.GetType();
    (void)nca.GetTitleId();
    (void)nca.GetSDKVersion();
    for (const auto& dir : nca.GetSubdirectories()) {
        Fuzz::Walk(dir, budget);
    }
    Fuzz::ReadRomFS(nca.GetRomFS(), budget);
    Fuzz::Walk(nca.GetExeFS(), budget);
    Fuzz::Walk(nca.GetLogoPartition(), budget);
}

} // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    using namespace Fuzz::Seeds;
    using Type = FileSys::NcaFsHeader::FsType;
    WriteIfAsked("FUZZ_NCA_SEED",
                 {{"program.dnca", ProgramNca()},
                  {"program.nca3", Nca(FileSys::NcaHeader::Magic3,
                                       FileSys::NcaHeader::ContentType::Program, ProgramId,
                                       {{Type::PartitionFs, ExeFS(), true},
                                        {Type::RomFs, RomFS(), true}})},
                  {"meta.dnca", MetaNca(ProgramNca(), "0123456789abcdef0123456789abcdef")}});
    Fuzz::UseSyntheticKeys();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const u8* data, std::size_t size) {
    auto file = std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>(data, data + size),
                                                         "00000000000000000000000000000000.nca");
    Fuzz::Budget budget;
    {
        FileSys::NCA nca{file};
        Exercise(nca, budget);
    }
    {
        FileSys::NCA nca{file, nullptr, true};
        Exercise(nca, budget);
    }
    FileSys::NSP nsp{Fuzz::WrapInPfs0(data, size, "00000000000000000000000000000000.nca")};
    if (nsp.GetStatus() == Loader::ResultStatus::Success) {
        Fuzz::ReadRomFS(nsp.GetRomFS(), budget);
        Fuzz::Walk(nsp.GetExeFS(), budget);
    }
    return 0;
}
