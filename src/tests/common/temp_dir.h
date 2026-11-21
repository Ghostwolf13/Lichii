// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <random>
#include <string>
#include <string_view>
#include <system_error>

namespace Tests {

/// A folder of its own in the temporary directory for one test, removed when it goes out of
/// scope. Its name ends in a random number and create_directory makes it, so no other test or
/// tests process can be using it, and none is removed first. Removal is best effort: a file a
/// network file system still holds may stay behind.
class TempDir {
public:
    explicit TempDir(std::string_view prefix) {
        std::random_device random;
        do {
            path = std::filesystem::temp_directory_path() /
                   (std::string{prefix} + '-' + std::to_string(random()));
        } while (!std::filesystem::create_directory(path));
    }

    ~TempDir() {
        std::error_code error;
        // A test may have taken the owner's permissions away to check a failure.
        std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add, error);
        std::filesystem::remove_all(path, error);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& Path() const {
        return path;
    }

    std::filesystem::path operator/(const std::filesystem::path& name) const {
        return path / name;
    }

private:
    std::filesystem::path path;
};

} // namespace Tests
