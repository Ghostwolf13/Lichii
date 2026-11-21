// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstdint>

namespace Vulkan {

[[nodiscard]] constexpr std::uint64_t ClampTexelBufferRange(
    std::uint64_t offset, std::uint64_t requested, std::uint64_t buffer_size,
    std::uint64_t alignment, std::uint32_t block_size, std::uint32_t texels_per_block,
    std::uint32_t max_elements) {
    if (offset >= buffer_size || alignment == 0 || offset % alignment != 0 ||
        block_size == 0 || texels_per_block == 0) {
        return 0;
    }
    const auto blocks = (std::min)({requested / block_size, (buffer_size - offset) / block_size,
                                    std::uint64_t{max_elements / texels_per_block}});
    return blocks * block_size;
}

} // namespace Vulkan
