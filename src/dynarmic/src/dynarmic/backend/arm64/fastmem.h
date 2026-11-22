// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <tuple>
#include "common/container/unordered_set.h"

#include "dynarmic/mcl/bit.hpp"
#include "common/common_types.h"
#include "dynarmic/backend/exception_handler.h"
#include "dynarmic/ir/location_descriptor.h"

namespace Dynarmic::Backend::Arm64 {

using DoNotFastmemMarker = std::tuple<IR::LocationDescriptor, unsigned>;

constexpr std::size_t xmrx(std::size_t x) noexcept {
    x ^= x >> 32;
    x *= 0xff51afd7ed558ccd;
    x ^= std::rotr(x, 47) ^ std::rotr(x, 23);
    return x;
}

struct DoNotFastmemMarkerHash {
    [[nodiscard]] std::size_t operator()(const DoNotFastmemMarker& value) const noexcept {
        return xmrx(std::get<0>(value).Value() ^ u64(std::get<1>(value)));
    }
};

struct FastmemPatchInfo {
    DoNotFastmemMarker marker;
    FakeCall fc;
    bool recompile;
};

class FastmemManager {
public:
    /// The JITs of one fastmem arena (each core of a process has its own) share the accesses that
    /// faulted: a fault on one core keeps the others from compiling that access with fastmem and
    /// recompiling it after their own fault.
    FastmemManager(ExceptionHandler& eh, std::optional<std::uintptr_t> arena);

    bool SupportsFastmem() const {
        return exception_handler.SupportsFastmem();
    }

    bool ShouldFastmem(DoNotFastmemMarker marker) const {
        std::shared_lock lock{records->mutex};
        return records->do_not_fastmem.count(marker) == 0;
    }

    void MarkDoNotFastmem(DoNotFastmemMarker marker) {
        std::unique_lock lock{records->mutex};
        records->do_not_fastmem.insert(marker);
    }

private:
    struct Records {
        std::shared_mutex mutex;
        ::Common::unordered_set<DoNotFastmemMarker, DoNotFastmemMarkerHash> do_not_fastmem;
    };

    ExceptionHandler& exception_handler;
    std::shared_ptr<Records> records;
};

}  // namespace Dynarmic::Backend::Arm64
