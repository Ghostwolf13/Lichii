// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>

#include <catch2/catch_test_macros.hpp>

#include "video_core/vulkan_common/vulkan_address_map.h"

using Vulkan::AddressMap;
using namespace std::chrono_literals;

TEST_CASE("AddressMap names the live binding that holds a fault address", "[video_core]") {
    AddressMap map;
    const AddressMap::Clock::time_point start{};
    map.Bind(0x28c800000, 0x20000, "image", 0x1234, false, start);
    map.Name(0x1234, "Image 0x401000 R8G8B8A8 256x128");
    const std::string text = map.Describe(0x28c812000, start + 30ms);
    REQUIRE(text.find("in live image 0x1234 'Image 0x401000 R8G8B8A8 256x128'") == 0);
    REQUIRE(text.find("at +0x12000") != std::string::npos);
    REQUIRE(text.find("bound 30 ms before") != std::string::npos);
}

TEST_CASE("AddressMap names the freed binding that held a fault address", "[video_core]") {
    AddressMap map;
    const AddressMap::Clock::time_point start{};
    map.Bind(0x299c00000, 0x10000, "buffer", 0x55, false, start);
    map.Name(0x55, "Buffer 0x2aa1000");
    map.Unbind(0x299c00000, 0x10000, 0x55, start + 1000ms);
    // A new object reusing the handle drops the old name.
    map.Bind(0x100000000, 0x1000, "buffer", 0x55, false, start + 1001ms);
    REQUIRE(map.LiveCount() == 1);
    const std::string text = map.Describe(0x299c0d000, start + 1005ms);
    REQUIRE(text.find("in freed buffer 0x55 'Buffer 0x2aa1000'") == 0);
    REQUIRE(text.find("unbound 5 ms before after 1000 ms bound") != std::string::npos);
}

TEST_CASE("AddressMap keeps a freed unnamed binding unnamed when its handle is reused",
          "[video_core]") {
    AddressMap map;
    map.Bind(0x4000, 0x1000, "buffer", 0x77, false);
    map.Unbind(0x4000, 0x1000, 0x77);
    map.Bind(0x9000, 0x1000, "image", 0x77, false);
    map.Name(0x77, "Image 0x500000");
    const std::string text = map.Describe(0x4800);
    REQUIRE(text.find("in freed buffer 0x77 [0x4000, 0x5000)") == 0);
    REQUIRE(text.find("Image 0x500000") == std::string::npos);
}

TEST_CASE("AddressMap lists every live binding over an address", "[video_core]") {
    AddressMap map;
    map.Bind(0x200000000, 0x100000, "device memory", 0x1, false);
    map.Bind(0x200010000, 0x1000, "buffer", 0x2, false);
    const std::string text = map.Describe(0x200010800);
    REQUIRE(text.find("device memory 0x1") != std::string::npos);
    REQUIRE(text.find("buffer 0x2") != std::string::npos);
}

TEST_CASE("AddressMap gives the nearest bindings around an unknown address", "[video_core]") {
    AddressMap map;
    map.Bind(0x1000, 0x1000, "buffer", 0xa, false);
    map.Bind(0x8000, 0x1000, "image", 0xb, true);
    map.Bind(0x9000, 0x1000, "image", 0xc, false);
    const std::string text = map.Describe(0x2800);
    REQUIRE(text.find("in no binding the driver reported") == 0);
    REQUIRE(text.find("0x800 bytes past the end of live buffer 0xa [0x1000, 0x2000)") !=
            std::string::npos);
    REQUIRE(text.find("0x5800 bytes below live driver-internal image 0xb") != std::string::npos);
}
