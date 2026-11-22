// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
// TODO: find out which don't require stable iters
#include <unordered_map>
#include <unordered_set>
#include "video_core/page_filter.h"
#include "common/container/unordered_map.h"
#include "common/container/unordered_set.h"
#include <vector>
#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>
#include <queue>

#include "common/common_types.h"
#include "common/spin_recursive_mutex.h"
#include "common/hash.h"
#include "common/literals.h"
#include "common/lru_cache.h"
#include <ranges>
#include "common/scratch_buffer.h"
#include "common/slot_vector.h"
#include "video_core/texture_cache/pending_download.h"
#include "common/thread_worker.h"
#include "video_core/compatible_formats.h"
#include "video_core/control/channel_state_cache.h"
#include "video_core/delayed_destruction_ring.h"
#include "video_core/engines/fermi_2d.h"
#include "video_core/game_dlss_status.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/descriptor_table.h"
#include "video_core/texture_cache/image_base.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/image_view_base.h"
#include "video_core/texture_cache/render_targets.h"
#include "video_core/texture_cache/sampled_views.h"
#include "video_core/texture_cache/stale_pages.h"
#include "video_core/texture_cache/types.h"
#include "video_core/texture_cache/writeback_ranges.h"
#include "video_core/textures/texture.h"
#include "video_core/textures/unswizzle_worker.h"

namespace Tegra {
namespace Control {
struct ChannelState;
}
} // namespace Tegra

namespace VideoCommon {

using Tegra::Texture::TICEntry;
using Tegra::Texture::TSCEntry;
using VideoCore::Surface::PixelFormat;
using namespace Common::Literals;

struct ImageViewInOut {
    u32 index{};
    bool blacklist{};
    ImageViewId id{};
};

/// A texture handle word a draw read, and where.
struct TextureHandleRead {
    GPUVAddr address{};
    u32 raw{};
};

/// What a draw's sampled textures were resolved from, kept by the pipeline that drew them. The
/// next draw with the same pipeline resolves its textures from it (ResolveMemoizedTextures) while
/// every byte they came from is the same: the handle words, the texture and sampler descriptors
/// and the tables, mappings and views behind them.
struct SampledTextureMemo {
    struct Entry {
        GPUVAddr handle_address{};
        const u8* handle_host{}; ///< The handle word in host memory
        const u8* tic_host{};
        const u8* tsc_host{};
        TICEntry tic{};
        TSCEntry tsc{};
        u32 raw{};        ///< The handle word
        u32 view_index{}; ///< The texture header index the handle gives
        ImageViewId view{};
        SamplerId sampler{};
    };
    std::vector<Entry> entries;
    const void* memory{};      ///< The address space the pointers belong to
    const void* image_table{}; ///< The channel's table the views were read from
    u64 host_serial{};         ///< MemoryManager::HostPointerSerial
    u64 image_changes{};       ///< DescriptorTable::changes of both tables
    u64 sampler_changes{};
    bool via_header_index{}; ///< The sampler binding the handles were split with
    bool valid{};
    u32 misses{}; ///< Draws in a row that did not find the memo good
};

struct AsyncDecodeContext {
    ImageId image_id;
    Common::ScratchBuffer<u8> decoded_data;
    boost::container::small_vector<BufferImageCopy, 16> copies;
    std::mutex mutex;
    std::atomic_bool complete;
    bool cancelled{}; ///< The image was deleted meanwhile: drop the result (GPU thread only).
};

using TextureCacheGPUMap = ::Common::unordered_map<u64, std::vector<ImageId>, Common::IdentityHash<u64>>;

class TextureCacheChannelInfo : public ChannelInfo {
public:
    TextureCacheChannelInfo() = delete;
    TextureCacheChannelInfo(Tegra::Control::ChannelState& state) noexcept;
    TextureCacheChannelInfo(const TextureCacheChannelInfo& state) = delete;
    TextureCacheChannelInfo& operator=(const TextureCacheChannelInfo&) = delete;

    DescriptorTable<TICEntry, ImageViewId> graphics_image_table;
    DescriptorTable<TSCEntry, SamplerId> graphics_sampler_table;
    DescriptorTable<TICEntry, ImageViewId> compute_image_table;
    DescriptorTable<TSCEntry, SamplerId> compute_sampler_table;

    // TODO: still relies on bad iterators :(
    std::unordered_map<TICEntry, ImageViewId> image_views;
    std::unordered_map<TSCEntry, SamplerId> samplers;

    TextureCacheGPUMap* gpu_page_table = nullptr;
    TextureCacheGPUMap* sparse_page_table = nullptr;
};

template <class P>
class TextureCache : public VideoCommon::ChannelSetupCaches<TextureCacheChannelInfo> {
    friend struct TextureCacheTestAccess;

    /// Address shift for caching images into a hash table
    static constexpr u64 LICHII_PAGEBITS = 20;

    /// Enables debugging features to the texture cache
    static constexpr bool ENABLE_VALIDATION = P::ENABLE_VALIDATION;
    /// Implement blits as copies between framebuffers
    static constexpr bool FRAMEBUFFER_BLITS = P::FRAMEBUFFER_BLITS;
    /// True when some copies have to be emulated
    static constexpr bool HAS_EMULATED_COPIES = P::HAS_EMULATED_COPIES;
    /// True when the API can provide info about the memory of the device.
    static constexpr bool HAS_DEVICE_MEMORY_INFO = P::HAS_DEVICE_MEMORY_INFO;
    /// True when the API can do asynchronous texture downloads.
    static constexpr bool IMPLEMENTS_ASYNC_DOWNLOADS = P::IMPLEMENTS_ASYNC_DOWNLOADS;

    static constexpr size_t UNSET_CHANNEL{(std::numeric_limits<size_t>::max)()};

#ifdef LICHII_LEGACY
    static constexpr s64 TARGET_THRESHOLD = 3_GiB;
#else
    static constexpr s64 TARGET_THRESHOLD = 4_GiB;
#endif

    // Periodic statistics (TickFrame).
    u64 gc_deletes{};
    u64 gc_costly_deletes{};
    u64 gc_download_deletes{}; ///< Evictions that downloaded the image first (a full GPU wait)
    u64 async_decodes_queued{};
    u64 images_created{};
    u64 images_uploaded{};   ///< Image contents uploaded from guest memory (last 600 frames)
    u64 uploaded_bytes{};    ///< Guest bytes those uploads read
    u64 largest_upload{};    ///< Largest of them
    u64 layer_uploads{};     ///< Of them, uploads of only the written layers
    bool native_thresholds_applied{};
    u64 native_critical_memory{};
    u64 native_expected_memory{};
    u64 budget_check_frames{};
    /// Usage under which the collector keeps recently used images (EvictionAge): the driver's
    /// budget less the out-of-memory and block slack margins, within the host's commit bound.
    /// Not less the base margin too: a Mario Kart World race on a 7.6 GB budget then reached it
    /// (6.48 GB) and the collector deleted ~40 recently used images a frame again; images that
    /// do not fit add to the out-of-memory margin.
    u64 retention_limit{};
    u64 retention_budget{}; ///< The lowest driver budget read lately (RetentionBudget)
    u64 logged_critical_memory{}; ///< The critical threshold of the last thresholds line logged
    u32 seen_pressure_events{};   ///< Allocator pressure events already answered
    u32 seen_failed_images{};     ///< Images the allocator could not make, already collected for
    u64 pressure_margin{};        ///< Extra room under the budget after images did not fit
    u64 pressure_quiet_frames{};  ///< Frames since the last such image
    u64 slack_margin{};           ///< Extra room for allocator block space the driver counts as used
    // Device-local memory the allocator placed in host memory (heap over budget): see TickFrame.
    u64 spill_margin{};           ///< Extra room for it, sized to what spilled (at most 768 MiB)
    u64 spill_window_bytes{};     ///< Spilled in the current window
    u64 spill_window_start{};     ///< frame_tick the current window began at
    u64 spill_quiet_frames{};     ///< Frames since the last spill
    u64 spill_collect_tick{};     ///< frame_tick of the first forced collect of this episode + 1
    u32 spill_forced_collects{};  ///< Forced collects in this episode
    u64 spill_last_collect{};     ///< frame_tick of the last forced collect
    u64 spill_log_bytes{};        ///< Spilled since the last spill log line
    u32 spill_log_collects{};     ///< Forced collects since the last spill log line
    std::chrono::steady_clock::time_point spill_log_time{};
    u64 astc_opaque_images{};
    u64 astc_alpha_images{};
    Common::ScratchBuffer<u8> astc_scan_buffer;
    u64 images_deleted{};
    /// Why the next DeleteImage happens, for EDEN_NX2_TC_TRACE: the collector (and its mode),
    /// an unmap or a join. Read and reset by DeleteImage.
    const char* trace_delete_reason = "other";
    /// The collector's deleted image: its flags before UnregisterImage cleared BadOverlap, and
    /// whether its contents were written back to guest memory. Read and reset by DeleteImage.
    u32 trace_delete_flags{};
    bool trace_delete_downloaded{};
    u64 scale_epoch{};
    std::map<u64, std::pair<u32, GPUVAddr>> created_shapes;
    bool debug_stats{}; ///< Collect what only the debug statistics lines use
    /// Render targets their lookup created, to check for early deletion (EvictDeadRenderTargets),
    /// with their allocation tick and how often they were found still in use.
    struct TransientTarget {
        ImageId id;
        u64 allocation_tick;
        u64 check_tick;
        u32 survived;
    };
    std::deque<TransientTarget> transient_targets;
    /// Most candidates kept waiting for early deletion, over budget or not.
    static constexpr size_t MaxTransientTargets = 8192;
    u64 transient_evictions{};
    u64 logged_transient_evictions{};
    /// Image made by the latest JoinImages call; FindRenderTargetView uses it to tell a target it
    /// just created from one that already existed.
    ImageId last_created_image{};
    /// Deletes dead render targets early while the cache is over its expected size.
    void EvictDeadRenderTargets();
    /// EDEN_NX2_MEM_TRACE: when the last memory trace lines were logged.
    std::chrono::steady_clock::time_point mem_trace_time{};

    static constexpr s64 DEFAULT_EXPECTED_MEMORY = 1_GiB + 125_MiB;
    static constexpr s64 DEFAULT_CRITICAL_MEMORY = 1_GiB + 625_MiB;
    /// The most device memory the collector lets the cache fill before it collects in earnest on
    /// cards up to 8 GiB; larger ones 5/8 of the driver's budget (ApplyThresholdOverrides).
    static constexpr u64 MaxCriticalMemory = 5_GiB;
    static constexpr size_t GC_EMERGENCY_COUNTS = 2;

    using Runtime = typename P::Runtime;
    using Image = typename P::Image;
    using ImageAlloc = typename P::ImageAlloc;
    using ImageView = typename P::ImageView;
    using Sampler = typename P::Sampler;
    using Framebuffer = typename P::Framebuffer;
    using AsyncBuffer = typename P::AsyncBuffer;
    using BufferType = typename P::BufferType;

    static constexpr u32 LayerUploadMinSize = 64U << 20;
    static constexpr size_t LargeScratchCapacity = 64U << 20;

    /// Splits large 2D image uploads into smaller chunks, avoiding multi-hundred MiB staging buffers.
    bool UploadInChunks(Image& image);

    /// Large layered images whose refresh can decode only the layers written since the last one.
    [[nodiscard]] bool UploadsByLayer(const ImageBase& image) const noexcept;

    /// Records the layers a write of [cpu_addr, +size) touches.
    void MarkLayersWritten(Image& image, DAddr cpu_addr, size_t size);

    /// SparseTiles images: notes a written range, uploaded with the next refresh.
    void MarkRangeWritten(Image& image, DAddr cpu_addr, size_t size);

    /// What a CPU write of [cpu_addr, +size) does to an image over it.
    void MarkImageWritten(ImageId image_id, Image& image, DAddr cpu_addr, size_t size);

    /// SparseTiles images: binds and uploads the parts written since the last upload.
    void RefreshSparseTiles(Image& image);

    /// Image::BindTiles, counting what it binds in total_used_memory.
    bool BindSparseTiles(Image& image, std::span<const LevelRect> rects);

    /// Image::BackTilesWhole, counting what it binds in total_used_memory.
    bool BackSparseTilesWhole(Image& image);

    /// Uploads the image's dirty layers. False when it needs a full upload instead.
    bool UploadWrittenLayers(Image& image);

    /// Gives the written layers of a SparseLayers image memory and uploads them.
    void RefreshSparseLayers(Image& image);

    /// Image::BindLayers, with the memory it binds counted in total_used_memory.
    std::vector<u32> BindSparseLayers(Image& image, std::span<const u32> layers);

    /// Uploads `layers` of the image from guest memory.
    void UploadLayers(Image& image, std::span<const u32> layers);

    struct PendingUnswizzle {
        ImageId image_id;
        VideoCommon::ImageInfo info;
        size_t current_offset = 0;
        size_t total_size = 0;
        AsyncBuffer staging_buffer;
        size_t last_submitted_offset = 0;
        size_t bytes_per_slice;
        bool initialized = false;
    };

    struct BlitImages {
        ImageId dst_id;
        ImageId src_id;
        PixelFormat dst_format;
        PixelFormat src_format;
    };

public:
    /// Device pages this cache ever registered objects on (InnerInvalidation skips the rest).
    VideoCommon::PageFilter page_filter;

    explicit TextureCache(Runtime&, Tegra::MaxwellDeviceMemoryManager&);

    /// Notify the cache that a new frame has been queued
    void TickFrame();

    /// Return a constant reference to the given image view id
    [[nodiscard]] const ImageView& GetImageView(ImageViewId id) const noexcept;

    /// Return a reference to the given image view id
    [[nodiscard]] ImageView& GetImageView(ImageViewId id) noexcept;

    /// Get the imageview from the graphics descriptor table in the specified index
    [[nodiscard]] ImageView& GetImageView(u32 index) noexcept;

    // Publish the CPU-readable collision image. Caller holds the cache mutex.
    bool PublishComputeCollisionImage(u32 index);

    /// Mark an image as modified from the GPU
    void MarkModification(ImageId id) noexcept;

    /// Mark the image behind a view a shader writes through as a storage image
    void MarkStorageWrite(ImageViewId id) noexcept;

    /// Whether a view's image is absent or holds a shader's storage writes over every level it
    /// covers, with nothing newer (no clear, render pass, copy or CPU write since).
    [[nodiscard]] bool StorageViewHoldsShaderWrites(ImageViewId id) const noexcept;

    /// Fill image_view_ids with the graphics/compute images in indices
    void FillImageViews(std::span<ImageViewInOut> views, bool compute, bool blacklist = true);

    /// Whether the handle words and descriptors `memo` was built from still match guest memory.
    [[nodiscard]] bool TextureMemoStillHolds(const SampledTextureMemo& memo,
                                             std::span<const GPUVAddr> handle_addresses) const;

    /// Fills views and samplers of a draw from the pipeline's memo when nothing they were
    /// resolved from has changed since (true), prepares the images as FillImageViews does.
    /// handle_addresses are where the draw reads its texture handles from, in binding order.
    [[nodiscard]] bool ResolveMemoizedTextures(SampledTextureMemo& memo,
                                               std::span<const GPUVAddr> handle_addresses,
                                               std::span<ImageViewInOut> views,
                                               std::span<SamplerId> samplers);

    /// Keeps what FillImageViews and GetSamplerId resolved the draw's textures from.
    void MemoizeTextures(SampledTextureMemo& memo, std::span<const TextureHandleRead> reads,
                         std::span<const ImageViewInOut> views, std::span<const SamplerId> samplers);

    /// Handle feedback loops during draws.
    void CheckFeedbackLoop(std::span<const ImageViewInOut> views);

    [[nodiscard]] bool SamplesRenderTarget(std::span<const ImageViewInOut> views);

    /// Whether sampling the views now needs PrepareImageView: an image with an upload pending or
    /// a newer alias to copy in (fast draw delta draws keep their views and prepare none).
    [[nodiscard]] bool SampledViewsChange(std::span<const ImageViewInOut> views) const noexcept {
        return VideoCommon::SampledViewsChange(views, slot_image_views, slot_images);
    }

    /// EDEN_NX2_TEXTURE_MEMO_VERIFY: PrepareImage adds the images it is asked to prepare to `log`
    /// (nullptr stops it) and, with `dry`, does nothing else.
    void LogPrepares(std::vector<ImageId>* log, bool dry) noexcept {
        prepare_log = log;
        prepare_log_dry = dry;
    }

    /// Get the sampler from the graphics/compute descriptor table in the specified index
    Sampler* GetSampler(u32 index, bool compute);

    /// Get the sampler id from the graphics/compute descriptor table in the specified index
    SamplerId GetSamplerId(u32 index, bool compute);

    /// Return a constant reference to the given sampler id
    [[nodiscard]] const Sampler& GetSampler(SamplerId id) const noexcept;

    /// Return a reference to the given sampler id
    [[nodiscard]] Sampler& GetSampler(SamplerId id) noexcept;

    /// Refresh the state for graphics/compute image view and sampler descriptors
    void SynchronizeDescriptors(bool compute);

    /// Updates the Render Targets if they can be rescaled
    /// @retval True if the Render Targets have been rescaled.
    bool RescaleRenderTargets();

    /// Update bound render targets and upload memory if necessary
    /// @param is_clear True when the render targets are being used for clears
    void UpdateRenderTargets(bool is_clear);

    /// Find a framebuffer with the currently bound render targets
    /// UpdateRenderTargets should be called before this
    Framebuffer* GetFramebuffer();

    /// Whether render target `index` was first written in an earlier frame: a target the title
    /// draws every frame, rather than one it fills once (Eglantine #874).
    [[nodiscard]] bool ColorTargetWrittenBeforeThisFrame(u32 index) noexcept;

    [[nodiscard]] const RenderTargets& GetRenderTargets() const noexcept {
        return render_targets;
    }

    [[nodiscard]] Runtime& GetRuntime() noexcept {
        return runtime;
    }

    /// Images created, deleted and created from overlapping ones (joins), invalidations of every
    /// channel's TIC tables, and TIC or TSC tables moved, since the last call.
    struct Churn {
        u64 created{};
        u64 deleted{};
        u64 joins{};
        u64 invalidations{};
        u64 refreshes{};
    };
    Churn TakeChurn() noexcept {
        return std::exchange(churn, {});
    }

    /// Mark images in a range as modified from the CPU
    void WriteMemory(DAddr cpu_addr, size_t size);

    /// After a DMA copy of [src_addr, +size) to [dest_addr, +size) in guest memory: marks the
    /// images over the destination whose contents guest memory now holds. gpu_written_dest are
    /// the destination ranges that took buffer cache GPU writes, which guest memory lacks.
    void WriteDmaCopy(GPUVAddr src_addr, GPUVAddr dest_addr, size_t size,
                      std::span<const std::pair<DAddr, size_t>> gpu_written_dest);

    /// Download contents of host images to guest memory in a region
    void DownloadMemory(DAddr cpu_addr, size_t size);

    std::optional<VideoCore::RasterizerDownloadArea> GetFlushArea(DAddr cpu_addr, u64 size);

    /// Remove images in a region
    void UnmapMemory(DAddr cpu_addr, size_t size);

    /// Remove images in a region
    void UnmapGPUMemory(size_t as_id, GPUVAddr gpu_addr, size_t size);

    /// Blit an image with the given parameters
    bool CopyVolumeSlice(const Tegra::Engines::Fermi2D::Surface& dst,
                         const Tegra::Engines::Fermi2D::Surface& src,
                         const Tegra::Engines::Fermi2D::Config& copy);

    bool BlitImage(const Tegra::Engines::Fermi2D::Surface& dst,
                   const Tegra::Engines::Fermi2D::Surface& src,
                   const Tegra::Engines::Fermi2D::Config& copy);

    /// Stand-in for a DLSS evaluation: temporal upscale of src into dst (see the runtime's
    /// TemporalUpscale). False when unsupported; the caller retains its native dispatch.
    bool TemporalUpscale(const Tegra::Engines::Fermi2D::Surface& dst,
                         const Tegra::Engines::Fermi2D::Surface& src,
                         const Tegra::Engines::Fermi2D::Surface& motion_vectors,
                         const Tegra::Texture::TICEntry* depth, std::array<f32, 2> jitter,
                         std::array<f32, 2> motion_scale, bool use_history,
                         std::optional<std::array<u32, 2>> render_region = std::nullopt);

    /// Health check of a game's DLSS network output (dst) against its input (src); see the
    /// runtime's ProbeGameDlssOutput. Returns an earlier check's result when one is ready.
    std::optional<VideoCore::GameDlssProbe> ProbeGameDlssOutput(
        const Tegra::Engines::Fermi2D::Surface& dst, const Tegra::Engines::Fermi2D::Surface& src,
        std::optional<std::array<u32, 2>> src_region = std::nullopt);

    /// Try to find a cached image view in the given CPU address
    [[nodiscard]] std::pair<ImageView*, bool> TryFindFramebufferImageView(
        const Tegra::FramebufferConfig& config, DAddr cpu_addr);

    /// Return true when there are uncommitted images to be downloaded
    [[nodiscard]] bool HasUncommittedFlushes() const noexcept;

    /// Whether CommitAsyncFlushes would commit anything (downloads or their buffers).
    [[nodiscard]] bool HasAsyncFlushWork() const noexcept;

    /// Return true when the caller should wait for async downloads
    [[nodiscard]] bool ShouldWaitAsyncFlushes() const noexcept;

    /// Commit asynchronous downloads
    void CommitAsyncFlushes();

    /// Pop asynchronous downloads
    void PopAsyncFlushes();

    [[nodiscard]] ImageId DmaImageId(const Tegra::DMA::ImageOperand& operand, bool is_upload);

    [[nodiscard]] std::pair<Image*, BufferImageCopy> DmaBufferImageCopy(
        const Tegra::DMA::ImageCopy& copy_info, const Tegra::DMA::BufferOperand& buffer_operand,
        const Tegra::DMA::ImageOperand& image_operand, ImageId image_id, bool modifies_image);

    void DownloadImageIntoBuffer(Image* image, BufferType buffer, size_t buffer_offset,
                                 std::span<const VideoCommon::BufferImageCopy> copies,
                                 GPUVAddr address = 0, size_t size = 0);

    /// Return true when a CPU region is modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(DAddr addr, size_t size);

    [[nodiscard]] bool IsRescaling() const noexcept;

    [[nodiscard]] bool IsRescaling(const ImageViewBase& image_view) const noexcept;

    /// Counts InvalidateScale calls: an image changed its scale and lost its views.
    [[nodiscard]] u64 ScaleEpoch() const noexcept {
        return scale_epoch;
    }

    /// Create channel state.
    void CreateChannel(Tegra::Control::ChannelState& channel) final override;

    /// Prepare an image to be used
    void PrepareImage(ImageId image_id, bool is_modification, bool invalidate);

    Common::SpinRecursiveMutex mutex;

private:
    /// Iterate over all page indices in a range
    template <typename Func>
    static void ForEachCPUPage(DAddr addr, size_t size, Func&& func) {
        static constexpr bool RETURNS_BOOL = std::is_same_v<std::invoke_result<Func, u64>, bool>;
        const u64 page_end = (addr + size - 1) >> LICHII_PAGEBITS;
        for (u64 page = addr >> LICHII_PAGEBITS; page <= page_end; ++page) {
            if constexpr (RETURNS_BOOL) {
                if (func(page)) {
                    break;
                }
            } else {
                func(page);
            }
        }
    }

    template <typename Func>
    static void ForEachGPUPage(GPUVAddr addr, size_t size, Func&& func) {
        static constexpr bool RETURNS_BOOL = std::is_same_v<std::invoke_result<Func, u64>, bool>;
        const u64 page_end = (addr + size - 1) >> LICHII_PAGEBITS;
        for (u64 page = addr >> LICHII_PAGEBITS; page <= page_end; ++page) {
            if constexpr (RETURNS_BOOL) {
                if (func(page)) {
                    break;
                }
            } else {
                func(page);
            }
        }
    }

    void OnGPUASRegister(size_t map_id) final override;

    /// Runs the Garbage Collector; at_critical collects as if usage were at the critical threshold.
    void RunGarbageCollector(bool out_of_memory, bool at_critical);

    /// Find or create an image view in the guest descriptor table
    ImageViewId VisitImageView(u32 index, bool compute);

    /// Find or create a framebuffer with the given render target parameters
    FramebufferId GetFramebufferId(const RenderTargets& key);

    /// Refresh the contents (pixel data) of an image
    [[nodiscard]] bool IsDownloadable(const ImageBase& image) const noexcept;

    void RefreshContents(Image& image, ImageId image_id);
    void NoteHostOutOfMemory(std::string_view operation, const ImageBase& image);

    /// Upload data from guest to an image
    template <typename StagingBuffer>
    void UploadImageContents(Image& image, StagingBuffer& staging_buffer);

    /// Create a new image view from a guest descriptor
    [[nodiscard]] ImageViewId CreateImageView(const TICEntry& config, bool& retry);

    /// Find or create an image from the given parameters
    [[nodiscard]] ImageId FindOrInsertImage(const ImageInfo& info, GPUVAddr gpu_addr,
                                            RelaxedOptions options = RelaxedOptions{},
                                            bool* retry = nullptr);

    /// Find an image from the given parameters
    [[nodiscard]] ImageId FindImage(const ImageInfo& info, GPUVAddr gpu_addr,
                                    RelaxedOptions options, bool* retry = nullptr);

    /// Create an image from the given parameters
    [[nodiscard]] ImageId InsertImage(const ImageInfo& info, GPUVAddr gpu_addr,
                                      RelaxedOptions options);

    /// Create a new image and join perfectly matching existing images
    /// Remove joined images from the cache
    [[nodiscard]] ImageId JoinImages(const ImageInfo& info, GPUVAddr gpu_addr, DAddr cpu_addr);

    [[nodiscard]] ImageId FindDMAImage(const ImageInfo& info, GPUVAddr gpu_addr);

    /// Return a blit image pair from the given guest blit parameters
    [[nodiscard]] std::optional<BlitImages> GetBlitImages(
        const Tegra::Engines::Fermi2D::Surface& dst, const Tegra::Engines::Fermi2D::Surface& src,
        const Tegra::Engines::Fermi2D::Config& copy);

    /// Find or create a sampler from a guest descriptor sampler
    [[nodiscard]] SamplerId FindSampler(const TSCEntry& config, bool compute);

    /// Find or create an image view for the given color buffer index
    [[nodiscard]] ImageViewId FindColorBuffer(size_t index);

    /// Find or create an image view for the depth buffer
    [[nodiscard]] ImageViewId FindDepthBuffer();

    /// Find or create a view for a render target with the given image parameters
    [[nodiscard]] ImageViewId FindRenderTargetView(const ImageInfo& info, GPUVAddr gpu_addr);

    /// Iterates over all the images in a region calling func
    template <typename Func>
    void ForEachImageInRegion(DAddr cpu_addr, size_t size, Func&& func);

    template <typename Func>
    void ForEachImageInRegionGPU(size_t as_id, GPUVAddr gpu_addr, size_t size, Func&& func);

    template <typename Func>
    void ForEachSparseImageInRegion(size_t as_id, GPUVAddr gpu_addr, size_t size, Func&& func);

    /// Iterates over all the images in a region calling func
    template <typename Func>
    void ForEachSparseSegment(ImageBase& image, Func&& func);

    /// Find or create an image view in the given image with the passed parameters
    [[nodiscard]] ImageViewId FindOrEmplaceImageView(ImageId image_id, const ImageViewInfo& info);

    /// Register image in the page table
    void RegisterImage(ImageId image);

    /// Unregister image from the page table
    void UnregisterImage(ImageId image);

    /// Logs where UnregisterImage's image was registered when one of its pages is missing.
    void ReportMissingImagePage(ImageId image_id, const Image& image, u64 page);

    /// Track CPU reads and writes for image
    void TrackImage(ImageBase& image, ImageId image_id);

    /// Stop tracking CPU reads and writes for image
    void UntrackImage(ImageBase& image, ImageId image_id);

    /// Delete image from the cache
    void DeleteImage(ImageId image, bool immediate_delete = false);

    /// Remove image views references from the cache
    void RemoveImageViewReferences(std::span<const ImageViewId> removed_views);

    /// Per image view, the (channel, descriptor) entries of the channels' image_views maps that
    /// point to it. Deleting an image erased them by scanning every map (title 1: ~5,000 entries,
    /// 50-130 us for each deleted image, dozens a frame while the collector runs).
    std::unordered_map<ImageViewId, boost::container::small_vector<std::pair<size_t, TICEntry>, 2>>
        image_view_descriptors;

    /// Remove framebuffers using the given image views from the cache
    void RemoveFramebuffers(std::span<const ImageViewId> removed_views);

    /// Mark an image as modified from the GPU
    void MarkModification(ImageBase& image) noexcept;

    /// Synchronize image aliases, copying data if needed
    void SynchronizeAliases(ImageId image_id);

    /// Prepare an image view to be used
    void PrepareImageView(ImageViewId image_view_id, bool is_modification, bool invalidate);

    /// Execute copies from one image to the other, even if they are incompatible
    void CopyImage(ImageId dst_id, ImageId src_id, std::vector<ImageCopy> copies);

    /// Bind an image view as render target, downloading resources preemtively if needed
    void BindRenderTarget(ImageViewId* old_id, ImageViewId new_id);

    /// Create a render target from a given image and image view parameters
    [[nodiscard]] std::pair<FramebufferId, ImageViewId> RenderTargetFromImage(
        ImageId, const ImageViewInfo& view_info);

    /// Returns true if the current clear parameters clear the whole image of a given image view
    [[nodiscard]] bool IsFullClear(ImageViewId id);

    /// Returns true if the current clear's render area and scissor cover a width x height image
    [[nodiscard]] bool ClearCovers(u32 width, u32 height) const;

    /// Whether the current clear writes every byte of a single-level, single-layer 2D image with
    /// this info at gpu_addr (every channel, the whole image, a target bound at that address)
    [[nodiscard]] bool ClearWritesWhole(const ImageInfo& info, GPUVAddr gpu_addr) const;

    /// Notes the bound targets the current clear writes whole as loaded at their clear's tick
    void NoteClearedWhole();

    [[nodiscard]] std::pair<u32, u32> PrepareDmaImage(ImageId dst_id, GPUVAddr base_addr,
                                                      bool mark_as_modified);

    bool ImageCanRescale(ImageBase& image);
    void InvalidateScale(Image& image);
    bool ScaleUp(Image& image);
    bool ScaleDown(Image& image);
    u64 GetScaledImageSizeBytes(const ImageBase& image);

    /// The device memory an image counts for in total_used_memory between two driver readings:
    /// none for an unbacked image (too large to back, or over unwritten sparse blocks), what is
    /// bound to its layers for a SparseLayers one.
    u64 ImageMemoryBytes(const Image& image) const;
    /// The image's allocation size where the runtime reports one, else ImageMemoryBytes.
    u64 ImageAllocationBytes(const Image& image) const;

    void QueueAsyncDecode(Image& image, ImageId image_id);
    void TickAsyncDecode();

    void QueueAsyncUnswizzle(Image& image, ImageId image_id);
    void TickAsyncUnswizzle();

    Runtime& runtime;

    Tegra::MaxwellDeviceMemoryManager& device_memory;
    std::deque<TextureCacheGPUMap> gpu_page_table_storage;

    RenderTargets render_targets;
    u64 render_targets_serial = 0;
    u32 rt_active_mask = 0;
    std::array<ImageId, 8> rt_image_id{};
    ImageId rt_depth_image_id{};
    u64 texture_bindings_serial = 0;
    Churn churn;
    u64 last_feedback_loop_serial = 0;
    u64 last_feedback_texture_serial = 0;
    bool last_feedback_loop_result = false;
    bool feedback_depth_dirty = true;
    FramebufferId last_framebuffer_id{};
    u64 last_framebuffer_serial = 0;

    ::Common::unordered_map<RenderTargets, FramebufferId> framebuffers;
    ::Common::unordered_map<u64, std::vector<ImageMapId>, Common::IdentityHash<u64>> page_table;
    ::Common::unordered_map<ImageId, boost::container::small_vector<ImageViewId, 16>> sparse_views;

    DAddr virtual_invalid_space{};

    /// Applies EDEN_NX2_TC_EXPECTED_MIB / EDEN_NX2_TC_CRITICAL_MIB and logs the thresholds.
    void ApplyThresholdOverrides(bool log = true);
    void LogThresholds() const;

    /// A GPU-written image dropped without being written back leaves its
    /// guest memory stale (the GPU's data is gone). Its whole pages are marked here and kept
    /// tracked, so CPU writes, unmaps and write-backs clear them. An image made entirely on marked
    /// pages starts without an upload: the upload would only copy stale bytes the GPU overwrites.
    /// Switch 2 titles only: the buffer cache feeds Nx2MayBeGpuWritten for them alone.
    /// Whether dropping this image without a download loses data only the GPU has (MarkStale).
    [[nodiscard]] static bool LosesGpuData(const ImageBase& image);
    void MarkStale(const ImageBase& image);
    [[nodiscard]] static u64 OverlapDownloadMaxBytes();
    [[nodiscard]] bool AsyncOverlapDownloadsEnabled();
    /// Device byte ranges [begin, end) of a write-back that newer data owns.
    using KeepRanges = boost::container::small_vector<std::pair<DAddr, DAddr>, 4>;
    /// Fills `keep` with the image's bytes that newer write-backs (or newer images dropped
    /// without one) cover, and `keep_ticks` (when given) with the modification tick of each;
    /// true when they cover all of it.
    [[nodiscard]] bool OverlapHasNewerData(ImageId image_id, const ImageBase& image,
                                           KeepRanges& keep,
                                           WritebackRanges::TickedRanges* keep_ticks = nullptr);
    /// SwizzleImage through `memory` that leaves the `keep` ranges of guest memory as they were.
    void SwizzleKeeping(Tegra::MemoryManager& memory, GPUVAddr gpu_addr, const ImageInfo& info,
                        std::span<const BufferImageCopy> copies, std::span<const u8> data,
                        const KeepRanges& keep);
    void NoteOverlapWriteback(const ImageBase& image, bool lost = false);
    /// The same for the image's contents at modification tick `tick`.
    void NoteOverlapWriteback(const ImageBase& image, bool lost, u64 tick);
    /// A noted write-back that was cancelled before it landed: its range stays newer than older
    /// images, but its data is gone (like an image dropped without one).
    void NoteWritebackLost(DAddr cpu_addr, u64 size, u64 tick);
    /// Whether the image's guest memory is the one range at its cpu_addr, which write-backs
    /// track, keep and note: not sparse, and not a stand-in address for GPU memory without CPU
    /// backing (ImageCpuAddress; TrackImage skips those the same way).
    [[nodiscard]] static bool HasGuestRange(const ImageBase& image) noexcept {
        return False(image.flags & ImageFlagBits::Sparse) && image.cpu_addr < ~(1ULL << 40);
    }
    [[nodiscard]] bool WrittenBackAlready(const ImageBase& image) const;
    /// Calls func(begin, end) for each range of guest memory the image's data is written to: the
    /// one range at cpu_addr, a sparse image's mapped segments, none for a stand-in address.
    template <typename Func>
    void ForEachWrittenRange(const ImageBase& image, Func&& func);
    /// WritebackRanges::KeepNewerBytes for every write-back waiting for the GPU and every range
    /// `image` was written to, after it was written to guest memory.
    void KeepInWaitingWritebacks(const ImageBase& image, u64 tick);
    /// After a flush wrote `image` to guest memory: write-backs still waiting keep its bytes, and
    /// later ones see it as newer data (NoteOverlapWriteback), while write-backs are on.
    void NoteFlushed(const ImageBase& image);
    /// NoteFlushed for contents staged at modification tick `tick` (asynchronous flushes: the
    /// image may have been written again since).
    void NoteFlushed(const ImageBase& image, u64 tick);
    /// Fills `keep` with the bytes of the image's guest memory that landed write-backs and
    /// flushes with contents newer than `tick` hold (an asynchronous flush's snapshot leaves them).
    /// Not those of `queued` write-backs, which have yet to land.
    void KeepNewerLanded(const ImageBase& image, u64 tick, KeepRanges& keep,
                         std::span<const WritebackRanges::TickedRange> queued = {});
    /// Whether the collector's or joins' write-backs are on (writeback_switches.h).
    [[nodiscard]] static bool OverlapWritebacksEnabled();
    u64 join_writeback_reuses{};
    void WriteBackBadOverlaps(ImageId new_image_id);
    [[nodiscard]] bool GobViewBytesReady(DAddr cpu_addr, size_t size);
    /// Collector write-backs waiting for the GPU: written to guest
    /// memory once their command buffer is done, except bytes the guest writes first; dropped if
    /// the memory is unmapped first. Their pages stay write-tracked meanwhile.
    struct AsyncOverlapDownload {
        u64 gpu_tick;
        GPUVAddr gpu_addr;
        DAddr cpu_addr;
        u64 size;
        ImageInfo info;
        boost::container::small_vector<BufferImageCopy, 16> copies;
        decltype(std::declval<Runtime&>().DownloadStagingBuffer(size_t{}, true)) map;
        bool cancelled;
        KeepRanges keep{};
        /// The modification tick of the data each kept range holds instead (newer images', or
        /// past the cache's tick for the guest's writes).
        WritebackRanges::TickedRanges keep_ticks{};
        /// Modification tick of the written-back image's contents.
        u64 tick{};
        /// The image's address space: gpu_addr is in it, whichever channel is bound on landing.
        size_t address_space{};
        bool gob_view{};
    };
    std::deque<AsyncOverlapDownload> async_overlap_downloads;
    /// The GPU memory of an address space the cache has seen, or null.
    [[nodiscard]] std::shared_ptr<Tegra::MemoryManager> AddressSpaceMemory(size_t as_id);
    void FinishAsyncOverlapDownloads(bool wait_all);
    /// After a write-back of [cpu_addr, cpu_addr + size) at modification tick `tick` landed,
    /// waiting or not: images over it the GPU did not write, with contents no newer, load it again.
    void ReloadUnderLanded(DAddr cpu_addr, u64 size, u64 tick);
    /// Whether a write-back still waiting covers the image and would reload it when it lands
    /// (the image has not been GPU-written since that write-back's contents).
    [[nodiscard]] bool WritebackWouldReload(const ImageBase& image);
    /// Whether a write-back still waiting writes to [cpu_addr, cpu_addr + size): its GPU data
    /// is not in guest memory yet.
    [[nodiscard]] bool WritebackPendingOver(DAddr cpu_addr, u64 size) const;
    void CancelAsyncOverlapDownloads(DAddr cpu_addr, u64 size);
    /// The guest wrote [cpu_addr, cpu_addr + size): waiting write-backs keep those bytes (they
    /// are newer) and still land the rest; one written all over is cancelled.
    void KeepGuestWrite(DAddr cpu_addr, u64 size);
    void CancelAsyncOverlapDownloadsGpu(size_t as_id, GPUVAddr gpu_addr, u64 size);
    u64 async_overlap_cancels{};
    u64 join_writebacks{};
    u64 join_writeback_images{};
    u64 join_writeback_wait_us{};
    static constexpr u32 OverlapPageBits = 12;
    /// Per 4 KiB device page: the newest modification tick of a dropped GPU-written overlapping
    /// image over it (written back or lost), so older ones are not written back over it.
    std::unordered_map<DAddr, u64> overlap_writeback_ticks;
    /// The same write-backs by byte range, per page they touch (a few each, CapPageRecords):
    /// small targets share pages, and a page-wide skip dropped a neighbour's data with them
    /// (black UI boxes).
    struct WritebackRange {
        DAddr begin;
        DAddr end;
        u64 tick;
        bool lost; ///< An image dropped without a write-back: newer, but its data is gone
    };
    /// Whether OverlapHasNewerData's last call found a lost image's range among the cover.
    bool last_cover_lost{};
    std::unordered_map<DAddr, boost::container::small_vector<WritebackRange, 4>>
        overlap_writeback_ranges;
    u64 overlap_partial_keeps{};
    u64 overlap_download_skips{};
    u64 overlap_download_batches{};  ///< GC overlap downloads (EDEN_DKB_FRAME_TRACE summary)
    u64 overlap_download_images{};
    u64 overlap_download_bytes{};
    u64 overlap_download_wait_us{};
    void ClearStale(DAddr addr, size_t size);
    /// The memory manager a downloaded image is written back through (see WriteBackMemory), or
    /// null when no live address space maps it.
    std::shared_ptr<Tegra::MemoryManager> WriteBackMemory(const ImageBase& image);
    /// Swizzles a downloaded image into guest memory through WriteBackMemory's memory manager and
    /// clears its stale pages. False, writing nothing, when there is none.
    bool WriteBackImage(const ImageBase& image, std::span<const BufferImageCopy> copies,
                        std::span<const u8> data, const KeepRanges& keep = {});
    /// Clears every device range [gpu_addr, gpu_addr + size) maps to in memory.
    void ClearStaleGpu(Tegra::MemoryManager& memory, GPUVAddr gpu_addr, size_t size);
    StalePages stale_pages;
    /// Pages early-deleted render targets left (EvictDeadRenderTargets); a CPU write clears them.
    StalePages evicted_pages;
    /// Render target shapes (format, width, height) the game read back after an early deletion:
    /// written back before one is deleted early, or left to the collector.
    std::unordered_set<u64> readback_shapes;
    /// Targets smaller than this aren't deleted early: they barely relieve memory.
    static constexpr u64 MinEvictBytes = u64{1} << 20;
    /// The latest early deletions, to find the shape a read-back image was made over.
    static constexpr size_t MaxRecentEvictions = 16384;
    struct RecentEviction {
        DAddr end;
        u64 shape;
        u64 sequence;
    };
    std::map<DAddr, RecentEviction> recent_evictions;
    std::deque<std::pair<DAddr, u64>> recent_eviction_order;
    u64 eviction_sequence{};
    u64 evict_born{};       ///< Images made over early-deleted pages
    u64 evict_read_back{};  ///< ...of which sampled before any draw
    u64 evict_saved{};      ///< Read-back shapes written back before deletion
    u64 evict_spared{};     ///< Read-back shapes left to the collector (not downloadable)
    u64 evict_small_kept{}; ///< Small targets left to the collector
    [[nodiscard]] static u64 RtShapeKey(const ImageBase& image) {
        return (u64{static_cast<u32>(image.info.format)} << 40) |
               (u64{image.info.size.width & 0xFFFFF} << 20) | (image.info.size.height & 0xFFFFF);
    }
    u64 tc_stale_skips{};        ///< Uploads skipped (last 600 frames)
    u64 tc_stale_skip_bytes{};
    u64 tc_stale_gpu_written{};  ///< Stale but buffer-written pages: uploaded after all

    /// The CPU address an image at gpu_addr is keyed by: its backing, or a fixed stand-in.
    [[nodiscard]] std::optional<DAddr> ImageCpuAddress(GPUVAddr gpu_addr, size_t size) const;

    bool has_deleted_images = false;
    std::vector<ImageId>* prepare_log{}; ///< LogPrepares
    bool prepare_log_dry{};
    /// True while a clear binds its render targets (UpdateRenderTargets(true)), so JoinImages can
    /// skip the upload of a new target the clear overwrites whole.
    bool clearing_render_targets = false;
    /// True while a draw binds its render targets (UpdateRenderTargets(false)): only these writes
    /// set first_written_frame, not blits, copies or DMA.
    bool drawing_render_targets = false;
    bool is_rescaling = false;
    /// The bound depth target is wider than tall by padding only (see FindDepthBuffer).
    bool depth_target_padded = false;
    u64 total_used_memory = 0;
    u64 minimum_memory;
    u64 expected_memory;
    u64 critical_memory;
    size_t gpu_unswizzle_maxsize = 0;
    size_t swizzle_chunk_size = 0;
    u32 swizzle_slices_per_batch = 0;

    struct BufferDownload {
        GPUVAddr address;
        size_t size;
    };

    using PendingDownload = PendingImageDownload<Image>;

    Common::SlotVector<Image> slot_images;
    Common::SlotVector<ImageMapView> slot_map_views;
    Common::SlotVector<ImageView> slot_image_views;
    Common::SlotVector<ImageAlloc> slot_image_allocs;
    Common::SlotVector<Sampler> slot_samplers;
    Common::SlotVector<Framebuffer> slot_framebuffers;
    Common::SlotVector<BufferDownload> slot_buffer_downloads;

    // TODO: This data structure is not optimal and it should be reworked

    std::vector<PendingDownload> uncommitted_downloads;
    std::deque<std::vector<PendingDownload>> committed_downloads;
    std::vector<AsyncBuffer> uncommitted_async_buffers;
    std::deque<std::vector<AsyncBuffer>> async_buffers;
    std::deque<AsyncBuffer> async_buffers_death_ring;

    struct LRUItemParams {
        using ObjectType = ImageId;
        using TickType = u64;
    };
    Common::LeastRecentlyUsedCache<LRUItemParams> lru_cache;

 #ifdef LICHII_LEGACY
    static constexpr size_t TICKS_TO_DESTROY = 6;
 #else
    static constexpr size_t TICKS_TO_DESTROY = 8;
#endif
    DelayedDestructionRing<Image, TICKS_TO_DESTROY> sentenced_images;
    /// Device memory of the images in each slot of sentenced_images (same index): deleted, but
    /// counted in the driver's usage until the ring destroys them.
    std::array<u64, TICKS_TO_DESTROY> sentenced_image_bytes{};
    size_t sentenced_image_index = 0;
    /// Deleted images on runtimes that report GPU progress (CurrentTick, IsFree): destroyed, with
    /// their device memory, as soon as the GPU is past them rather than TICKS_TO_DESTROY frames
    /// later. While the collector deletes dozens a frame, eight frames of them held 1-1.5 GB of
    /// VRAM above its threshold (title 1 on an 8 GB card: 6.6-7.0 GB in use against 5 GiB).
    struct GpuSentencedImage {
        u64 tick;
        u64 bytes;
        Image image;
    };
    std::deque<GpuSentencedImage> gpu_sentenced_images;
    u64 gpu_sentenced_bytes{};

    /// Bytes of images per device memory block, for the collector (refreshed every 60 frames).
    std::unordered_map<u64, u64> block_image_bytes;
    u64 block_image_bytes_frame{~0ULL};
    DelayedDestructionRing<ImageView, TICKS_TO_DESTROY> sentenced_image_view;
    DelayedDestructionRing<Framebuffer, TICKS_TO_DESTROY> sentenced_framebuffers;

    ::Common::unordered_map<GPUVAddr, ImageAllocId> image_allocs_table;

    Common::ScratchBuffer<u8> swizzle_data_buffer;
    Common::ScratchBuffer<u8> unswizzle_data_buffer;
    u64 host_oom_retry_frame{};
    u64 host_oom_failures{};
    std::chrono::steady_clock::time_point host_oom_log_time{};
    /// What the last CPU upload spent touching, reading and untiling guest memory, for
    /// EDEN_NX2_TC_TRACE.
    struct {
        double prefault_ms{};
        double read_ms{};
        double untile_ms{};
    } upload_trace;
    Tegra::Texture::UnswizzleWorker unswizzle_worker;

    u64 modification_tick = 0;
    u64 frame_tick = 0;

    Common::ThreadWorker texture_decode_worker{1, "TextureDecoder", {},
                                               Common::ThreadPlacement::Efficiency};
    std::vector<std::unique_ptr<AsyncDecodeContext>> async_decodes;
    /// Guest memory images were uploaded from since the last cold hint (device address, size).
    std::vector<std::pair<DAddr, u64>> uploaded_ranges;

    std::deque<PendingUnswizzle> unswizzle_queue;
    u8 current_unswizzle_frame;

    // Join caching
    boost::container::small_vector<ImageId, 4> join_overlap_ids;
    ::Common::unordered_set<ImageId> join_overlaps_found;
    boost::container::small_vector<ImageId, 4> join_left_aliased_ids;
    boost::container::small_vector<ImageId, 4> join_right_aliased_ids;
    ::Common::unordered_set<ImageId> join_ignore_textures;
    boost::container::small_vector<ImageId, 4> join_bad_overlap_ids;
    struct JoinCopy {
        bool is_alias;
        ImageId id;
    };
    boost::container::small_vector<JoinCopy, 4> join_copies_to_do;
    ::Common::unordered_map<ImageId, size_t> join_alias_indices;
};

} // namespace VideoCommon
