// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;

vk::ShaderModule BuildShader(const Device& device, std::span<const u32> code);

/// A guest program's module, named after its hash for attached debugging tools and, with RADV's
/// GPU hang report on, carrying the hash in its SPIR-V (NameSpirvProgram).
vk::ShaderModule BuildGuestShader(const Device& device, std::span<const u32> code, u64 hash);

/// The module with OpSourceExtension "lichii <hash>" first in its debug section, so a driver's
/// copy of the SPIR-V (RADV's GPU hang report keeps the bound pipelines') names the program.
[[nodiscard]] inline std::vector<u32> NameSpirvProgram(std::span<const u32> code, u64 hash) {
    // Opcodes of the instructions that must come before the debug section.
    static constexpr std::array<u32, 7> preamble_ops{
        17,  // OpCapability
        10,  // OpExtension
        11,  // OpExtInstImport
        14,  // OpMemoryModel
        15,  // OpEntryPoint
        16,  // OpExecutionMode
        331, // OpExecutionModeId
    };
    static constexpr u32 OpSourceExtension = 4;
    static constexpr size_t header_words = 5;

    size_t at = header_words;
    while (at < code.size() && std::ranges::find(preamble_ops, code[at] & 0xffff) !=
                                   preamble_ops.end()) {
        const u32 word_count = code[at] >> 16;
        if (word_count == 0) {
            return {code.begin(), code.end()};
        }
        at += word_count;
    }
    if (at > code.size()) {
        return {code.begin(), code.end()};
    }
    const std::string text = fmt::format("lichii {:016x}", hash);
    // The literal string is nul-terminated and padded to whole words.
    const size_t string_words = text.size() / 4 + 1;
    std::vector<u32> instruction(1 + string_words);
    instruction[0] = static_cast<u32>(instruction.size() << 16) | OpSourceExtension;
    std::memcpy(instruction.data() + 1, text.data(), text.size());

    std::vector<u32> named;
    named.reserve(code.size() + instruction.size());
    named.insert(named.end(), code.begin(), code.begin() + at);
    named.insert(named.end(), instruction.begin(), instruction.end());
    named.insert(named.end(), code.begin() + at, code.end());
    return named;
}

} // namespace Vulkan
