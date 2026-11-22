// SPDX-FileCopyrightText: Copyright 2026 Lichii contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/literals.h"
#include "core/hle/kernel/k_address_space_info.h"

using namespace Common::Literals;

constexpr u64 Xenoblade2Switch2 = 0x0400E95004038001ULL;

TEST_CASE("Xenoblade 2 Switch 2 alias space holds both address reservations", "[kernel][alias]") {
    using Space = Kernel::KAddressSpaceInfo;
    using Type = Space::Type;

    const auto alias_size = Space::GetAddressSpaceSize(39, Type::Alias, Xenoblade2Switch2);
    REQUIRE(alias_size == 128_GiB);
    REQUIRE(alias_size >= 2 * 60_GiB);
    REQUIRE(Space::GetAddressSpaceSize(39, Type::Alias, 0x0100E95004038000ULL) == 64_GiB);
    REQUIRE(Space::GetAddressSpaceSize(39, Type::Alias, 0x0400FF500E34A001ULL) == 64_GiB);
    REQUIRE(Space::GetAddressSpaceSize(39, Type::Alias, 0x0400453019AA8001ULL) == 64_GiB);
    REQUIRE(Space::GetAddressSpaceSize(39, Type::Alias) == 64_GiB);
}

TEST_CASE("Xenoblade 2 alias expansion preserves the other address layouts", "[kernel][alias]") {
    using Space = Kernel::KAddressSpaceInfo;
    using Type = Space::Type;

    REQUIRE(Space::GetAddressSpaceSize(32, Type::Alias, Xenoblade2Switch2) == 1_GiB);
    REQUIRE(Space::GetAddressSpaceSize(36, Type::Alias, Xenoblade2Switch2) == 6_GiB);
    for (const auto type : {Type::Map39Bit, Type::MapSmall, Type::Heap, Type::Stack}) {
        REQUIRE(Space::GetAddressSpaceSize(39, type, Xenoblade2Switch2) ==
                Space::GetAddressSpaceSize(39, type));
    }
}

TEST_CASE("Xenoblade 2 address regions fit the reduced NCE address space", "[kernel][alias]") {
    using Space = Kernel::KAddressSpaceInfo;
    using Type = Space::Type;

    const auto reserved_size = Space::GetAddressSpaceSize(39, Type::Alias, Xenoblade2Switch2) +
                               Space::GetAddressSpaceSize(39, Type::Heap, Xenoblade2Switch2) +
                               Space::GetAddressSpaceSize(39, Type::Stack, Xenoblade2Switch2) +
                               Space::GetAddressSpaceSize(39, Type::MapSmall, Xenoblade2Switch2);
    REQUIRE(reserved_size < 256_GiB - 2_GiB - 16_MiB);
}
