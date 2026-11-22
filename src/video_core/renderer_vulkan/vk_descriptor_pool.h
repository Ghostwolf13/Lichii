// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <vector>

#include "common/assert.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Scheduler;

struct DescriptorBank;

struct DescriptorBankInfo {
    [[nodiscard]] bool IsSuperset(const DescriptorBankInfo& subset) const noexcept;

    u32 uniform_buffers{}; ///< Number of uniform buffer descriptors
    u32 storage_buffers{}; ///< Number of storage buffer descriptors
    u32 texture_buffers{}; ///< Number of texture buffer descriptors
    u32 image_buffers{};   ///< Number of image buffer descriptors
    u32 textures{};        ///< Number of texture descriptors
    u32 images{};          ///< Number of image descriptors
    u32 sampled_images{};  ///< Number of sampled image descriptors (without a sampler)
    u32 samplers{};        ///< Number of sampler descriptors
    s32 score{};           ///< Number of descriptors in total
    bool update_after_bind{}; ///< Pools for layouts made for update-after-bind pools
};

class DescriptorAllocator final : public ResourcePool {
    friend class DescriptorPool;

public:
    explicit DescriptorAllocator() = default;
    ~DescriptorAllocator() override = default;

    DescriptorAllocator& operator=(DescriptorAllocator&&) noexcept = default;
    DescriptorAllocator(DescriptorAllocator&&) noexcept = default;

    DescriptorAllocator& operator=(const DescriptorAllocator&) = delete;
    DescriptorAllocator(const DescriptorAllocator&) = delete;

    VkDescriptorSet Commit();

private:
    explicit DescriptorAllocator(const Device& device_, MasterSemaphore& master_semaphore_,
                                 DescriptorBank& bank_, VkDescriptorSetLayout layout_);

    void Allocate(size_t begin, size_t end) override;

    vk::DescriptorSets AllocateDescriptors(size_t count);

    const Device* device{};
    DescriptorBank* bank{};
    VkDescriptorSetLayout layout{};

    std::vector<vk::DescriptorSets> sets;
};

/// Descriptor set and pipeline layouts shared by every pipeline with the same bindings. They live
/// until the device goes; pipelines only hold their handles (one layout per pipeline cost ~28 KiB
/// of driver memory each, with ~90% duplicates in Ocarina of Time).
class PipelineLayoutCache {
public:
    struct Entry {
        vk::DescriptorSetLayout set;
        vk::PipelineLayout pipeline;
    };

    /// Everything layout creation reads that can differ between pipelines on one device: the set
    /// flags, the stages and size of the push constant range (from compute, mesh and mesh
    /// emulation) and each binding. Bindings never carry immutable samplers.
    static std::vector<u32> MakeKey(bool use_push_descriptor, bool use_descriptor_buffer,
                                    bool is_compute, bool uses_mesh, bool mesh_emulation,
                                    std::span<const VkDescriptorSetLayoutBinding> bindings) {
        std::vector<u32> key{static_cast<u32>(use_push_descriptor) |
                             static_cast<u32>(use_descriptor_buffer) << 1 |
                             static_cast<u32>(is_compute) << 2 | static_cast<u32>(uses_mesh) << 3 |
                             static_cast<u32>(mesh_emulation) << 4};
        key.reserve(1 + bindings.size() * 4);
        for (const VkDescriptorSetLayoutBinding& entry : bindings) {
            ASSERT(entry.pImmutableSamplers == nullptr);
            key.insert(key.end(), {entry.binding, static_cast<u32>(entry.descriptorType),
                                   entry.descriptorCount, entry.stageFlags});
        }
        return key;
    }

    template <typename Make>
    const Entry& Get(std::vector<u32> key, Make&& make) {
        std::scoped_lock lock{mutex};
        const auto [it, fresh] = entries.try_emplace(std::move(key));
        if (fresh) {
            try {
                it->second = make();
            } catch (...) {
                entries.erase(it);
                throw;
            }
        }
        return it->second;
    }

private:
    std::mutex mutex;
    std::map<std::vector<u32>, Entry> entries;
};

class DescriptorPool {
public:
    explicit DescriptorPool(const Device& device, Scheduler& scheduler);
    ~DescriptorPool();

    DescriptorPool& operator=(const DescriptorPool&) = delete;
    DescriptorPool(const DescriptorPool&) = delete;

    DescriptorAllocator Allocator(const Device& device, Scheduler& scheduler, VkDescriptorSetLayout layout, std::span<const Shader::Info> infos, bool update_after_bind);
    DescriptorAllocator Allocator(const Device& device, Scheduler& scheduler, VkDescriptorSetLayout layout, const Shader::Info& info, bool update_after_bind);
    DescriptorAllocator Allocator(const Device& device, Scheduler& scheduler, VkDescriptorSetLayout layout, const DescriptorBankInfo& info);

    PipelineLayoutCache& LayoutCache() noexcept {
        return layout_cache;
    }

private:
    PipelineLayoutCache layout_cache;
    DescriptorBank& Bank(const Device& device, const DescriptorBankInfo& reqs);
    std::shared_mutex banks_mutex;
    std::vector<DescriptorBankInfo> bank_infos;
    std::vector<std::unique_ptr<DescriptorBank>> banks;
};

} // namespace Vulkan
