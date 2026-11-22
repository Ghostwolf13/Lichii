// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <string_view>
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/vulkan_common/vulkan_address_map.h"
#include "video_core/vulkan_common/vulkan_debug_callback.h"
#include "video_core/gpu_logging/gpu_logging.h"

namespace Vulkan {
namespace {

// Helper to get message type as string for GPU logging
const char* GetMessageTypeName(VkDebugUtilsMessageTypeFlagsEXT type) {
    if (type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) {
        return "Validation";
    } else if (type & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) {
        return "Performance";
    } else {
        return "General";
    }
}

VkBool32 DebugUtilCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                           VkDebugUtilsMessageTypeFlagsEXT type,
                           const VkDebugUtilsMessengerCallbackDataEXT* data,
                           [[maybe_unused]] void* user_data) {
    // Skip logging known false-positive validation errors
    switch (static_cast<u32>(data->messageIdNumber)) {
#ifdef __ANDROID__
    case 0xbf9cf353u: // VUID-vkCmdBindVertexBuffers2-pBuffers-04111
    // The below are due to incorrect reporting of extendedDynamicState
    case 0x1093bebbu: // VUID-vkCmdSetCullMode-None-03384
    case 0x9215850fu: // VUID-vkCmdSetDepthTestEnable-None-03352
    case 0x86bf18dcu: // VUID-vkCmdSetDepthWriteEnable-None-03354
    case 0x0792ad08u: // VUID-vkCmdSetStencilOp-None-03351
    case 0x93e1ba4eu: // VUID-vkCmdSetFrontFace-None-03383
    case 0xac9c13c5u: // VUID-vkCmdSetStencilTestEnable-None-03350
    case 0xc9a2001bu: // VUID-vkCmdSetDepthBoundsTestEnable-None-03349
    case 0x8b7159a7u: // VUID-vkCmdSetDepthCompareOp-None-03353
    // The below are due to incorrect reporting of extendedDynamicState2
    case 0xb13c8036u: // VUID-vkCmdSetDepthBiasEnable-None-04872
    case 0xdff2e5c1u: // VUID-vkCmdSetRasterizerDiscardEnable-None-04871
    case 0x0cc85f41u: // VUID-vkCmdSetPrimitiveRestartEnable-None-04866
    case 0x01257b492: // VUID-vkCmdSetLogicOpEXT-None-0486
    // The below are due to incorrect reporting of vertexInputDynamicState
    case 0x398e0dabu: // VUID-vkCmdSetVertexInputEXT-None-04790
    // The below are due to incorrect reporting of extendedDynamicState3
    case 0x970c11a5u: // VUID-vkCmdSetColorWriteMaskEXT-extendedDynamicState3ColorWriteMask-07364
    case 0x6b453f78u: // VUID-vkCmdSetColorBlendEnableEXT-extendedDynamicState3ColorBlendEnable-07355
    case 0xf66469d0u: // VUID-vkCmdSetColorBlendEquationEXT-extendedDynamicState3ColorBlendEquation-07356
    case 0x1d43405eu: // VUID-vkCmdSetLogicOpEnableEXT-extendedDynamicState3LogicOpEnable-07365
    case 0x638462e8u: // VUID-vkCmdSetDepthClampEnableEXT-extendedDynamicState3DepthClampEnable-07448
    // Misc
    case 0xe0a2da61u: // VUID-vkCmdDrawIndexed-format-07753
#else
    case 0x682a878au: // VUID-vkCmdBindVertexBuffers2EXT-pBuffers-parameter
    case 0x99fb7dfdu: // UNASSIGNED-RequiredParameter (vkCmdBindVertexBuffers2EXT pBuffers[0])
    case 0xe8616bf2u: // Bound VkDescriptorSet 0x0[] was destroyed. Likely push_descriptor related
    case 0x1608dec0u: // Image layout in vkUpdateDescriptorSet doesn't match descriptor use
    case 0x55362756u: // Descriptor binding and framebuffer attachment overlap
#endif
        return VK_FALSE;
    default:
        break;
    }
    const std::string_view message{data->pMessage};
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        LOG_CRITICAL(Render_Vulkan, "{}", message);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        LOG_WARNING(Render_Vulkan, "{}", message);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
        LOG_INFO(Render_Vulkan, "{}", message);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) {
        LOG_DEBUG(Render_Vulkan, "{}", message);
    }

    // Route to GPU logger for tracking Vulkan validation messages
    if (GPU::Logging::IsActive() &&
        Settings::values.gpu_log_vulkan_calls.GetValue()) {
        // Convert severity to result code for logging (negative = error)
        int result_code = 0;
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
            result_code = -1;
        } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
            result_code = -2;
        }

        // Get message ID name or use generic name
        const char* call_name = data->pMessageIdName ? data->pMessageIdName : "VulkanDebug";

        GPU::Logging::GPULogger::GetInstance().LogVulkanCall(
            call_name,
            std::string(GetMessageTypeName(type)) + ": " + std::string(message),
            result_code
        );
    }

    return VK_FALSE;
}

const char* ObjectTypeName(VkObjectType type) {
    switch (type) {
    case VK_OBJECT_TYPE_BUFFER:
        return "buffer";
    case VK_OBJECT_TYPE_IMAGE:
        return "image";
    case VK_OBJECT_TYPE_DEVICE_MEMORY:
        return "device memory";
    case VK_OBJECT_TYPE_PIPELINE:
        return "pipeline";
    case VK_OBJECT_TYPE_DESCRIPTOR_POOL:
        return "descriptor pool";
    case VK_OBJECT_TYPE_COMMAND_BUFFER:
        return "command buffer";
    case VK_OBJECT_TYPE_COMMAND_POOL:
        return "command pool";
    case VK_OBJECT_TYPE_QUERY_POOL:
        return "query pool";
    case VK_OBJECT_TYPE_SWAPCHAIN_KHR:
        return "swapchain";
    default:
        return "object";
    }
}

VkBool32 AddressBindingCallback(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                VkDebugUtilsMessageTypeFlagsEXT type,
                                const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (!(type & VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT) ||
        data->objectCount == 0) {
        return VK_FALSE;
    }
    for (auto* next = static_cast<const VkBaseInStructure*>(data->pNext); next;
         next = next->pNext) {
        if (next->sType != VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT) {
            continue;
        }
        const auto& binding = *reinterpret_cast<const VkDeviceAddressBindingCallbackDataEXT*>(next);
        const VkDebugUtilsObjectNameInfoEXT& object = data->pObjects[0];
        AddressMap& map = AddressMap::Get();
        if (binding.bindingType == VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT) {
            map.Bind(binding.baseAddress, binding.size, ObjectTypeName(object.objectType),
                     object.objectHandle,
                     (binding.flags & VK_DEVICE_ADDRESS_BINDING_INTERNAL_OBJECT_BIT_EXT) != 0);
        } else {
            map.Unbind(binding.baseAddress, binding.size, object.objectHandle);
        }
    }
    return VK_FALSE;
}

} // Anonymous namespace

vk::DebugUtilsMessenger CreateDebugUtilsCallback(const vk::Instance& instance) {
    return instance.CreateDebugUtilsMessenger(VkDebugUtilsMessengerCreateInfoEXT{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .pNext = nullptr,
        .flags = 0,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = DebugUtilCallback,
        .pUserData = nullptr,
    });
}

vk::DebugUtilsMessenger CreateAddressBindingCallback(const vk::Instance& instance,
                                                     const vk::InstanceDispatch& dld) {
    const auto properties = vk::EnumerateInstanceExtensionProperties(dld);
    if (!properties || std::ranges::none_of(*properties, [](const VkExtensionProperties& p) {
            return std::string_view{p.extensionName} == VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        })) {
        LOG_WARNING(Render_Vulkan, "EDEN_NX2_FAULT_MAP: the Vulkan loader lacks {}",
                    VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        return {};
    }
    return instance.CreateDebugUtilsMessenger(VkDebugUtilsMessengerCreateInfoEXT{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .pNext = nullptr,
        .flags = 0,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT,
        .pfnUserCallback = AddressBindingCallback,
        .pUserData = nullptr,
    });
}

} // namespace Vulkan
