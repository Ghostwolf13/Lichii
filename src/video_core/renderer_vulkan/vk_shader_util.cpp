// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

vk::ShaderModule BuildShader(const Device& device, std::span<const u32> code) {
    return device.GetLogical().CreateShaderModule({
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = static_cast<u32>(code.size_bytes()),
        .pCode = code.data(),
    });
}

vk::ShaderModule BuildGuestShader(const Device& device, std::span<const u32> code, u64 hash) {
    vk::ShaderModule module{device.HasRadvHangReport()
                                ? BuildShader(device, NameSpirvProgram(code, hash))
                                : BuildShader(device, code)};
    if (device.HasDebuggingToolAttached()) {
        const std::string name{fmt::format("Shader {:016x}", hash)};
        module.SetObjectNameEXT(name.c_str());
    }
    return module;
}

} // namespace Vulkan
