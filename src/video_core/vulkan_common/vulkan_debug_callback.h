// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

vk::DebugUtilsMessenger CreateDebugUtilsCallback(const vk::Instance& instance);

/// Feeds AddressMap (EDEN_NX2_FAULT_MAP) with the driver's device address binding reports.
vk::DebugUtilsMessenger CreateAddressBindingCallback(const vk::Instance& instance,
                                                     const vk::InstanceDispatch& dld);

} // namespace Vulkan
