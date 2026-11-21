// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cctype>

#include <openssl/evp.h>

#include "core/file_sys/vfs/vfs.h"
#include "core/hle/service/bcat/bcat_result.h"
#include "core/hle/service/bcat/bcat_types.h"

namespace Service::BCAT {

// For a name to be valid it must be non-empty, must have a null terminating character as the final
// char, can only contain numbers, letters, underscores and a hyphen if directory and a period if
// file.
constexpr Result VerifyNameValidInternal(std::array<char, 0x20> name, char match_char) {
    const auto null_chars = std::count(name.begin(), name.end(), 0);
    const auto bad_chars = std::count_if(name.begin(), name.end(), [match_char](char c) {
        return !std::isalnum(static_cast<u8>(c)) && c != '_' && c != match_char && c != '\0';
    });
    if (null_chars == 0x20 || null_chars == 0 || bad_chars != 0 || name[0x1F] != '\0') {
        LOG_ERROR(Service_BCAT, "Name passed was invalid!");
        return ResultInvalidArgument;
    }

    return ResultSuccess;
}

constexpr Result VerifyNameValidDir(DirectoryName name) {
    return VerifyNameValidInternal(name, '-');
}

constexpr Result VerifyNameValidFile(FileName name) {
    return VerifyNameValidInternal(name, '.');
}

// The digest is only used to determine if a file is unique compared to others of the same name.
// Since the algorithm isn't ever checked in game, MD5 is safe.
inline BcatDigest DigestFile(const FileSys::VirtualFile& file) {
    BcatDigest out{};
    const auto bytes = file->ReadAllBytes();

    u32 hash_len = 0;
    EVP_Digest(bytes.data(), bytes.size(), out.data(), &hash_len, EVP_md5(), nullptr);

    return out;
}

} // namespace Service::BCAT
