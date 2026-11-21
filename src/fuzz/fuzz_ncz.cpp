// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// NCZ section and NCZBLOCK headers and the decoders behind them. Inputs start with a 0x4000-byte
// NCA header (any bytes), then "NCZSECTN".
// Run: build-fuzz/bin/fuzz_ncz -max_len=32768 -rss_limit_mb=2048 -malloc_limit_mb=2048 corpus/ncz
// Seed: FUZZ_NCZ_SEED=corpus/ncz build-fuzz/bin/fuzz_ncz writes made-up solid, block and AES-CTR
// NCZs (no game data) into that directory and exits.

#include <memory>
#include <vector>

#include "core/crypto/ncz_decompressor.h"
#include "fuzz/fuzz_common.h"
#include "fuzz/fuzz_seeds.h"

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    using namespace Fuzz::Seeds;
    WriteIfAsked("FUZZ_NCZ_SEED", {{"solid.ncz", Ncz(false, 1)},
                                   {"block.ncz", Ncz(true, 1)},
                                   {"ctr.ncz", Ncz(false, 3)}});
    Fuzz::UseScratchHome("fuzz-ncz");
    Fuzz::UseSyntheticKeys();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const u8* data, std::size_t size) {
    auto file =
        std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>(data, data + size), "fuzz.ncz");
    (void)Core::Crypto::IsBlockCompressedNCZ(file);
    (void)Core::Crypto::IsSupportedNCZ(file);
    Fuzz::Budget budget;
    budget.bytes = 4u << 20;
    Fuzz::ReadAll(Core::Crypto::DecompressNCZ(file, "fuzz.nca"), budget);
    return 0;
}
