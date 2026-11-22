// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <numeric>
#include <cstdlib>
#include <limits>
#include <map>
#include <new>
#include <vector>
#include <tuple>
#include <optional>
#include <bit>
#include <chrono>
#include <string>
#include <type_traits>
#include <utility>
#include "common/container/unordered_map.h"
#include <boost/container/small_vector.hpp>

#include "common/alignment.h"
#include "common/memory_detect.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/dkb_frame_trace.h"
#include "common/slot_vector.h"
#include "video_core/control/channel_state.h"
#include "video_core/dirty_flags.h"
#include "video_core/dlss_contract.h"
#include "video_core/nx2_fast_draw.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/guest_memory.h"
#include "video_core/texture_cache/writeback_switches.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/nx2_profile.h"
#include "video_core/render_trace.h"
#include "video_core/texture_cache/conversion.h"
#include "video_core/texture_cache/eviction_age.h"
#include "video_core/texture_cache/host_memory_limit.h"
#include "video_core/texture_cache/host_mip_limits.h"
#include "video_core/texture_cache/image_view_base.h"
#include "video_core/texture_cache/samples_helper.h"
#include "video_core/texture_cache/texture_cache_base.h"
#include "video_core/texture_cache/util.h"
#include "video_core/textures/decoders.h"

namespace VideoCommon {

/// EDEN_NATIVE_DRAW_DIAGNOSTICS is read once: getenv scans the whole environment block on Windows.
[[nodiscard]] inline bool NativeDrawDiagnostics() {
    static const bool enabled = std::getenv("EDEN_NATIVE_DRAW_DIAGNOSTICS") != nullptr;
    return enabled;
}

/// EDEN_NX2_TC_TRACE=1: log every image the cache creates, uploads and deletes.
[[nodiscard]] inline bool TextureCacheTrace() {
    static const bool enabled = [] {
        const char* const v = std::getenv("EDEN_NX2_TC_TRACE");
        return v != nullptr && v[0] == '1';
    }();
    return enabled;
}

using Tegra::Texture::TICEntry;
using Tegra::Texture::TSCEntry;
using VideoCore::Surface::GetFormatType;
using VideoCore::Surface::PixelFormat;
using VideoCore::Surface::SurfaceType;
using namespace Common::Literals;

template <class P>
TextureCache<P>::TextureCache(Runtime& runtime_, Tegra::MaxwellDeviceMemoryManager& device_memory_)
    : runtime{runtime_}, device_memory{device_memory_} {
    debug_stats = std::getenv("EDEN_NX2_TC_LOG") != nullptr &&
                  Common::Log::IsLevelEnabled(Common::Log::Class::HW_GPU, Common::Log::Level::Debug);

    // Configure null sampler
    TSCEntry sampler_descriptor{};
    sampler_descriptor.min_filter.Assign(Tegra::Texture::TextureFilter::Linear);
    sampler_descriptor.mag_filter.Assign(Tegra::Texture::TextureFilter::Linear);
    sampler_descriptor.mipmap_filter.Assign(Tegra::Texture::TextureMipmapFilter::Linear);
    sampler_descriptor.cubemap_anisotropy.Assign(1);

    // These values were chosen based on typical peak swizzle data sizes seen in some titles
    constexpr size_t SWIZZLE_DATA_BUFFER_INITIAL_CAPACITY = 8_MiB;
    constexpr size_t UNSWIZZLE_DATA_BUFFER_INITIAL_CAPACITY = 1_MiB;
    swizzle_data_buffer.resize_destructive(SWIZZLE_DATA_BUFFER_INITIAL_CAPACITY);
    unswizzle_data_buffer.resize_destructive(UNSWIZZLE_DATA_BUFFER_INITIAL_CAPACITY);

    // Make sure the first index is reserved for the null resources
    // This way the null resource becomes a compile time constant
    void(slot_images.insert(NullImageParams{}));
    void(slot_image_views.insert(runtime, NullImageViewParams{}));
    void(slot_samplers.insert(runtime, sampler_descriptor));

    if constexpr (HAS_DEVICE_MEMORY_INFO) {
        const s64 device_local_memory = static_cast<s64>(runtime.GetDeviceLocalMemory());
        const s64 min_spacing_expected = device_local_memory - 1_GiB;
        const s64 min_spacing_critical = device_local_memory - 512_MiB;
        const s64 mem_threshold = (std::min)(device_local_memory, TARGET_THRESHOLD);
        const s64 min_vacancy_expected = (6 * mem_threshold) / 10;
        const s64 min_vacancy_critical = (2 * mem_threshold) / 10;
        expected_memory = static_cast<u64>(
            (std::max)((std::min)(device_local_memory - min_vacancy_expected, min_spacing_expected),
                     DEFAULT_EXPECTED_MEMORY));
        critical_memory = static_cast<u64>(
            (std::max)((std::min)(device_local_memory - min_vacancy_critical, min_spacing_critical),
                     DEFAULT_CRITICAL_MEMORY));
        minimum_memory = static_cast<u64>((device_local_memory - mem_threshold) / 2);
    } else {
        expected_memory = DEFAULT_EXPECTED_MEMORY + 512_MiB;
        critical_memory = DEFAULT_CRITICAL_MEMORY + 1_GiB;
        minimum_memory = 0;
    }
    ApplyThresholdOverrides();

    const bool gpu_unswizzle_enabled = Settings::values.gpu_unswizzle_enabled.GetValue();

    if (gpu_unswizzle_enabled) {
        switch (Settings::values.gpu_unswizzle_texture_size.GetValue()) {
            case Settings::GpuUnswizzleSize::VerySmall:    gpu_unswizzle_maxsize = 16_MiB; break;
            case Settings::GpuUnswizzleSize::Small:        gpu_unswizzle_maxsize = 32_MiB; break;
            case Settings::GpuUnswizzleSize::Normal:       gpu_unswizzle_maxsize = 128_MiB; break;
            case Settings::GpuUnswizzleSize::Large:        gpu_unswizzle_maxsize = 256_MiB; break;
            case Settings::GpuUnswizzleSize::VeryLarge:    gpu_unswizzle_maxsize = 512_MiB; break;
            default:                                       gpu_unswizzle_maxsize = 128_MiB; break;
        }

        switch (Settings::values.gpu_unswizzle_stream_size.GetValue()) {
            case Settings::GpuUnswizzle::VeryLow: swizzle_chunk_size = 4_MiB; break;
            case Settings::GpuUnswizzle::Low:     swizzle_chunk_size = 8_MiB; break;
            case Settings::GpuUnswizzle::Normal:  swizzle_chunk_size = 16_MiB; break;
            case Settings::GpuUnswizzle::Medium:  swizzle_chunk_size = 32_MiB; break;
            case Settings::GpuUnswizzle::High:    swizzle_chunk_size = 64_MiB; break;
            default:                              swizzle_chunk_size = 16_MiB;
        }

        switch (Settings::values.gpu_unswizzle_chunk_size.GetValue()) {
            case Settings::GpuUnswizzleChunk::VeryLow: swizzle_slices_per_batch = 32; break;
            case Settings::GpuUnswizzleChunk::Low:     swizzle_slices_per_batch = 64; break;
            case Settings::GpuUnswizzleChunk::Normal:  swizzle_slices_per_batch = 128; break;
            case Settings::GpuUnswizzleChunk::Medium:  swizzle_slices_per_batch = 256; break;
            case Settings::GpuUnswizzleChunk::High:    swizzle_slices_per_batch = 512; break;
            default:                                   swizzle_slices_per_batch = 128;
        }
    } else {
        gpu_unswizzle_maxsize = 0;
        swizzle_chunk_size = 0;
        swizzle_slices_per_batch = 0;
    }
}

template <class P>
void TextureCache<P>::RunGarbageCollector(bool out_of_memory, bool at_critical) {
    const StutterTrace::SiteScope site{StutterTrace::Site::TextureCollect};
    host_oom_retry_frame = 0;
    bool high_priority_mode = false;
    bool aggressive_mode = false;
    bool evict_old_costly = false;
    bool evict_bad_overlaps = false;
    u64 mode_age = 0;
    u64 ticks_to_destroy = 0;
    size_t num_iterations = 0;
    const auto Configure = [&](bool allow_aggressive) {
        high_priority_mode =
            out_of_memory || at_critical || total_used_memory >= expected_memory;
        aggressive_mode = allow_aggressive &&
                          (out_of_memory || at_critical || total_used_memory >= critical_memory);
        mode_age = aggressive_mode ? 10ULL : high_priority_mode ? 25ULL : 50ULL;
        // EDEN_NX2_TC_MIN_AGE=<frames>: keep images used more recently while there is room
        // (thrash tests).
        static const u64 min_age = [] {
            const char* value = std::getenv("EDEN_NX2_TC_MIN_AGE");
            return value != nullptr ? std::strtoull(value, nullptr, 10) : 0ULL;
        }();
        ticks_to_destroy =
            EvictionAge(mode_age, min_age, total_used_memory, retention_limit, out_of_memory);
        num_iterations = aggressive_mode ? 40 : (high_priority_mode ? 20 : 10);
    };
    // GPU-written images that overlap others without fitting them (BadOverlap) were dropped
    // without a download, their pages marked stale. The Duskbloods renders its characters'
    // small detail maps that way, into regions other images sample: once the sampling image
    // was made again, the outfit, blade or enemy drew zeros (black) or, before the image
    // loaded stale pages, leftover host memory (rainbow noise). Download them, all with one
    // wait, oldest modification first so the newest data wins where they overlap.
    using DownloadMap = decltype(runtime.DownloadStagingBuffer(size_t{}));
    struct OverlapDownload {
        u64 tick;
        GPUVAddr gpu_addr;
        DAddr cpu_addr;
        u64 size;
        ImageInfo info;
        boost::container::small_vector<BufferImageCopy, 16> copies;
        DownloadMap map;
        KeepRanges keep;
        WritebackRanges::TickedRanges keep_ticks;
        size_t address_space;
    };
    std::vector<OverlapDownload> overlap_downloads;
    // Staging maps that release themselves when destroyed (OpenGL) cannot wait in a vector.
    // Not const, here and below: with OpenGL both would be constant expressions, which the
    // lambda below must then not capture (-Wunused-lambda-capture), while Vulkan's must be.
    bool download_overlaps = std::is_trivially_destructible_v<DownloadMap> &&
                             WritebackSwitches::DuskbloodsDefault();
    // The collector does not wait for its
    // write-backs (each wait was 5-10 ms, several a second in combat); they reach guest memory
    // when the GPU is done with them.
    bool async_downloads = download_overlaps && AsyncOverlapDownloadsEnabled();
    KeepRanges keep;
    WritebackRanges::TickedRanges keep_ticks;
    const auto CanEvict = [this, &high_priority_mode, &aggressive_mode, &evict_old_costly,
                           &evict_bad_overlaps](ImageId image_id,
                                                bool* out_must_download = nullptr) {
        const auto& image = slot_images[image_id];
        if (True(image.flags & ImageFlagBits::IsDecoding) ||
            (evict_bad_overlaps && False(image.flags & ImageFlagBits::BadOverlap))) {
            return false;
        }
        const bool must_download =
            IsDownloadable(image) && False(image.flags & ImageFlagBits::BadOverlap);
        if (out_must_download != nullptr) {
            *out_must_download = must_download;
        }
        if ((!aggressive_mode && !evict_old_costly &&
             True(image.flags & ImageFlagBits::CostlyLoad)) ||
            (!high_priority_mode && must_download)) {
            return false;
        }
        return true;
    };
    const auto Cleanup = [this, &num_iterations, &high_priority_mode, &aggressive_mode, &CanEvict,
                          &evict_bad_overlaps, &keep, &keep_ticks, out_of_memory,
                          &overlap_downloads, download_overlaps, async_downloads](ImageId image_id) {
        if (num_iterations == 0) {
            return true;
        }
        bool must_download = false;
        if (!CanEvict(image_id, &must_download)) {
            return false;
        }
        --num_iterations;
        auto& image = slot_images[image_id];
        bool download_queued = false;
        // A GPU-written overlap the collector may write back, and whether newer write-backs
        // already hold all of it (then its data is not lost with it).
        const bool overlap_writeback =
            download_overlaps && True(image.flags & ImageFlagBits::BadOverlap) &&
            (async_downloads || image.guest_size_bytes <= OverlapDownloadMaxBytes()) &&
            IsDownloadable(image) && HasGuestRange(image);
        const bool covered =
            overlap_writeback && OverlapHasNewerData(image_id, image, keep, &keep_ticks);
        if (must_download) {
            auto map = runtime.DownloadStagingBuffer(image.unswizzled_size_bytes);
            const auto copies = FullDownloadCopies(image.info);
            image.DownloadMemory(map, copies);
            runtime.Finish();
            bool written;
            try {
                // Newer data that landed over it stays; write-backs this pass queued land after it.
                boost::container::small_vector<WritebackRanges::TickedRange, 8> queued;
                for (const auto& download : overlap_downloads) {
                    queued.push_back({download.cpu_addr, download.cpu_addr + download.size,
                                      download.tick});
                }
                KeepRanges newer;
                KeepNewerLanded(image, image.modification_tick, newer, queued);
                written = WriteBackImage(image, copies, map.mapped_span, newer);
            } catch (const std::bad_alloc&) {
                NoteHostOutOfMemory("write back", image);
                return false;
            }
            if (!written) {
                MarkStale(image);
            } else {
                // Older write-backs still queued keep what this one wrote, and later ones see it as
                // newer data: also joins' when the collector's are off (NoteFlushed).
                if (!overlap_downloads.empty()) {
                    ForEachWrittenRange(image, [&](DAddr begin, DAddr end) {
                        for (auto& download : overlap_downloads) {
                            WritebackRanges::KeepNewerBytes(download, begin, end,
                                                            image.modification_tick);
                        }
                    });
                }
                NoteFlushed(image);
            }
            ++gc_download_deletes;
        } else if (overlap_writeback && !covered) {
            download_queued = true;
            NoteOverlapWriteback(image);
            // Deferred also when waiting: a later image's synchronous download in this pass
            // finishes the GPU work, after which the pool would hand this buffer out again.
            auto map = runtime.DownloadStagingBuffer(image.unswizzled_size_bytes, true);
            auto copies = FullDownloadCopies(image.info);
            image.DownloadMemory(map, copies);
            overlap_downloads.push_back(OverlapDownload{
                .tick = image.modification_tick,
                .gpu_addr = image.gpu_addr,
                .cpu_addr = image.cpu_addr,
                .size = image.guest_size_bytes,
                .info = image.info,
                .copies = std::move(copies),
                .map = std::move(map),
                .keep = keep,
                .keep_ticks = keep_ticks,
                .address_space = image.address_space,
            });
            ++gc_download_deletes;
            if (False(image.flags & ImageFlagBits::Sparse)) {
                ClearStale(image.cpu_addr, image.guest_size_bytes);
            } else if (const auto memory = WriteBackMemory(image)) {
                ClearStaleGpu(*memory, image.gpu_addr, image.guest_size_bytes);
            }
        } else {
            if (OverlapWritebacksEnabled() && True(image.flags & ImageFlagBits::GpuModified) &&
                HasGuestRange(image)) {
                // Its data is lost, but it is newer than what older images would write back.
                // Under newer write-backs it is not lost: they hold what supersedes it.
                NoteOverlapWriteback(image, !covered || last_cover_lost);
            }
            MarkStale(image); // before UntrackImage (see MarkStale)
        }
        if (True(image.flags & ImageFlagBits::Tracked)) {
            UntrackImage(image, image_id);
        }
        if constexpr (requires(const Image& img) { img.MemoryBlock(); }) {
            const auto block = image.MemoryBlock();
            if (block.block != 0) {
                const auto it = block_image_bytes.find(block.block);
                if (it != block_image_bytes.end()) {
                    it->second -= (std::min)(it->second, block.size);
                }
            }
        }
        ++(True(image.flags & ImageFlagBits::CostlyLoad) ? gc_costly_deletes : gc_deletes);
        trace_delete_flags = static_cast<u32>(image.flags);
        trace_delete_downloaded = must_download;
        UnregisterImage(image_id);
        trace_delete_reason = evict_bad_overlaps   ? "gc-bad-overlap"
                              : aggressive_mode    ? "gc-aggressive"
                              : high_priority_mode ? "gc-high-priority"
                                                   : "gc";
        DeleteImage(image_id, !download_queued && image.scale_tick > frame_tick + 5);
        if (out_of_memory) {
            return false;
        }
        if (aggressive_mode && total_used_memory < critical_memory) {
            num_iterations >>= 2;
            aggressive_mode = false;
        } else if (high_priority_mode && total_used_memory < expected_memory) {
            num_iterations >>= 1;
            high_priority_mode = false;
        }
        return false;
    };
    // Deleting by age alone left every device memory block partly used: the collector deletes
    // thousands of images a minute and new ones land in the holes, so blocks seldom empty (title
    // 1: ~0.8 GB of holes in ~190 32 MiB blocks after ten minutes, which the driver counts as
    // used). Among the oldest candidates (8 per image this pass may delete), those in the
    // emptiest blocks go first, so blocks drain and go back to the driver; placement already
    // prefers the fullest blocks (best fit). Images with memory of their own sit in the middle.
    const auto Collect = [&](u64 age_override = 0) {
        const u64 required_age = age_override != 0 ? age_override : ticks_to_destroy;
        if (num_iterations == 0 || frame_tick < required_age) {
            return;
        }
        const u64 last_tick = frame_tick - required_age;
        if constexpr (requires(const Image& image) { image.MemoryBlock(); }) {
            std::vector<ImageId> candidates;
            const size_t limit = num_iterations * 8;
            lru_cache.ForEachItemBelow(last_tick, [&](ImageId image_id) {
                if (CanEvict(image_id)) {
                    candidates.push_back(image_id);
                }
                return candidates.size() >= limit;
            });
            if (candidates.empty()) {
                return;
            }
            if (block_image_bytes_frame == ~0ULL || frame_tick - block_image_bytes_frame >= 60) {
                block_image_bytes_frame = frame_tick;
                block_image_bytes.clear();
                for (const auto [image_id, image] : slot_images) {
                    const auto block = image->MemoryBlock();
                    if (block.block != 0) {
                        block_image_bytes[block.block] += block.size;
                    }
                }
            }
            std::vector<std::pair<double, ImageId>> ordered;
            ordered.reserve(candidates.size());
            for (const ImageId image_id : candidates) {
                const auto block = slot_images[image_id].MemoryBlock();
                double fill = 0.5;
                if (block.block != 0 && block.block_size != 0) {
                    const auto it = block_image_bytes.find(block.block);
                    fill = it != block_image_bytes.end()
                               ? static_cast<double>(it->second) / static_cast<double>(block.block_size)
                               : 0.0;
                }
                ordered.emplace_back(fill, image_id);
            }
            std::stable_sort(ordered.begin(), ordered.end(),
                             [](const auto& a, const auto& b) { return a.first < b.first; });
            for (const auto& [fill, image_id] : ordered) {
                if (Cleanup(image_id)) {
                    break;
                }
            }
        } else {
            lru_cache.ForEachItemBelow(last_tick, Cleanup);
        }
    };
    const u64 usage_at_start = total_used_memory;
    Configure(false);
    // Images newer ones partly overwrote (BadOverlap: their bytes belong to the newer images, so
    // they are dropped without a download) get the mode's age, not the retention floor. Ocarina
    // allocates each frame's render targets at shifting offsets of a transient heap: in the
    // Skulltula House about 9 a frame, kept 150 frames, filled 16 GB of a 24 GB card (#1755).
    // Same pass as kraibse 662af0bfaf's EDEN_NX2_TC_STALE_AGE, at the mode's age.
    if (ticks_to_destroy > mode_age) {
        evict_bad_overlaps = true;
        Collect(mode_age);
        evict_bad_overlaps = false;
    }
    static const u64 old_age = [] {
        const char* value = std::getenv("EDEN_NX2_TC_OLD_AGE");
        return value != nullptr ? std::strtoull(value, nullptr, 10) : 1800ULL;
    }();
    if (high_priority_mode && old_age != 0 && frame_tick > old_age) {
        evict_old_costly = true;
        Collect(old_age);
        evict_old_costly = false;
    }
    Collect();
    // A spill collect takes the aggressive pass only while the passes above freed nothing.
    if (out_of_memory || total_used_memory >= critical_memory ||
        (at_critical && total_used_memory >= usage_at_start)) {
        Configure(true);
        Collect();
    }
    if (!overlap_downloads.empty() && async_downloads) {
        std::stable_sort(overlap_downloads.begin(), overlap_downloads.end(),
                         [](const auto& a, const auto& b) { return a.tick < b.tick; });
        if constexpr (requires { runtime.CurrentTick(); }) {
            const u64 gpu_tick = runtime.CurrentTick();
            for (auto& download : overlap_downloads) {
                device_memory.UpdatePagesCachedCount(download.cpu_addr, download.size, 1);
                overlap_download_bytes += download.map.mapped_span.size();
                async_overlap_downloads.push_back(AsyncOverlapDownload{
                    .gpu_tick = gpu_tick,
                    .gpu_addr = download.gpu_addr,
                    .cpu_addr = download.cpu_addr,
                    .size = download.size,
                    .info = download.info,
                    .copies = std::move(download.copies),
                    .map = download.map,
                    .cancelled = false,
                    .keep = download.keep,
                    .keep_ticks = download.keep_ticks,
                    .tick = download.tick,
                    .address_space = download.address_space,
                });
            }
        }
        ++overlap_download_batches;
        overlap_download_images += overlap_downloads.size();
        overlap_downloads.clear();
    }
    if (!overlap_downloads.empty()) {
        const auto wait_start = std::chrono::steady_clock::now();
        runtime.Finish();
        overlap_download_wait_us += static_cast<u64>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - wait_start)
                .count());
        ++overlap_download_batches;
        overlap_download_images += overlap_downloads.size();
        std::stable_sort(overlap_downloads.begin(), overlap_downloads.end(),
                         [](const auto& a, const auto& b) { return a.tick < b.tick; });
        u64 bytes = 0;
        for (auto& download : overlap_downloads) {
            if (const auto memory = AddressSpaceMemory(download.address_space)) {
                SwizzleKeeping(*memory, download.gpu_addr, download.info, download.copies,
                               download.map.mapped_span, download.keep);
                ReloadUnderLanded(download.cpu_addr, download.size, download.tick);
            } else {
                NoteWritebackLost(download.cpu_addr, download.size, download.tick);
            }
            bytes += download.map.mapped_span.size();
            runtime.FreeDeferredStagingBuffer(download.map);
        }
        overlap_download_bytes += bytes;
        if (Common::DkbFrameTrace::Enabled()) {
            static u64 batches{};
            static u64 images{};
            images += overlap_downloads.size();
            if (batches++ % 32 == 0) {
                LOG_INFO(HW_GPU,
                         "DKB gc overlap download batch #{} images={} bytes={:#x} total_images={} "
                         "first gpu={:#x} {}x{}",
                         batches, overlap_downloads.size(), bytes, images,
                         overlap_downloads.front().gpu_addr,
                         overlap_downloads.front().info.size.width,
                         overlap_downloads.front().info.size.height);
            }
        }
    }
}

template <class P>
bool TextureCache<P>::OverlapHasNewerData(ImageId image_id, const ImageBase& image,
                                          KeepRanges& keep,
                                          WritebackRanges::TickedRanges* keep_ticks) {
    // Memory the game reuses for other targets: an older image written back over it put another
    // format's data under the newer image's pages (purple enemies when that image was made again).
    keep.clear();
    if (keep_ticks != nullptr) {
        keep_ticks->clear();
    }
    last_cover_lost = false;
    const u64 tick = image.modification_tick;
    const DAddr begin = image.cpu_addr;
    const DAddr end = image.cpu_addr + image.guest_size_bytes;
    const DAddr first_page = begin >> OverlapPageBits;
    const DAddr last_page = (end - 1) >> OverlapPageBits;
    // Only the bytes newer data owns are kept: small targets packed together share pages, and
    // skipping the whole image left its own bytes stale (black boxes where it was sampled).
    for (DAddr page = first_page; page <= last_page; ++page) {
        const auto it = overlap_writeback_ranges.find(page);
        if (it == overlap_writeback_ranges.end()) {
            continue;
        }
        for (const WritebackRange& range : it->second) {
            if (range.end <= begin || end <= range.begin) {
                continue;
            }
            // A smaller image written back after this
            // one took its contents (or a clear replaced all of them) holds bytes this one only
            // has old copies of: a large render target written back whole put its stale snapshot
            // over The Duskbloods' 128x128 character maps (rainbow, purple, black outfits and
            // enemies). Its draws since don't count: the cache can't tell a draw over those
            // bytes from one elsewhere in the image.
            const bool newer = range.tick > tick;
            const bool small_after_load = !range.lost &&
                                          range.tick > image.load_tick &&
                                          range.end - range.begin < image.guest_size_bytes;
            if (!newer && !small_after_load) {
                continue;
            }
            keep.emplace_back((std::max)(range.begin, begin), (std::min)(range.end, end));
            if (keep_ticks != nullptr) {
                WritebackRanges::NoteTick(*keep_ticks, keep.back().first, keep.back().second,
                                          range.tick);
            }
            last_cover_lost |= range.lost;
        }
    }
    if (keep.empty()) {
        return false;
    }
    std::ranges::sort(keep);
    KeepRanges merged;
    for (const auto& range : keep) {
        if (!merged.empty() && range.first <= merged.back().second) {
            merged.back().second = (std::max)(merged.back().second, range.second);
        } else {
            merged.push_back(range);
        }
    }
    keep = std::move(merged);
    if (keep.size() == 1 && keep.front().first == begin && keep.front().second == end) {
        ++overlap_download_skips;
        keep.clear();
        if (keep_ticks != nullptr) {
            keep_ticks->clear();
        }
        return true;
    }
    ++overlap_partial_keeps;
    // Live overlapping images are not checked: an image made over them writes them back first
    // (WriteBackBadOverlaps), newest last.
    (void)image_id;
    return false;
}

template <class P>
void TextureCache<P>::SwizzleKeeping(Tegra::MemoryManager& memory, GPUVAddr gpu_addr,
                                     const ImageInfo& info,
                                     std::span<const BufferImageCopy> copies,
                                     std::span<const u8> data, const KeepRanges& keep) {
    if (keep.empty()) {
        SwizzleImage(memory, gpu_addr, info, copies, data, swizzle_data_buffer);
        return;
    }
    size_t total = 0;
    for (const auto& [begin, end] : keep) {
        total += end - begin;
    }
    std::vector<u8> saved(total);
    size_t offset = 0;
    for (const auto& [begin, end] : keep) {
        device_memory.ReadBlockUnsafe(begin, saved.data() + offset, end - begin);
        offset += end - begin;
    }
    SwizzleImage(memory, gpu_addr, info, copies, data, swizzle_data_buffer);
    offset = 0;
    for (const auto& [begin, end] : keep) {
        device_memory.WriteBlockUnsafe(begin, saved.data() + offset, end - begin);
        offset += end - begin;
    }
}

template <class P>
std::shared_ptr<Tegra::MemoryManager> TextureCache<P>::AddressSpaceMemory(size_t as_id) {
    std::scoped_lock lock{config_mutex};
    const auto it = address_spaces.find(as_id);
    return it != address_spaces.end() ? it->second.owner.lock() : nullptr;
}

template <class P>
void TextureCache<P>::FinishAsyncOverlapDownloads(bool wait_all) {
    if constexpr (requires { runtime.CurrentTick(); }) {
        if (async_overlap_downloads.empty()) {
            return;
        }
        if (wait_all && !runtime.IsFree(async_overlap_downloads.back().gpu_tick)) {
            runtime.Finish();
        }
        while (!async_overlap_downloads.empty() &&
               runtime.IsFree(async_overlap_downloads.front().gpu_tick)) {
            auto& download = async_overlap_downloads.front();
            // Its address space, not the bound channel's: another one may be bound by now.
            const auto memory = AddressSpaceMemory(download.address_space);
            if (!download.cancelled && memory == nullptr) {
                download.cancelled = true;
                ++async_overlap_cancels;
                NoteWritebackLost(download.cpu_addr, download.size, download.tick);
            }
            if (!download.cancelled) {
                SwizzleKeeping(*memory, download.gpu_addr, download.info, download.copies,
                               download.map.mapped_span, download.keep);
                if (download.gob_view) {
                    ClearStale(download.cpu_addr, download.size);
                }
                ReloadUnderLanded(download.cpu_addr, download.size, download.tick);
            }
            runtime.FreeDeferredStagingBuffer(download.map);
            device_memory.UpdatePagesCachedCount(download.cpu_addr, download.size, -1);
            async_overlap_downloads.pop_front();
        }
    }
}

template <class P>
bool TextureCache<P>::GobViewBytesReady(DAddr cpu_addr, size_t size) {
    if (WritebackPendingOver(cpu_addr, size)) {
        return false;
    }
    ImageId source_id{};
    ForEachImageInRegion(cpu_addr, size, [&](ImageId id, ImageBase& image) {
        if (!image.info.native_tiling || False(image.flags & ImageFlagBits::GpuModified) ||
            True(image.flags & (ImageFlagBits::Remapped | ImageFlagBits::GuestReplacedContents))) {
            return false;
        }
        KeepRanges landed;
        KeepNewerLanded(image, image.modification_tick == 0 ? 0 : image.modification_tick - 1,
                        landed);
        const DAddr begin = (std::max)(cpu_addr, image.cpu_addr);
        const DAddr end = (std::min)(cpu_addr + size, image.cpu_addr_end);
        if (std::ranges::any_of(landed, [&](const auto& range) {
                return range.first <= begin && end <= range.second;
            })) {
            return false;
        }
        if (!source_id || image.modification_tick < slot_images[source_id].modification_tick) {
            source_id = id;
        }
        return false;
    });
    if (!source_id) {
        return true;
    }
    Image& source = slot_images[source_id];
    constexpr size_t MaxGobReadbackBytes = 64_MiB;
    if (std::ranges::any_of(async_overlap_downloads, &AsyncOverlapDownload::gob_view) ||
        source.unswizzled_size_bytes > MaxGobReadbackBytes ||
        !IsDownloadable(source) || !HasGuestRange(source) ||
        True(source.flags & (ImageFlagBits::IsDecoding | ImageFlagBits::Converted))) {
        return false;
    }
    const auto memory = AddressSpaceMemory(source.address_space);
    if (!memory || memory->GpuToCpuAddress(source.gpu_addr) != source.cpu_addr ||
        !memory->IsContinuousRange(source.gpu_addr, source.guest_size_bytes)) {
        return false;
    }
    runtime.FlushDeferredClear();
    KeepRanges keep;
    KeepNewerLanded(source, source.modification_tick, keep);
    auto copies = FullDownloadCopies(source.info);
    if constexpr (requires { runtime.CurrentTick(); }) {
        const u64 gpu_tick = runtime.CurrentTick();
        auto map = runtime.DownloadStagingBuffer(source.unswizzled_size_bytes, true);
        source.DownloadMemory(map, copies);
        device_memory.UpdatePagesCachedCount(source.cpu_addr, source.guest_size_bytes, 1);
        async_overlap_downloads.push_back(AsyncOverlapDownload{
            .gpu_tick = gpu_tick,
            .gpu_addr = source.gpu_addr,
            .cpu_addr = source.cpu_addr,
            .size = source.guest_size_bytes,
            .info = source.info,
            .copies = std::move(copies),
            .map = std::move(map),
            .cancelled = false,
            .keep = std::move(keep),
            .tick = source.modification_tick,
            .address_space = source.address_space,
            .gob_view = true,
        });
        NoteOverlapWriteback(source);
        ++join_writebacks;
        ++join_writeback_images;
    } else {
        auto map = runtime.DownloadStagingBuffer(source.unswizzled_size_bytes);
        source.DownloadMemory(map, copies);
        const auto start = std::chrono::steady_clock::now();
        runtime.Finish();
        join_writeback_wait_us += static_cast<u64>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count());
        ++join_writebacks;
        ++join_writeback_images;
        if (WriteBackImage(source, copies, map.mapped_span, keep)) {
            NoteOverlapWriteback(source);
            ReloadUnderLanded(source.cpu_addr, source.guest_size_bytes, source.modification_tick);
        }
    }
    return false;
}

template <class P>
void TextureCache<P>::ReloadUnderLanded(DAddr cpu_addr, u64 size, u64 tick) {
    // Images the GPU did not write loaded these pages before the data landed (made in the
    // meantime by a join, or kept from before it): they load them again. Not one whose contents
    // are newer than the landed data: a view made over a newer target took the target's pixels
    // by a GPU copy without becoming GPU-modified, and reloading it put the older images' bytes in
    // their place (The Duskbloods' 698x80 message banner band came out black or with other HUD
    // text in it).
    ForEachImageInRegion(cpu_addr, size, [tick](ImageId, ImageBase& image) {
        if (False(image.flags & ImageFlagBits::GpuModified) && image.modification_tick <= tick) {
            image.flags |= ImageFlagBits::CpuModified;
        }
    });
}

template <class P>
bool TextureCache<P>::WritebackWouldReload(const ImageBase& image) {
    // As a landing write-back's reload (ReloadUnderLanded): not GPU-written, contents no newer
    // than the data, over any of the image's guest memory (a sparse image's mapped segments).
    if (True(image.flags & ImageFlagBits::GpuModified)) {
        return false;
    }
    bool reload = false;
    ForEachWrittenRange(image, [&](DAddr begin, DAddr end) {
        for (const AsyncOverlapDownload& download : async_overlap_downloads) {
            reload |= !download.cancelled && download.cpu_addr < end &&
                      begin < download.cpu_addr + download.size &&
                      image.modification_tick <= download.tick;
        }
    });
    return reload;
}

template <class P>
bool TextureCache<P>::WritebackPendingOver(DAddr cpu_addr, u64 size) const {
    return std::ranges::any_of(async_overlap_downloads, [&](const AsyncOverlapDownload& download) {
        return !download.cancelled && download.cpu_addr < cpu_addr + size &&
               cpu_addr < download.cpu_addr + download.size;
    });
}

template <class P>
void TextureCache<P>::KeepGuestWrite(DAddr cpu_addr, u64 size) {
    // A write-back's image is newer than what guest memory held, and the guest's write is newer
    // still: guest memory as the console has it is the image's bytes with the write over them.
    // Cancelling the whole write-back for a write to part of it left the rest stale.
    constexpr size_t MaxGuestKeeps = 32;
    for (auto& download : async_overlap_downloads) {
        const DAddr download_end = download.cpu_addr + download.size;
        if (download.cancelled || download_end <= cpu_addr ||
            cpu_addr + size <= download.cpu_addr) {
            continue;
        }
        const DAddr begin = (std::max)(cpu_addr, download.cpu_addr);
        const DAddr end = (std::min)(cpu_addr + size, download_end);
        // The guest's bytes are newer than every image's contents so far, and older than those
        // of images modified from now on.
        WritebackRanges::NoteTick(download.keep_ticks, begin, end, modification_tick + 1);
        // Coalesce every overlapping or touching range (a write can bridge two), so the checks
        // below see the distinct ranges.
        auto& keep = download.keep;
        keep.emplace_back(begin, end);
        std::ranges::sort(keep);
        KeepRanges merged;
        for (const auto& range : keep) {
            if (!merged.empty() && range.first <= merged.back().second) {
                merged.back().second = (std::max)(merged.back().second, range.second);
            } else {
                merged.push_back(range);
            }
        }
        keep = std::move(merged);
        const bool all_written = keep.size() == 1 && keep.front().first <= download.cpu_addr &&
                                 download_end <= keep.front().second;
        if (all_written || keep.size() > MaxGuestKeeps) {
            // Nothing (or too scattered a rest) to land: as before, a write-back that never lands.
            download.cancelled = true;
            ++async_overlap_cancels;
            NoteWritebackLost(download.cpu_addr, download.size, download.tick);
        }
    }
}

template <class P>
void TextureCache<P>::CancelAsyncOverlapDownloads(DAddr cpu_addr, u64 size) {
    for (auto& download : async_overlap_downloads) {
        if (!download.cancelled && download.cpu_addr < cpu_addr + size &&
            cpu_addr < download.cpu_addr + download.size) {
            download.cancelled = true;
            ++async_overlap_cancels;
            NoteWritebackLost(download.cpu_addr, download.size, download.tick);
        }
    }
}

template <class P>
void TextureCache<P>::CancelAsyncOverlapDownloadsGpu(size_t as_id, GPUVAddr gpu_addr, u64 size) {
    for (auto& download : async_overlap_downloads) {
        if (!download.cancelled && download.address_space == as_id &&
            download.gpu_addr < gpu_addr + size &&
            gpu_addr < download.gpu_addr + download.size) {
            download.cancelled = true;
            ++async_overlap_cancels;
            NoteWritebackLost(download.cpu_addr, download.size, download.tick);
        }
    }
}

template <class P>
void TextureCache<P>::WriteBackBadOverlaps(ImageId new_image_id) {
    // An image made over
    // GPU-written images it overlaps without fitting (kept alive, not copied in) loaded guest
    // memory without their data. The Duskbloods renders its characters' 64x64 detail maps through
    // other render targets at the same addresses and samples them through new images, which
    // showed the memory's fill pattern (purple enemies) or zeros (black outfits).
    if (!WritebackSwitches::DuskbloodsDefault()) {
        return;
    }
    const bool async = AsyncOverlapDownloadsEnabled();
    // Collector write-backs still on the GPU may cover this image: they land first. Async: when
    // they land, images over their pages that the GPU did not write reload (one frame late).
    if (!async && !async_overlap_downloads.empty()) {
        const DAddr begin = slot_images[new_image_id].cpu_addr;
        const u64 size = slot_images[new_image_id].guest_size_bytes;
        for (const auto& download : async_overlap_downloads) {
            if (!download.cancelled && download.cpu_addr < begin + size &&
                begin < download.cpu_addr + download.size) {
                FinishAsyncOverlapDownloads(true);
                break;
            }
        }
    }
    using DownloadMap = decltype(runtime.DownloadStagingBuffer(size_t{}));
    if constexpr (std::is_trivially_destructible_v<DownloadMap>) {
        std::vector<ImageId> ids;
        for (const ImageId overlap_id : join_bad_overlap_ids) {
            if (overlap_id == new_image_id) {
                continue;
            }
            const ImageBase& overlap = slot_images[overlap_id];
            if (IsDownloadable(overlap) &&
                (async || overlap.guest_size_bytes <= OverlapDownloadMaxBytes()) &&
                False(overlap.flags & ImageFlagBits::IsDecoding) && HasGuestRange(overlap)) {
                if (WrittenBackAlready(overlap)) {
                    ++join_writeback_reuses;
                    continue;
                }
                ids.push_back(overlap_id);
            }
        }
        if (ids.empty()) {
            return;
        }
        std::ranges::sort(ids, [this](ImageId a, ImageId b) {
            return slot_images[a].modification_tick < slot_images[b].modification_tick;
        });
        struct Pending {
            ImageId id;
            boost::container::small_vector<BufferImageCopy, 16> copies;
            DownloadMap map;
            KeepRanges keep;
        };
        if constexpr (requires { runtime.CurrentTick(); }) {
            if (async) {
                // Waiting here took 60-70 ms per join in The Duskbloods (the GPU's whole queue,
                // frames ahead included): queue them with the collector's instead.
                const u64 gpu_tick = runtime.CurrentTick();
                for (const ImageId id : ids) {
                    Image& overlap = slot_images[id];
                    // Bytes newer write-backs own stay theirs (OverlapHasNewerData); one they
                    // own entirely is not written back at all.
                    KeepRanges keep;
                    WritebackRanges::TickedRanges keep_ticks;
                    if (OverlapHasNewerData(id, overlap, keep, &keep_ticks)) {
                        continue;
                    }
                    auto map = runtime.DownloadStagingBuffer(overlap.unswizzled_size_bytes, true);
                    auto copies = FullDownloadCopies(overlap.info);
                    overlap.DownloadMemory(map, copies);
                    device_memory.UpdatePagesCachedCount(overlap.cpu_addr,
                                                         overlap.guest_size_bytes, 1);
                    async_overlap_downloads.push_back(AsyncOverlapDownload{
                        .gpu_tick = gpu_tick,
                        .gpu_addr = overlap.gpu_addr,
                        .cpu_addr = overlap.cpu_addr,
                        .size = overlap.guest_size_bytes,
                        .info = overlap.info,
                        .copies = std::move(copies),
                        .map = map,
                        .cancelled = false,
                        .keep = keep,
                        .keep_ticks = keep_ticks,
                        .tick = overlap.modification_tick,
                        .address_space = overlap.address_space,
                    });
                    NoteOverlapWriteback(overlap);
                }
                ++join_writebacks;
                join_writeback_images += ids.size();
                return;
            }
        }
        std::vector<Pending> pending;
        pending.reserve(ids.size());
        for (const ImageId id : ids) {
            Image& overlap = slot_images[id];
            // As queued ones: bytes newer write-backs own stay theirs.
            KeepRanges keep;
            if (OverlapHasNewerData(id, overlap, keep)) {
                continue;
            }
            auto map = runtime.DownloadStagingBuffer(overlap.unswizzled_size_bytes);
            auto copies = FullDownloadCopies(overlap.info);
            overlap.DownloadMemory(map, copies);
            pending.push_back(Pending{id, std::move(copies), map, std::move(keep)});
        }
        if (pending.empty()) {
            return;
        }
        const auto wait_start = std::chrono::steady_clock::now();
        runtime.Finish();
        join_writeback_wait_us += static_cast<u64>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - wait_start)
                .count());
        for (auto& item : pending) {
            const Image& overlap = slot_images[item.id];
            if (WriteBackImage(overlap, item.copies, item.map.mapped_span, item.keep)) {
                ReloadUnderLanded(overlap.cpu_addr, overlap.guest_size_bytes,
                                  overlap.modification_tick);
                NoteOverlapWriteback(overlap);
            } else {
                NoteWritebackLost(overlap.cpu_addr, overlap.guest_size_bytes,
                                  overlap.modification_tick);
            }
        }
        ++join_writebacks;
        join_writeback_images += pending.size();
    }
}

template <class P>
bool TextureCache<P>::WrittenBackAlready(const ImageBase& image) const {
    // Guest memory still holds this image as it is now: the last write-back over each of its
    // pages was this image at its current modification (the 36 joins a second of a fight
    // otherwise waited for the same small targets again and again).
    const DAddr first_page = image.cpu_addr >> OverlapPageBits;
    const DAddr last_page = (image.cpu_addr + image.guest_size_bytes - 1) >> OverlapPageBits;
    for (DAddr page = first_page; page <= last_page; ++page) {
        const auto it = overlap_writeback_ticks.find(page);
        if (it == overlap_writeback_ticks.end() || it->second != image.modification_tick) {
            return false;
        }
    }
    // ...and that write-back landed (it was not cancelled).
    const auto ranges = overlap_writeback_ranges.find(first_page);
    return ranges != overlap_writeback_ranges.end() &&
           std::ranges::any_of(ranges->second, [&](const WritebackRange& range) {
               return range.begin == image.cpu_addr &&
                      range.end == image.cpu_addr + image.guest_size_bytes &&
                      range.tick == image.modification_tick && !range.lost;
           });
}

template <class P>
void TextureCache<P>::NoteWritebackLost(DAddr cpu_addr, u64 size, u64 tick) {
    const DAddr first_page = cpu_addr >> OverlapPageBits;
    const DAddr last_page = (cpu_addr + size - 1) >> OverlapPageBits;
    for (DAddr page = first_page; page <= last_page; ++page) {
        const auto it = overlap_writeback_ranges.find(page);
        if (it == overlap_writeback_ranges.end()) {
            continue;
        }
        for (WritebackRange& range : it->second) {
            if (range.begin == cpu_addr && range.end == cpu_addr + size && range.tick == tick) {
                range.lost = true;
            }
        }
    }
}

template <class P>
void TextureCache<P>::NoteOverlapWriteback(const ImageBase& image, bool lost) {
    NoteOverlapWriteback(image, lost, image.modification_tick);
}

template <class P>
void TextureCache<P>::NoteOverlapWriteback(const ImageBase& image, bool lost, u64 tick) {
    const DAddr first_page = image.cpu_addr >> OverlapPageBits;
    const DAddr last_page = (image.cpu_addr + image.guest_size_bytes - 1) >> OverlapPageBits;
    const DAddr begin = image.cpu_addr;
    const DAddr end = image.cpu_addr + image.guest_size_bytes;
    const u64 image_tick = tick;
    for (DAddr page = first_page; page <= last_page; ++page) {
        u64& page_tick = overlap_writeback_ticks[page];
        page_tick = (std::max)(page_tick, image_tick);
        auto& ranges = overlap_writeback_ranges[page];
        ranges.erase(std::remove_if(ranges.begin(), ranges.end(),
                                    [&](const WritebackRange& range) {
                                        return range.tick <= image_tick && (range.lost || !lost) &&
                                               begin <= range.begin && range.end <= end;
                                    }),
                     ranges.end());
        ranges.push_back(WritebackRange{begin, end, image_tick, lost});
        // Not simply the oldest past the limit: bytes only it holds would lose their protection
        // from older write-backs (OverlapHasNewerData).
        const DAddr page_begin = page << OverlapPageBits;
        WritebackRanges::CapPageRecords(ranges, page_begin,
                                        page_begin + (DAddr{1} << OverlapPageBits));
    }
}

template <class P>
void TextureCache<P>::KeepInWaitingWritebacks(const ImageBase& image, u64 tick) {
    if (async_overlap_downloads.empty()) {
        return;
    }
    ForEachWrittenRange(image, [&](DAddr begin, DAddr end) {
        for (AsyncOverlapDownload& download : async_overlap_downloads) {
            if (!download.cancelled) {
                WritebackRanges::KeepNewerBytes(download, begin, end, tick);
            }
        }
    });
}

template <class P>
void TextureCache<P>::NoteFlushed(const ImageBase& image) {
    NoteFlushed(image, image.modification_tick);
}

template <class P>
void TextureCache<P>::NoteFlushed(const ImageBase& image, u64 tick) {
    if (!OverlapWritebacksEnabled()) {
        return;
    }
    KeepInWaitingWritebacks(image, tick);
    if (HasGuestRange(image)) {
        NoteOverlapWriteback(image, false, tick);
    }
}

template <class P>
void TextureCache<P>::KeepNewerLanded(const ImageBase& image, u64 tick, KeepRanges& keep,
                                      std::span<const WritebackRanges::TickedRange> queued) {
    keep.clear();
    if (overlap_writeback_ranges.empty()) {
        return;
    }
    const auto waiting = [this, queued](const WritebackRange& range) {
        if (std::ranges::any_of(queued, [&](const WritebackRanges::TickedRange& download) {
                return download.tick == range.tick && download.begin == range.begin &&
                       download.end == range.end;
            })) {
            return true;
        }
        return std::ranges::any_of(
            async_overlap_downloads, [&](const AsyncOverlapDownload& download) {
                return !download.cancelled && download.tick == range.tick &&
                       download.cpu_addr == range.begin &&
                       download.cpu_addr + download.size == range.end;
            });
    };
    ForEachWrittenRange(image, [&](DAddr begin, DAddr end) {
        const DAddr first_page = begin >> OverlapPageBits;
        const DAddr last_page = (end - 1) >> OverlapPageBits;
        for (DAddr page = first_page; page <= last_page; ++page) {
            const auto it = overlap_writeback_ranges.find(page);
            if (it != overlap_writeback_ranges.end()) {
                WritebackRanges::AppendNewerLanded(keep, it->second, begin, end, tick, waiting);
            }
        }
    });
    WritebackRanges::MergeRanges(keep);
}

template <class P>
bool TextureCache<P>::OverlapWritebacksEnabled() {
    // As RunGarbageCollector and WriteBackBadOverlaps: staging maps that release themselves
    // (OpenGL) cannot wait, so neither writes back there.
    using DownloadMap = decltype(std::declval<Runtime&>().DownloadStagingBuffer(size_t{}));
    if constexpr (std::is_trivially_destructible_v<DownloadMap>) {
        return WritebackSwitches::AnyWritebackOn();
    }
    return false;
}

template <class P>
template <typename Func>
void TextureCache<P>::ForEachWrittenRange(const ImageBase& image, Func&& func) {
    if (False(image.flags & ImageFlagBits::Sparse)) {
        if (HasGuestRange(image)) {
            func(image.cpu_addr, image.cpu_addr + image.guest_size_bytes);
        }
        return;
    }
    // As ForEachSparseSegment: each mapped segment is one range of guest memory.
    const auto memory = WriteBackMemory(image);
    if (!memory) {
        return;
    }
    for (const auto& [gpu_addr, size] :
         memory->GetSubmappedRange(image.gpu_addr, image.guest_size_bytes)) {
        if (const std::optional<DAddr> cpu_addr = memory->GpuToCpuAddress(gpu_addr)) {
            func(*cpu_addr, *cpu_addr + size);
        }
    }
}

template <class P>
bool TextureCache<P>::AsyncOverlapDownloadsEnabled() {
    if constexpr (requires { runtime.CurrentTick(); }) {
        return true;
    }
    return false;
}

template <class P>
u64 TextureCache<P>::OverlapDownloadMaxBytes() {
    // Larger overlapping images are
    // dropped as before. The Duskbloods' character maps are 16x42 to 256x256; the large ones are
    // full-screen and shadow targets it draws again before reading.
    return 1024ULL << 10;
}

template <class P>
bool TextureCache<P>::LosesGpuData(const ImageBase& image) {
    return Tegra::g_nx2_ampere.load(std::memory_order_relaxed) &&
           True(image.flags & ImageFlagBits::GpuModified) &&
           False(image.flags & (ImageFlagBits::CpuModified | ImageFlagBits::Sparse |
                                ImageFlagBits::Unbacked));
}

template <class P>
void TextureCache<P>::MarkStale(const ImageBase& image) {
    if (!LosesGpuData(image)) {
        return;
    }
    if (Common::DkbFrameTrace::Enabled()) {
        static u64 marked{};
        if (marked++ % 32 == 0) {
            LOG_INFO(HW_GPU, "DKB stale mark #{} gpu={:#x} device={:#x} size={:#x} {}x{} format={} flags={:#x}",
                     marked, image.gpu_addr, image.cpu_addr, image.guest_size_bytes,
                     image.info.size.width, image.info.size.height,
                     static_cast<u32>(image.info.format), static_cast<u32>(image.flags));
        }
    }
    // Tracked from here on so CPU writes reach WriteMemory. Marking before the image's own
    // tracking goes keeps the pages' write protection from switching off and on again.
    stale_pages.Mark(image.cpu_addr, image.guest_size_bytes, [this](DAddr addr, u64 size) {
        device_memory.UpdatePagesCachedCount(addr, size, 1);
    });
}

template <class P>
void TextureCache<P>::ClearStale(DAddr addr, size_t size) {
    evicted_pages.Clear(addr, size, [](DAddr, u64) {});
    stale_pages.Clear(addr, size, [this](DAddr run_addr, u64 run_size) {
        device_memory.UpdatePagesCachedCount(run_addr, run_size, -1);
    });
}

template <class P>
std::shared_ptr<Tegra::MemoryManager> TextureCache<P>::WriteBackMemory(const ImageBase& image) {
    // The garbage collector and CPU-side downloads reach images of every address space, so the
    // bound channel's memory may map image.gpu_addr to other memory. The image's own address
    // space writes it; once nvdrv has closed that one, another address space that maps all of
    // the image's GPU range to its device memory does (FindImage shares images between them).
    // Only for an image its own address space mapped to one device range and has not remapped
    // since: the device pages of any other are no longer known, so it is not written back
    // (MarkStale) rather than risk writing other memory.
    // The returned reference keeps the memory manager alive while the image is written.
    boost::container::small_vector<std::shared_ptr<Tegra::MemoryManager>, 4> others;
    {
        std::scoped_lock lock{config_mutex};
        for (const auto& [id, ref] : address_spaces) {
            std::shared_ptr<Tegra::MemoryManager> memory = ref.owner.lock();
            if (!memory) {
                continue;
            }
            if (id == image.address_space) {
                return memory;
            }
            others.push_back(std::move(memory));
        }
    }
    if (True(image.flags &
             (ImageFlagBits::Sparse | ImageFlagBits::NonContiguous | ImageFlagBits::Remapped))) {
        return nullptr;
    }
    for (std::shared_ptr<Tegra::MemoryManager>& memory : others) {
        if (memory->GpuToCpuAddress(image.gpu_addr) == image.cpu_addr &&
            memory->IsContinuousRange(image.gpu_addr, image.guest_size_bytes)) {
            return std::move(memory);
        }
    }
    return nullptr;
}

template <class P>
bool TextureCache<P>::WriteBackImage(const ImageBase& image,
                                     std::span<const BufferImageCopy> copies,
                                     std::span<const u8> data, const KeepRanges& keep) {
    const std::shared_ptr<Tegra::MemoryManager> memory = WriteBackMemory(image);
    if (!memory) {
        return false;
    }
    SwizzleKeeping(*memory, image.gpu_addr, image.info, copies, data, keep);
    if (stale_pages.Count() == 0) {
        return true;
    }
    if (False(image.flags & ImageFlagBits::Sparse)) {
        ClearStale(image.cpu_addr, image.guest_size_bytes);
    } else {
        ClearStaleGpu(*memory, image.gpu_addr, image.guest_size_bytes);
    }
    return true;
}

template <class P>
void TextureCache<P>::ClearStaleGpu(Tegra::MemoryManager& memory, GPUVAddr gpu_addr, size_t size) {
    if (stale_pages.Count() == 0) {
        return;
    }
    // Each piece is contiguous in device memory (GetSubmappedRange splits where it is not).
    for (const auto& [piece_addr, piece_size] : memory.GetSubmappedRange(gpu_addr, size)) {
        if (const auto device_addr = memory.GpuToCpuAddress(piece_addr)) {
            ClearStale(*device_addr, piece_size);
        }
    }
}

template <class P>
void TextureCache<P>::ApplyThresholdOverrides(bool log) {
    // Past what a frame needs, more device memory only keeps the textures of earlier scenes
    // (title 1 filled 6.7 GB of an 8 GB card and draws its races in ~5 GB), so collect from
    // MaxCriticalMemory on rather than near the card's budget. Larger cards get 5/8 of the
    // driver's budget (GetDeviceLocalMemory is capped near 7 GiB): at 5 GiB title 1's races
    // recreate and upload ~8 MiB of render targets a frame (16 GB card: 53 -> 59 fps at 10 GiB).
    // What the device budget allows (budget minus margins), before that cap.
    const u64 budget_critical = critical_memory;
    const u64 budget_expected = expected_memory;
    u64 max_critical = MaxCriticalMemory;
    if constexpr (requires { runtime.GetDeviceMemoryBudget(); }) {
        max_critical = (std::max)(max_critical, runtime.GetDeviceMemoryBudget() / 8 * 5);
    }
    critical_memory = (std::min)(critical_memory, max_critical);
    expected_memory =
        (std::min)(expected_memory, critical_memory - (std::min)(critical_memory, u64{768_MiB}));
    minimum_memory =
        (std::min)(minimum_memory, expected_memory - (std::min)(expected_memory, u64{1_GiB}));
    // EDEN_NX2_TC_EXPECTED_MIB / EDEN_NX2_TC_CRITICAL_MIB raise the collector's thresholds past
    // MaxCriticalMemory, but never past what the device budget allows: set outright, they kept
    // the cache above a shrinking budget (other programs' video memory, out-of-memory margins) and
    // the driver paged (drops to 9 fps on an 8 GB card in Ocarina's Kokiri Forest).
    for (const auto& [name, target, limit] :
         {std::tuple{"EDEN_NX2_TC_EXPECTED_MIB", &expected_memory, budget_expected},
          std::tuple{"EDEN_NX2_TC_CRITICAL_MIB", &critical_memory, budget_critical}}) {
        if (const char* value = std::getenv(name)) {
            *target = (std::min)(static_cast<u64>(std::strtoull(value, nullptr, 10)) << 20, limit);
        }
    }
    expected_memory = (std::min)(expected_memory, critical_memory);
    if (log) {
        LogThresholds();
    }
}

template <class P>
void TextureCache<P>::LogThresholds() const {
    LOG_INFO(HW_GPU, "Texture cache thresholds: expected {} MiB, critical {} MiB, minimum {} MiB",
             expected_memory >> 20, critical_memory >> 20, minimum_memory >> 20);
}

template <class P>
void TextureCache<P>::EvictDeadRenderTargets() {
    // While the cache is over its expected size, render targets their lookup created and that went
    // unused for DeadTargetFrames are deleted now, before the age-based collector reaches them.
    // In Ocarina of Time's Market the crowds make a new target every couple of frames; the dead
    // ones piled up, the cache went over the card's budget, the driver moved video memory into
    // system RAM and on an 8 GB card the device was lost after ~1-4 min. The collector can't keep
    // up there: it waits 10-50 frames and downloads each GPU-written image first. These are
    // marked stale instead, the way the collector handles an image it doesn't download, with two
    // exceptions so no data the game reads again is lost:
    // - targets under MinEvictBytes (text and label boxes sized to their contents) are left to
    //   the collector: they barely relieve memory and were most of what the game read back;
    // - a shape the game read back after an early deletion (FillImageViews) is written back to
    //   guest memory before deletion, a few a frame, or left to the collector if it can't be.
    static constexpr u64 DeadTargetFrames = 8;
    // At most this many deletions a frame, so a long queue doesn't turn into one long frame when
    // pressure starts; the Market needs about 10 a frame.
    static constexpr u32 MaxEvictionsPerFrame = 64;
    // How often a target found still in use is checked again before it counts as a lasting one.
    static constexpr u32 MaxChecks = 4;
    // (FindRenderTargetView keeps the queue at MaxTransientTargets.)
    if (total_used_memory <= expected_memory) {
        return;
    }
    u32 evicted = 0;
    // Shapes the game read back after an early deletion are written back to guest memory first,
    // at most this many a frame; the rest wait for a later frame.
    static constexpr u32 MaxSavesPerFrame = 4;
    u32 saved = 0;
    for (size_t budget = transient_targets.size(); budget != 0 && !transient_targets.empty();
         --budget) {
        if (evicted >= MaxEvictionsPerFrame || total_used_memory <= expected_memory) {
            break;
        }
        TransientTarget entry = transient_targets.front();
        if (frame_tick - entry.check_tick < DeadTargetFrames) {
            break;
        }
        transient_targets.pop_front();
        if (!slot_images.contains(entry.id)) {
            continue;
        }
        Image& image = slot_images[entry.id];
        if (image.allocation_tick != entry.allocation_tick ||
            False(image.flags & ImageFlagBits::TransientTarget) ||
            False(image.flags & ImageFlagBits::Registered)) {
            continue;
        }
        const u64 last_use = lru_cache.GetTick(image.lru_index);
        if (frame_tick - last_use < DeadTargetFrames ||
            True(image.flags & ImageFlagBits::IsDecoding)) {
            // Still in use: check again later; one used for long is a lasting target.
            if (++entry.survived < MaxChecks) {
                entry.check_tick = frame_tick;
                transient_targets.push_back(entry);
            } else {
                image.flags &= ~ImageFlagBits::TransientTarget;
            }
            continue;
        }
        if (image.guest_size_bytes < MinEvictBytes) {
            // Small targets (text and label boxes, sized to their contents) barely relieve memory
            // and are the ones read back most often: leave them to the collector.
            image.flags &= ~ImageFlagBits::TransientTarget;
            ++evict_small_kept;
            continue;
        }
        // Dropped without a write-back: stale, and newer than what older images would write
        // back (the collector does the same for a GPU-modified image it drops).
        const auto NoteLostTransientTarget = [this](ImageBase& lost_image) {
            if (OverlapWritebacksEnabled() &&
                True(lost_image.flags & ImageFlagBits::GpuModified) && HasGuestRange(lost_image)) {
                NoteOverlapWriteback(lost_image, true);
            }
            MarkStale(lost_image); // before UntrackImage (see MarkStale)
        };
        bool saved_this = false;
        if (readback_shapes.contains(RtShapeKey(image))) {
            if (!IsDownloadable(image) || True(image.flags & ImageFlagBits::BadOverlap)) {
                // Can't be written back safely: leave it to the collector.
                image.flags &= ~ImageFlagBits::TransientTarget;
                ++evict_spared;
                continue;
            }
            if (saved >= MaxSavesPerFrame) {
                entry.check_tick = frame_tick;
                transient_targets.push_back(entry);
                continue;
            }
            auto map = runtime.DownloadStagingBuffer(image.unswizzled_size_bytes);
            const auto copies = FullDownloadCopies(image.info);
            image.DownloadMemory(map, copies);
            runtime.Finish();
            // As the collector's write-back: through the image's own address space
            // (WriteBackImage, 0b48a2f74d), keeping newer write-backs that already landed, and
            // noted (NoteFlushed) so older ones still queued don't land over it. An address space
            // closed since writes nothing; the pages go stale as for a target not saved.
            bool written = false;
            try {
                KeepRanges newer;
                KeepNewerLanded(image, image.modification_tick, newer);
                written = WriteBackImage(image, copies, map.mapped_span, newer);
            } catch (const std::bad_alloc&) {
                // A read-back shape: leave it to the collector rather than drop its data.
                NoteHostOutOfMemory("write back", image);
                image.flags &= ~ImageFlagBits::TransientTarget;
                ++evict_spared;
                continue;
            }
            if (written) {
                NoteFlushed(image);
                ++saved;
                ++evict_saved;
                saved_this = true;
            } else {
                NoteLostTransientTarget(image);
            }
        } else {
            NoteLostTransientTarget(image);
        }
        if (!saved_this && LosesGpuData(image)) {
            // Only a target whose GPU data is really lost can teach a read-back shape.
            evicted_pages.Mark(image.cpu_addr, image.guest_size_bytes, [](DAddr, u64) {});
            const u64 sequence = ++eviction_sequence;
            recent_evictions[image.cpu_addr] = {image.cpu_addr + image.guest_size_bytes,
                                                RtShapeKey(image), sequence};
            recent_eviction_order.emplace_back(image.cpu_addr, sequence);
            if (recent_eviction_order.size() > MaxRecentEvictions) {
                const auto [old_addr, old_sequence] = recent_eviction_order.front();
                recent_eviction_order.pop_front();
                if (const auto it = recent_evictions.find(old_addr);
                    it != recent_evictions.end() && it->second.sequence == old_sequence) {
                    recent_evictions.erase(it);
                }
            }
        }
        if (True(image.flags & ImageFlagBits::Tracked)) {
            UntrackImage(image, entry.id);
        }
        // Keeps the collector's VMA block fill ratios right until their next rebuild (as its
        // Cleanup does).
        if constexpr (requires(const Image& img) { img.MemoryBlock(); }) {
            const auto block = image.MemoryBlock();
            if (block.block != 0) {
                const auto it = block_image_bytes.find(block.block);
                if (it != block_image_bytes.end()) {
                    it->second -= (std::min)(it->second, block.size);
                }
            }
        }
        UnregisterImage(entry.id);
        trace_delete_reason = "dead-rt";
        DeleteImage(entry.id, false);
        ++transient_evictions;
        ++evicted;
    }
    if (frame_tick % 600 == 0 && transient_evictions != logged_transient_evictions) {
        LOG_INFO(HW_GPU, "Dead render targets deleted early: {} so far, {} waiting",
                 transient_evictions, transient_targets.size());
        LOG_INFO(HW_GPU, "Dead render targets: {} small ones and {} read-back shapes not "
                         "downloadable left to the collector, {} written back before deletion; "
                         "{} of {} images made over deleted pages were sampled before any draw, "
                         "{} shapes known",
                 evict_small_kept, evict_spared, evict_saved, evict_read_back, evict_born,
                 readback_shapes.size());
        logged_transient_evictions = transient_evictions;
    }
}

template <class P>
void TextureCache<P>::TickFrame() {
    FinishAsyncOverlapDownloads(false);
    if (Common::DkbFrameTrace::Enabled()) {
        static u64 frames{};
        if (++frames % 600 == 0 && (overlap_download_batches != 0 || overlap_download_skips != 0 || join_writebacks != 0 ||
                                    join_writeback_reuses != 0)) {
            LOG_INFO(HW_GPU,
                     "DKB gc overlap downloads last 600 frames: {} batches, {} images, {} KiB, "
                     "waited {:.1f} ms, {} skipped under newer data, {} written around newer "
                     "bytes, {} pages noted, {} cancelled",
                     overlap_download_batches, overlap_download_images,
                     overlap_download_bytes >> 10,
                     static_cast<double>(overlap_download_wait_us) / 1000.0,
                     overlap_download_skips, overlap_partial_keeps,
                     overlap_writeback_ticks.size(), async_overlap_cancels);
            async_overlap_cancels = 0;
            overlap_partial_keeps = 0;
            if (join_writebacks != 0) {
                LOG_INFO(HW_GPU,
                         "DKB join writebacks last 600 frames: {} joins, {} images, waited {:.1f} ms, {} "
                         "already in memory",
                         join_writebacks, join_writeback_images,
                         static_cast<double>(join_writeback_wait_us) / 1000.0,
                         join_writeback_reuses);
            }
            join_writebacks = join_writeback_images = join_writeback_wait_us = 0;
            join_writeback_reuses = 0;
            overlap_download_skips = 0;
            overlap_download_batches = overlap_download_images = 0;
            overlap_download_bytes = overlap_download_wait_us = 0;
        }
    }
    // Upload buffers grown for one huge image (title 1's 341 MiB terrain layer pools) are freed
    // rather than kept for the rest of the session.
    for (auto* const scratch : {&swizzle_data_buffer, &unswizzle_data_buffer}) {
        if (scratch->capacity() > LargeScratchCapacity) {
            scratch->release();
        }
    }
    if constexpr (requires { runtime.CurrentTick(); }) {
        while (!gpu_sentenced_images.empty() &&
               runtime.IsFree(gpu_sentenced_images.front().tick)) {
            gpu_sentenced_bytes -= gpu_sentenced_images.front().bytes;
            gpu_sentenced_images.pop_front();
        }
    }
    // If we can obtain the memory info, use it instead of the estimate. Read before the budget
    // check below, which counts what isn't images from it.
    if (runtime.CanReportMemoryUsage()) {
        total_used_memory = runtime.GetDeviceMemoryUsage();
        // Images the collector deleted keep their memory for TICKS_TO_DESTROY frames: counting
        // it kept the collector in aggressive mode, deleting 40+ images a frame for frames after
        // it had freed enough (title 1: frames over 100 ms for seconds at a time).
        for (const u64 bytes : sentenced_image_bytes) {
            total_used_memory -= (std::min)(total_used_memory, bytes);
        }
        total_used_memory -= (std::min)(total_used_memory, gpu_sentenced_bytes);
    }
    bool spill_collect = false; ///< Memory spilled to host memory: collect hard this frame
    if constexpr (HAS_DEVICE_MEMORY_INFO) {
        // Switch 2 titles (native tiling) stream far more texture data than Eden's thresholds
        // were tuned for: title 1's attract loop alone keeps ~5.3 GB resident, so the collector
        // sat in aggressive mode evicting decoded ASTC (and decoding it again). Total use is
        // measured from the driver, so collect only near the device budget instead.
        if (!native_thresholds_applied && gpu_memory != nullptr && gpu_memory->UsesNativeTiling()) {
            native_thresholds_applied = true;
            const u64 device_local = runtime.GetDeviceLocalMemory();
            critical_memory = (std::max)(critical_memory, device_local - 256_MiB);
            expected_memory = (std::max)(expected_memory, device_local - 1280_MiB);
            ApplyThresholdOverrides();
            native_critical_memory = critical_memory;
            native_expected_memory = expected_memory;
        }
        // Follow the driver's budget (re-read now and then): it shrinks when other programs use
        // VRAM (another game: 7.0 -> 6.4 GB), and collecting only past it means the driver pages
        // textures out. It is also the room to use: Eden's device memory figure is capped near
        // 7 GB, which on a 12 GB card (11.2 GB budget) kept title 1 evicting textures with 3 GB
        // unused. Both thresholds stay under it with a margin (1/8 of it, at most 1 GiB): past it
        // allocations fail and images end up unbacked (garbage textures), and Windows shrinks the
        // budget as the card fills (16 GB card: 13.5 GB left mid-race), hence the re-read every 30
        // frames. EDEN_NX2_TC_*_MIB overrides can raise the thresholds up to this budget.
        if constexpr (requires { runtime.GetDeviceMemoryBudget(); }) {
            if constexpr (requires { runtime.MemoryPressureEvents(); }) {
                // Images that did not fit (made in host memory or with nothing bound) mean the
                // thresholds sit too high for this card and driver: leave more room until the
                // pressure has been gone for a while.
                if (native_thresholds_applied) {
                    const u32 events = runtime.MemoryPressureEvents();
                    if (events != seen_pressure_events) {
                        seen_pressure_events = events;
                        pressure_margin = (std::min)(pressure_margin + 512_MiB, 3_GiB);
                        pressure_quiet_frames = 0;
                        budget_check_frames = 0; // thresholds are recomputed below
                        LOG_WARNING(HW_GPU, "Out of device memory: keeping {} MiB more free",
                                    pressure_margin >> 20);
                    } else if (pressure_margin != 0 && ++pressure_quiet_frames % 600 == 0) {
                        pressure_margin -= (std::min)(pressure_margin, 128_MiB);
                        budget_check_frames = 0;
                    }
                }
            }
            if constexpr (requires { runtime.TakeSpilledBytes(); }) {
                // Device-local memory the allocator placed in host memory (heap over its
                // budget, discrete GPU). RTX 4050 Laptop in title 1's Castle Town: 290-420 MiB
                // spilled for minutes, 50-57 -> 20-26 fps. Answering each spill like an
                // out-of-memory event (+512 MiB) took the margin to 1.5 GiB while the scene
                // loaded (the old scene still resident) and left the cache under the new
                // scene's working set: ASTC decodes 55/s -> 950/s, no better fps. So, per
                // episode: first a few forced collects; only if it still spills after them,
                // a margin of what spilled in the last ~2 s plus 128 MiB (at most 768 MiB),
                // which shrinks by 128 MiB every 240 quiet frames.
                if (native_thresholds_applied) {
                    constexpr u64 SpillWindowFrames = 120;
                    constexpr u64 SpillGraceFrames = 60;
                    constexpr u64 SpillCollectSpacing = 15;
                    constexpr u32 MaxSpillCollects = 4;
                    constexpr u64 SpillDecayFrames = 240;
                    constexpr u64 SpillEpisodeEndFrames = 300;
                    const u64 spilled = runtime.TakeSpilledBytes();
                    if (frame_tick - spill_window_start >= SpillWindowFrames) {
                        spill_window_start = frame_tick;
                        spill_window_bytes = 0;
                    }
                    spill_log_bytes += spilled;
                    if (spilled != 0) {
                        spill_window_bytes += spilled;
                        spill_quiet_frames = 0;
                        if (spill_collect_tick == 0) {
                            spill_collect_tick = frame_tick + 1;
                        }
                        // The forced collects run only on frames that spill, so a sparse
                        // episode may still have some left after the grace period: use them
                        // all before raising the margin.
                        const u64 since = frame_tick + 1 - spill_collect_tick;
                        if (spill_forced_collects < MaxSpillCollects) {
                            if (spill_forced_collects == 0 ||
                                frame_tick >= spill_last_collect + SpillCollectSpacing) {
                                spill_last_collect = frame_tick;
                                ++spill_forced_collects;
                                ++spill_log_collects;
                                spill_collect = true;
                            }
                        } else if (since >= SpillGraceFrames) {
                            const u64 target = (std::min)(spill_window_bytes + 128_MiB, 768_MiB);
                            if (target > spill_margin) {
                                spill_margin = target;
                                budget_check_frames = 0; // thresholds are recomputed below
                                LOG_WARNING(HW_GPU,
                                            "Still spilling to host memory after {} forced "
                                            "collects: keeping {} MiB more free",
                                            spill_forced_collects, spill_margin >> 20);
                            }
                        }
                    } else {
                        ++spill_quiet_frames;
                        if (spill_quiet_frames == SpillEpisodeEndFrames) {
                            spill_collect_tick = 0;
                            spill_forced_collects = 0;
                        }
                        if (spill_margin != 0 && spill_quiet_frames % SpillDecayFrames == 0) {
                            spill_margin -= (std::min)(spill_margin, 128_MiB);
                            budget_check_frames = 0;
                        }
                    }
                    if (spill_margin != 0 || spill_log_bytes != 0) {
                        const auto now = std::chrono::steady_clock::now();
                        if (now - spill_log_time >= std::chrono::seconds{10}) {
                            spill_log_time = now;
                            LOG_INFO(HW_GPU,
                                     "Host spill: {} MiB placed in host memory and {} forced "
                                     "collects since the last line; spill margin {} MiB, out of "
                                     "memory margin {} MiB, critical {} MiB",
                                     spill_log_bytes >> 20, spill_log_collects, spill_margin >> 20,
                                     pressure_margin >> 20, critical_memory >> 20);
                            spill_log_bytes = 0;
                            spill_log_collects = 0;
                        }
                    }
                }
            }
            // The larger of the two answers to memory that did not fit.
            const u64 fit_margin = (std::max)(pressure_margin, spill_margin);
            if (native_thresholds_applied && budget_check_frames++ % 30 == 0) {
                const u64 budget = runtime.GetDeviceMemoryBudget();
                retention_limit = 0;
                if (budget > 2_GiB) {
                    const u64 margin = (std::min)(budget / 8, 1_GiB);
                    if constexpr (requires { runtime.GetDeviceMemorySlack(); }) {
                        // The usage counted here leaves out the unused part of the allocator's
                        // blocks, which the driver counts and a full heap cannot hand to a new
                        // image (RX 7800 XT: 1.06 GiB of it, the whole margin, with the heap at
                        // its budget for a minute before images stopped fitting). Reserve it
                        // too: at once when it grows, slowly when it shrinks, so evicting images
                        // (which leaves partly used blocks) does not chase the threshold down.
                        const u64 slack = (std::min)(runtime.GetDeviceMemorySlack(), 1536_MiB);
                        slack_margin = slack >= slack_margin
                                           ? slack
                                           : slack_margin - (std::min)(slack_margin - slack, 16_MiB);
                    }
                    const u64 reserved = margin + fit_margin + slack_margin;
                    critical_memory = budget - (std::min)(reserved, budget / 2);
                    // The device's budget is not all the room there is: see HostCriticalMemory.
                    // The usage is the driver's, which still counts images waiting to be freed.
                    const std::optional<u64> commit_room = Common::GetProcessCommitRoom();
                    const u64 host_critical =
                        commit_room
                            ? HostCriticalMemory(runtime.GetDeviceMemoryUsage(), *commit_room)
                            : (std::numeric_limits<u64>::max)();
                    critical_memory = (std::min)(critical_memory, host_critical);
                    retention_budget = RetentionBudget(retention_budget, budget);
                    const u64 room_margin =
                        (std::min)(fit_margin + slack_margin, retention_budget / 2);
                    retention_limit = (std::min)(retention_budget - room_margin, host_critical);
                    expected_memory = critical_memory - (std::min)(margin, critical_memory / 2);
                    ApplyThresholdOverrides(false);
                    u64 other_memory = 0;
                    if constexpr (requires { runtime.GetDeviceHeapMemoryBudget(); }) {
                        // An integrated GPU's budget is the share of system memory it is capped
                        // to (4 or 6 GiB) so that textures don't fill RAM, but the usage compared
                        // with the thresholds is everything in device memory. Ocarina's ~4 GB of
                        // other allocations kept a Radeon 860M past critical with 1.3 GB of
                        // images: deleting them all could not get under it, and the collector
                        // recreated ~480 images a second, downloading ~70 with a GPU wait (#739).
                        // There the share bounds the images: what isn't images is added to the
                        // thresholds, up to the heap's own budget and the host's commit. The
                        // EDEN_NX2_TC_*_MIB overrides set the thresholds by hand, so they are left
                        // as the overrides made them.
                        static const bool overridden =
                            std::getenv("EDEN_NX2_TC_EXPECTED_MIB") != nullptr ||
                            std::getenv("EDEN_NX2_TC_CRITICAL_MIB") != nullptr;
                        const u64 heap_budget = runtime.GetDeviceHeapMemoryBudget();
                        if (heap_budget > budget) {
                            u64 image_bytes = 0;
                            for (const auto [image_id, image] : slot_images) {
                                image_bytes += ImageAllocationBytes(*image);
                                if (image->HasScaled()) {
                                    image_bytes += GetScaledImageSizeBytes(*image);
                                }
                            }
                            other_memory =
                                total_used_memory - (std::min)(total_used_memory, image_bytes);
                        }
                        if (heap_budget > budget && !overridden) {
                            const u64 heap_critical =
                                heap_budget - (std::min)(reserved, heap_budget / 2);
                            const u64 gap = critical_memory - expected_memory;
                            const u64 raised = (std::min)(
                                (std::min)(critical_memory + other_memory, heap_critical),
                                host_critical);
                            critical_memory = (std::max)(critical_memory, raised);
                            expected_memory =
                                critical_memory - (std::min)(gap, critical_memory / 2);
                            // The retention floor is compared with the same usage, so it gets the
                            // same raise: without it the usage stayed past the floor, and images
                            // went after the mode's age instead of MinEvictionAge.
                            const u64 heap_room =
                                heap_budget - (std::min)(room_margin, heap_budget / 2);
                            retention_limit = (std::min)(
                                (std::min)(retention_limit + other_memory, heap_room),
                                host_critical);
                        }
                    }
                    // Log the first values and real changes (the budget moves by a few MiB all
                    // the time). Compared after the cap: before it, every check differed from
                    // the capped value by more than 256 MiB and logged twice a second. Compared
                    // with the last values logged, so a threshold that host memory lowers a
                    // little at each check is logged too.
                    const u64 change = critical_memory > logged_critical_memory
                                           ? critical_memory - logged_critical_memory
                                           : logged_critical_memory - critical_memory;
                    const bool log = budget_check_frames == 1 || change >= 256_MiB;
                    if (log) {
                        logged_critical_memory = critical_memory;
                        LogThresholds();
                        LOG_INFO(HW_GPU,
                                 "Memory margins: base {} MiB, out of memory {} MiB, host spill {} "
                                 "MiB, block slack {} MiB, not textures {} MiB; budget {} MiB, "
                                 "retention limit {} MiB (lowest budget lately {} MiB)",
                                 margin >> 20, pressure_margin >> 20, spill_margin >> 20,
                                 slack_margin >> 20, other_memory >> 20, budget >> 20,
                                 retention_limit >> 20, retention_budget >> 20);
                        if (commit_room) {
                            LOG_INFO(HW_GPU,
                                     "Host memory: {} MiB of commit left for this process, {}",
                                     *commit_room >> 20,
                                     critical_memory == host_critical
                                         ? "which bounds the thresholds"
                                         : "the device budget bounds the thresholds");
                        }
                    }
                }
            }
        }
    }
    // EDEN_NX2_MEM_TRACE=1: every 2 s, what the cache holds next to what the process and the
    // driver report (memory climbs).
    static const bool mem_trace = std::getenv("EDEN_NX2_MEM_TRACE") != nullptr;
    if (const auto now = std::chrono::steady_clock::now();
        mem_trace && now - mem_trace_time >= std::chrono::seconds{2}) {
        mem_trace_time = now;
        u64 image_bytes{};
        u64 gpu_written_bytes{};
        size_t images{};
        size_t unbacked{};
        for (const auto [image_id, image] : slot_images) {
            ++images;
            if (True(image->flags & ImageFlagBits::Unbacked)) {
                ++unbacked;
                continue;
            }
            const u64 bytes = GetScaledImageSizeBytes(*image);
            image_bytes += bytes;
            if (True(image->flags & ImageFlagBits::GpuModified)) {
                gpu_written_bytes += bytes;
            }
        }
        u64 decode_bytes{};
        for (const auto& decode : async_decodes) {
            // The worker sizes the buffer before it publishes complete.
            if (decode->complete.load(std::memory_order_acquire)) {
                decode_bytes += decode->decoded_data.capacity();
            }
        }
        const u64 sentenced = std::accumulate(sentenced_image_bytes.begin(),
                                              sentenced_image_bytes.end(), gpu_sentenced_bytes);
        LOG_INFO(HW_GPU,
                 "Memory trace: texture cache {} images {} MiB ({} MiB GPU-written, {} unbacked), "
                 "device usage {} MiB (expected {} MiB, critical {} MiB), sentenced {} MiB, {} "
                 "async decodes {} MiB, scratch {} MiB",
                 images, image_bytes >> 20, gpu_written_bytes >> 20, unbacked,
                 total_used_memory >> 20, expected_memory >> 20, critical_memory >> 20,
                 sentenced >> 20, async_decodes.size(), decode_bytes >> 20,
                 (swizzle_data_buffer.capacity() + unswizzle_data_buffer.capacity()) >> 20);
        if constexpr (requires { runtime.LogMemoryTrace(); }) {
            runtime.LogMemoryTrace();
        }
    }
    // Every 600 frames: device memory in use against the collector's thresholds.
    // Skip expensive heap allocation, map construction, and sorting unless debug logging is explicitly enabled.
    static const bool log_tc = std::getenv("EDEN_NX2_TC_LOG") != nullptr;
    static u64 frames{};
    if (log_tc && ++frames % 600 == 0) {
        u64 image_bytes{};
        size_t images{};
        std::vector<std::pair<u64, const Image*>> largest;
        std::map<u32, std::pair<u64, u32>> by_format;
        for (const auto [image_id, image] : slot_images) {
            const u64 bytes = GetScaledImageSizeBytes(*image);
            image_bytes += bytes;
            ++images;
            if (debug_stats) {
                largest.emplace_back(bytes, image);
                auto& format = by_format[static_cast<u32>(image->info.format)];
                format.first += bytes;
                ++format.second;
            }
        }
        LOG_INFO(HW_GPU, "Texture cache: device memory {} MiB (expected {} MiB, critical {} MiB), {} images {} MiB",
                 total_used_memory >> 20, expected_memory >> 20, critical_memory >> 20, images,
                 image_bytes >> 20);
        std::partial_sort(largest.begin(), largest.begin() + std::min<size_t>(12, largest.size()),
                          largest.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (size_t i = 0; i < std::min<size_t>(12, largest.size()); ++i) {
            const Image& image = *largest[i].second;
            LOG_DEBUG(HW_GPU, "  {} MiB: {:#x} {}x{}x{} layers {} levels {} format {} type {} flags {:#x}",
                     largest[i].first >> 20, image.gpu_addr, image.info.size.width, image.info.size.height,
                     image.info.size.depth, image.info.resources.layers, image.info.resources.levels,
                     static_cast<u32>(image.info.format), static_cast<u32>(image.info.type),
                     static_cast<u32>(image.flags));
        }
        std::vector<std::pair<u64, u32>> formats;
        for (const auto& [format, total] : by_format) formats.emplace_back(total.first, format);
        std::sort(formats.rbegin(), formats.rend());
        for (size_t i = 0; i < std::min<size_t>(8, formats.size()); ++i) {
            LOG_DEBUG(HW_GPU, "  format {}: {} MiB in {} images", formats[i].second, formats[i].first >> 20,
                     by_format[formats[i].second].second);
        }
        if constexpr (requires { runtime.LogMemoryStatistics(); }) {
            runtime.LogMemoryStatistics();
        }
        LOG_INFO(HW_GPU, "Texture cache last 600 frames: created {} images, deleted {} (collector {}, {} costly, {} downloaded), {} async decodes, ASTC {} opaque / {} alpha",
                 images_created, images_deleted, gc_deletes + gc_costly_deletes, gc_costly_deletes,
                 gc_download_deletes,
                 async_decodes_queued, astc_opaque_images, astc_alpha_images);
        LOG_INFO(HW_GPU, "Texture cache last 600 frames: {} uploads from guest memory, {} MiB (largest {} MiB, {} by layer)",
                 images_uploaded, uploaded_bytes >> 20, largest_upload >> 20, layer_uploads);
        LOG_INFO(HW_GPU,
                 "Texture cache last 600 frames: stale uploads skipped {} ({} MiB), {} uploaded "
                 "for buffer-written pages; {} MiB of pages marked stale",
                 tc_stale_skips, tc_stale_skip_bytes >> 20, tc_stale_gpu_written,
                 (stale_pages.Count() << StalePages::PAGE_BITS) >> 20);
        tc_stale_skips = tc_stale_skip_bytes = tc_stale_gpu_written = 0;
        images_uploaded = uploaded_bytes = largest_upload = layer_uploads = 0;
        astc_opaque_images = astc_alpha_images = 0;
        std::vector<std::pair<u32, u64>> shapes;
        for (const auto& [key, value] : created_shapes) shapes.emplace_back(value.first, key);
        std::sort(shapes.rbegin(), shapes.rend());
        for (size_t i = 0; i < std::min<size_t>(10, shapes.size()); ++i) {
            const u64 key = shapes[i].second;
            LOG_DEBUG(HW_GPU, "  created {}x: format {} type {} levels {} layers {} {}x{} (last at {:#x})",
                     shapes[i].first, key >> 56, (key >> 52) & 0xF, (key >> 48) & 0xF,
                     (key >> 36) & 0xFFF, (key >> 18) & 0x3FFFF, key & 0x3FFFF,
                     created_shapes[key].second);
        }
        created_shapes.clear();
        // The per-image and per-shape tallies only feed debug lines: skip them while those are off.
        debug_stats = Common::Log::IsLevelEnabled(Common::Log::Class::HW_GPU, Common::Log::Level::Debug);
        gc_deletes = gc_costly_deletes = gc_download_deletes = async_decodes_queued = 0;
        images_created = images_deleted = 0;
    }
    EvictDeadRenderTargets();
    // An image that could not be made at all: the driver is out of memory whatever the counts
    // say, so collect as at the critical threshold.
    bool out_of_memory = false;
    if constexpr (requires { runtime.FailedImages(); }) {
        const u32 failed = runtime.FailedImages();
        out_of_memory = failed != seen_failed_images;
        seen_failed_images = failed;
    }
    // A forced collect for memory spilled to host memory collects as at the critical threshold
    // for this frame: it stops once usage is under it and keeps the retention floor. Collecting as
    // out of memory instead deleted every image unused for 10 frames, decoded ASTC included, on a
    // 4 GB GTX 1650 whose scene does not fit (#2127): each spill episode evicted the working set,
    // which was decoded again (ASTC decodes 1000-1585 per 5 s against 2-15 on a 16 GB card) and
    // spilled again, with frames up to 2.5 s.
    if (out_of_memory || spill_collect || total_used_memory > minimum_memory) {
        RunGarbageCollector(out_of_memory, spill_collect);
    }
    if (out_of_memory) {
        // Textures left out for lack of memory are made again on their next read, once a frame
        // while images keep failing.
        ++churn.invalidations;
        for (size_t c : active_channel_ids) {
            channel_storage[c].graphics_image_table.Invalidate();
            channel_storage[c].compute_image_table.Invalidate();
        }
    }
    sentenced_images.Tick();
    sentenced_image_index = (sentenced_image_index + 1) % TICKS_TO_DESTROY;
    sentenced_image_bytes[sentenced_image_index] = 0;
    sentenced_framebuffers.Tick();
    sentenced_image_view.Tick();
    TickAsyncDecode();
    TickAsyncUnswizzle();
    if (frame_tick % 600 == 0 && !uploaded_ranges.empty()) {
        for (const auto& [address, size] : uploaded_ranges) {
            device_memory.HintCold(address, size);
        }
        uploaded_ranges.clear();
    }

    runtime.TickFrame();
    ++frame_tick;

    if constexpr (IMPLEMENTS_ASYNC_DOWNLOADS) {
        for (auto& buffer : async_buffers_death_ring) {
            runtime.FreeDeferredStagingBuffer(buffer);
        }
        async_buffers_death_ring.clear();
    }
}

template <class P>
const typename P::ImageView& TextureCache<P>::GetImageView(ImageViewId id) const noexcept {
    return slot_image_views[id];
}

template <class P>
typename P::ImageView& TextureCache<P>::GetImageView(ImageViewId id) noexcept {
    return slot_image_views[id];
}

template <class P>
typename P::ImageView& TextureCache<P>::GetImageView(u32 index) noexcept {
    // Not compute!
    const auto image_view_id = VisitImageView(index, false);
    return slot_image_views[image_view_id];
}

template <class P>
void TextureCache<P>::MarkModification(ImageId id) noexcept {
    MarkModification(slot_images[id]);
}

template <class P>
void TextureCache<P>::MarkStorageWrite(ImageViewId id) noexcept {
    const ImageViewBase& view = slot_image_views[id];
    if (!view.image_id) {
        return;
    }
    ImageBase& image = slot_images[view.image_id];
    const u32 levels_before = image.storage_written_levels;
    MarkModification(image);
    NoteStorageWrite(image, levels_before, view.range);
}

template <class P>
bool TextureCache<P>::StorageViewHoldsShaderWrites(ImageViewId id) const noexcept {
    const ImageViewBase& view = slot_image_views[id];
    return !view.image_id || HoldsStorageWrites(slot_images[view.image_id], view.range);
}

template <class P>
bool TextureCache<P>::PublishComputeCollisionImage(u32 index) {
    const auto view_id = VisitImageView(index, true);
    if (!view_id || !slot_image_views[view_id].image_id) {
        return false;
    }
    const ImageId image_id = slot_image_views[view_id].image_id;
    Image& image = slot_images[image_id];
    const auto& info = image.info;
    // The title writes a CPU-readable 256x256 pitch-linear SNORM height map.
    if (info.type != ImageType::Linear || info.format != PixelFormat::R16G16B16A16_SNORM ||
        info.size.width != 256 || info.size.height != 256 || info.size.depth != 1 ||
        info.pitch != 2048 || info.resources.layers != 1 || info.resources.levels != 1 ||
        !IsDownloadable(image) ||
        !gpu_memory->IsFullyMappedRange(image.gpu_addr, image.guest_size_bytes)) {
        return false;
    }
    // Downloaded with the next fence's flushes and written back once the GPU reaches that fence,
    // as a preemptively downloaded render target is, instead of waiting for the GPU here.
    uncommitted_downloads.emplace_back(PendingDownload{true, 0, image_id});
    return true;
}

template <class P>
void TextureCache<P>::FillImageViews(std::span<ImageViewInOut> views, bool compute, bool blacklist) {
    bool has_blacklisted = false;
    do {
        has_deleted_images = false;
        if (blacklist) {
            has_blacklisted = false;
        }
        const ImageViewInOut* previous{};
        for (ImageViewInOut& view : views) {
            // An element repeating the one before (a handle table holding one shadow map in
            // many elements, say) is the same view: visiting it again only prepares it again.
            // Not once this pass has deleted image views (scaling an image down below does):
            // that view may have gone with them.
            view.id = previous != nullptr && previous->index == view.index && !has_deleted_images
                          ? previous->id
                          : VisitImageView(view.index, compute);
            if (view.id != NULL_IMAGE_VIEW_ID) {
                const ImageId probe_id = slot_image_views[view.id].image_id;
                if (probe_id != NULL_IMAGE_ID) {
                    Image& probe = slot_images[probe_id];
                    if (True(probe.flags & ImageFlagBits::EvictBorn)) {
                        probe.flags &= ~ImageFlagBits::EvictBorn;
                        if (False(probe.flags & ImageFlagBits::GpuModified)) {
                            // Sampled before the GPU drew into it: the game read back what an
                            // early deletion threw away.
                            const u64 key = RtShapeKey(probe);
                            ++evict_read_back;
                            // From now on, targets of the deleted one's shape are written back
                            // before an early deletion (EvictDeadRenderTargets).
                            auto it = recent_evictions.upper_bound(probe.cpu_addr);
                            if (it != recent_evictions.begin() &&
                                (--it)->second.end > probe.cpu_addr) {
                                const u64 deleted = it->second.shape;
                                if (readback_shapes.insert(deleted).second) {
                                    LOG_INFO(HW_GPU,
                                             "Dead render targets: format {} {}x{} was read back "
                                             "(as format {} {}x{}), now written back before "
                                             "deletion",
                                             deleted >> 40, (deleted >> 20) & 0xFFFFF,
                                             deleted & 0xFFFFF, key >> 40, (key >> 20) & 0xFFFFF,
                                             key & 0xFFFFF);
                                }
                            }
                        }
                    }
                }
            }
            previous = &view;
            if (blacklist) {
                if (view.blacklist && view.id != NULL_IMAGE_VIEW_ID) {
                    const ImageViewBase& image_view = slot_image_views[view.id];
                    if (image_view.image_id == NULL_IMAGE_ID) {
                        continue;
                    }
                    auto& image = slot_images[image_view.image_id];
                    has_blacklisted |= ScaleDown(image);
                    image.scale_rating = 0;
                }
            }
        }
    } while (has_deleted_images || (blacklist && has_blacklisted));
}

template <class P>
bool TextureCache<P>::TextureMemoStillHolds(const SampledTextureMemo& memo,
                                            std::span<const GPUVAddr> handle_addresses) const {
    const auto& image_table = channel_state->graphics_image_table;
    const auto& sampler_table = channel_state->graphics_sampler_table;
    const bool via_header_index =
        maxwell3d->regs.sampler_binding == Tegra::Engines::Maxwell3D::Regs::SamplerBinding::ViaHeaderBinding;
    if (!memo.valid || memo.memory != gpu_memory || memo.image_table != &image_table ||
        memo.image_changes != image_table.changes ||
        memo.sampler_changes != sampler_table.changes ||
        memo.via_header_index != via_header_index ||
        memo.entries.size() != handle_addresses.size() ||
        memo.host_serial != gpu_memory->HostPointerSerial()) {
        return false;
    }
    // The bytes the ids came from are what they were: the handle words, then the texture and
    // sampler descriptors they select. Nothing else decides the views and samplers.
    for (size_t i = 0; i < handle_addresses.size(); ++i) {
        const SampledTextureMemo::Entry& entry = memo.entries[i];
        if (entry.handle_address != handle_addresses[i] ||
            std::memcmp(entry.handle_host, &entry.raw, sizeof(entry.raw)) != 0 ||
            std::memcmp(entry.tic_host, &entry.tic, sizeof(entry.tic)) != 0 ||
            std::memcmp(entry.tsc_host, &entry.tsc, sizeof(entry.tsc)) != 0) {
            return false;
        }
    }
    return true;
}

template <class P>
bool TextureCache<P>::ResolveMemoizedTextures(SampledTextureMemo& memo,
                                              std::span<const GPUVAddr> handle_addresses,
                                              std::span<ImageViewInOut> views,
                                              std::span<SamplerId> samplers) {
    if (views.size() != handle_addresses.size() || samplers.size() != handle_addresses.size() ||
        !TextureMemoStillHolds(memo, handle_addresses)) {
        return false;
    }
    has_deleted_images = false;
    for (size_t i = 0; i < handle_addresses.size(); ++i) {
        const SampledTextureMemo::Entry& entry = memo.entries[i];
        views[i] = ImageViewInOut{.index = entry.view_index, .blacklist = false, .id = entry.view};
        samplers[i] = entry.sampler;
        PrepareImageView(entry.view, false, false);
    }
    if (has_deleted_images) {
        // Preparing an image deleted others: the full path resolves the views again.
        memo.valid = false;
        return false;
    }
    return true;
}

template <class P>
void TextureCache<P>::MemoizeTextures(SampledTextureMemo& memo,
                                      std::span<const TextureHandleRead> reads,
                                      std::span<const ImageViewInOut> views,
                                      std::span<const SamplerId> samplers) {
    memo.valid = false;
    memo.entries.clear();
    auto& image_table = channel_state->graphics_image_table;
    auto& sampler_table = channel_state->graphics_sampler_table;
    if (image_table.invalidated || sampler_table.invalidated || reads.size() != views.size() ||
        reads.size() != samplers.size()) {
        return;
    }
    const bool via_header_index =
        maxwell3d->regs.sampler_binding == Tegra::Engines::Maxwell3D::Regs::SamplerBinding::ViaHeaderBinding;
    const bool native_tiling = gpu_memory->UsesNativeTiling();
    const bool byte_offset_handles = Tegra::Texture::UsesHeaderByteOffsetHandles(
        native_tiling, Settings::GetCurrentProgramID());
    // The descriptors are read through one host page each (see DescriptorTable::Read).
    const auto host_of = [&](GPUVAddr address, size_t size) -> const u8* {
        if ((address & Core::Memory::LICHII_PAGEMASK) + size > Core::Memory::LICHII_PAGESIZE) {
            return nullptr;
        }
        return gpu_memory->GetPointer(address);
    };
    for (size_t i = 0; i < reads.size(); ++i) {
        const auto [view_index, sampler_index] = Tegra::Texture::TexturePair(reads[i].raw, via_header_index);
        const u32 tic_index =
            Tegra::Texture::TextureHeaderIndex(view_index, image_table.current_limit,
                                               byte_offset_handles);
        if (!views[i].id || !samplers[i] || !image_table.ContainsIndex(tic_index) ||
            !sampler_table.ContainsIndex(sampler_index)) {
            return;
        }
        const ImageId image_id = slot_image_views[views[i].id].image_id;
        if (native_tiling && image_id && slot_images[image_id].info.maxwell_gob_layout) {
            return;
        }
        SampledTextureMemo::Entry entry{
            .handle_address = reads[i].address,
            .handle_host = host_of(reads[i].address, sizeof(u32)),
            .tic_host = host_of(image_table.current_gpu_addr + tic_index * sizeof(TICEntry),
                                sizeof(TICEntry)),
            .tsc_host = host_of(sampler_table.current_gpu_addr + sampler_index * sizeof(TSCEntry),
                                sizeof(TSCEntry)),
            .tic = image_table.descriptors[tic_index],
            .tsc = sampler_table.descriptors[sampler_index],
            .raw = reads[i].raw,
            .view_index = view_index,
            .view = views[i].id,
            .sampler = samplers[i],
        };
        if (entry.handle_host == nullptr || entry.tic_host == nullptr || entry.tsc_host == nullptr ||
            std::memcmp(entry.handle_host, &entry.raw, sizeof(entry.raw)) != 0 ||
            std::memcmp(entry.tic_host, &entry.tic, sizeof(entry.tic)) != 0 ||
            std::memcmp(entry.tsc_host, &entry.tsc, sizeof(entry.tsc)) != 0) {
            memo.entries.clear();
            return;
        }
        memo.entries.push_back(entry);
    }
    memo.memory = gpu_memory;
    memo.image_table = &image_table;
    memo.host_serial = gpu_memory->HostPointerSerial();
    memo.image_changes = image_table.changes;
    memo.sampler_changes = sampler_table.changes;
    memo.via_header_index = via_header_index;
    memo.valid = true;
}

template <class P>
void TextureCache<P>::CheckFeedbackLoop(std::span<const ImageViewInOut> views) {
    if (!Settings::values.barrier_feedback_loops.GetValue()) {
        return;
    }
    const auto& regs = maxwell3d->regs;
    const bool writes_depth_stencil = regs.depth_write_enabled != 0 || regs.stencil_enable != 0;
    const auto barrier = [&] {
        if (feedback_depth_dirty) {
            runtime.BarrierFeedbackLoop();
            feedback_depth_dirty = false;
        }
    };
    SCOPE_EXIT {
        if (writes_depth_stencil) {
            feedback_depth_dirty = true;
        }
    };

    if (render_targets_serial == last_feedback_loop_serial &&
        texture_bindings_serial == last_feedback_texture_serial) {
        if (last_feedback_loop_result) {
            barrier();
        }
        return;
    }
    if (render_targets_serial != last_feedback_loop_serial) {
        feedback_depth_dirty = true;
    }

    if (rt_active_mask == 0) {
        last_feedback_loop_serial = render_targets_serial;
        last_feedback_texture_serial = texture_bindings_serial;
        last_feedback_loop_result = false;
        return;
    }
    const u32 depth_bit = 1u << NUM_RT;
    const bool depth_active = (rt_active_mask & depth_bit) != 0;

    const bool requires_barrier = [&] {
        for (const auto& view : views) {
            if (!view.id) {
                continue;
            }

            {
                bool is_continue = false;
                for (size_t i = 0; i < 8; ++i)
                    is_continue |= (rt_active_mask & (1u << i)) && view.id == render_targets.color_buffer_ids[i];
                if (is_continue)
                    continue;
            }

            if (depth_active && view.id == render_targets.depth_buffer_id)
                continue;

            const ImageId view_image_id = slot_image_views[view.id].image_id;
            {
                bool is_continue = false;
                for (size_t i = 0; i < 8; ++i)
                    is_continue |= (rt_active_mask & (1u << i)) && view_image_id == rt_image_id[i];
                if (is_continue)
                    continue;
            }
            if (depth_active && view_image_id == rt_depth_image_id) {
                return true;
            }
        }

        return false;
    }();

    last_feedback_loop_serial = render_targets_serial;
    last_feedback_texture_serial = texture_bindings_serial;
    last_feedback_loop_result = requires_barrier;
    if (requires_barrier) {
        barrier();
    }
}

template <class P>
bool TextureCache<P>::SamplesRenderTarget(std::span<const ImageViewInOut> views) {
    if (rt_active_mask == 0) {
        return false;
    }
    const bool depth_active = (rt_active_mask & (1u << NUM_RT)) != 0;
    for (const auto& view : views) {
        if (!view.id) {
            continue;
        }
        const ImageId image_id = slot_image_views[view.id].image_id;
        if (depth_active && image_id == rt_depth_image_id) {
            return true;
        }
        for (size_t i = 0; i < NUM_RT; ++i) {
            if ((rt_active_mask & (1u << i)) != 0 && image_id == rt_image_id[i]) {
                return true;
            }
        }
    }
    return false;
}

template <class P>
typename P::Sampler* TextureCache<P>::GetSampler(u32 index, bool compute) {
    return &GetSampler(GetSamplerId(index, compute));
}

template <class P>
SamplerId TextureCache<P>::GetSamplerId(u32 index, bool compute) {
    auto& table = compute ? channel_state->compute_sampler_table : channel_state->graphics_sampler_table;
    if (!table.ContainsIndex(index)) {
        LOG_TRACE(HW_GPU, "Invalid sampler index={}", index);
        return NULL_SAMPLER_ID;
    }
    auto const [descriptor, is_new] = table.Read(*gpu_memory, index);
    if (is_new) {
        auto const id = FindSampler(descriptor, compute);
        table.bound_ids[index] = id;
        return id;
    }
    return table.bound_ids[index];
}

template <class P>
const typename P::Sampler& TextureCache<P>::GetSampler(SamplerId id) const noexcept {
    if (!slot_samplers.contains(id)) {
        static std::atomic<u32> logged{};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 20) {
            LOG_ERROR(HW_GPU, "Invalid sampler id {:#x}, using the null sampler", id.index);
        }
        return slot_samplers[NULL_SAMPLER_ID];
    }
    return slot_samplers[id];
}

template <class P>
typename P::Sampler& TextureCache<P>::GetSampler(SamplerId id) noexcept {
    return const_cast<Sampler&>(std::as_const(*this).GetSampler(id));
}

template <class P>
void TextureCache<P>::SynchronizeDescriptors(bool compute) {
    if (compute) {
        const bool linked_tsc = kepler_compute->launch_description.linked_tsc;
        const u32 tic_limit = kepler_compute->regs.tic.limit;
        const u32 tsc_limit = linked_tsc ? tic_limit : kepler_compute->regs.tsc.limit;
        bool bindings_changed = false;
        if (channel_state->compute_sampler_table.Synchronize(kepler_compute->regs.tsc.Address(), tsc_limit))
            bindings_changed = true;
        if (channel_state->compute_image_table.Synchronize(kepler_compute->regs.tic.Address(), tic_limit))
            bindings_changed = true;
        if (bindings_changed) {
            ++texture_bindings_serial;
            ++churn.refreshes;
        }
    } else {
        const bool linked_tsc = maxwell3d->regs.sampler_binding == Tegra::Engines::Maxwell3D::Regs::SamplerBinding::ViaHeaderBinding;
        const u32 tic_limit = maxwell3d->regs.tex_header.limit;
        const u32 tsc_limit = linked_tsc ? tic_limit : maxwell3d->regs.tex_sampler.limit;
        bool bindings_changed = false;
        if (channel_state->graphics_sampler_table.Synchronize(maxwell3d->regs.tex_sampler.Address(), tsc_limit))
            bindings_changed = true;
        if (channel_state->graphics_image_table.Synchronize(maxwell3d->regs.tex_header.Address(), tic_limit))
            bindings_changed = true;
        if (bindings_changed) {
            ++texture_bindings_serial;
            ++churn.refreshes;
        }
    }
}

template <class P>
bool TextureCache<P>::RescaleRenderTargets() {
    auto& flags = maxwell3d->dirty.flags;
    u32 scale_rating = 0;
    bool rescaled = false;
    std::array<ImageId, NUM_RT> tmp_color_images{};
    ImageId tmp_depth_image{};
    do {
        flags[Dirty::RenderTargets] = false;

        has_deleted_images = false;
        // Render target control is used on all render targets, so force look ups when this one is
        // up
        const bool force = flags[Dirty::RenderTargetControl];
        flags[Dirty::RenderTargetControl] = false;

        scale_rating = 0;
        bool any_rescaled = false;
        bool can_rescale = true;
        const auto check_rescale = [&](ImageViewId view_id, ImageId& id_save) {
            if (view_id != NULL_IMAGE_VIEW_ID && view_id != ImageViewId{}) {
                const auto& view = slot_image_views[view_id];
                const auto image_id = view.image_id;
                id_save = image_id;
                auto& image = slot_images[image_id];
                can_rescale &= ImageCanRescale(image);
                any_rescaled |= True(image.flags & ImageFlagBits::Rescaled) ||
                                GetFormatType(image.info.format) != SurfaceType::ColorTexture;
                scale_rating = std::max<u32>(scale_rating, image.scale_tick <= frame_tick
                                                               ? image.scale_rating + 1U
                                                               : image.scale_rating);
            } else {
                id_save = CORRUPT_ID;
            }
        };
        for (size_t index = 0; index < NUM_RT; ++index) {
            ImageViewId& color_buffer_id = render_targets.color_buffer_ids[index];
            if (flags[Dirty::ColorBuffer0 + index] || force) {
                flags[Dirty::ColorBuffer0 + index] = false;
                BindRenderTarget(&color_buffer_id, FindColorBuffer(index));
            }
            check_rescale(color_buffer_id, tmp_color_images[index]);
        }
        // The surface clip, which a padded depth target's size depends on, has no flag of its own.
        if (flags[Dirty::ZetaBuffer] || force || depth_target_padded) {
            flags[Dirty::ZetaBuffer] = false;
            BindRenderTarget(&render_targets.depth_buffer_id, FindDepthBuffer());
        }
        check_rescale(render_targets.depth_buffer_id, tmp_depth_image);

        if (can_rescale) {
            rescaled = any_rescaled || scale_rating >= 2;
            const auto scale_up = [this](ImageId image_id) {
                if (image_id != CORRUPT_ID) {
                    Image& image = slot_images[image_id];
                    ScaleUp(image);
                }
            };
            if (rescaled) {
                for (size_t index = 0; index < NUM_RT; ++index) {
                    scale_up(tmp_color_images[index]);
                }
                scale_up(tmp_depth_image);
                scale_rating = 2;
            }
        } else {
            rescaled = false;
            const auto scale_down = [this](ImageId image_id) {
                if (image_id != CORRUPT_ID) {
                    Image& image = slot_images[image_id];
                    ScaleDown(image);
                }
            };
            for (size_t index = 0; index < NUM_RT; ++index) {
                scale_down(tmp_color_images[index]);
            }
            scale_down(tmp_depth_image);
            scale_rating = 1;
        }
    } while (has_deleted_images);
    const auto set_rating = [this, scale_rating](ImageId image_id) {
        if (image_id != CORRUPT_ID) {
            Image& image = slot_images[image_id];
            image.scale_rating = scale_rating;
            if (image.scale_tick <= frame_tick) {
                image.scale_tick = frame_tick + 1;
            }
        }
    };
    for (size_t index = 0; index < NUM_RT; ++index) {
        set_rating(tmp_color_images[index]);
    }
    set_rating(tmp_depth_image);

    return rescaled;
}

template <class P>
void TextureCache<P>::UpdateRenderTargets(bool is_clear) {
    using namespace VideoCommon::Dirty;
    struct ClearingScope {
        bool& clearing;
        bool& drawing;
        ~ClearingScope() {
            clearing = false;
            drawing = false;
        }
    } clearing_scope{clearing_render_targets, drawing_render_targets};
    clearing_render_targets = is_clear;
    drawing_render_targets = !is_clear;
    auto& flags = maxwell3d->dirty.flags;
    if (!flags[Dirty::RenderTargets]) {
        for (size_t index = 0; index < NUM_RT; ++index) {
            ImageViewId& color_buffer_id = render_targets.color_buffer_ids[index];
            PrepareImageView(color_buffer_id, true, is_clear && IsFullClear(color_buffer_id));
        }
        const ImageViewId depth_buffer_id = render_targets.depth_buffer_id;
        PrepareImageView(depth_buffer_id, true, is_clear && IsFullClear(depth_buffer_id));
        if (is_clear) {
            NoteClearedWhole();
        }
        return;
    }

    const VideoCommon::RenderTargets previous_render_targets = render_targets;
    const bool rescaled = RescaleRenderTargets();
    if (is_rescaling != rescaled) {
        flags[Dirty::RescaleViewports] = true;
        flags[Dirty::RescaleScissors] = true;
        is_rescaling = rescaled;
    }

    for (size_t index = 0; index < NUM_RT; ++index) {
        ImageViewId& color_buffer_id = render_targets.color_buffer_ids[index];
        PrepareImageView(color_buffer_id, true, is_clear && IsFullClear(color_buffer_id));
    }
    const ImageViewId depth_buffer_id = render_targets.depth_buffer_id;

    PrepareImageView(depth_buffer_id, true, is_clear && IsFullClear(depth_buffer_id));
    if (is_clear) {
        NoteClearedWhole();
    }

    rt_active_mask = 0;
    rt_image_id = {};
    for (size_t i = 0; i < rt_image_id.size(); ++i) {
        if (ImageViewId const view = render_targets.color_buffer_ids[i]; view) {
            rt_active_mask |= 1u << i;
            rt_image_id[i] = slot_image_views[view].image_id;
        }
    }
    if (depth_buffer_id) {
        rt_active_mask |= (1u << NUM_RT);
        rt_depth_image_id = slot_image_views[depth_buffer_id].image_id;
    } else {
        rt_depth_image_id = ImageId{};
    }

    for (size_t index = 0; index < NUM_RT; ++index) {
        render_targets.draw_buffers[index] = static_cast<u8>(maxwell3d->regs.rt_control.Map(index));
    }
    u32 up_scale = 1;
    u32 down_shift = 0;
    if (is_rescaling) {
        up_scale = Settings::values.resolution_info.up_scale;
        down_shift = Settings::values.resolution_info.down_shift;
    }
    render_targets.size = Extent2D{
        (maxwell3d->regs.surface_clip.width * up_scale) >> down_shift,
        (maxwell3d->regs.surface_clip.height * up_scale) >> down_shift,
    };
    render_targets.is_rescaled = is_rescaling;

    if (render_targets != previous_render_targets) {
        ++render_targets_serial;
    }

    flags[Dirty::DepthBiasGlobal] = true;
}

template <class P>
typename P::Framebuffer* TextureCache<P>::GetFramebuffer() {
    if (last_framebuffer_id && last_framebuffer_serial == render_targets_serial) {
        return &slot_framebuffers[last_framebuffer_id];
    }
    const FramebufferId framebuffer_id = GetFramebufferId(render_targets);
    last_framebuffer_id = framebuffer_id;
    last_framebuffer_serial = render_targets_serial;
    return &slot_framebuffers[framebuffer_id];
}

template <class P>
bool TextureCache<P>::ColorTargetWrittenBeforeThisFrame(u32 index) noexcept {
    if (!maxwell3d || index >= rt_image_id.size()) {
        return false;
    }
    const auto& regs = maxwell3d->regs;
    const auto& rt = regs.rt[index];
    const GPUVAddr gpu_addr = rt.Address();
    if (gpu_addr == 0 || rt.format == Tegra::RenderTargetFormat::NONE) {
        return false;
    }
    std::scoped_lock lock{mutex};
    ImageId image_id{};
    if (!maxwell3d->dirty.flags[Dirty::RenderTargets] &&
        !maxwell3d->dirty.flags[Dirty::ColorBuffer0 + index]) {
        image_id = rt_image_id[index];
    } else {
        const ImageInfo info(rt, regs.anti_alias_samples_mode);
        if (info.format == PixelFormat::Invalid || info.size.width == 0 || info.size.height == 0 ||
            info.size.depth == 0 || info.size.width > 65536 || info.size.height > 65536 ||
            info.size.depth > 2048 || info.resources.layers <= 0 ||
            info.resources.layers > 2048) {
            return false;
        }
        image_id = FindImage(info, gpu_addr, RelaxedOptions{});
    }
    if (!image_id) {
        return false;
    }
    const ImageBase& image = slot_images[image_id];
    return image.first_written_frame != 0 && image.first_written_frame < frame_tick + 1;
}

template <class P>
ImageViewId TextureCache<P>::VisitImageView(u32 index, bool compute) {
    auto& table = compute ? channel_state->compute_image_table : channel_state->graphics_image_table;
    index = Tegra::Texture::TextureHeaderIndex(
        index, table.current_limit,
        Tegra::Texture::UsesHeaderByteOffsetHandles(gpu_memory->UsesNativeTiling(),
                                                   Settings::GetCurrentProgramID()));
    if (!table.ContainsIndex(index)) {
        LOG_TRACE(HW_GPU, "Invalid image view index={}", index);
        return NULL_IMAGE_VIEW_ID;
    }
    const auto bytes_ready = [&](ImageViewId id) {
        if (id == NULL_IMAGE_VIEW_ID) {
            return true;
        }
        const ImageId image_id = slot_image_views[id].image_id;
        if (!image_id || !gpu_memory->UsesNativeTiling()) {
            return true;
        }
        const ImageBase& image = slot_images[image_id];
        return !image.info.maxwell_gob_layout ||
               (image.overlapping_images.empty() && async_overlap_downloads.empty()) ||
               GobViewBytesReady(image.cpu_addr, image.guest_size_bytes);
    };
    auto const [descriptor, is_new] = table.Read(*gpu_memory, index);
    if (is_new) {
        if (IsValidEntry(*gpu_memory, descriptor)) {
            const auto [pair, is_new_tc] = channel_state->image_views.try_emplace(descriptor);
            if (is_new_tc) {
                const auto failed_images = [&]() -> u32 {
                    if constexpr (requires { runtime.FailedImages(); }) {
                        return runtime.FailedImages();
                    } else {
                        return 0;
                    }
                };
                const u32 failed_before = failed_images();
                bool retry = false;
                pair->second = CreateImageView(descriptor, retry);
                if (retry) {
                    channel_state->image_views.erase(pair);
                    table.bound_ids[index] = NULL_IMAGE_VIEW_ID;
                    table.Retry(index);
                    return NULL_IMAGE_VIEW_ID;
                }
                if (pair->second == NULL_IMAGE_VIEW_ID && failed_images() != failed_before) {
                    // No memory for its image: TickFrame invalidates the table after collecting,
                    // and the descriptor's next read makes the image again.
                    channel_state->image_views.erase(pair);
                    table.bound_ids[index] = NULL_IMAGE_VIEW_ID;
                    return NULL_IMAGE_VIEW_ID;
                }
                if (pair->second != NULL_IMAGE_VIEW_ID) {
                    image_view_descriptors[pair->second].emplace_back(current_channel_id,
                                                                      descriptor);
                }
            }
            if (!is_new_tc && !bytes_ready(pair->second)) {
                table.bound_ids[index] = pair->second;
                return NULL_IMAGE_VIEW_ID;
            }
            PrepareImageView(pair->second, false, false);
            table.bound_ids[index] = pair->second;
            return pair->second;
        }
        table.bound_ids[index] = NULL_IMAGE_VIEW_ID;
        return NULL_IMAGE_VIEW_ID;
    }
    const auto id = table.bound_ids[index];
    if (!bytes_ready(id)) {
        return NULL_IMAGE_VIEW_ID;
    }
    if (id != NULL_IMAGE_VIEW_ID)
        PrepareImageView(id, false, false);
    return id;
}

template <class P>
FramebufferId TextureCache<P>::GetFramebufferId(const RenderTargets& key) {
    const auto [pair, is_new] = framebuffers.try_emplace(key);
    FramebufferId& framebuffer_id = pair->second;
    if (!is_new) {
        return framebuffer_id;
    }
    std::array<ImageView*, NUM_RT> color_buffers;
    std::ranges::transform(key.color_buffer_ids, color_buffers.begin(), [this](ImageViewId id) {
        return id ? &slot_image_views[id] : nullptr;
    });
    ImageView* const depth_buffer = key.depth_buffer_id ? &slot_image_views[key.depth_buffer_id] : nullptr;
    runtime.FlushDeferredClear();
    framebuffer_id = slot_framebuffers.insert(runtime, color_buffers, depth_buffer, key);
    return framebuffer_id;
}

template <class P>
void TextureCache<P>::WriteMemory(DAddr cpu_addr, size_t size) {
    if (!async_overlap_downloads.empty()) {
        KeepGuestWrite(cpu_addr, size);
    }
    ClearStale(cpu_addr, size);
    ForEachImageInRegion(cpu_addr, size, [this, cpu_addr, size](ImageId image_id, Image& image) {
        MarkImageWritten(image_id, image, cpu_addr, size);
    });
}

template <class P>
void TextureCache<P>::MarkImageWritten(ImageId image_id, Image& image, DAddr cpu_addr,
                                       size_t size) {
    VideoCommon::Nx2FastDraw::Break(VideoCommon::Nx2FastDraw::Reason::Invalidation);
    if (image.info.native_tiling && True(image.flags & ImageFlagBits::GpuModified) &&
        HasGuestRange(image) && False(image.flags & ImageFlagBits::NonContiguous) &&
        cpu_addr <= image.cpu_addr && image.cpu_addr_end - cpu_addr <= size) {
        image.flags |= ImageFlagBits::GuestReplacedContents;
    }
    if (True(image.flags & ImageFlagBits::UnwrittenSparse)) {
        // Written at last: a backed image replaces it where it is used next.
        image.flags |= ImageFlagBits::CpuModified | ImageFlagBits::Remapped;
        return;
    }
    if (True(image.flags & ImageFlagBits::SparseTiles)) {
        MarkRangeWritten(image, cpu_addr, size);
        return;
    }
    if (UploadsByLayer(image)) {
        MarkLayersWritten(image, cpu_addr, size);
        return;
    }
    if (True(image.flags & ImageFlagBits::CpuModified)) {
        return;
    }
    image.flags |= ImageFlagBits::CpuModified;
    if (True(image.flags & ImageFlagBits::Tracked)) {
        UntrackImage(image, image_id);
    }
}

template <class P>
void TextureCache<P>::WriteDmaCopy(GPUVAddr src_addr, GPUVAddr dest_addr, size_t size,
                                   std::span<const std::pair<DAddr, size_t>> gpu_written_dest) {
    const std::optional<DAddr> src = gpu_memory->GpuToCpuAddress(src_addr);
    const std::optional<DAddr> dest = gpu_memory->GpuToCpuAddress(dest_addr);
    if (!src || !dest) {
        return;
    }
    if (!async_overlap_downloads.empty()) {
        KeepGuestWrite(*dest, size);
    }
    // Destination bytes guest memory doesn't hold yet: the buffer cache's GPU writes and the
    // bytes of images the GPU wrote over the source.
    boost::container::small_vector<std::pair<DAddr, size_t>, 8> unheld(gpu_written_dest.begin(),
                                                                       gpu_written_dest.end());
    ForEachImageInRegion(*src, size, [&](ImageId, Image& image) {
        if (False(image.flags & ImageFlagBits::GpuModified)) {
            return;
        }
        const DAddr begin = (std::max)(static_cast<DAddr>(image.cpu_addr), *src);
        const DAddr end = (std::min)(static_cast<DAddr>(image.cpu_addr_end), *src + size);
        unheld.emplace_back(*dest + (begin - *src), end - begin);
    });
    std::ranges::sort(unheld);
    DAddr held = *dest;
    for (const auto& [begin, length] : unheld) {
        if (begin > held) {
            ClearStale(held, begin - held);
        }
        held = (std::max)(held, begin + length);
    }
    const DAddr dest_end = *dest + size;
    if (held < dest_end) {
        ClearStale(held, dest_end - held);
    }
    ForEachImageInRegion(*dest, size, [&](ImageId image_id, Image& image) {
        // An upload would take bytes guest memory doesn't hold.
        const bool takes_unheld = std::ranges::any_of(unheld, [&image](const auto& range) {
            return range.first < image.cpu_addr_end && image.cpu_addr < range.first + range.second;
        });
        // GPU-written contents outside the copy are only in the image.
        const bool keeps_gpu_contents = True(image.flags & ImageFlagBits::GpuModified) &&
                                        (image.cpu_addr < *dest || image.cpu_addr_end > dest_end);
        if (!takes_unheld && !keeps_gpu_contents) {
            MarkImageWritten(image_id, image, *dest, size);
        }
    });
}

template <class P>
bool TextureCache<P>::IsDownloadable(const ImageBase& image) const noexcept {
    if (!image.IsSafeGpuCopy() || True(image.flags & (ImageFlagBits::Unbacked | ImageFlagBits::TrimmedLevels))) {
        return false;
    }
    if (image.info.num_samples == 1) {
        return true;
    }
    if constexpr (P::HAS_MSAA_DOWNLOADS) {
        return runtime.CanDownloadMsaa(image.info);
    } else {
        return false;
    }
}

template <class P>
void TextureCache<P>::DownloadMemory(DAddr cpu_addr, size_t size) {
    VideoCommon::Nx2Profile::Scope profile_scope{VideoCommon::Nx2Profile::Zone::TextureCache};
    // Write-backs landed for this read: an older image downloaded below leaves their ranges as
    // they are, newest data winning as between write-backs. A range a write-back left to newer
    // data (or to the guest's writes) holds that data, at its own tick.
    using WritebackRanges::TickedRange;
    boost::container::small_vector<TickedRange, 8> landed;
    if (WritebackPendingOver(cpu_addr, size)) {
        for (const AsyncOverlapDownload& download : async_overlap_downloads) {
            if (!download.cancelled) {
                WritebackRanges::AppendLanded(landed, download.cpu_addr, download.size,
                                              download.tick, download.keep, download.keep_ticks);
            }
        }
        // A write-back still on the GPU holds this memory's data: land it before the guest reads
        // (waiting as the downloads below do, on the flushing thread with the cache's lock).
        FinishAsyncOverlapDownloads(true);
    }
    boost::container::small_vector<ImageId, 16> images;
    ForEachImageInRegion(cpu_addr, size, [this, &images](ImageId image_id, ImageBase& image) {
        if (!IsDownloadable(image)) {
            return;
        }
        image.flags &= ~ImageFlagBits::GpuModified;
        images.push_back(image_id);
    });
    if (images.empty()) {
        return;
    }
    const StutterTrace::SiteScope site{StutterTrace::Site::TextureDownload};
    std::ranges::sort(images, [this](ImageId lhs, ImageId rhs) {
        return slot_images[lhs].modification_tick < slot_images[rhs].modification_tick;
    });
    // Record the copies of a batch, wait for the GPU once, then swizzle in modification order
    // (the copies only read, so they do not depend on each other). A batch is capped so the
    // staging memory held at once stays bounded.
    constexpr size_t MaxBatchBytes = 64_MiB;
    using Map = decltype(runtime.DownloadStagingBuffer(size_t{}));
    using Copies = decltype(FullDownloadCopies(std::declval<const ImageInfo&>()));
    struct Pending {
        ImageId image_id;
        Map map;
        Copies copies;
    };
    boost::container::small_vector<Pending, 16> pending;
    size_t pending_bytes = 0;
    const auto flush_pending = [&] {
        if (pending.empty()) {
            return;
        }
        runtime.Finish();
        for (Pending& entry : pending) {
            Image& image = slot_images[entry.image_id];
            KeepRanges keep;
            KeepNewerLanded(image, image.modification_tick, keep);
            if (!landed.empty()) {
                ForEachWrittenRange(image, [&](DAddr begin, DAddr end) {
                    for (const TickedRange& range : landed) {
                        if (range.tick > image.modification_tick && range.begin < end &&
                            begin < range.end) {
                            keep.emplace_back((std::max)(range.begin, begin),
                                              (std::min)(range.end, end));
                        }
                    }
                });
            }
            if (WriteBackImage(image, entry.copies, entry.map.mapped_span, keep)) {
                NoteFlushed(image);
            }
        }
        pending.clear();
        pending_bytes = 0;
    };
    for (const ImageId image_id : images) {
        Image& image = slot_images[image_id];
        if (!pending.empty() && pending_bytes + image.unswizzled_size_bytes > MaxBatchBytes) {
            flush_pending();
        }
        auto map = runtime.DownloadStagingBuffer(image.unswizzled_size_bytes);
        auto copies = FullDownloadCopies(image.info);
        image.DownloadMemory(map, copies);
        pending_bytes += image.unswizzled_size_bytes;
        pending.push_back(Pending{image_id, map, std::move(copies)});
    }
    flush_pending();
}

template <class P>
std::optional<VideoCore::RasterizerDownloadArea> TextureCache<P>::GetFlushArea(DAddr cpu_addr,
                                                                               u64 size) {
    std::optional<VideoCore::RasterizerDownloadArea> area{};
    ForEachImageInRegion(cpu_addr, size, [&](ImageId, ImageBase& image) {
        if (False(image.flags & ImageFlagBits::GpuModified)) {
            return;
        }
        if (!area) {
            area.emplace();
            area->start_address = cpu_addr;
            area->end_address = cpu_addr + size;
            area->preemtive = true;
        }
        area->start_address = (std::min)(area->start_address, image.cpu_addr);
        area->end_address = (std::max)(area->end_address, image.cpu_addr_end);
        for (auto image_view_id : image.image_view_ids) {
            auto& image_view = slot_image_views[image_view_id];
            image_view.flags |= ImageViewFlagBits::PreemtiveDownload;
        }
        area->preemtive &= image.info.forced_flushed;
        image.info.forced_flushed = true;
    });
    // Write-backs still on the GPU: DownloadMemory lands them.
    for (const AsyncOverlapDownload& download : async_overlap_downloads) {
        if (download.cancelled || download.cpu_addr + download.size <= cpu_addr ||
            cpu_addr + size <= download.cpu_addr) {
            continue;
        }
        if (!area) {
            area.emplace();
            area->start_address = cpu_addr;
            area->end_address = cpu_addr + size;
            area->preemtive = true;
        }
        area->start_address = (std::min)(area->start_address, download.cpu_addr);
        area->end_address = (std::max)(area->end_address, download.cpu_addr + download.size);
    }
    return area;
}

template <class P>
void TextureCache<P>::UnmapMemory(DAddr cpu_addr, size_t size) {
    if (!async_overlap_downloads.empty()) {
        CancelAsyncOverlapDownloads(cpu_addr, size);
    }
    boost::container::small_vector<ImageId, 16> deleted_images;
    ForEachImageInRegion(cpu_addr, size, [&](ImageId id, Image&) { deleted_images.push_back(id); });
    for (const ImageId id : deleted_images) {
        Image& image = slot_images[id];
        if (True(image.flags & ImageFlagBits::Tracked)) {
            UntrackImage(image, id);
        }
        UnregisterImage(id);
        trace_delete_reason = "unmap";
        DeleteImage(id);
    }
    ClearStale(cpu_addr, size);
}

template <class P>
void TextureCache<P>::UnmapGPUMemory(size_t as_id, GPUVAddr gpu_addr, size_t size) {
    if (!async_overlap_downloads.empty()) {
        CancelAsyncOverlapDownloadsGpu(as_id, gpu_addr, size);
    }
    boost::container::small_vector<ImageId, 16> deleted_images;
    ForEachImageInRegionGPU(as_id, gpu_addr, size,
                            [&](ImageId id, Image&) { deleted_images.push_back(id); });
    if (!deleted_images.empty()) {
        VideoCommon::Nx2FastDraw::Break(VideoCommon::Nx2FastDraw::Reason::Map);
    }
    for (const ImageId id : deleted_images) {
        Image& image = slot_images[id];
        if (False(image.flags & ImageFlagBits::CpuModified)) {
            image.flags |= ImageFlagBits::CpuModified;
            if (True(image.flags & ImageFlagBits::Tracked)) {
                UntrackImage(image, id);
            }
        }
        image.dirty_layers.clear(); // all of it
        if (True(image.flags & ImageFlagBits::Remapped)) {
            continue;
        }
        image.flags |= ImageFlagBits::Remapped;
    }
}

// A 2D-engine copy between two 3D surfaces names one depth slice by its address inside the
// volume (The Duskbloods network test copies its 16x16x16 grading table slice by slice). Copy
// that slice of the cached 3D images when both exist with the surface's size and format;
// otherwise the generic path treats each slice as its own 2D image.
template <class P>
bool TextureCache<P>::CopyVolumeSlice(const Tegra::Engines::Fermi2D::Surface& dst,
                                      const Tegra::Engines::Fermi2D::Surface& src,
                                      const Tegra::Engines::Fermi2D::Config& copy) {
    using Tegra::Engines::Fermi2D;
    const auto is_volume = [](const Fermi2D::Surface& surface) {
        return surface.depth > 1 && surface.linear == Fermi2D::MemoryLayout::BlockLinear &&
               surface.layer == 0;
    };
    const bool whole_slice = copy.src_x0 == 0 && copy.src_y0 == 0 && copy.dst_x0 == 0 &&
                             copy.dst_y0 == 0 && copy.src_x1 == static_cast<s32>(src.width) &&
                             copy.src_y1 == static_cast<s32>(src.height) &&
                             copy.dst_x1 == static_cast<s32>(dst.width) &&
                             copy.dst_y1 == static_cast<s32>(dst.height);
    if (!is_volume(src) || !is_volume(dst) || !whole_slice || src.format != dst.format ||
        src.width != dst.width || src.height != dst.height) {
        return false;
    }
    const auto format = VideoCore::Surface::PixelFormatFromRenderTargetFormat(src.format);
    struct Slice {
        ImageId id;
        s32 depth;
    };
    const auto find_slice = [&](const Fermi2D::Surface& surface) -> std::optional<Slice> {
        std::optional<Slice> found;
        const GPUVAddr address = surface.Address();
        ForEachImageInRegionGPU(channel_state->gpu_memory.GetID(), address, 1,
                                [&](ImageId id, Image& image) {
            if (image.info.type != ImageType::e3D || image.info.format != format ||
                image.info.size.width != surface.width ||
                image.info.size.height != surface.height ||
                image.info.size.depth != surface.depth || address < image.gpu_addr) {
                return false;
            }
            const auto offset = static_cast<u32>(address - image.gpu_addr);
            const auto it = std::ranges::find(image.slice_offsets, offset);
            if (it == image.slice_offsets.end()) {
                return false;
            }
            const SubresourceBase base =
                image.slice_subresources[std::distance(image.slice_offsets.begin(), it)];
            if (base.level != 0) {
                return false;
            }
            found = Slice{id, base.layer};
            return true;
        });
        return found;
    };
    const auto src_slice = find_slice(src);
    const auto dst_slice = find_slice(dst);
    if (!src_slice || !dst_slice || src_slice->id == dst_slice->id) {
        static u32 reported{};
        if (reported++ < 16) {
            LOG_WARNING(HW_GPU, "2D copy between 3D surfaces {:#x} -> {:#x} without cached volumes",
                        src.Address(), dst.Address());
        }
        return false;
    }
    runtime.FlushDeferredClear();
    PrepareImage(src_slice->id, false, false);
    PrepareImage(dst_slice->id, true, false);
    const ImageCopy slice_copy{
        .src_subresource = {.base_level = 0, .base_layer = 0, .num_layers = 1},
        .dst_subresource = {.base_level = 0, .base_layer = 0, .num_layers = 1},
        .src_offset = {0, 0, src_slice->depth},
        .dst_offset = {0, 0, dst_slice->depth},
        .extent = {src.width, src.height, 1},
    };
    runtime.CopyImage(slot_images[dst_slice->id], slot_images[src_slice->id],
                      std::span{&slice_copy, 1});
    return true;
}

template <class P>
bool TextureCache<P>::BlitImage(const Tegra::Engines::Fermi2D::Surface& dst,
                                const Tegra::Engines::Fermi2D::Surface& src,
                                const Tegra::Engines::Fermi2D::Config& copy) {
    VideoCommon::Nx2Profile::Scope profile_scope{VideoCommon::Nx2Profile::Zone::TextureCache};
    // A deferred clear parked before this copy has to land before it: realizing it afterwards
    // starts its clear pass after the copy and wipes what the copy wrote. That is what made
    // Ocarina of Time draw its terrain layer over the scene (#101): the game clears the
    // occlusion depth, blits the scene depth over it, and reads it back during the layer draw.
    // GetFramebufferId only flushes when it creates a framebuffer, so the bug starts on the
    // second frame. Texture cache copies bypass render passes and keep stream order here.
    // Only when the copy proceeds: a rejected blit writes nothing and must not realize the
    // clear ahead of the next draw that consumes it as its load-op. GetBlitImages cannot
    // invalidate the parked clear: framebuffer removal flushes first (RemoveFramebuffers).
    if (CopyVolumeSlice(dst, src, copy)) {
        return true;
    }
    const auto result = GetBlitImages(dst, src, copy);
    if (!result) {
        return false;
    }
    runtime.FlushDeferredClear();
    const BlitImages images = *result;
    const ImageId dst_id = images.dst_id;
    const ImageId src_id = images.src_id;

    PrepareImage(src_id, false, false);
    PrepareImage(dst_id, true, false);

    Image& dst_image = slot_images[dst_id];
    Image& src_image = slot_images[src_id];
    if (True(dst_image.flags & ImageFlagBits::Unbacked)) {
        static u32 logged{};
        if (logged++ < 64) {
            LOG_INFO(HW_GPU, "2D blit into unbacked image {:#x}: dst {:#x} {}x{} from {:#x} {}x{}",
                     dst_image.gpu_addr, dst.Address(), dst.width, dst.height, src.Address(),
                     src.width, src.height);
        }
    }
    bool is_src_rescaled = True(src_image.flags & ImageFlagBits::Rescaled);
    bool is_dst_rescaled = True(dst_image.flags & ImageFlagBits::Rescaled);

    const bool is_resolve = src_image.info.num_samples != 1 && dst_image.info.num_samples == 1;
    if (is_src_rescaled != is_dst_rescaled) {
        if (ImageCanRescale(src_image)) {
            ScaleUp(src_image);
            is_src_rescaled = True(src_image.flags & ImageFlagBits::Rescaled);
            if (is_resolve) {
                dst_image.info.rescaleable = true;
                for (const auto& alias : dst_image.aliased_images) {
                    Image& other_image = slot_images[alias.id];
                    other_image.info.rescaleable = true;
                }
            }
        }
        if (ImageCanRescale(dst_image)) {
            ScaleUp(dst_image);
            is_dst_rescaled = True(dst_image.flags & ImageFlagBits::Rescaled);
        }
    }
    if (is_resolve && (is_src_rescaled != is_dst_rescaled)) {
        // A resolve requires both images to be the same dimensions. Resize down if needed.
        ScaleDown(src_image);
        ScaleDown(dst_image);
        is_src_rescaled = True(src_image.flags & ImageFlagBits::Rescaled);
        is_dst_rescaled = True(dst_image.flags & ImageFlagBits::Rescaled);
    }
    const auto& resolution = Settings::values.resolution_info;
    const auto scale_region = [&](Region2D& region) {
        region.start.x = resolution.ScaleUp(region.start.x);
        region.start.y = resolution.ScaleUp(region.start.y);
        region.end.x = resolution.ScaleUp(region.end.x);
        region.end.y = resolution.ScaleUp(region.end.y);
    };

    // TODO: Deduplicate
    const std::optional src_base = src_image.TryFindBase(src.Address());
    const SubresourceRange src_range{.base = src_base.value(), .extent = {1, 1}};
    const ImageViewInfo src_view_info(ImageViewType::e2D, images.src_format, src_range);
    const auto [src_framebuffer_id, src_view_id] = RenderTargetFromImage(src_id, src_view_info);
    const auto [src_samples_x, src_samples_y] = SamplesLog2(src_image.info.num_samples);
    Region2D src_region{
        Offset2D{.x = copy.src_x0 >> src_samples_x, .y = copy.src_y0 >> src_samples_y},
        Offset2D{.x = copy.src_x1 >> src_samples_x, .y = copy.src_y1 >> src_samples_y},
    };
    if (is_src_rescaled) {
        scale_region(src_region);
    }

    const std::optional dst_base = dst_image.TryFindBase(dst.Address());
    const SubresourceRange dst_range{.base = dst_base.value(), .extent = {1, 1}};
    const ImageViewInfo dst_view_info(ImageViewType::e2D, images.dst_format, dst_range);
    const auto [dst_framebuffer_id, dst_view_id] = RenderTargetFromImage(dst_id, dst_view_info);
    const auto [dst_samples_x, dst_samples_y] = SamplesLog2(dst_image.info.num_samples);
    Region2D dst_region{
        Offset2D{.x = copy.dst_x0 >> dst_samples_x, .y = copy.dst_y0 >> dst_samples_y},
        Offset2D{.x = copy.dst_x1 >> dst_samples_x, .y = copy.dst_y1 >> dst_samples_y},
    };
    if (is_dst_rescaled) {
        scale_region(dst_region);
    }

    // Always call this after src_framebuffer_id was queried, as the address might be invalidated.
    Framebuffer* const dst_framebuffer = &slot_framebuffers[dst_framebuffer_id];
    if constexpr (FRAMEBUFFER_BLITS) {
        // OpenGL blits from framebuffers, not images
        Framebuffer* const src_framebuffer = &slot_framebuffers[src_framebuffer_id];
        runtime.BlitFramebuffer(dst_framebuffer, src_framebuffer, dst_region, src_region,
                                copy.filter, copy.operation);
    } else {
        // Vulkan can blit images, but it lacks format reinterpretations
        // Provide a framebuffer in case it's necessary
        ImageView& dst_view = slot_image_views[dst_view_id];
        ImageView& src_view = slot_image_views[src_view_id];
        runtime.BlitImage(dst_framebuffer, dst_view, src_view, dst_region, src_region, copy.filter,
                          copy.operation);
    }
    return true;
}

template <class P>
std::optional<VideoCore::GameDlssProbe> TextureCache<P>::ProbeGameDlssOutput(
    const Tegra::Engines::Fermi2D::Surface& dst, const Tegra::Engines::Fermi2D::Surface& src,
    std::optional<std::array<u32, 2>> src_region) {
    if constexpr (FRAMEBUFFER_BLITS) {
        return std::nullopt;
    } else {
        const Tegra::Engines::Fermi2D::Config copy{
            .operation = Tegra::Engines::Fermi2D::Operation::SrcCopy,
            .filter = Tegra::Engines::Fermi2D::Filter::Point,
            .must_accelerate = true,
            .dst_x0 = 0,
            .dst_y0 = 0,
            .dst_x1 = static_cast<s32>(dst.width),
            .dst_y1 = static_cast<s32>(dst.height),
            .src_x0 = 0,
            .src_y0 = 0,
            .src_x1 = static_cast<s32>(src.width),
            .src_y1 = static_cast<s32>(src.height),
        };
        const auto result = GetBlitImages(dst, src, copy);
        if (!result) {
            return std::nullopt;
        }
        const ImageId dst_id = result->dst_id;
        const ImageId src_id = result->src_id;
        PrepareImage(src_id, false, false);
        PrepareImage(dst_id, false, false);
        for (const ImageId id : {src_id, dst_id}) {
            // The network runs only at the game's own resolution.
            if (slot_images[id].info.num_samples != 1 ||
                True(slot_images[id].flags & ImageFlagBits::Rescaled)) {
                return std::nullopt;
            }
        }
        const auto view = [&](ImageId id, const Tegra::Engines::Fermi2D::Surface& surface,
                              PixelFormat format) -> std::optional<ImageViewId> {
            const std::optional base = slot_images[id].TryFindBase(surface.Address());
            if (!base) {
                return std::nullopt;
            }
            const SubresourceRange range{.base = *base, .extent = {1, 1}};
            return FindOrEmplaceImageView(id, ImageViewInfo(ImageViewType::e2D, format, range));
        };
        const auto src_view = view(src_id, src, result->src_format);
        const auto dst_view = view(dst_id, dst, result->dst_format);
        if (!src_view || !dst_view) {
            return std::nullopt;
        }
        return runtime.ProbeGameDlssOutput(slot_image_views[*src_view],
                                           slot_image_views[*dst_view], src_region);
    }
}

template <class P>
bool TextureCache<P>::TemporalUpscale(const Tegra::Engines::Fermi2D::Surface& dst,
                                      const Tegra::Engines::Fermi2D::Surface& src,
                                      const Tegra::Engines::Fermi2D::Surface& motion_vectors,
                                      const Tegra::Texture::TICEntry* depth,
                                      std::array<f32, 2> jitter, std::array<f32, 2> motion_scale,
                                      bool use_history,
                                      std::optional<std::array<u32, 2>> render_region) {
    if constexpr (FRAMEBUFFER_BLITS) {
        return false;
    } else {
        const Tegra::Engines::Fermi2D::Config copy{
            .operation = Tegra::Engines::Fermi2D::Operation::SrcCopy,
            .filter = Tegra::Engines::Fermi2D::Filter::Bilinear,
            .must_accelerate = true,
            .dst_x0 = 0,
            .dst_y0 = 0,
            .dst_x1 = static_cast<s32>(dst.width),
            .dst_y1 = static_cast<s32>(dst.height),
            .src_x0 = 0,
            .src_y0 = 0,
            .src_x1 = static_cast<s32>(src.width),
            .src_y1 = static_cast<s32>(src.height),
        };
        const auto result = GetBlitImages(dst, src, copy);
        if (!result) {
            return false;
        }
        const auto& motion_surface = use_history ? motion_vectors : src;
        const ImageInfo motion_info(motion_surface);
        has_deleted_images = false;
        const ImageId motion_id =
            FindOrInsertImage(motion_info, motion_surface.Address(), RelaxedOptions{});
        // The depth only feeds NVIDIA DLSS and FSR 3.1 (looked up only when they can run); without
        // it the stand-in runs.
        ImageId depth_id{};
        if (use_history && depth && motion_id && runtime.WantsTemporalUpscaleDepth()) {
            depth_id = FindOrInsertImage(ImageInfo(*depth), depth->Address(), RelaxedOptions{});
            if (!depth_id) {
                return false;
            }
        }
        if (has_deleted_images || !motion_id) {
            return false;
        }
        const ImageId dst_id = result->dst_id;
        const ImageId src_id = result->src_id;
        PrepareImage(src_id, false, false);
        PrepareImage(motion_id, false, false);
        PrepareImage(dst_id, true, false);
        if (depth_id) {
            PrepareImage(depth_id, false, false);
            if (slot_images[depth_id].info.num_samples != 1 ||
                !VideoCommon::DlssContract::IsUpscalerDepthFormat(slot_images[depth_id].info.format)) {
                return false;
            }
        }
        for (const ImageId id : {src_id, motion_id, dst_id}) {
            if (slot_images[id].info.num_samples != 1) {
                return false;
            }
        }
        // With resolution scaling, bring the images that can follow to the same scale, as BlitImage
        // does; the stand-in reads each image at its own size, so any left behind still work.
        const auto rescaled = [&](ImageId id) {
            return True(slot_images[id].flags & ImageFlagBits::Rescaled);
        };
        if (rescaled(src_id) || rescaled(motion_id) || rescaled(dst_id) ||
            (depth_id && rescaled(depth_id))) {
            for (const ImageId id : {src_id, motion_id, dst_id, depth_id}) {
                if (id && !rescaled(id) && ImageCanRescale(slot_images[id])) {
                    ScaleUp(slot_images[id]);
                }
            }
        }
        // FSR 3.1 reads colour, motion and depth texel for texel: all at the colour's scale.
        if (depth_id && (rescaled(depth_id) != rescaled(src_id) ||
                         rescaled(motion_id) != rescaled(src_id))) {
            return false;
        }
        const Extent3D src_size = slot_images[src_id].info.size;
        if (src_size.width != src.width || src_size.height != src.height) {
            return false;
        }
        // The game may render only part of its colour target.
        const std::array<u32, 2> guest_size =
            render_region.value_or(std::array{src_size.width, src_size.height});
        const std::array<u32, 2> render_size =
            rescaled(src_id)
                ? std::array{Settings::values.resolution_info.ScaleUp(guest_size[0]),
                             Settings::values.resolution_info.ScaleUp(guest_size[1])}
                : guest_size;
        // The jitter is in the input's guest pixels.
        if (rescaled(src_id)) {
            const f32 factor = Settings::values.resolution_info.up_factor;
            jitter = {jitter[0] * factor, jitter[1] * factor};
        }
        const auto view = [&](ImageId id, const Tegra::Engines::Fermi2D::Surface& surface,
                              PixelFormat format) -> std::optional<std::pair<FramebufferId, ImageViewId>> {
            const std::optional base = slot_images[id].TryFindBase(surface.Address());
            if (!base) {
                return std::nullopt;
            }
            const SubresourceRange range{.base = *base, .extent = {1, 1}};
            return RenderTargetFromImage(id, ImageViewInfo(ImageViewType::e2D, format, range));
        };
        const auto src_view = view(src_id, src, result->src_format);
        const auto motion_view = view(motion_id, motion_surface, motion_info.format);
        const auto dst_view = view(dst_id, dst, result->dst_format);
        if (!src_view || !motion_view || !dst_view) {
            return false;
        }
        ImageViewId depth_view{};
        if (depth_id) {
            if (const std::optional base = slot_images[depth_id].TryFindBase(depth->Address())) {
                const SubresourceRange range{.base = *base, .extent = {1, 1}};
                depth_view = FindOrEmplaceImageView(
                    depth_id, ImageViewInfo(ImageViewType::e2D, slot_images[depth_id].info.format,
                                            range));
            }
            if (!depth_view) {
                return false;
            }
        }
        // After the views: creating them may reallocate the framebuffer slots.
        Framebuffer* const dst_framebuffer = &slot_framebuffers[dst_view->first];
        return runtime.TemporalUpscale(
            dst_framebuffer, slot_image_views[dst_view->second], slot_image_views[src_view->second],
            slot_image_views[motion_view->second],
            depth_view ? &slot_image_views[depth_view] : nullptr, render_size, jitter,
            motion_scale, use_history);
    }
}

template <class P>
std::pair<typename P::ImageView*, bool> TextureCache<P>::TryFindFramebufferImageView(
    const Tegra::FramebufferConfig& config, DAddr cpu_addr) {
    // TODO: Properly implement this
    const auto it = page_table.find(cpu_addr >> LICHII_PAGEBITS);
    if (it == page_table.end()) {
        return {};
    }
    const auto view_format = [&]() {
        switch (config.pixel_format) {
        case Service::android::PixelFormat::Rgb565:
            return PixelFormat::R5G6B5_UNORM;
        case Service::android::PixelFormat::Bgra8888:
            return PixelFormat::B8G8R8A8_UNORM;
        case Service::android::PixelFormat::Rgba1010102:
            return PixelFormat::A2B10G10R10_UNORM;
        case Service::android::PixelFormat::RgbaFp16:
            return PixelFormat::R16G16B16A16_FLOAT;
        default:
            return PixelFormat::A8B8G8R8_UNORM;
        }
    }();

    const auto& image_map_ids = it->second;
    boost::container::small_vector<ImageId, 4> valid_image_ids;
    for (const ImageMapId map_id : image_map_ids) {
        const ImageMapView& map = slot_map_views[map_id];
        const ImageBase& image = slot_images[map.image_id];
        if (image.cpu_addr != cpu_addr) {
            continue;
        }
        if (image.image_view_ids.empty()) {
            continue;
        }
        if (!VideoCore::Surface::IsViewCompatible(image.info.format, view_format, false, true)) {
            static u32 rejected_alias_count{};
            if (NativeDrawDiagnostics() && rejected_alias_count++ < 8) {
                LOG_WARNING(HW_GPU, "Ignoring incompatible framebuffer alias: address={:#x}, image={}, view={}",
                            cpu_addr, image.info.format, view_format);
            }
            continue;
        }
        valid_image_ids.push_back(map.image_id);
    }

    const auto GetImageViewForFramebuffer = [&](ImageId image_id) {
        ImageViewInfo info{ImageViewType::e2D, view_format};
        if (config.blending == Tegra::BlendMode::Opaque) {
            info.x_source = static_cast<u8>(SwizzleSource::R);
            info.y_source = static_cast<u8>(SwizzleSource::G);
            info.z_source = static_cast<u8>(SwizzleSource::B);
            info.w_source = static_cast<u8>(SwizzleSource::OneFloat);
        }
        return std::make_pair(&slot_image_views[FindOrEmplaceImageView(image_id, info)],
                              slot_images[image_id].IsRescaled());
    };

    if (valid_image_ids.size() == 1) [[likely]] {
        return GetImageViewForFramebuffer(valid_image_ids.front());
    }

    if (valid_image_ids.size() > 0) [[unlikely]] {
        auto most_recent = std::ranges::max_element(valid_image_ids, [&](auto a, auto b) {
            return slot_images[a].modification_tick < slot_images[b].modification_tick;
        });
        return GetImageViewForFramebuffer(*most_recent);
    }

    return {};
}

template <class P>
bool TextureCache<P>::HasUncommittedFlushes() const noexcept {
    return !uncommitted_downloads.empty();
}

template <class P>
bool TextureCache<P>::HasAsyncFlushWork() const noexcept {
    return !uncommitted_downloads.empty() || !uncommitted_async_buffers.empty();
}

template <class P>
bool TextureCache<P>::ShouldWaitAsyncFlushes() const noexcept {
    return !committed_downloads.empty() && !committed_downloads.front().empty();
}

template <class P>
void TextureCache<P>::CommitAsyncFlushes() {
    VideoCommon::Nx2Profile::Scope profile_scope{VideoCommon::Nx2Profile::Zone::TextureCache};
    // This is intentionally passing the value by copy
    if constexpr (IMPLEMENTS_ASYNC_DOWNLOADS) {
        auto& download_ids = uncommitted_downloads;
        const size_t redundant_images = DeduplicatePendingImageDownloads(download_ids, slot_images);
        if (download_ids.empty()) {
            committed_downloads.emplace_back(std::move(uncommitted_downloads));
            uncommitted_downloads.clear();
            async_buffers.emplace_back(std::move(uncommitted_async_buffers));
            uncommitted_async_buffers.clear();
            return;
        }
        size_t total_size_bytes = 0;
        size_t last_async_buffer_id = uncommitted_async_buffers.size();
        bool any_none_dma = false;
        for (PendingDownload& download_info : download_ids) {
            if (download_info.is_swizzle) {
                const ImageBase& image = download_info.GetImage(slot_images);
                total_size_bytes += Common::AlignUp(image.unswizzled_size_bytes, 64);
                any_none_dma = true;
                download_info.async_buffer_id = last_async_buffer_id;
                // Staged below: the flush writes these contents, however the image changes.
                download_info.tick = image.modification_tick;
            }
        }

        if (any_none_dma) {
            if (NativeDrawDiagnostics() && total_size_bytes >= 64_MiB) {
                LOG_WARNING(HW_GPU, "Large image readback batch: bytes={}, entries={}, duplicates_removed={}, submitted_batches={}, retained_buffers={}",
                            total_size_bytes, download_ids.size(), redundant_images,
                            committed_downloads.size(), async_buffers_death_ring.size());
                size_t traced_images = 0;
                for (const auto& download : download_ids) {
                    if (!download.is_swizzle || traced_images++ >= 8) continue;
                    const auto& image = download.GetImage(slot_images);
                    LOG_WARNING(HW_GPU, "Readback image: gpu={:#x}, cpu={:#x}, format={}, size={}x{}x{}, levels={}, layers={}, layer_stride={}, guest_bytes={}, download_bytes={}",
                                image.gpu_addr, image.cpu_addr, image.info.format,
                                image.info.size.width, image.info.size.height, image.info.size.depth,
                                image.info.resources.levels, image.info.resources.layers,
                                image.info.layer_stride, image.guest_size_bytes, image.unswizzled_size_bytes);
                }
            }
            auto download_map = runtime.DownloadStagingBuffer(total_size_bytes, true);
            for (const PendingDownload& download_info : download_ids) {
                if (download_info.is_swizzle) {
                    Image& image = download_info.GetImage(slot_images);
                    const auto copies = FullDownloadCopies(image.info);
                    image.DownloadMemory(download_map, copies);
                    download_map.offset += Common::AlignUp(image.unswizzled_size_bytes, 64);
                }
            }
            uncommitted_async_buffers.emplace_back(download_map);
        }

        async_buffers.emplace_back(std::move(uncommitted_async_buffers));
        uncommitted_async_buffers.clear();
    }
    committed_downloads.emplace_back(std::move(uncommitted_downloads));
    uncommitted_downloads.clear();
}

template <class P>
void TextureCache<P>::PopAsyncFlushes() {
    if (committed_downloads.empty()) {
        return;
    }
    VideoCommon::Nx2Profile::Scope profile_scope{VideoCommon::Nx2Profile::Zone::TextureCache};
    if constexpr (IMPLEMENTS_ASYNC_DOWNLOADS) {
        const auto& download_ids = committed_downloads.front();
        if (download_ids.empty()) {
            committed_downloads.pop_front();
            async_buffers.pop_front();
            return;
        }
        auto download_map = std::move(async_buffers.front());
        for (size_t i = download_ids.size(); i > 0; i--) {
            auto& download_info = download_ids[i - 1];
            auto& download_buffer = download_map[download_info.async_buffer_id];
            if (download_info.is_swizzle) {
                const ImageBase& image = download_info.GetImage(slot_images);
                const auto copies = FullDownloadCopies(image.info);
                download_buffer.offset -= Common::AlignUp(image.unswizzled_size_bytes, 64);
                std::span<u8> download_span =
                    download_buffer.mapped_span.subspan(download_buffer.offset);
                // Newer data written meanwhile stays: a write-back queued after this flush was
                // committed can land before it pops, and the snapshot is older.
                KeepRanges keep;
                KeepNewerLanded(image, download_info.tick, keep);
                if (WriteBackImage(image, copies, download_span, keep)) {
                    NoteFlushed(image, download_info.tick);
                }
            } else {
                const BufferDownload& buffer_info = slot_buffer_downloads[download_info.object_id];
                std::span<u8> download_span =
                    download_buffer.mapped_span.subspan(download_buffer.offset);
                gpu_memory->WriteBlockUnsafe(buffer_info.address, download_span.data(),
                                             buffer_info.size);
                ClearStaleGpu(*gpu_memory, buffer_info.address, buffer_info.size);
                slot_buffer_downloads.erase(download_info.object_id);
            }
        }
        for (auto& download_buffer : download_map) {
            async_buffers_death_ring.emplace_back(download_buffer);
        }
        committed_downloads.pop_front();
        async_buffers.pop_front();
    } else {
        const auto& download_ids = committed_downloads.front();
        if (download_ids.empty()) {
            committed_downloads.pop_front();
            return;
        }
        size_t total_size_bytes = 0;
        for (const PendingDownload& download_info : download_ids) {
            if (download_info.is_swizzle) {
                total_size_bytes += download_info.GetImage(slot_images).unswizzled_size_bytes;
            }
        }
        auto download_map = runtime.DownloadStagingBuffer(total_size_bytes);
        const size_t original_offset = download_map.offset;
        for (const PendingDownload& download_info : download_ids) {
            if (!download_info.is_swizzle) {
                continue;
            }
            Image& image = download_info.GetImage(slot_images);
            const auto copies = FullDownloadCopies(image.info);
            image.DownloadMemory(download_map, copies);
            download_map.offset += image.unswizzled_size_bytes;
        }
        // Wait for downloads to finish
        runtime.Finish();
        download_map.offset = original_offset;
        std::span<u8> download_span = download_map.mapped_span;
        for (const PendingDownload& download_info : download_ids) {
            if (!download_info.is_swizzle) {
                continue;
            }
            const ImageBase& image = download_info.GetImage(slot_images);
            const auto copies = FullDownloadCopies(image.info);
            if (WriteBackImage(image, copies, download_span)) {
                NoteFlushed(image);
            }
            download_map.offset += image.unswizzled_size_bytes;
            download_span = download_span.subspan(image.unswizzled_size_bytes);
        }
        committed_downloads.pop_front();
    }
}

template <class P>
ImageId TextureCache<P>::DmaImageId(const Tegra::DMA::ImageOperand& operand, bool is_upload) {
    const ImageInfo dst_info(operand);
    const ImageId dst_id = FindDMAImage(dst_info, operand.address);
    if (!dst_id) {
        return NULL_IMAGE_ID;
    }
    auto& image = slot_images[dst_id];
    if (False(image.flags & ImageFlagBits::GpuModified)) {
        // No need to waste time on an image that's synced with guest
        return NULL_IMAGE_ID;
    }
    if (image.info.type == ImageType::e3D) {
        // Don't accelerate 3D images.
        return NULL_IMAGE_ID;
    }
    if (!is_upload && !image.info.dma_downloaded) {
        // Force a full sync.
        image.info.dma_downloaded = true;
        return NULL_IMAGE_ID;
    }
    const auto base = image.TryFindBase(operand.address);
    if (!base) {
        return NULL_IMAGE_ID;
    }
    return dst_id;
}

template <class P>
bool TextureCache<P>::IsRescaling() const noexcept {
    return is_rescaling;
}

template <class P>
bool TextureCache<P>::IsRescaling(const ImageViewBase& image_view) const noexcept {
    if (image_view.type == ImageViewType::Buffer) {
        return false;
    }
    const ImageBase& image = slot_images[image_view.image_id];
    return True(image.flags & ImageFlagBits::Rescaled);
}

template <class P>
bool TextureCache<P>::IsRegionGpuModified(DAddr addr, size_t size) {
    bool is_modified = false;
    ForEachImageInRegion(addr, size, [&is_modified](ImageId, ImageBase& image) {
        if (False(image.flags & ImageFlagBits::GpuModified)) {
            return false;
        }
        is_modified = true;
        return true;
    });
    return is_modified || WritebackPendingOver(addr, size);
}

template <class P>
std::pair<typename TextureCache<P>::Image*, BufferImageCopy> TextureCache<P>::DmaBufferImageCopy(
    const Tegra::DMA::ImageCopy& copy_info, const Tegra::DMA::BufferOperand& buffer_operand,
    const Tegra::DMA::ImageOperand& image_operand, ImageId image_id, bool modifies_image) {
    const auto [level, base] = PrepareDmaImage(image_id, image_operand.address, modifies_image);
    auto* image = &slot_images[image_id];
    const u32 buffer_size = static_cast<u32>(buffer_operand.pitch * buffer_operand.height);
    const u32 bpp = VideoCore::Surface::BytesPerBlock(image->info.format);
    const auto convert = [old_bpp = image_operand.bytes_per_pixel, bpp](u32 value) {
        return (old_bpp * value) / bpp;
    };
    const u32 base_x = convert(image_operand.params.origin.x.Value());
    const u32 base_y = image_operand.params.origin.y.Value();
    const u32 length_x = convert(copy_info.length_x);
    const u32 length_y = copy_info.length_y;

    const BufferImageCopy copy{
        .buffer_offset = 0,
        .buffer_size = buffer_size,
        .buffer_row_length = convert(buffer_operand.pitch),
        .buffer_image_height = buffer_operand.height,
        .image_subresource =
            {
                .base_level = static_cast<s32>(level),
                .base_layer = static_cast<s32>(base),
                .num_layers = 1,
            },
        .image_offset =
            {
                .x = static_cast<s32>(base_x),
                .y = static_cast<s32>(base_y),
                .z = 0,
            },
        .image_extent =
            {
                .width = length_x,
                .height = length_y,
                .depth = 1,
            },
    };
    return {image, copy};
}

template <class P>
void TextureCache<P>::DownloadImageIntoBuffer(typename TextureCache<P>::Image* image,
                                              typename TextureCache<P>::BufferType buffer,
                                              size_t buffer_offset,
                                              std::span<const VideoCommon::BufferImageCopy> copies,
                                              GPUVAddr address, size_t size) {
    if constexpr (IMPLEMENTS_ASYNC_DOWNLOADS) {
        const BufferDownload new_buffer_download{address, size};
        auto slot = slot_buffer_downloads.insert(new_buffer_download);
        const PendingDownload new_download{false, uncommitted_async_buffers.size(), slot};
        uncommitted_downloads.emplace_back(new_download);
        auto download_map = runtime.DownloadStagingBuffer(size, true);
        uncommitted_async_buffers.emplace_back(download_map);
        std::array buffers{
            buffer,
            download_map.buffer,
        };
        std::array<size_t, 2> buffer_offsets{
            buffer_offset,
            download_map.offset,
        };
        image->DownloadMemory(buffers, buffer_offsets, copies);
    } else {
        image->DownloadMemory(buffer, buffer_offset, copies);
    }
}

template <class P>
void TextureCache<P>::NoteHostOutOfMemory(std::string_view operation, const ImageBase& image) {
    swizzle_data_buffer.release();
    unswizzle_data_buffer.release();
    host_oom_retry_frame = frame_tick + 60;
    ++host_oom_failures;
    const auto now = std::chrono::steady_clock::now();
    if (host_oom_log_time == std::chrono::steady_clock::time_point{} ||
        now - host_oom_log_time >= std::chrono::seconds{10}) {
        LOG_ERROR(HW_GPU,
                  "No host memory to {} image {:#x} ({} MiB); kept for a later try ({} failures "
                  "since the last log)",
                  operation, image.gpu_addr, image.guest_size_bytes >> 20, host_oom_failures);
        host_oom_failures = 0;
        host_oom_log_time = now;
    }
}

template <class P>
void TextureCache<P>::RefreshContents(Image& image, ImageId image_id) {
    if (False(image.flags & ImageFlagBits::CpuModified)) {
        // Only upload modified images
        return;
    }
    if (frame_tick < host_oom_retry_frame && image.guest_size_bytes >= 64_MiB) {
        runtime.TransitionImageLayout(image);
        return;
    }
    VideoCommon::Nx2Profile::Scope profile_scope{VideoCommon::Nx2Profile::Zone::TextureUpload};

    const u64 previous_load_tick = image.load_tick;
    const u32 previous_storage_written_levels = image.storage_written_levels;
    image.flags &= ~ImageFlagBits::CpuModified;
    image.load_tick = modification_tick;
    image.storage_written_levels = 0;

    // Images uploaded by layer stay tracked while some of their layers are dirty.
    if (False(image.flags & ImageFlagBits::Tracked)) {
        TrackImage(image, image_id);
    }

    // Whole pages inside it too: Mark leaves a deleted target's partial edge pages unmarked.
    const u64 evicted_first = Common::AlignUp(image.cpu_addr, StalePages::PAGE_SIZE);
    const u64 evicted_end =
        Common::AlignDown(image.cpu_addr + image.guest_size_bytes, StalePages::PAGE_SIZE);
    if (evicted_pages.IsAll(image.cpu_addr, image.guest_size_bytes) ||
        (evicted_end > evicted_first &&
         evicted_pages.IsAll(evicted_first, evicted_end - evicted_first))) {
        image.flags |= ImageFlagBits::EvictBorn;
        ++evict_born;
    }
    // Made where a dropped GPU-written image left stale bytes (MarkStale): nothing to upload.
    // Pages the buffer cache wrote on the GPU may hold data written back since: upload those.
    if (Tegra::g_nx2_ampere.load(std::memory_order_relaxed) && image.dirty_layers.empty() &&
        False(image.flags & (ImageFlagBits::Sparse | ImageFlagBits::SparseLayers |
                             ImageFlagBits::SparseTiles | ImageFlagBits::Unbacked)) &&
        stale_pages.IsAll(image.cpu_addr, image.guest_size_bytes)) {
        if (!Tegra::Nx2MayBeGpuWritten(image.cpu_addr, image.guest_size_bytes)) {
            const bool upload = WritebackSwitches::UploadStale();
            if (Common::DkbFrameTrace::Enabled()) {
                static u64 stale_images{};
                if (stale_images++ % 32 == 0) {
                    LOG_INFO(HW_GPU,
                             "DKB stale image #{} gpu={:#x} device={:#x} size={:#x} {}x{} "
                             "format={} levels={} uploaded={}",
                             stale_images, image.gpu_addr, image.cpu_addr,
                             image.guest_size_bytes, image.info.size.width,
                             image.info.size.height, static_cast<u32>(image.info.format),
                             image.info.resources.levels, upload);
                }
            }
            if (!upload) {
                if (TextureCacheTrace()) {
                    LOG_WARNING(HW_GPU,
                                "TCTRACE stale-skip gpu={:#x} fmt={} {}x{}x{} guest={}KiB frame={}",
                                image.gpu_addr, static_cast<u32>(image.info.format),
                                image.info.size.width, image.info.size.height,
                                image.info.size.depth, image.guest_size_bytes / 1024, frame_tick);
                }
                ++tc_stale_skips;
                tc_stale_skip_bytes += image.guest_size_bytes;
                runtime.TransitionImageLayout(image);
                return;
            }
        } else {
            ++tc_stale_gpu_written;
        }
    }

    if (True(image.flags & ImageFlagBits::SparseLayers)) {
        RefreshSparseLayers(image);
        return;
    }
    if (True(image.flags & ImageFlagBits::SparseTiles)) {
        RefreshSparseTiles(image);
        return;
    }
    if (!image.dirty_layers.empty() && UploadWrittenLayers(image)) {
        runtime.InsertUploadMemoryBarrier();
        return;
    }
    image.dirty_layers.clear();

    if (True(image.flags & ImageFlagBits::Unbacked)) {
        // Nothing to upload into (and staging it could take tens of GB).
        runtime.TransitionImageLayout(image);
        return;
    }

    if (image.info.num_samples > 1 && !runtime.CanUploadMSAA()) {
        LOG_WARNING(HW_GPU, "MSAA image uploads are not implemented");
        runtime.TransitionImageLayout(image);
        return;
    }
    if (True(image.flags & ImageFlagBits::AsynchronousDecode)) {
        QueueAsyncDecode(image, image_id);
        return;
    }

    const bool gpu_unswizzle_enabled = Settings::values.gpu_unswizzle_enabled.GetValue();

    if (gpu_unswizzle_enabled &&
        IsPixelFormatBCn(image.info.format) &&
        image.info.type == ImageType::e3D &&
        image.info.resources.levels == 1 &&
        image.info.resources.layers == 1 &&
        MapSizeBytes(image) >= gpu_unswizzle_maxsize &&
        False(image.flags & ImageFlagBits::GpuModified)) {

        QueueAsyncUnswizzle(image, image_id);
        return;
    }
    using TraceClock = std::chrono::steady_clock;
    const bool tc_trace = TextureCacheTrace();
    const auto trace_begin = tc_trace ? TraceClock::now() : TraceClock::time_point{};
    const u64 trace_faults = tc_trace ? Common::GetProcessPageFaults() : 0;
    auto trace_staged = trace_begin;
    upload_trace = {};
    const bool chunked = UploadInChunks(image);
    if (!chunked) {
        auto staging = runtime.UploadStagingBuffer(MapSizeBytes(image));
        if (tc_trace) {
            trace_staged = TraceClock::now();
        }
        try {
            UploadImageContents(image, staging);
        } catch (const std::bad_alloc&) {
            image.flags |= ImageFlagBits::CpuModified;
            image.load_tick = previous_load_tick;
            image.storage_written_levels = previous_storage_written_levels;
            if (False(image.flags & ImageFlagBits::Sparse) ||
                True(image.flags & ImageFlagBits::Registered)) {
                UntrackImage(image, image_id);
            }
            NoteHostOutOfMemory("upload", image);
            runtime.TransitionImageLayout(image);
            return;
        }
        runtime.InsertUploadMemoryBarrier();
    }
    ++images_uploaded;
    uploaded_bytes += image.guest_size_bytes;
    largest_upload = (std::max<u64>)(largest_upload, image.guest_size_bytes);
    if (tc_trace) {
        using Ms = std::chrono::duration<double, std::milli>;
        LOG_WARNING(HW_GPU,
                    "TCTRACE upload gpu={:#x} fmt={} {}x{}x{} lv={} ly={} guest={}KiB host={}KiB "
                    "ms={:.2f} staging={:.2f} prefault={:.2f} read={:.2f} untile={:.2f} faults={} "
                    "chunked={} frame={}",
                    image.gpu_addr, static_cast<u32>(image.info.format), image.info.size.width,
                    image.info.size.height, image.info.size.depth, image.info.resources.levels,
                    image.info.resources.layers, image.guest_size_bytes / 1024,
                    MapSizeBytes(image) / 1024, Ms(TraceClock::now() - trace_begin).count(),
                    Ms(trace_staged - trace_begin).count(), upload_trace.prefault_ms,
                    upload_trace.read_ms, upload_trace.untile_ms,
                    Common::GetProcessPageFaults() - trace_faults, chunked, frame_tick);
    }
    // The host GPU has its own copy now and the guest rarely reads this memory again: TickFrame
    // marks it cold, so the OS pages it out before anything else when it needs RAM (title 1
    // streams several GB of textures through it).
    constexpr u64 ColdHintMinBytes = 64_KiB;
    if (image.guest_size_bytes >= ColdHintMinBytes &&
        False(image.flags & ImageFlagBits::GpuModified)) {
        if (True(image.flags & ImageFlagBits::Sparse)) {
            // Non-contiguous: each mapped piece (title 1 maps its own memory into these).
            ForEachSparseSegment(image, [&](GPUVAddr, DAddr address, size_t size) {
                uploaded_ranges.emplace_back(address, size);
            });
        } else {
            uploaded_ranges.emplace_back(image.cpu_addr, image.guest_size_bytes);
        }
    }
}

template <class P>
bool TextureCache<P>::UploadInChunks(Image& image) {
    // A whole upload of a very large image needs a staging buffer of its full size (rounded up to
    // a power of two: 1 GiB for 600 MiB of data), which can fail when memory is tight.
    constexpr u64 min_size = 64_MiB;
    constexpr u64 chunk_size = 32_MiB;
    constexpr ImageFlagBits excluded = ImageFlagBits::Sparse | ImageFlagBits::Converted |
                                       ImageFlagBits::AcceleratedUpload | ImageFlagBits::Unbacked |
                                       ImageFlagBits::TrimmedLevels;
    if (True(image.flags & excluded) || image.info.type != ImageType::e2D ||
        MapSizeBytes(image) < min_size) {
        return false;
    }
    for (u64 offset = 0; offset < image.guest_size_bytes; offset += chunk_size) {
        const std::array range{
            std::pair<u64, u64>{offset, std::min<u64>(offset + chunk_size, image.guest_size_bytes)}};
        const auto plan = PlanUnswizzleImageRows(image.gpu_addr, image.info, range);
        if (!plan.layout_valid) {
            if (offset == 0) {
                return false; // layout not handled: one staging buffer after all
            }
            continue;
        }
        if (plan.copies.empty()) {
            continue; // padding between levels or layers
        }
        auto staging = runtime.UploadStagingBuffer(plan.total_host_bytes);
        const auto prefault_begin = std::chrono::steady_clock::now();
        PrefaultGuestMemory(*gpu_memory, image.gpu_addr + range[0].first,
                            range[0].second - range[0].first);
        if (TextureCacheTrace()) {
            upload_trace.prefault_ms += std::chrono::duration<double, std::milli>(
                                            std::chrono::steady_clock::now() - prefault_begin)
                                            .count();
        }
        ExecuteUnswizzlePlan(*gpu_memory, plan, staging.mapped_span);
        image.UploadMemory(staging, plan.copies);
    }
    runtime.InsertUploadMemoryBarrier();
    return true;
}

template <class P>
bool TextureCache<P>::UploadsByLayer(const ImageBase& image) const noexcept {
    // Title 1's terrain arrays (8192x8192x68 ASTC, 1.45 GB each) get a few layers streamed in at a
    // time; re-decoding all of them for each took a 2 GiB staging buffer and stalled the GPU.
    if constexpr (requires { &Runtime::AccelerateImageUploadLayers; }) {
        if (True(image.flags & ImageFlagBits::SparseLayers)) {
            return true;
        }
        return True(image.flags & ImageFlagBits::AcceleratedUpload) &&
               image.info.type == ImageType::e2D && image.info.resources.layers > 1 &&
               image.guest_size_bytes >= LayerUploadMinSize &&
               False(image.flags & (ImageFlagBits::Unbacked | ImageFlagBits::Sparse |
                                    ImageFlagBits::Remapped));
    } else {
        return false;
    }
}

template <class P>
void TextureCache<P>::MarkLayersWritten(Image& image, DAddr cpu_addr, size_t size) {
    if (True(image.flags & ImageFlagBits::CpuModified) && image.dirty_layers.empty()) {
        return; // all of it is dirty already
    }
    const u32 layers = static_cast<u32>(image.info.resources.layers);
    if (False(image.flags & ImageFlagBits::CpuModified)) {
        image.flags |= ImageFlagBits::CpuModified;
        image.dirty_layers.assign(Common::DivCeil(layers, 64U), 0);
    }
    const DAddr begin = (std::max<DAddr>)(cpu_addr, image.cpu_addr) - image.cpu_addr;
    const DAddr end = (std::min<DAddr>)(cpu_addr + size, image.cpu_addr + image.guest_size_bytes) -
                      image.cpu_addr;
    const u32 stride = image.info.layer_stride;
    if (end <= begin || stride == 0) {
        return;
    }
    const u32 last = (std::min)(static_cast<u32>((end - 1) / stride), layers - 1);
    for (u32 layer = static_cast<u32>(begin / stride); layer <= last; ++layer) {
        image.dirty_layers[layer / 64] |= u64{1} << (layer % 64);
    }
}

template <class P>
void TextureCache<P>::MarkRangeWritten(Image& image, DAddr cpu_addr, size_t size) {
    if (True(image.flags & ImageFlagBits::CpuModified) && image.dirty_ranges.empty()) {
        return; // all of it is dirty already
    }
    const DAddr begin = (std::max<DAddr>)(cpu_addr, image.cpu_addr);
    const DAddr end = (std::min<DAddr>)(cpu_addr + size, image.cpu_addr + image.guest_size_bytes);
    if (end <= begin) {
        return;
    }
    image.flags |= ImageFlagBits::CpuModified;
    image.dirty_ranges.emplace_back(begin - image.cpu_addr, end - begin);
}

template <class P>
void TextureCache<P>::RefreshSparseTiles(Image& image) {
    if constexpr (requires { &Image::BindTiles; }) {
        // The ranges written since the last upload, or, for a new image, all of it that can hold
        // data (the tiles the game maps there, or the blocks written).
        std::vector<std::pair<u64, u64>> ranges = std::move(image.dirty_ranges);
        image.dirty_ranges.clear();
        if (ranges.empty()) {
            ranges = Tegra::Nx2SparseDataRanges(image.gpu_addr, image.guest_size_bytes);
        }
        const std::vector<LevelRect> rects =
            SparseTileRects(image.info, ranges, image.SparseTileWidth(), image.SparseTileHeight(),
                            image.SparseTailLevel());
        if (rects.empty() || !BindSparseTiles(image, rects)) {
            runtime.TransitionImageLayout(image);
            return;
        }
        const size_t bytes = UnswizzledRectsBytes(image.info, rects);
        auto staging = runtime.UploadStagingBuffer(bytes);
        const std::vector<BufferImageCopy> copies =
            UnswizzleImageRects(*gpu_memory, image.gpu_addr, image.info, rects, staging.mapped_span);
        image.UploadMemory(staging, copies);
        runtime.InsertUploadMemoryBarrier();
        ++images_uploaded;
        uploaded_bytes += bytes;
    }
}

template <class P>
bool TextureCache<P>::BindSparseTiles(Image& image, std::span<const LevelRect> rects) {
    if constexpr (requires { &Image::BindTiles; }) {
        const u64 bound_bytes = ImageMemoryBytes(image);
        const bool bound = image.BindTiles(rects);
        // RegisterImage counts what an image has bound by then; later binds are added here.
        if (True(image.flags & ImageFlagBits::Registered)) {
            total_used_memory += ImageMemoryBytes(image) - bound_bytes;
        }
        return bound;
    } else {
        return false;
    }
}

template <class P>
bool TextureCache<P>::BackSparseTilesWhole(Image& image) {
    if constexpr (requires { &Image::BackTilesWhole; }) {
        const u64 bound_bytes = ImageMemoryBytes(image);
        const bool backed = image.BackTilesWhole();
        if (True(image.flags & ImageFlagBits::Registered)) {
            total_used_memory += ImageMemoryBytes(image) - bound_bytes;
        }
        return backed;
    } else {
        return false;
    }
}

template <class P>
bool TextureCache<P>::UploadWrittenLayers(Image& image) {
    if constexpr (requires { &Runtime::AccelerateImageUploadLayers; }) {
        const u32 num_layers = static_cast<u32>(image.info.resources.layers);
        std::vector<u32> layers;
        for (u32 layer = 0; layer < num_layers; ++layer) {
            if ((image.dirty_layers[layer / 64] >> (layer % 64)) & 1) {
                layers.push_back(layer);
            }
        }
        image.dirty_layers.clear();
        if (layers.empty() || layers.size() == num_layers) {
            return false;
        }
        UploadLayers(image, layers);
        return true;
    } else {
        return false;
    }
}

template <class P>
void TextureCache<P>::RefreshSparseLayers(Image& image) {
    if constexpr (requires { &Image::BindLayers; }) {
        // The layers written since the last upload, or, for a new image, every layer any of whose
        // blocks has been written.
        const u32 num_layers = static_cast<u32>(image.info.resources.layers);
        const u64 stride = image.info.layer_stride;
        std::vector<u32> layers;
        for (u32 layer = 0; layer < num_layers; ++layer) {
            const bool written =
                image.dirty_layers.empty()
                    ? !Tegra::Nx2SparseUnwritten(image.gpu_addr + layer * stride, stride)
                    : ((image.dirty_layers[layer / 64] >> (layer % 64)) & 1) != 0;
            if (written) {
                layers.push_back(layer);
            }
        }
        image.dirty_layers.clear();
        const std::vector<u32> bound =
            layers.empty() ? std::vector<u32>{} : BindSparseLayers(image, layers);
        if (bound.empty()) {
            runtime.TransitionImageLayout(image);
            return;
        }
        UploadLayers(image, bound);
        runtime.InsertUploadMemoryBarrier();
    }
}

template <class P>
std::vector<u32> TextureCache<P>::BindSparseLayers(Image& image, std::span<const u32> layers) {
    if constexpr (requires { &Image::BindLayers; }) {
        const u64 bound_bytes = ImageMemoryBytes(image);
        std::vector<u32> bound = image.BindLayers(layers);
        // RegisterImage counts what an image has bound by then; later binds are added here.
        if (True(image.flags & ImageFlagBits::Registered)) {
            total_used_memory += ImageMemoryBytes(image) - bound_bytes;
        }
        return bound;
    } else {
        return {};
    }
}

template <class P>
void TextureCache<P>::UploadLayers(Image& image, std::span<const u32> layers) {
    if constexpr (requires { &Runtime::AccelerateImageUploadLayers; }) {
        // At most 32 MiB of layers per staging buffer, as UploadInChunks: all of a sparse array's
        // bound layers at once took a 128 MiB one an 8 GB laptop had no memory for (#262).
        constexpr u64 ChunkBytes = 32_MiB;
        const size_t chunk_layers = static_cast<size_t>(
            (std::max<u64>)(1, ChunkBytes / (std::max<u64>)(image.info.layer_stride, 1)));
        if (layers.size() > chunk_layers) {
            for (size_t first = 0; first < layers.size(); first += chunk_layers) {
                UploadLayers(image, layers.subspan(first, (std::min)(chunk_layers,
                                                                     layers.size() - first)));
            }
            return;
        }
        if (False(image.flags & ImageFlagBits::AcceleratedUpload)) {
            // Plain formats: each layer unswizzled on its own into its layer of the image.
            ImageInfo layer_info = image.info;
            layer_info.resources.layers = 1;
            const size_t layer_size = CalculateUnswizzledSizeBytes(layer_info);
            auto staging = runtime.UploadStagingBuffer(layer_size * layers.size());
            std::vector<BufferImageCopy> copies;
            for (size_t i = 0; i < layers.size(); ++i) {
                const GPUVAddr layer_addr = image.gpu_addr + u64{layers[i]} * image.info.layer_stride;
                Tegra::Memory::GpuGuestMemory<u8, Tegra::Memory::GuestMemoryFlags::UnsafeRead>
                    swizzle_data(*gpu_memory, layer_addr, image.info.layer_stride,
                                 &swizzle_data_buffer);
                auto layer_copies =
                    UnswizzleImage(*gpu_memory, layer_addr, layer_info, swizzle_data,
                                   staging.mapped_span.subspan(i * layer_size, layer_size));
                for (BufferImageCopy copy : layer_copies) {
                    copy.buffer_offset += i * layer_size;
                    copy.image_subresource.base_layer = static_cast<s32>(layers[i]);
                    copies.push_back(copy);
                }
            }
            image.UploadMemory(staging, copies);
            ++images_uploaded;
            uploaded_bytes += u64{image.info.layer_stride} * layers.size();
            ++layer_uploads;
            return;
        }
        // Each layer's mip chain from the first level the host keeps on (as UploadImageContents).
        auto uploads = FullUploadSwizzles(image.info);
        const size_t level_offset = uploads[image.host_level_offset].buffer_offset;
        const size_t tail = image.info.layer_stride - level_offset;
        auto staging = runtime.UploadStagingBuffer(tail * layers.size());
        for (size_t i = 0; i < layers.size(); ++i) {
            gpu_memory->ReadBlock(image.gpu_addr + u64{layers[i]} * image.info.layer_stride +
                                      level_offset,
                                  staging.mapped_span.data() + i * tail, tail,
                                  VideoCommon::CacheType::NoTextureCache);
        }
        uploads.erase(uploads.begin(), uploads.begin() + image.host_level_offset);
        for (auto& upload : uploads) {
            upload.buffer_offset -= level_offset;
        }
        runtime.AccelerateImageUploadLayers(image, staging, uploads, layers);
        ++images_uploaded;
        uploaded_bytes += tail * layers.size();
        ++layer_uploads;
    }
}

template <class P>
template <typename StagingBuffer>
void TextureCache<P>::UploadImageContents(Image& image, StagingBuffer& staging) {
    const std::span<u8> mapped_span = staging.mapped_span;
    const GPUVAddr gpu_addr = image.gpu_addr;

    if (True(image.flags & ImageFlagBits::AcceleratedUpload) && image.host_level_offset != 0) {
        // Only levels >= host_level_offset exist on the host: read each layer's tail of the mip
        // chain into a compact buffer (layer stride = that tail's size) and decode those levels.
        auto uploads = FullUploadSwizzles(image.info);
        const size_t level_offset = uploads[image.host_level_offset].buffer_offset;
        const size_t tail = image.info.layer_stride - level_offset;
        for (s32 layer = 0; layer < image.info.resources.layers; ++layer) {
            gpu_memory->ReadBlock(gpu_addr + layer * image.info.layer_stride + level_offset,
                                  mapped_span.data() + layer * tail, tail,
                                  VideoCommon::CacheType::NoTextureCache);
        }
        uploads.erase(uploads.begin(), uploads.begin() + image.host_level_offset);
        for (auto& upload : uploads) {
            upload.buffer_offset -= level_offset;
        }
        runtime.AccelerateImageUpload(image, staging, uploads, 0, 0);
        return;
    }

    using TraceClock = std::chrono::steady_clock;
    const bool tc_trace = TextureCacheTrace();
    const auto prefault_begin = tc_trace ? TraceClock::now() : TraceClock::time_point{};
    PrefaultGuestMemory(*gpu_memory, gpu_addr, image.guest_size_bytes);
    const auto read_begin = tc_trace ? TraceClock::now() : TraceClock::time_point{};
    if (tc_trace) {
        upload_trace.prefault_ms =
            std::chrono::duration<double, std::milli>(read_begin - prefault_begin).count();
    }
    if (True(image.flags & ImageFlagBits::AcceleratedUpload)) {
        gpu_memory->ReadBlock(gpu_addr, mapped_span.data(), mapped_span.size_bytes(),
                              VideoCommon::CacheType::NoTextureCache);
        const auto uploads = FullUploadSwizzles(image.info);
        runtime.AccelerateImageUpload(image, staging, uploads, 0, 0);
        return;
    }

    std::span<const u8> swizzle_data;
    if (const u8* direct = gpu_memory->GetSpan(gpu_addr, image.guest_size_bytes)) {
        swizzle_data = {direct, image.guest_size_bytes};
    } else {
        swizzle_data_buffer.resize_destructive(image.guest_size_bytes);
        gpu_memory->ReadBlockUnsafe(gpu_addr, swizzle_data_buffer.data(), image.guest_size_bytes);
        swizzle_data = {swizzle_data_buffer.data(), swizzle_data_buffer.size()};
    }
    const auto untile_begin = tc_trace ? TraceClock::now() : TraceClock::time_point{};
    const auto trace_untile = [&] {
        if (tc_trace) {
            using Ms = std::chrono::duration<double, std::milli>;
            upload_trace.read_ms = Ms(untile_begin - read_begin).count();
            upload_trace.untile_ms = Ms(TraceClock::now() - untile_begin).count();
        }
    };
    if (True(image.flags & ImageFlagBits::Converted)) {
        unswizzle_data_buffer.resize_destructive(image.unswizzled_size_bytes);
        auto copies = UnswizzleImage(*gpu_memory, gpu_addr, image.info, swizzle_data,
                                    unswizzle_data_buffer, &unswizzle_worker);
        ConvertImage(unswizzle_data_buffer, image.info, mapped_span, copies);
        trace_untile();
        image.UploadMemory(staging, copies);
    } else {
        const auto copies = UnswizzleImage(*gpu_memory, gpu_addr, image.info, swizzle_data,
                                          mapped_span, &unswizzle_worker);
        trace_untile();
        image.UploadMemory(staging, copies);
    }
}

template <class P>
ImageViewId TextureCache<P>::CreateImageView(const TICEntry& raw_config, bool& retry) {
    const bool native = gpu_memory->UsesNativeTiling();
    if (native && raw_config.format.Value() == Tegra::Texture::TextureFormat{} &&
        RenderTrace::Enabled()) {
        static u32 traces{};
        if (traces++ < 64) {
            LOG_WARNING(HW_GPU,
                        "Zero-format image TIC: image={:#x} size={}x{}x{} header={} type={} "
                        "raw={:016x}/{:016x}/{:016x}/{:016x}",
                        raw_config.Address(), raw_config.Width(), raw_config.Height(),
                        raw_config.Depth(), static_cast<u32>(raw_config.header_version.Value()),
                        static_cast<u32>(raw_config.texture_type.Value()), raw_config.raw[0],
                        raw_config.raw[1], raw_config.raw[2], raw_config.raw[3]);
        }
    }
    if (native && raw_config.BaseLayer() && NativeDrawDiagnostics()) {
        static u32 texture_view_traces{};
        if (texture_view_traces++ < 8) {
            LOG_WARNING(HW_GPU, "Native texture view header: address={:#x}, dimensions={}x{}x{}, type={}, legacy_layer={}, raw={:016x}/{:016x}/{:016x}/{:016x}",
                        raw_config.Address(), raw_config.Width(), raw_config.Height(), raw_config.Depth(),
                        static_cast<u32>(raw_config.texture_type.Value()), raw_config.BaseLayer(),
                        raw_config.raw[0], raw_config.raw[1], raw_config.raw[2], raw_config.raw[3]);
        }
    }
    const TICEntry config = TextureViewHeader(raw_config, native);
    const ImageInfo info(config);
    if (info.type == ImageType::Buffer) {
        const ImageViewInfo view_info(config, 0);
        return slot_image_views.insert(runtime, info, view_info, config.Address());
    }
    const u32 layer_offset = config.BaseLayer() * info.layer_stride;
    const GPUVAddr image_gpu_addr = config.Address() - layer_offset;
    const ImageId image_id = FindOrInsertImage(info, image_gpu_addr, RelaxedOptions{}, &retry);
    if (!image_id) {
        return NULL_IMAGE_VIEW_ID;
    }
    ImageBase& image = slot_images[image_id];
    const SubresourceBase base = image.TryFindBase(config.Address()).value();
    ASSERT(base.level == 0);
    const ImageViewInfo view_info(config, base.layer);
    const ImageViewId image_view_id = FindOrEmplaceImageView(image_id, view_info);
    ImageViewBase& image_view = slot_image_views[image_view_id];
    image_view.flags |= ImageViewFlagBits::Strong;
    image.flags |= ImageFlagBits::Strong;
    return image_view_id;
}

template <class P>
ImageId TextureCache<P>::FindOrInsertImage(const ImageInfo& info, GPUVAddr gpu_addr,
                                           RelaxedOptions options, bool* retry) {
    bool deferred = false;
    if (const ImageId image_id = FindImage(info, gpu_addr, options, &deferred); image_id) {
        return image_id;
    }
    if (deferred) {
        if (retry != nullptr) {
            *retry = true;
        }
        return NULL_IMAGE_ID;
    }
    return InsertImage(info, gpu_addr, options);
}

template <class P>
ImageId TextureCache<P>::FindImage(const ImageInfo& info, GPUVAddr gpu_addr,
                                   RelaxedOptions options, bool* retry) {
    const size_t size = CalculateGuestSizeInBytes(info);
    const std::optional<DAddr> cpu_addr = ImageCpuAddress(gpu_addr, size);
    if (!cpu_addr) {
        return ImageId{};
    }
    if (retry && info.maxwell_gob_layout && gpu_memory->UsesNativeTiling() &&
        !GobViewBytesReady(*cpu_addr, size)) {
        *retry = true;
        return NULL_IMAGE_ID;
    }
    const bool broken_views =
        runtime.HasBrokenTextureViewFormats() || True(options & RelaxedOptions::ForceBrokenViews);
    const bool native_bgr = runtime.HasNativeBgr();
    const bool native_tiling = gpu_memory->UsesNativeTiling() && !info.maxwell_gob_layout;
    const bool flexible_formats = True(options & RelaxedOptions::Format);
    ImageId image_id{};
    boost::container::small_vector<ImageId, 8> image_ids;
    const auto lambda = [&](ImageId existing_image_id, ImageBase& existing_image) {
        if (True(existing_image.flags & ImageFlagBits::Remapped)) {
            return false;
        }
        if (info.type == ImageType::Linear || existing_image.info.type == ImageType::Linear)
            [[unlikely]] {
            const bool strict_size = False(options & RelaxedOptions::Size) &&
                                     True(existing_image.flags & ImageFlagBits::Strong);
            const ImageInfo& existing = existing_image.info;
            if (existing_image.gpu_addr == gpu_addr && existing.type == info.type &&
                existing.pitch == info.pitch &&
                IsPitchLinearSameSize(existing, info, strict_size) &&
                IsViewCompatible(existing.format, info.format, broken_views, native_bgr)) {
                image_id = existing_image_id;
                image_ids.push_back(existing_image_id);
                return !flexible_formats && existing.format == info.format;
            }
        } else if (existing_image.info.native_tiling == native_tiling &&
                   IsSubresource(info, existing_image, gpu_addr, options, broken_views,
                                 native_bgr)) {
            image_id = existing_image_id;
            image_ids.push_back(existing_image_id);
            return !flexible_formats && existing_image.info.format == info.format;
        }
        return false;
    };
    ForEachImageInRegion(*cpu_addr, size, lambda);
    if (image_ids.size() <= 1) [[likely]] {
        return image_id;
    }
    auto image_ids_compare = [this](ImageId a, ImageId b) {
        auto& image_a = slot_images[a];
        auto& image_b = slot_images[b];
        return image_a.modification_tick < image_b.modification_tick;
    };
    return *std::ranges::max_element(image_ids, image_ids_compare);
}

template <class P>
bool TextureCache<P>::ImageCanRescale(ImageBase& image) {
    if (!image.info.rescaleable) {
        return false;
    }
    if constexpr (requires { runtime.GetMaxScaledExtent(); }) {
        // Past the device's limits the scaled image is invalid. Ocarina of Time's 12288x12288
        // sun shadow map is 18432 wide at 1.5x and 24576 at 2x, over AMD's 16384, and drawing
        // into it hangs the GPU (#1885).
        if (!ScaledImageFits(image.info, Settings::values.resolution_info,
                             runtime.GetMaxScaledExtent())) {
            return false;
        }
    }
    if (Settings::values.resolution_info.downscale && !image.info.downscaleable) {
        return false;
    }
    if (Settings::values.resolution_info.downscale) {
        const auto& info = image.info;
        const auto& resolution = Settings::values.resolution_info;
        const u32 levels = HostMipLevelCount(info.size.width, info.size.height, info.size.depth,
                                             static_cast<u32>(info.resources.levels));
        const u32 width = resolution.ScaleUp(info.size.width);
        const u32 height =
            info.type == ImageType::e2D ? resolution.ScaleUp(info.size.height) : info.size.height;
        if (HostMipLevelCount(width, height, info.size.depth, levels) < levels)
            return false;
    }
    if (True(image.flags & (ImageFlagBits::Rescaled | ImageFlagBits::CheckingRescalable))) {
        return true;
    }
    if (True(image.flags & ImageFlagBits::IsRescalable)) {
        return true;
    }
    image.flags |= ImageFlagBits::CheckingRescalable;
    for (const auto& alias : image.aliased_images) {
        Image& other_image = slot_images[alias.id];
        if (!ImageCanRescale(other_image)) {
            image.flags &= ~ImageFlagBits::CheckingRescalable;
            return false;
        }
    }
    image.flags &= ~ImageFlagBits::CheckingRescalable;
    image.flags |= ImageFlagBits::IsRescalable;
    return true;
}

template <class P>
void TextureCache<P>::InvalidateScale(Image& image) {
    // Its views are deleted: a fast draw delta draw keeping one must not follow.
    VideoCommon::Nx2FastDraw::Break(VideoCommon::Nx2FastDraw::Reason::Invalidation);
    ++scale_epoch;
    if (image.scale_tick <= frame_tick) {
        image.scale_tick = frame_tick + 1;
    }
    const std::span<const ImageViewId> image_view_ids = image.image_view_ids;
    auto& dirty = maxwell3d->dirty.flags;
    dirty[Dirty::RenderTargets] = true;
    dirty[Dirty::ZetaBuffer] = true;
    for (size_t rt = 0; rt < NUM_RT; ++rt) {
        dirty[Dirty::ColorBuffer0 + rt] = true;
    }
    for (const ImageViewId image_view_id : image_view_ids) {
        std::ranges::replace(render_targets.color_buffer_ids, image_view_id, ImageViewId{});
        if (render_targets.depth_buffer_id == image_view_id) {
            render_targets.depth_buffer_id = ImageViewId{};
        }
    }
    RemoveImageViewReferences(image_view_ids);
    RemoveFramebuffers(image_view_ids);
    for (const ImageViewId image_view_id : image_view_ids) {
        sentenced_image_view.Push(std::move(slot_image_views[image_view_id]));
        slot_image_views.erase(image_view_id);
    }
    image.image_view_ids.clear();
    image.image_view_infos.clear();
    for (size_t c : active_channel_ids) {
        auto& channel_info = channel_storage[c];

#ifndef NDEBUG
        if constexpr (ENABLE_VALIDATION) {
            std::ranges::fill(channel_info.graphics_image_table.LiveBoundIds(), CORRUPT_ID);
            std::ranges::fill(channel_info.compute_image_table.LiveBoundIds(), CORRUPT_ID);
        }
#endif
        channel_info.graphics_image_table.Invalidate();
        channel_info.compute_image_table.Invalidate();
    }
    ++churn.invalidations;
    has_deleted_images = true;
}

template <class P>
u64 TextureCache<P>::GetScaledImageSizeBytes(const ImageBase& image) {
    const u64 scale_up = static_cast<u64>(Settings::values.resolution_info.up_scale *
                                          Settings::values.resolution_info.up_scale);
    const u64 down_shift = static_cast<u64>(Settings::values.resolution_info.down_shift +
                                            Settings::values.resolution_info.down_shift);
    const u64 image_size_bytes =
        static_cast<u64>((std::max)(image.guest_size_bytes, image.unswizzled_size_bytes));
    const u64 tentative_size = (image_size_bytes * scale_up) >> down_shift;
    const u64 fitted_size = Common::AlignUp(tentative_size, 1024);
    return fitted_size;
}

template <class P>
u64 TextureCache<P>::ImageMemoryBytes(const Image& image) const {
    if (True(image.flags & ImageFlagBits::Unbacked)) {
        // No device memory but what BindLayers gave a SparseLayers image: at its full size (GBs
        // for title 1's sparse terrain images), deleting it would look to the collector like
        // freeing that much.
        if constexpr (requires { image.SparseBoundBytes(); }) {
            return image.SparseBoundBytes();
        }
        return 0;
    }
    u64 tentative_size = (std::max)(image.guest_size_bytes, image.unswizzled_size_bytes);
    if ((IsPixelFormatASTC(image.info.format) &&
         True(image.flags & ImageFlagBits::AcceleratedUpload)) ||
        True(image.flags & ImageFlagBits::Converted)) {
        tentative_size = TranscodedAstcSize(tentative_size, image.info.format);
    }
    return Common::AlignUp(tentative_size, 1024);
}

template <class P>
u64 TextureCache<P>::ImageAllocationBytes(const Image& image) const {
    // ImageMemoryBytes counts ASTC kept as RGBA8 (uneven block mips on AMD) at its BC size. Where
    // usage comes from the driver, the difference would count as memory that isn't images.
    if constexpr (requires { image.MemoryBlock(); }) {
        if (const u64 bytes = image.MemoryBlock().size; bytes != 0) {
            return bytes;
        }
    }
    return ImageMemoryBytes(image);
}

template <class P>
void TextureCache<P>::QueueAsyncDecode(Image& image, ImageId image_id) {
    UNIMPLEMENTED_IF(False(image.flags & ImageFlagBits::Converted));
    LOG_DEBUG(HW_GPU, "Queuing async texture decode {:#x}", image.gpu_addr);

    image.flags |= ImageFlagBits::IsDecoding;
    ++async_decodes_queued;
    auto decode = std::make_unique<AsyncDecodeContext>();
    auto* decode_ptr = decode.get();
    decode->image_id = image_id;
    async_decodes.push_back(std::move(decode));

    std::vector<u8> local_unswizzle_data_buffer(image.unswizzled_size_bytes, 0);
    Tegra::Memory::GpuGuestMemory<u8, Tegra::Memory::GuestMemoryFlags::UnsafeRead> swizzle_data(*gpu_memory, image.gpu_addr, image.guest_size_bytes, &swizzle_data_buffer);
    auto copies = UnswizzleImage(*gpu_memory, image.gpu_addr, image.info, swizzle_data, local_unswizzle_data_buffer);
    const size_t out_size = MapSizeBytes(image);
    if (False(image.flags & ImageFlagBits::ZeroFilled)) {
        // Until the decode lands (a second or more while title 1 streams ~40 textures a second)
        // a new image showed whatever its memory last held, often other textures. Zero blocks
        // read as opaque black (BC1) or transparent (BC3) instead.
        image.flags |= ImageFlagBits::ZeroFilled;
        auto zero_copies = copies;
        ConvertedLayout(image.info, zero_copies);
        auto staging = runtime.UploadStagingBuffer(out_size);
        std::memset(staging.mapped_span.data(), 0, out_size);
        image.UploadMemory(staging, zero_copies);
        runtime.InsertUploadMemoryBarrier();
    }

    auto func = [out_size, copies, info = image.info,
                 input = std::move(local_unswizzle_data_buffer),
                 async_decode = decode_ptr]() mutable {
        async_decode->decoded_data.resize_destructive(out_size);
        std::span copies_span{copies.data(), copies.size()};
        ConvertImage(input, info, async_decode->decoded_data, copies_span);

        // TODO: Do we need this lock?
        std::unique_lock lock{async_decode->mutex};
        async_decode->copies = std::move(copies);
        async_decode->complete = true;
    };
    texture_decode_worker.QueueWork(std::move(func));
}

template <class P>
void TextureCache<P>::QueueAsyncUnswizzle(Image& image, ImageId image_id) {
    if (True(image.flags & ImageFlagBits::IsDecoding)) {
        return;
    }

    image.flags |= ImageFlagBits::IsDecoding;

    unswizzle_queue.push_back({
        .image_id = image_id,
        .info = image.info
    });
}

template <class P>
void TextureCache<P>::TickAsyncDecode() {
    VideoCommon::Nx2Profile::Scope profile_scope{VideoCommon::Nx2Profile::Zone::TextureCache};
    bool has_uploads{};
    auto i = async_decodes.begin();
    while (i != async_decodes.end()) {
        auto* async_decode = i->get();
        {
            // Release the lock before the context is erased below: unlocking it afterwards wrote
            // into freed memory (heap corruption, crashes minutes into title 1).
            std::unique_lock lock{async_decode->mutex};
            if (!async_decode->complete) {
                ++i;
                continue;
            }
        }
        if (async_decode->cancelled) {
            i = async_decodes.erase(i);
            continue;
        }
        Image& image = slot_images[async_decode->image_id];
        if (async_decode->decoded_data.size() > MapSizeBytes(image)) {
            LOG_ERROR(HW_GPU, "Async decode of {} bytes for a {}-byte image at {:#x}, dropped",
                      async_decode->decoded_data.size(), MapSizeBytes(image), image.gpu_addr);
            i = async_decodes.erase(i);
            continue;
        }
        auto staging = runtime.UploadStagingBuffer(MapSizeBytes(image));
        std::memcpy(staging.mapped_span.data(), async_decode->decoded_data.data(),
                    async_decode->decoded_data.size());
        image.UploadMemory(staging, async_decode->copies);
        image.flags &= ~ImageFlagBits::IsDecoding;
        has_uploads = true;
        i = async_decodes.erase(i);
    }
    if (has_uploads) {
        runtime.InsertUploadMemoryBarrier();
    }
}

template <class P>
void TextureCache<P>::TickAsyncUnswizzle() {
    if (unswizzle_queue.empty()) {
        return;
    }

    if(current_unswizzle_frame > 0) {
        current_unswizzle_frame--;
        return;
    }

    PendingUnswizzle& task = unswizzle_queue.front();
    Image& image = slot_images[task.image_id];

    if (!task.initialized) {
        task.total_size = MapSizeBytes(image);
        task.staging_buffer = runtime.UploadStagingBuffer(task.total_size, true);

        const auto& info = image.info;
        const u32 bytes_per_block = BytesPerBlock(info.format);
        const u32 width_blocks = Common::DivCeil(info.size.width, 4u);
        const u32 height_blocks = Common::DivCeil(info.size.height, 4u);

        const u32 stride = width_blocks * bytes_per_block;
        const u32 aligned_height = height_blocks;
        task.bytes_per_slice = static_cast<size_t>(stride) * aligned_height;
        task.last_submitted_offset = 0;
        task.initialized = true;
    }

    // Read data
    if (task.current_offset < task.total_size) {
        const size_t remaining = task.total_size - task.current_offset;

        size_t copy_amount = (std::min)(swizzle_chunk_size, remaining);

        if (remaining > swizzle_chunk_size) {
            copy_amount = (copy_amount / task.bytes_per_slice) * task.bytes_per_slice;
            if (copy_amount == 0) copy_amount = task.bytes_per_slice;
        }

        gpu_memory->ReadBlock(image.gpu_addr + task.current_offset,
                              task.staging_buffer.mapped_span.data() + task.current_offset,
                              copy_amount);
        task.current_offset += copy_amount;
    }

    const bool is_final_batch = task.current_offset >= task.total_size;
    const size_t bytes_ready = task.current_offset - task.last_submitted_offset;
    const u32 complete_slices = static_cast<u32>(bytes_ready / task.bytes_per_slice);

    if (complete_slices >= swizzle_slices_per_batch || (is_final_batch && complete_slices > 0)) {
        const u32 z_start = static_cast<u32>(task.last_submitted_offset / task.bytes_per_slice);
        const u32 slices_to_process = (std::min)(complete_slices, swizzle_slices_per_batch);
        const u32 z_count = (std::min)(slices_to_process, image.info.size.depth - z_start);

        if (z_count > 0) {
            const auto uploads = FullUploadSwizzles(task.info);
            runtime.AccelerateImageUpload(image, task.staging_buffer, uploads, z_start, z_count);
            task.last_submitted_offset += (static_cast<size_t>(z_count) * task.bytes_per_slice);
        }
    }

    // Check if complete
    const u32 slices_submitted = static_cast<u32>(task.last_submitted_offset / task.bytes_per_slice);
    const bool all_slices_submitted = slices_submitted >= image.info.size.depth;

    if (is_final_batch && all_slices_submitted) {
        runtime.FreeDeferredStagingBuffer(task.staging_buffer);
        image.flags &= ~ImageFlagBits::IsDecoding;
        unswizzle_queue.pop_front();

        // Wait 4 frames to process the next entry
        current_unswizzle_frame = 4u;
    }
}

template <class P>
bool TextureCache<P>::ScaleUp(Image& image) {
    const bool has_copy = image.HasScaled();
    const bool rescaled = image.ScaleUp();
    if (!has_copy && image.HasScaled()) {
        total_used_memory += GetScaledImageSizeBytes(image);
    }
    if (!rescaled) {
        return false;
    }
    InvalidateScale(image);
    return true;
}

template <class P>
bool TextureCache<P>::ScaleDown(Image& image) {
    const bool rescaled = image.ScaleDown();
    if (!rescaled) {
        return false;
    }
    InvalidateScale(image);
    return true;
}

template <class P>
std::optional<DAddr> TextureCache<P>::ImageCpuAddress(GPUVAddr gpu_addr, size_t size) const {
    if (const std::optional<DAddr> cpu_addr = gpu_memory->GpuToCpuAddress(gpu_addr)) {
        return cpu_addr;
    }
    if (const std::optional<DAddr> cpu_addr = gpu_memory->GpuToCpuAddress(gpu_addr, size)) {
        return cpu_addr;
    }
    // No CPU backing (title 1 renders a bloom chain at GPU 0xf0000-0x15b600): a stand-in address
    // fixed by the GPU address, at or above ~(1 << 40) so TrackImage skips it. A fresh address per
    // lookup made every use create another image (thousands a minute, never found again).
    constexpr GPUVAddr StandInRange = 1ULL << 40;
    if (gpu_addr >= StandInRange || size > StandInRange - gpu_addr - 1) {
        return std::nullopt;
    }
    return ~(1ULL << 40ULL) + gpu_addr;
}

template <class P>
ImageId TextureCache<P>::InsertImage(const ImageInfo& info, GPUVAddr gpu_addr,
                                     RelaxedOptions options) {
    const StutterTrace::SiteScope site{StutterTrace::Site::ImageCreate};
    std::optional<DAddr> cpu_addr = ImageCpuAddress(gpu_addr, CalculateGuestSizeInBytes(info));
    if (!cpu_addr) {
        const DAddr fake_addr = ~(1ULL << 40ULL) + virtual_invalid_space;
        virtual_invalid_space += Common::AlignUp(CalculateGuestSizeInBytes(info), 32);
        cpu_addr = std::optional<DAddr>(fake_addr);
    }
    ASSERT_MSG(cpu_addr, "Tried to insert an image to an invalid gpu_addr={:#x}", gpu_addr);
    ImageInfo storage_info = info;
    storage_info.native_tiling = gpu_memory->UsesNativeTiling() && !info.maxwell_gob_layout;
    if (storage_info.native_tiling && info.type == ImageType::e3D) {
        // Switch 2 3D images (lighting/fog volumes) are rare: log each one's shape.
        LOG_DEBUG(HW_GPU, "3D image at {:#x}: {}x{}x{} levels {} format {} block {}x{}x{}",
                 gpu_addr, info.size.width, info.size.height, info.size.depth, info.resources.levels,
                 static_cast<u32>(info.format), info.block.width, info.block.height, info.block.depth);
    }
    const bool tc_trace = TextureCacheTrace();
    const auto tc_t0 = tc_trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const ImageId image_id = JoinImages(storage_info, gpu_addr, *cpu_addr);
    if (!image_id) {
        return image_id;
    }
    const Image& image = slot_images[image_id];
    if (tc_trace) {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tc_t0).count();
        LOG_WARNING(HW_GPU,
                    "TCTRACE create id={} gpu={:#x} type={} fmt={} {}x{}x{} lv={} ly={} guest={}KiB "
                    "joined={} found={} ms={:.2f} frame={}",
                    image_id.index, image.gpu_addr, static_cast<u32>(info.type), static_cast<u32>(info.format),
                    info.size.width, info.size.height, info.size.depth, image.info.resources.levels,
                    image.info.resources.layers, image.guest_size_bytes / 1024, join_overlap_ids.size(),
                    join_overlaps_found.size(), ms, frame_tick);
    }
    // Using "image.gpu_addr" instead of "gpu_addr" is important because it might be different
    const auto [it, is_new] = image_allocs_table.try_emplace(image.gpu_addr);
    if (is_new) {
        it->second = slot_image_allocs.insert();
    }
    slot_image_allocs[it->second].images.push_back(image_id);
    return image_id;
}

template <class P>
ImageId TextureCache<P>::JoinImages(const ImageInfo& info, GPUVAddr gpu_addr, DAddr cpu_addr) {
    VideoCommon::Nx2Profile::Scope profile_scope{VideoCommon::Nx2Profile::Zone::TextureCache};
    ImageInfo new_info = info;
    const size_t size_bytes = CalculateGuestSizeInBytes(new_info);
    const bool broken_views = runtime.HasBrokenTextureViewFormats();
    const bool native_bgr = runtime.HasNativeBgr();
    join_overlap_ids.clear();
    join_overlaps_found.clear();
    join_left_aliased_ids.clear();
    join_right_aliased_ids.clear();
    join_ignore_textures.clear();
    join_bad_overlap_ids.clear();
    join_copies_to_do.clear();
    join_alias_indices.clear();
    const bool this_is_linear = info.type == ImageType::Linear;
    const auto region_check = [&](ImageId overlap_id, ImageBase& overlap) {
        if (True(overlap.flags & ImageFlagBits::Remapped)) {
            join_ignore_textures.insert(overlap_id);
            return;
        }
        const bool overlap_is_linear = overlap.info.type == ImageType::Linear;
        if (this_is_linear != overlap_is_linear) {
            return;
        }
        if (this_is_linear && overlap_is_linear) {
            if (info.pitch == overlap.info.pitch && gpu_addr == overlap.gpu_addr) {
                // Alias linear images with the same pitch
                join_left_aliased_ids.push_back(overlap_id);
            }
            return;
        }
        join_overlaps_found.insert(overlap_id);
        if (new_info.native_tiling != overlap.info.native_tiling) {
            join_bad_overlap_ids.push_back(overlap_id);
            return;
        }
        constexpr bool strict_size = true;
        const std::optional<OverlapResult> solution = ResolveOverlap(
            new_info, gpu_addr, cpu_addr, overlap, strict_size, broken_views, native_bgr);
        if (solution) {
            gpu_addr = solution->gpu_addr;
            cpu_addr = solution->cpu_addr;
            new_info.resources = solution->resources;
            join_overlap_ids.push_back(overlap_id);
            join_copies_to_do.emplace_back(JoinCopy{false, overlap_id});
            return;
        }
        constexpr auto options = RelaxedOptions::Size | RelaxedOptions::Format;
        const ImageBase new_image_base(new_info, gpu_addr, cpu_addr);
        if (IsSubresource(new_info, overlap, gpu_addr, options, broken_views, native_bgr)) {
            join_left_aliased_ids.push_back(overlap_id);
            overlap.flags |= ImageFlagBits::Alias;
            join_copies_to_do.emplace_back(JoinCopy{true, overlap_id});
        } else if (IsSubresource(overlap.info, new_image_base, overlap.gpu_addr, options,
                                 broken_views, native_bgr)) {
            join_right_aliased_ids.push_back(overlap_id);
            overlap.flags |= ImageFlagBits::Alias;
            join_copies_to_do.emplace_back(JoinCopy{true, overlap_id});
        } else {
            join_bad_overlap_ids.push_back(overlap_id);
        }
    };
    ForEachImageInRegion(cpu_addr, size_bytes, region_check);
    const auto region_check_gpu = [&](ImageId overlap_id, ImageBase& overlap) {
        if (!join_overlaps_found.contains(overlap_id)) {
            if (True(overlap.flags & ImageFlagBits::Remapped)) {
                join_ignore_textures.insert(overlap_id);
            }
            if (overlap.gpu_addr == gpu_addr && overlap.guest_size_bytes == size_bytes &&
                new_info.native_tiling == overlap.info.native_tiling) {
                join_ignore_textures.insert(overlap_id);
            }
        }
    };
    ForEachSparseImageInRegion(channel_state->gpu_memory.GetID(), gpu_addr, size_bytes,
                               region_check_gpu);

    bool can_rescale = info.rescaleable;
    bool any_rescaled = false;
    for (const auto& copy : join_copies_to_do) {
        if (!can_rescale) {
            break;
        }
        Image& sibling = slot_images[copy.id];
        can_rescale &= ImageCanRescale(sibling);
        any_rescaled |= True(sibling.flags & ImageFlagBits::Rescaled);
    }

    can_rescale &= any_rescaled;

    if (can_rescale) {
        for (const auto& copy : join_copies_to_do) {
            Image& sibling = slot_images[copy.id];
            ScaleUp(sibling);
        }
    } else {
        for (const auto& copy : join_copies_to_do) {
            Image& sibling = slot_images[copy.id];
            ScaleDown(sibling);
        }
    }

    if constexpr (requires { runtime.UseOpaqueAstcFormat(); }) {
        // Opaque ASTC goes to BC1 rather than BC3 when it is re-compressed (the Vulkan runtime
        // decides). The guest data is already there when an image is first used.
        constexpr size_t MaxScanBytes = 64_MiB;
        if (IsPixelFormatASTC(new_info.format) && runtime.UseOpaqueAstcFormat()) {
            const size_t scan_bytes = CalculateGuestSizeInBytes(new_info);
            if (scan_bytes <= MaxScanBytes) {
                // Scanned where it is when it is in one piece; only copied (into the scratch
                // buffer) when the range is split.
                Tegra::Memory::GpuGuestMemory<u8, Tegra::Memory::GuestMemoryFlags::UnsafeRead>
                    scan_data(*gpu_memory, gpu_addr, scan_bytes, &astc_scan_buffer);
                new_info.astc_opaque =
                    AstcDataLooksOpaque(std::span<const u8>{scan_data.data(), scan_data.size()});
                ++(new_info.astc_opaque ? astc_opaque_images : astc_alpha_images);
            }
        }
    }
    const ImageId new_image_id = slot_images.insert(runtime, new_info, gpu_addr, cpu_addr);
    slot_images[new_image_id].load_tick = modification_tick;
    if constexpr (requires(const Image& image) { image.OutOfMemory(); }) {
        if (slot_images[new_image_id].OutOfMemory()) {
            // Left out of the cache: lookups see no image (a null view) and make it again once
            // the collector has freed memory.
            slot_images.erase(new_image_id);
            static u32 logged{};
            if (logged++ < 32) {
                LOG_ERROR(HW_GPU, "No memory for the {}x{}x{} image at {:#x} (format {}); skipping it",
                          new_info.size.width, new_info.size.height, new_info.size.depth, gpu_addr,
                          static_cast<u32>(new_info.format));
            }
            return ImageId{};
        }
    }
    Image& new_image = slot_images[new_image_id];

    new_image.allocation_tick = frame_tick;
    last_created_image = new_image_id;
    {
        // Which shapes are being created (periodic log): format, type, levels, layers, size.
        const u64 key = (u64{static_cast<u32>(new_info.format)} << 56) |
                        (u64{static_cast<u32>(new_info.type)} << 52) |
                        (u64{static_cast<u32>(new_info.resources.levels) & 0xF} << 48) |
                        (u64{static_cast<u32>(new_info.resources.layers) & 0xFFF} << 36) |
                        (u64{new_info.size.width & 0x3FFFF} << 18) | (new_info.size.height & 0x3FFFF);
        if (debug_stats) {
            auto& shape = created_shapes[key];
            ++shape.first;
            shape.second = gpu_addr;
        }
        ++images_created;
        ++churn.created;
        churn.joins += join_overlap_ids.empty() ? 0 : 1;
    }

    if (!gpu_memory->IsContinuousRange(new_image.gpu_addr, new_image.guest_size_bytes)) {
        new_image.flags |= ImageFlagBits::NonContiguous;
        if (new_info.is_sparse) {
            new_image.flags |= ImageFlagBits::Sparse;
        }
    }

    for (const ImageId overlap_id : join_ignore_textures) {
        Image& overlap = slot_images[overlap_id];
        // Remapped images no longer describe the current guest memory. Discard their GPU
        // contents rather than downloading them through the replacement mapping.
        if (True(overlap.flags & ImageFlagBits::GpuModified) &&
            False(overlap.flags & ImageFlagBits::Remapped)) {
            UNIMPLEMENTED();
        }
        if (True(overlap.flags & ImageFlagBits::Tracked)) {
            UntrackImage(overlap, overlap_id);
        }
        UnregisterImage(overlap_id);
        trace_delete_reason = "join-ignored";
        DeleteImage(overlap_id);
    }

    // A new render target that the clear binding it overwrites whole (every channel, the whole
    // image) needs no upload from guest memory first: Ocarina of Time creates ~4 such targets a
    // frame outdoors, and uploading them was ~15% of the GPU back thread.
    const auto overwritten_by_clear = [&] {
        return clearing_render_targets && join_copies_to_do.empty() &&
               ClearWritesWhole(new_info, gpu_addr);
    };
    std::ranges::sort(join_copies_to_do, [this](const JoinCopy& lhs, const JoinCopy& rhs) {
        const ImageBase& lhs_image = slot_images[lhs.id];
        const ImageBase& rhs_image = slot_images[rhs.id];
        return lhs_image.modification_tick < rhs_image.modification_tick;
    });

    ImageBase& new_image_base = new_image;
    for (const ImageId aliased_id : join_right_aliased_ids) {
        ImageBase& aliased = slot_images[aliased_id];
        size_t alias_index = new_image_base.aliased_images.size();
        if (!AddImageAlias(new_image_base, aliased, new_image_id, aliased_id)) {
            continue;
        }
        join_alias_indices.emplace(aliased_id, alias_index);
        new_image.flags |= ImageFlagBits::Alias;
    }
    for (const ImageId aliased_id : join_left_aliased_ids) {
        ImageBase& aliased = slot_images[aliased_id];
        size_t alias_index = new_image_base.aliased_images.size();
        if (!AddImageAlias(aliased, new_image_base, aliased_id, new_image_id)) {
            continue;
        }
        join_alias_indices.emplace(aliased_id, alias_index);
        new_image.flags |= ImageFlagBits::Alias;
    }
    for (const ImageId aliased_id : join_bad_overlap_ids) {
        ImageBase& aliased = slot_images[aliased_id];
        aliased.overlapping_images.push_back(new_image_id);
        new_image.overlapping_images.push_back(aliased_id);
        if (aliased.info.resources.levels == 1 && aliased.info.block.depth == 0 &&
            aliased.overlapping_images.size() > 1) {
            aliased.flags |= ImageFlagBits::BadOverlap;
        }
        if (new_image.info.resources.levels == 1 && new_image.info.block.depth == 0 &&
            new_image.overlapping_images.size() > 1) {
            new_image.flags |= ImageFlagBits::BadOverlap;
        }
    }

    // The copy from an alias into the new image, or null when the loop below skips it.
    const auto alias_copy = [&](const JoinCopy& copy) -> const AliasedImage* {
        if (!copy.is_alias) {
            return nullptr;
        }
        const Image& overlap = slot_images[copy.id];
        if (!overlap.IsSafeGpuCopy() || overlap.info.num_samples != new_image.info.num_samples) {
            return nullptr;
        }
        const auto alias_pointer = join_alias_indices.find(copy.id);
        if (alias_pointer == join_alias_indices.end()) {
            return nullptr;
        }
        return &new_image.aliased_images[alias_pointer->second];
    };
    // Copies run oldest first. When one of them writes the whole image, the upload from guest
    // memory and the alias copies before it would be overwritten whole: neither is made. Ocarina
    // of Time resizes a shadow map every frame in parts of Kokiri Forest, at the same address:
    // each size is a new depth target and a new texture, both copied from older sizes, and their
    // uploads took up to 17% of the GPU back thread there. Only at 1x: a rescaled copy's extent is
    // the scaled guest mip size, which can fall short of the host mip (a 5 px wide image at 2x:
    // host mip 1 is 5 px, the copy 4).
    size_t first_copy = 0;
    bool copied_whole = false;
    for (size_t index = join_copies_to_do.size(); !can_rescale && index-- > 0;) {
        const AliasedImage* const aliased = alias_copy(join_copies_to_do[index]);
        if (aliased != nullptr &&
            CopiesWriteWholeImage(new_image, slot_images[aliased->id], aliased->copies)) {
            first_copy = index;
            copied_whole = true;
            break;
        }
    }
    // The copies from overlaps the GPU wrote can each write a part of it, together all of it:
    // Mario Kart World renders each layer of its shadow map and colour arrays as a target of its
    // own, then samples the array, and the upload under those copies took 13-31 ms an array.
    if (!can_rescale && !copied_whole && new_info.num_samples == 1) {
        std::vector<u8> written(
            static_cast<size_t>(new_info.resources.levels * new_info.resources.layers));
        for (const JoinCopy& copy : join_copies_to_do) {
            const Image& overlap = slot_images[copy.id];
            if (copy.is_alias || False(overlap.flags & ImageFlagBits::GpuModified) ||
                overlap.info.num_samples != 1) {
                continue;
            }
            const SubresourceBase base = new_image.TryFindBase(overlap.gpu_addr).value();
            MarkSubresourcesWrittenWhole(new_image, overlap,
                                         MakeShrinkImageCopies(new_info, overlap.info, base, 1, 0),
                                         written);
        }
        copied_whole = std::ranges::all_of(written, [](u8 whole) { return whole != 0; });
    }
    if (overwritten_by_clear()) {
        // Stays CpuModified: UpdateRenderTargets' PrepareImage drops the upload only when
        // IsFullClear agrees the clear covers the whole image, and uploads it otherwise.
        runtime.TransitionImageLayout(new_image);
    } else if (copied_whole) {
        // What RefreshContents does, without the upload.
        new_image.flags &= ~ImageFlagBits::CpuModified;
        if (False(new_image.flags & ImageFlagBits::Tracked)) {
            TrackImage(new_image, new_image_id);
        }
        runtime.TransitionImageLayout(new_image);
    } else {
        if (True(new_image.flags & ImageFlagBits::CpuModified)) {
            WriteBackBadOverlaps(new_image_id);
        }
        // TODO: Only upload what we need
        RefreshContents(new_image, new_image_id);
    }

    if (can_rescale) {
        ScaleUp(new_image);
    } else {
        ScaleDown(new_image);
    }

    for (size_t index = 0; index < join_copies_to_do.size(); ++index) {
        const JoinCopy& copy_object = join_copies_to_do[index];
        Image& overlap = slot_images[copy_object.id];
        if (copy_object.is_alias) {
            const AliasedImage* const aliased =
                index >= first_copy ? alias_copy(copy_object) : nullptr;
            if (aliased == nullptr) {
                continue;
            }
            CopyImage(new_image_id, aliased->id, aliased->copies);
            SetModificationTick(slot_images, new_image, overlap.modification_tick);
            continue;
        }
        if (True(overlap.flags & ImageFlagBits::GpuModified)) {
            new_image.flags |= ImageFlagBits::GpuModified;
            const auto& resolution = Settings::values.resolution_info;
            const SubresourceBase base = new_image.TryFindBase(overlap.gpu_addr).value();
            const u32 up_scale = can_rescale ? resolution.up_scale : 1;
            const u32 down_shift = can_rescale ? resolution.down_shift : 0;
            auto copies = MakeShrinkImageCopies(new_info, overlap.info, base, up_scale, down_shift);
            if (overlap.info.num_samples != new_image.info.num_samples) {
                runtime.CopyImageMSAA(new_image, overlap, copies);
            } else {
                runtime.CopyImage(new_image, overlap, copies);
            }
            SetModificationTick(slot_images, new_image, overlap.modification_tick);
        }
        if (True(overlap.flags & ImageFlagBits::Tracked)) {
            UntrackImage(overlap, copy_object.id);
        }
        UnregisterImage(copy_object.id);
        trace_delete_reason = "join";
        DeleteImage(copy_object.id);
    }

    RegisterImage(new_image_id);
    return new_image_id;
}

template <class P>
std::optional<typename TextureCache<P>::BlitImages> TextureCache<P>::GetBlitImages(
    const Tegra::Engines::Fermi2D::Surface& dst, const Tegra::Engines::Fermi2D::Surface& src,
    const Tegra::Engines::Fermi2D::Config& copy) {

    constexpr auto FIND_OPTIONS = RelaxedOptions::Samples;
    const GPUVAddr dst_addr = dst.Address();
    const GPUVAddr src_addr = src.Address();
    ImageInfo dst_info(dst);
    ImageInfo src_info(src);
    const bool can_be_depth_blit =
        dst_info.format == src_info.format && copy.filter == Tegra::Engines::Fermi2D::Filter::Point;
    ImageId dst_id;
    ImageId src_id;
    RelaxedOptions try_options = FIND_OPTIONS;
    if (can_be_depth_blit) {
        try_options |= RelaxedOptions::Format;
    }
    do {
        has_deleted_images = false;
        src_id = FindImage(src_info, src_addr, try_options);
        dst_id = FindImage(dst_info, dst_addr, try_options);
        if (!copy.must_accelerate) {
            do {
                if (!src_id && !dst_id) {
                    return std::nullopt;
                }
                if (src_id && True(slot_images[src_id].flags & ImageFlagBits::GpuModified)) {
                    break;
                }
                if (dst_id && True(slot_images[dst_id].flags & ImageFlagBits::GpuModified)) {
                    break;
                }
                return std::nullopt;
            } while (false);
        }
        const ImageBase* const src_image = src_id ? &slot_images[src_id] : nullptr;
        if (src_image && src_image->info.num_samples > 1) {
            RelaxedOptions find_options{FIND_OPTIONS | RelaxedOptions::ForceBrokenViews};
            src_id = FindOrInsertImage(src_info, src_addr, find_options);
            dst_id = FindOrInsertImage(dst_info, dst_addr, find_options);
            if (has_deleted_images) {
                continue;
            }
            break;
        }
        if (can_be_depth_blit) {
            const ImageBase* const dst_image = dst_id ? &slot_images[dst_id] : nullptr;
            DeduceBlitImages(dst_info, src_info, dst_image, src_image);
            if (GetFormatType(dst_info.format) != GetFormatType(src_info.format)) {
                continue;
            }
        }
        if (!src_id) {
            src_id = InsertImage(src_info, src_addr, RelaxedOptions{});
        }
        if (!dst_id) {
            dst_id = InsertImage(dst_info, dst_addr, RelaxedOptions{});
        }
    } while (has_deleted_images);
    if (!src_id || !dst_id) {
        return std::nullopt;
    }
    const ImageBase& src_image = slot_images[src_id];
    const ImageBase& dst_image = slot_images[dst_id];
    const bool native_bgr = runtime.HasNativeBgr();
    if (GetFormatType(dst_info.format) != GetFormatType(dst_image.info.format) ||
        GetFormatType(src_info.format) != GetFormatType(src_image.info.format) ||
        !VideoCore::Surface::IsViewCompatible(dst_info.format, dst_image.info.format, false,
                                              native_bgr) ||
        !VideoCore::Surface::IsViewCompatible(src_info.format, src_image.info.format, false,
                                              native_bgr)) {
        // Make sure the images match the expected format.
        do {
            has_deleted_images = false;
            src_id = FindOrInsertImage(src_info, src_addr, RelaxedOptions{});
            dst_id = FindOrInsertImage(dst_info, dst_addr, RelaxedOptions{});
        } while (has_deleted_images);
        if (!src_id || !dst_id) {
            return std::nullopt;
        }
    }
    return {BlitImages{
        .dst_id = dst_id,
        .src_id = src_id,
        .dst_format = dst_info.format,
        .src_format = src_info.format,
    }};
}

template <class P>
ImageId TextureCache<P>::FindDMAImage(const ImageInfo& info, GPUVAddr gpu_addr) {
    const std::optional<DAddr> cpu_addr = ImageCpuAddress(gpu_addr, CalculateGuestSizeInBytes(info));
    if (!cpu_addr) {
        return ImageId{};
    }
    ImageId image_id{};
    boost::container::small_vector<ImageId, 8> image_ids;
    const auto lambda = [&](ImageId existing_image_id, ImageBase& existing_image) {
        if (True(existing_image.flags & ImageFlagBits::Remapped)) {
            return false;
        }
        if (info.type == ImageType::Linear || existing_image.info.type == ImageType::Linear)
            [[unlikely]] {
            const bool strict_size = True(existing_image.flags & ImageFlagBits::Strong);
            const ImageInfo& existing = existing_image.info;
            if (existing_image.gpu_addr == gpu_addr && existing.type == info.type &&
                existing.pitch == info.pitch &&
                IsPitchLinearSameSize(existing, info, strict_size) &&
                IsViewCompatible(existing.format, info.format, false, true)) {
                image_id = existing_image_id;
                image_ids.push_back(existing_image_id);
                return true;
            }
        } else if (IsSubCopy(info, existing_image, gpu_addr)) {
            if (True(existing_image.flags & ImageFlagBits::Unbacked)) {
                static u32 logged{};
                if (logged++ < 64) {
                    LOG_INFO(HW_GPU, "DMA copy at {:#x} ({}x{}) into unbacked image {:#x}", gpu_addr,
                             info.size.width, info.size.height, existing_image.gpu_addr);
                }
            }
            image_id = existing_image_id;
            image_ids.push_back(existing_image_id);
            return true;
        }
        return false;
    };
    ForEachImageInRegion(*cpu_addr, CalculateGuestSizeInBytes(info), lambda);
    if (image_ids.size() <= 1) [[likely]] {
        return image_id;
    }
    auto image_ids_compare = [this](ImageId a, ImageId b) {
        auto& image_a = slot_images[a];
        auto& image_b = slot_images[b];
        return image_a.modification_tick < image_b.modification_tick;
    };
    return *std::ranges::max_element(image_ids, image_ids_compare);
}

template <class P>
std::pair<u32, u32> TextureCache<P>::PrepareDmaImage(ImageId dst_id, GPUVAddr base_addr,
                                                     bool mark_as_modified) {
    // As in BlitImage: land a parked deferred clear before the DMA write, but only when the
    // write happens (mark_as_modified); a read-only access writes nothing.
    if (mark_as_modified) {
        runtime.FlushDeferredClear();
    }
    const auto& image = slot_images[dst_id];
    const auto base = image.TryFindBase(base_addr);
    PrepareImage(dst_id, mark_as_modified, false);
    const auto& new_image = slot_images[dst_id];
    lru_cache.Touch(new_image.lru_index, frame_tick);
    return std::make_pair(base->level, base->layer);
}

template <class P>
SamplerId TextureCache<P>::FindSampler(const TSCEntry& config, bool compute) {
    if (std::ranges::all_of(config.raw, [](u64 value) { return value == 0; })) {
        return NULL_SAMPLER_ID;
    }
    const auto [pair, is_new] = channel_state->samplers.try_emplace(config);
    if (is_new) {
        pair->second = slot_samplers.insert(runtime, config);
    }
    return pair->second;
}

template <class P>
ImageViewId TextureCache<P>::FindColorBuffer(size_t index) {
    const auto& regs = maxwell3d->regs;
    if (index >= regs.rt_control.count) {
        return ImageViewId{};
    }
    const auto& rt = regs.rt[index];
    const GPUVAddr gpu_addr = rt.Address();
    if (gpu_addr == 0) {
        return ImageViewId{};
    }
    if (rt.format == Tegra::RenderTargetFormat::NONE) {
        return ImageViewId{};
    }
    const ImageInfo info(regs.rt[index], regs.anti_alias_samples_mode);
    return FindRenderTargetView(info, gpu_addr);
}

template <class P>
ImageViewId TextureCache<P>::FindDepthBuffer() {
    const auto& regs = maxwell3d->regs;
    depth_target_padded = false;
    if (!regs.zeta_enable) {
        return ImageViewId{};
    }
    const GPUVAddr gpu_addr = regs.zeta.Address();
    if (gpu_addr == 0) {
        return ImageViewId{};
    }
    ImageInfo info(regs.zeta, regs.zeta_size, regs.anti_alias_samples_mode);
    depth_target_padded = IsPaddedSquare(info);
    // Ocarina of Time draws its shadow maps into depth targets padded to whole GOBs (800x793) and
    // samples them as the square texture they are (793x793). The padding is not part of the
    // image: the surface clip bounds rendering, so a clip inside the square never reaches it.
    // One image then serves both, instead of two with a copy for each new size.
    const u32 clip_right = regs.surface_clip.x.Value() + regs.surface_clip.width.Value();
    if (depth_target_padded && clip_right <= info.size.height) {
        info.size.width = info.size.height;
    }
    return FindRenderTargetView(info, gpu_addr);
}

template <class P>
ImageViewId TextureCache<P>::FindRenderTargetView(const ImageInfo& info, GPUVAddr gpu_addr) {
    static constexpr u32 MaxRenderTargetSize = 32768;
    static constexpr s32 MaxRenderTargetLayers = 2048;
    if (info.size.width == 0 || info.size.height == 0 || info.size.width > MaxRenderTargetSize ||
        info.size.height > MaxRenderTargetSize || info.resources.layers > MaxRenderTargetLayers) {
        LOG_ERROR(HW_GPU, "Ignoring {}x{} render target with {} layers at {:#x}", info.size.width,
                  info.size.height, info.resources.layers, gpu_addr);
        return ImageViewId{};
    }
    ImageId image_id{};
    bool delete_state = has_deleted_images;
    last_created_image = ImageId{};
    do {
        has_deleted_images = false;
        image_id = FindOrInsertImage(info, gpu_addr);
        delete_state |= has_deleted_images;
    } while (has_deleted_images);
    has_deleted_images = delete_state;
    if (!image_id) {
        // No attachment rather than the null view: it has no render target view, and a
        // framebuffer made with it hands the driver a null handle (AMD: access violation).
        return ImageViewId{};
    }
    Image& image = slot_images[image_id];
    // A render target this lookup just created, on a Switch 2 title, is a candidate for early
    // deletion once it goes unused (EvictDeadRenderTargets). Its upload is unchanged.
    if (Tegra::g_nx2_ampere.load(std::memory_order_relaxed) && image_id == last_created_image &&
        image.modification_tick == 0 && False(image.flags & ImageFlagBits::TransientTarget) &&
        False(image.flags & (ImageFlagBits::Sparse | ImageFlagBits::SparseLayers |
                             ImageFlagBits::SparseTiles | ImageFlagBits::Unbacked))) {
        // A hard limit, also while over budget: the oldest candidate leaves the queue and stays
        // like any other image (its flag is dropped, so it is never deleted early).
        while (transient_targets.size() >= MaxTransientTargets) {
            const TransientTarget oldest = transient_targets.front();
            transient_targets.pop_front();
            if (slot_images.contains(oldest.id)) {
                Image& old_image = slot_images[oldest.id];
                if (old_image.allocation_tick == oldest.allocation_tick) {
                    old_image.flags &= ~ImageFlagBits::TransientTarget;
                }
            }
        }
        image.flags |= ImageFlagBits::TransientTarget;
        transient_targets.push_back({image_id, image.allocation_tick, frame_tick, 0});
    }
    const ImageViewType view_type = RenderTargetImageViewType(info);
    SubresourceBase base;
    if (image.info.type == ImageType::Linear) {
        base = SubresourceBase{.level = 0, .layer = 0};
    } else {
        base = image.TryFindBase(gpu_addr).value();
    }
    const s32 layers = image.info.type == ImageType::e3D ? info.size.depth : info.resources.layers;
    if (RenderTrace::SampleFrame(frame_tick)) {
        LOG_DEBUG(HW_GPU,
                  "Render trace target match: docked={} requested={:#x} {}x{} layers={} format={} "
                  "stride={} block={}/{}/{} selected={:#x} {}x{} layers={} format={} stride={} "
                  "block={}/{}/{} base_level={} base_layer={} aliases={} overlaps={}",
                  Settings::IsDockedMode(), gpu_addr, info.size.width, info.size.height, layers,
                  info.format, info.layer_stride, info.block.width, info.block.height,
                  info.block.depth, image.gpu_addr, image.info.size.width, image.info.size.height,
                  image.info.resources.layers, image.info.format, image.info.layer_stride,
                  image.info.block.width, image.info.block.height, image.info.block.depth,
                  base.level, base.layer, image.aliased_images.size(), image.overlapping_images.size());
    }
    if (True(image.flags & ImageFlagBits::Unbacked)) {
        static u32 logged{};
        if (logged++ < 64) {
            LOG_INFO(HW_GPU, "Render target in unbacked image {:#x} at {:#x}: {}x{} level {} layer {}",
                     image.gpu_addr, gpu_addr, info.size.width, info.size.height, base.level, base.layer);
        }
    }
    const SubresourceRange range{
        .base = base,
        .extent = {.levels = 1, .layers = layers},
    };
    return FindOrEmplaceImageView(image_id, ImageViewInfo(view_type, info.format, range));
}

template <class P>
template <typename Func>
void TextureCache<P>::ForEachImageInRegion(DAddr cpu_addr, size_t size, Func&& func) {
    using FuncReturn = typename std::invoke_result<Func, ImageId, Image&>::type;
    constexpr bool BOOL_BREAK = std::is_same_v<FuncReturn, bool>;
    boost::container::small_vector<ImageId, 32> images;
    boost::container::small_vector<ImageMapId, 32> maps;
    ForEachCPUPage(cpu_addr, size, [this, &images, &maps, cpu_addr, size, func](u64 page) {
        const auto it = page_table.find(page);
        if (it == page_table.end()) {
            if constexpr (BOOL_BREAK) {
                return false;
            } else {
                return;
            }
        }
        for (const ImageMapId map_id : it->second) {
            ImageMapView& map = slot_map_views[map_id];
            if (map.picked) {
                continue;
            }
            if (!map.Overlaps(cpu_addr, size)) {
                continue;
            }
            map.picked = true;
            maps.push_back(map_id);
            Image& image = slot_images[map.image_id];
            if (True(image.flags & ImageFlagBits::Picked)) {
                continue;
            }
            image.flags |= ImageFlagBits::Picked;
            images.push_back(map.image_id);
            if constexpr (BOOL_BREAK) {
                if (func(map.image_id, image)) {
                    return true;
                }
            } else {
                func(map.image_id, image);
            }
        }
        if constexpr (BOOL_BREAK) {
            return false;
        }
    });
    for (const ImageId image_id : images) {
        slot_images[image_id].flags &= ~ImageFlagBits::Picked;
    }
    for (const ImageMapId map_id : maps) {
        slot_map_views[map_id].picked = false;
    }
}

template <class P>
template <typename Func>
void TextureCache<P>::ForEachImageInRegionGPU(size_t as_id, GPUVAddr gpu_addr, size_t size,
                                              Func&& func) {
    using FuncReturn = typename std::invoke_result<Func, ImageId, Image&>::type;
    constexpr bool BOOL_BREAK = std::is_same_v<FuncReturn, bool>;
    boost::container::small_vector<ImageId, 8> images;
    auto storage_id = getStorageID(as_id);
    if (!storage_id) {
        return;
    }
    auto& gpu_page_table = gpu_page_table_storage[*storage_id * 2];
    ForEachGPUPage(gpu_addr, size,
                   [this, &gpu_page_table, &images, gpu_addr, size, func](u64 page) {
                       const auto it = gpu_page_table.find(page);
                       if (it == gpu_page_table.end()) {
                           if constexpr (BOOL_BREAK) {
                               return false;
                           } else {
                               return;
                           }
                       }
                       for (const ImageId image_id : it->second) {
                           Image& image = slot_images[image_id];
                           if (True(image.flags & ImageFlagBits::Picked)) {
                               continue;
                           }
                           if (!image.OverlapsGPU(gpu_addr, size)) {
                               continue;
                           }
                           image.flags |= ImageFlagBits::Picked;
                           images.push_back(image_id);
                           if constexpr (BOOL_BREAK) {
                               if (func(image_id, image)) {
                                   return true;
                               }
                           } else {
                               func(image_id, image);
                           }
                       }
                       if constexpr (BOOL_BREAK) {
                           return false;
                       }
                   });
    for (const ImageId image_id : images) {
        slot_images[image_id].flags &= ~ImageFlagBits::Picked;
    }
}

template <class P>
template <typename Func>
void TextureCache<P>::ForEachSparseImageInRegion(size_t as_id, GPUVAddr gpu_addr, size_t size,
                                                 Func&& func) {
    using FuncReturn = typename std::invoke_result<Func, ImageId, Image&>::type;
    constexpr bool BOOL_BREAK = std::is_same_v<FuncReturn, bool>;
    boost::container::small_vector<ImageId, 8> images;
    auto storage_id = getStorageID(as_id);
    if (!storage_id) {
        return;
    }
    auto& sparse_page_table = gpu_page_table_storage[*storage_id * 2 + 1];
    ForEachGPUPage(gpu_addr, size,
                   [this, &sparse_page_table, &images, gpu_addr, size, func](u64 page) {
                       const auto it = sparse_page_table.find(page);
                       if (it == sparse_page_table.end()) {
                           if constexpr (BOOL_BREAK) {
                               return false;
                           } else {
                               return;
                           }
                       }
                       for (const ImageId image_id : it->second) {
                           Image& image = slot_images[image_id];
                           if (True(image.flags & ImageFlagBits::Picked)) {
                               continue;
                           }
                           if (!image.OverlapsGPU(gpu_addr, size)) {
                               continue;
                           }
                           image.flags |= ImageFlagBits::Picked;
                           images.push_back(image_id);
                           if constexpr (BOOL_BREAK) {
                               if (func(image_id, image)) {
                                   return true;
                               }
                           } else {
                               func(image_id, image);
                           }
                       }
                       if constexpr (BOOL_BREAK) {
                           return false;
                       }
                   });
    for (const ImageId image_id : images) {
        slot_images[image_id].flags &= ~ImageFlagBits::Picked;
    }
}

template <class P>
template <typename Func>
void TextureCache<P>::ForEachSparseSegment(ImageBase& image, Func&& func) {
    using FuncReturn = typename std::invoke_result<Func, GPUVAddr, DAddr, size_t>::type;
    constexpr bool RETURNS_BOOL = std::is_same_v<FuncReturn, bool>;
    const auto segments = gpu_memory->GetSubmappedRange(image.gpu_addr, image.guest_size_bytes);
    for (const auto& [gpu_addr, size] : segments) {
        const std::optional<DAddr> cpu_addr = gpu_memory->GpuToCpuAddress(gpu_addr);
        if (!cpu_addr) {
            static std::atomic<u32> logged{};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
                LOG_WARNING(HW_GPU,
                            "Skipping sparse texture segment {:#x}+{:#x}: no device address",
                            gpu_addr, size);
            }
            continue;
        }
        if constexpr (RETURNS_BOOL) {
            if (func(gpu_addr, *cpu_addr, size)) {
                break;
            }
        } else {
            func(gpu_addr, *cpu_addr, size);
        }
    }
}

template <class P>
ImageViewId TextureCache<P>::FindOrEmplaceImageView(ImageId image_id, const ImageViewInfo& info) {
    Image& image = slot_images[image_id];
    if (const ImageViewId image_view_id = image.FindView(info); image_view_id) {
        return image_view_id;
    }
    if (True(image.flags & ImageFlagBits::Unbacked) &&
        False(image.flags & ImageFlagBits::SparseLayers)) {
        // How the game uses an image too large to back (e.g. a sparse virtual texture).
        LOG_INFO(HW_GPU, "View of unbacked image {:#x}: type {} format {} levels {}+{} layers {}+{}",
                 image.gpu_addr, static_cast<u32>(info.type), static_cast<u32>(info.format),
                 info.range.base.level, info.range.extent.levels, info.range.base.layer,
                 info.range.extent.layers);
    }
    const ImageViewId image_view_id = slot_image_views.insert(runtime, info, image_id, image, slot_images);
    image.InsertView(info, image_view_id);
    return image_view_id;
}

template <class P>
void TextureCache<P>::RegisterImage(ImageId image_id) {
    ImageBase& image = slot_images[image_id];
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered),
               "Trying to register an already registered image");
    image.flags |= ImageFlagBits::Registered;
    image.channel = current_channel_id;
    image.address_space = current_address_space;
    total_used_memory += ImageMemoryBytes(slot_images[image_id]);
    image.lru_index = lru_cache.Insert(image_id, frame_tick);

    ForEachGPUPage(image.gpu_addr, image.guest_size_bytes, [this, image_id](u64 page) {
        (*channel_state->gpu_page_table)[page].push_back(image_id);
    });
    if (False(image.flags & ImageFlagBits::Sparse)) {
        auto map_id =
            slot_map_views.insert(image.gpu_addr, image.cpu_addr, image.guest_size_bytes, image_id);
        page_filter.Mark(image.cpu_addr, image.guest_size_bytes);
        ForEachCPUPage(image.cpu_addr, image.guest_size_bytes,
                       [this, map_id](u64 page) { page_table[page].push_back(map_id); });
        image.map_view_id = map_id;
        return;
    }
    boost::container::small_vector<ImageViewId, 16> sparse_maps;
    ForEachSparseSegment(
        image, [this, image_id, &sparse_maps](GPUVAddr gpu_addr, DAddr cpu_addr, size_t size) {
            auto map_id = slot_map_views.insert(gpu_addr, cpu_addr, size, image_id);
            page_filter.Mark(cpu_addr, size);
            ForEachCPUPage(cpu_addr, size,
                           [this, map_id](u64 page) { page_table[page].push_back(map_id); });
            sparse_maps.push_back(map_id);
        });
    sparse_views.emplace(image_id, std::move(sparse_maps));
    ForEachGPUPage(image.gpu_addr, image.guest_size_bytes, [this, image_id](u64 page) {
        (*channel_state->sparse_page_table)[page].push_back(image_id);
    });
}

template <class P>
void TextureCache<P>::ReportMissingImagePage(ImageId image_id, const Image& image, u64 page) {
    std::string spaces;
    for (const auto& [id, ref] : address_spaces) {
        spaces += fmt::format(" {}->{}", id, ref.storage_id);
    }
    std::string holders;
    for (size_t index = 0; index < gpu_page_table_storage.size(); ++index) {
        const auto& table = gpu_page_table_storage[index];
        const auto it = table.find(page);
        if (it == table.end()) {
            continue;
        }
        const bool has_image = std::ranges::find(it->second, image_id) != it->second.end();
        holders += fmt::format(" {}{}{}", index / 2, index % 2 == 0 ? "" : "s",
                               has_image ? "+image" : "");
    }
    const ImageInfo& info = image.info;
    LOG_CRITICAL(HW_GPU,
                 "Unregistering image {} at {:#x}+{:#x} (type {} format {} {}x{}x{} levels {} "
                 "layers {} flags {:#x}): page {:#x} is missing; registered on channel {} address "
                 "space {}, now channel {} address space {}; address space->storage:{}; tables "
                 "holding the page:{}",
                 image_id.index, image.gpu_addr, image.guest_size_bytes,
                 static_cast<u32>(info.type), static_cast<u32>(info.format), info.size.width,
                 info.size.height, info.size.depth, info.resources.levels, info.resources.layers,
                 static_cast<u32>(image.flags), page << LICHII_PAGEBITS, image.channel,
                 image.address_space, current_channel_id, current_address_space, spaces,
                 holders.empty() ? " none" : holders);
}

template <class P>
void TextureCache<P>::UnregisterImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(True(image.flags & ImageFlagBits::Registered),
               "Trying to unregister an already registered image");
    image.flags &= ~ImageFlagBits::Registered;
    image.flags &= ~ImageFlagBits::BadOverlap;
    lru_cache.Free(image.lru_index);
    const std::optional<size_t> storage_id = getStorageID(image.address_space);
    ASSERT(storage_id);
    TextureCacheGPUMap& gpu_page_table = gpu_page_table_storage[*storage_id * 2];
    TextureCacheGPUMap& sparse_page_table = gpu_page_table_storage[*storage_id * 2 + 1];
    bool reported_missing_page = false;
    const auto& clear_page_table =
        [this, image_id, &image, &reported_missing_page](u64 page, ::Common::unordered_map<u64, std::vector<ImageId>, Common::IdentityHash<u64>>& selected_page_table) {
            const auto page_it = selected_page_table.find(page);
            if (page_it == selected_page_table.end()) {
                if (!std::exchange(reported_missing_page, true)) {
                    ReportMissingImagePage(image_id, image, page);
                }
                ASSERT_MSG(false, "Unregistering unregistered page={:#x}", page << LICHII_PAGEBITS);
                return;
            }
            std::vector<ImageId>& image_ids = page_it->second;
            const auto vector_it = std::ranges::find(image_ids, image_id);
            if (vector_it == image_ids.end()) {
                ASSERT_MSG(false, "Unregistering unregistered image in page={:#x}",
                           page << LICHII_PAGEBITS);
                return;
            }
            image_ids.erase(vector_it);
        };
    ForEachGPUPage(image.gpu_addr, image.guest_size_bytes,
                   [&clear_page_table, &gpu_page_table](u64 page) {
                       clear_page_table(page, gpu_page_table);
                   });
    if (False(image.flags & ImageFlagBits::Sparse)) {
        const auto map_id = image.map_view_id;
        ForEachCPUPage(image.cpu_addr, image.guest_size_bytes, [this, map_id](u64 page) {
            const auto page_it = page_table.find(page);
            if (page_it == page_table.end()) {
                ASSERT_MSG(false, "Unregistering unregistered page={:#x}", page << LICHII_PAGEBITS);
                return;
            }
            std::vector<ImageMapId>& image_map_ids = page_it->second;
            const auto vector_it = std::ranges::find(image_map_ids, map_id);
            if (vector_it == image_map_ids.end()) {
                ASSERT_MSG(false, "Unregistering unregistered image in page={:#x}",
                           page << LICHII_PAGEBITS);
                return;
            }
            image_map_ids.erase(vector_it);
        });
        slot_map_views.erase(map_id);
        return;
    }
    ForEachGPUPage(image.gpu_addr, image.guest_size_bytes,
                   [&clear_page_table, &sparse_page_table](u64 page) {
                       clear_page_table(page, sparse_page_table);
                   });
    auto it = sparse_views.find(image_id);
    ASSERT(it != sparse_views.end());
    auto& sparse_maps = it->second;
    for (auto& map_view_id : sparse_maps) {
        const auto& map_range = slot_map_views[map_view_id];
        const DAddr cpu_addr = map_range.cpu_addr;
        const std::size_t size = map_range.size;
        ForEachCPUPage(cpu_addr, size, [this, image_id](u64 page) {
            const auto page_it = page_table.find(page);
            if (page_it == page_table.end()) {
                ASSERT_MSG(false, "Unregistering unregistered page={:#x}", page << LICHII_PAGEBITS);
                return;
            }
            std::vector<ImageMapId>& image_map_ids = page_it->second;
            auto vector_it = image_map_ids.begin();
            while (vector_it != image_map_ids.end()) {
                ImageMapView& map = slot_map_views[*vector_it];
                if (map.image_id != image_id) {
                    vector_it++;
                    continue;
                }
                if (!map.picked) {
                    map.picked = true;
                }
                vector_it = image_map_ids.erase(vector_it);
            }
        });
        slot_map_views.erase(map_view_id);
    }
    sparse_views.erase(it);
}

template <class P>
void TextureCache<P>::TrackImage(ImageBase& image, ImageId image_id) {
    ASSERT(False(image.flags & ImageFlagBits::Tracked));
    image.flags |= ImageFlagBits::Tracked;
    // Marked before the pages trap: a new image is tracked (and uploaded) before RegisterImage
    // marks it, and RasterizerVulkan::OnCPUWrite skips the texture cache on unmarked pages.
    if (False(image.flags & ImageFlagBits::Sparse)) {
        if (image.cpu_addr < ~(1ULL << 40)) {
            page_filter.Mark(image.cpu_addr, image.guest_size_bytes);
            device_memory.UpdatePagesCachedCount(image.cpu_addr, image.guest_size_bytes, 1);
        }
        return;
    }
    if (True(image.flags & ImageFlagBits::Registered)) {
        auto it = sparse_views.find(image_id);
        ASSERT(it != sparse_views.end());
        auto& sparse_maps = it->second;
        for (auto& map_view_id : sparse_maps) {
            const auto& map = slot_map_views[map_view_id];
            const DAddr cpu_addr = map.cpu_addr;
            const std::size_t size = map.size;
            page_filter.Mark(cpu_addr, size);
            device_memory.UpdatePagesCachedCount(cpu_addr, size, 1);
        }
        return;
    }
    ForEachSparseSegment(image,
                         [this]([[maybe_unused]] GPUVAddr gpu_addr, DAddr cpu_addr, size_t size) {
                             page_filter.Mark(cpu_addr, size);
                             device_memory.UpdatePagesCachedCount(cpu_addr, size, 1);
                         });
}

template <class P>
void TextureCache<P>::UntrackImage(ImageBase& image, ImageId image_id) {
    ASSERT(True(image.flags & ImageFlagBits::Tracked));
    image.flags &= ~ImageFlagBits::Tracked;
    if (False(image.flags & ImageFlagBits::Sparse)) {
        if (image.cpu_addr < ~(1ULL << 40)) {
            device_memory.UpdatePagesCachedCount(image.cpu_addr, image.guest_size_bytes, -1);
        }
        return;
    }
    ASSERT(True(image.flags & ImageFlagBits::Registered));
    auto it = sparse_views.find(image_id);
    ASSERT(it != sparse_views.end());
    auto& sparse_maps = it->second;
    for (auto& map_view_id : sparse_maps) {
        const auto& map = slot_map_views[map_view_id];
        const DAddr cpu_addr = map.cpu_addr;
        const std::size_t size = map.size;
        device_memory.UpdatePagesCachedCount(cpu_addr, size, -1);
    }
}

template <class P>
void TextureCache<P>::DeleteImage(ImageId image_id, bool immediate_delete) {
    const StutterTrace::SiteScope site{StutterTrace::Site::ImageDelete};
    ImageBase& image = slot_images[image_id];
    ++images_deleted;
    ++churn.deleted;
    {
        if (TextureCacheTrace()) {
            LOG_WARNING(HW_GPU,
                        "TCTRACE delete id={} gpu={:#x} fmt={} {}x{}x{} guest={}KiB immediate={} "
                        "reason={} flags={:#x} downloaded={} frame={}",
                        image_id.index, image.gpu_addr, static_cast<u32>(image.info.format),
                        image.info.size.width, image.info.size.height, image.info.size.depth,
                        image.guest_size_bytes / 1024, immediate_delete, trace_delete_reason,
                        static_cast<u32>(image.flags) | trace_delete_flags,
                        trace_delete_downloaded, frame_tick);
        }
        trace_delete_reason = "other";
        trace_delete_flags = 0;
        trace_delete_downloaded = false;
    }
    VideoCommon::Nx2FastDraw::Break(VideoCommon::Nx2FastDraw::Reason::Invalidation);
    // Drop every decode still queued for it, whatever IsDecoding says: an image re-decoded while
    // a decode was in flight has two, and the first to finish clears the flag. A decode left
    // behind was uploaded into the next image to take this slot (another texture's data: garbage
    // textures, and a staging overrun when that image was smaller).
    for (auto& async_decode : async_decodes) {
        if (async_decode->image_id == image_id) {
            async_decode->cancelled = true;
        }
    }
    if (image.HasScaled()) {
        total_used_memory -= GetScaledImageSizeBytes(image);
    }
    // TickFrame sets the total from the driver, which can be under the sum of these estimates.
    total_used_memory -= (std::min)(total_used_memory, ImageMemoryBytes(slot_images[image_id]));
    const u64 freed_bytes = (image.HasScaled() ? GetScaledImageSizeBytes(image) : 0) +
                            ImageAllocationBytes(slot_images[image_id]);
    const GPUVAddr gpu_addr = image.gpu_addr;
    const auto alloc_it = image_allocs_table.find(gpu_addr);
    if (alloc_it == image_allocs_table.end()) {
        ASSERT_MSG(false, "Trying to delete an image alloc that does not exist in address {:#x}",
                   gpu_addr);
        return;
    }
    const ImageAllocId alloc_id = alloc_it->second;
    std::vector<ImageId>& alloc_images = slot_image_allocs[alloc_id].images;
    const auto alloc_image_it = std::ranges::find(alloc_images, image_id);
    if (alloc_image_it == alloc_images.end()) {
        ASSERT_MSG(false, "Trying to delete an image that does not exist");
        return;
    }
    ASSERT_MSG(False(image.flags & ImageFlagBits::Tracked), "Image was not untracked");
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered), "Image was not unregistered");

    // Mark render targets as dirty
    auto& dirty = maxwell3d->dirty.flags;
    dirty[Dirty::RenderTargets] = true;
    dirty[Dirty::ZetaBuffer] = true;
    for (size_t rt = 0; rt < NUM_RT; ++rt) {
        dirty[Dirty::ColorBuffer0 + rt] = true;
    }
    const std::span<const ImageViewId> image_view_ids = image.image_view_ids;
    for (const ImageViewId image_view_id : image_view_ids) {
        std::ranges::replace(render_targets.color_buffer_ids, image_view_id, ImageViewId{});
        if (render_targets.depth_buffer_id == image_view_id) {
            render_targets.depth_buffer_id = ImageViewId{};
        }
    }
    RemoveImageViewReferences(image_view_ids);
    RemoveFramebuffers(image_view_ids);

    for (const AliasedImage& alias : image.aliased_images) {
        ImageBase& other_image = slot_images[alias.id];
        [[maybe_unused]] const size_t num_removed_aliases =
            std::erase_if(other_image.aliased_images, [image_id](const AliasedImage& other_alias) {
                return other_alias.id == image_id;
            });
        other_image.CheckAliasState();
        ASSERT_MSG(num_removed_aliases == 1, "Invalid number of removed aliases: {}",
                   num_removed_aliases);
    }
    for (const ImageId overlap_id : image.overlapping_images) {
        ImageBase& other_image = slot_images[overlap_id];
        [[maybe_unused]] const size_t num_removed_overlaps = std::erase_if(
            other_image.overlapping_images,
            [image_id](const ImageId other_overlap_id) { return other_overlap_id == image_id; });
        other_image.CheckBadOverlapState();
        ASSERT_MSG(num_removed_overlaps == 1, "Invalid number of removed overlapps: {}",
                   num_removed_overlaps);
    }
    for (const ImageViewId image_view_id : image_view_ids) {
        if (!immediate_delete) {
            sentenced_image_view.Push(std::move(slot_image_views[image_view_id]));
        }
        slot_image_views.erase(image_view_id);
    }
    const bool retained = RetainPendingImageDownloads(image_id, slot_images[image_id],
                                                       uncommitted_downloads, committed_downloads);
    if (retained) {
        LOG_INFO(Render_Vulkan, "Retained image for queued download: id={}", image_id.Value());
    } else if (!immediate_delete) {
        if constexpr (requires { runtime.CurrentTick(); }) {
            gpu_sentenced_images.push_back(
                {runtime.CurrentTick(), freed_bytes, std::move(slot_images[image_id])});
            gpu_sentenced_bytes += freed_bytes;
        } else {
            sentenced_images.Push(std::move(slot_images[image_id]));
            sentenced_image_bytes[sentenced_image_index] += freed_bytes;
        }
    }
    slot_images.erase(image_id);

    alloc_images.erase(alloc_image_it);
    if (alloc_images.empty()) {
        image_allocs_table.erase(alloc_it);
        slot_image_allocs.erase(alloc_id);
    }
    for (size_t c : active_channel_ids) {
        auto& channel_info = channel_storage[c];
#ifndef NDEBUG
        if constexpr (ENABLE_VALIDATION) {
            std::ranges::fill(channel_info.graphics_image_table.LiveBoundIds(), CORRUPT_ID);
            std::ranges::fill(channel_info.compute_image_table.LiveBoundIds(), CORRUPT_ID);
        }
#endif
        channel_info.graphics_image_table.Invalidate();
        channel_info.compute_image_table.Invalidate();
    }
    ++churn.invalidations;
    has_deleted_images = true;
}

template <class P>
void TextureCache<P>::RemoveImageViewReferences(std::span<const ImageViewId> removed_views) {
    for (const ImageViewId removed : removed_views) {
        const auto refs = image_view_descriptors.find(removed);
        if (refs == image_view_descriptors.end()) {
            continue;
        }
        for (const auto& [channel, descriptor] : refs->second) {
            // The channel may be gone, or its entry point to another view since.
            if (std::ranges::find(active_channel_ids, channel) == active_channel_ids.end()) {
                continue;
            }
            auto& image_views = channel_storage[channel].image_views;
            if (const auto it = image_views.find(descriptor);
                it != image_views.end() && it->second == removed) {
                image_views.erase(it);
            }
        }
        image_view_descriptors.erase(refs);
    }
}

template <class P>
void TextureCache<P>::RemoveFramebuffers(std::span<const ImageViewId> removed_views) {
    runtime.FlushDeferredClear();
    auto it = framebuffers.begin();
    while (it != framebuffers.end()) {
        if (it->first.Contains(removed_views)) {
            auto framebuffer_id = it->second;
            ASSERT(framebuffer_id);
            if (framebuffer_id == last_framebuffer_id) {
                last_framebuffer_id = {};
                last_framebuffer_serial = 0;
            }
            sentenced_framebuffers.Push(std::move(slot_framebuffers[framebuffer_id]));
            slot_framebuffers.erase(framebuffer_id);
            it = framebuffers.erase(it);
        } else {
            ++it;
        }
    }
}

template <class P>
void TextureCache<P>::MarkModification(ImageBase& image) noexcept {
    image.flags |= ImageFlagBits::GpuModified;
    image.storage_written_levels = 0;
    SetModificationTick(slot_images, image, ++modification_tick);
}

template <class P>
void TextureCache<P>::SynchronizeAliases(ImageId image_id) {
    boost::container::small_vector<const AliasedImage*, 8> aliased_images;
    Image& image = slot_images[image_id];
    // Nothing aliased was modified after this image: skip walking its (possibly long) alias list
    // for every draw that samples it (~7% of the GPU back thread in DK Bananza's open areas).
    if (image.newest_alias_tick <= image.modification_tick) {
        DEBUG_ASSERT(std::ranges::none_of(image.aliased_images, [&](const AliasedImage& alias) {
            return slot_images[alias.id].modification_tick > image.modification_tick;
        }));
        return;
    }
    bool any_rescaled = True(image.flags & ImageFlagBits::Rescaled);
    bool any_modified = True(image.flags & ImageFlagBits::GpuModified);
    u64 most_recent_tick = image.modification_tick;
    for (const AliasedImage& aliased : image.aliased_images) {
        ImageBase& aliased_image = slot_images[aliased.id];
        if (image.modification_tick < aliased_image.modification_tick) {
            most_recent_tick = (std::max)(most_recent_tick, aliased_image.modification_tick);
            aliased_images.push_back(&aliased);
            any_rescaled |= True(aliased_image.flags & ImageFlagBits::Rescaled);
            any_modified |= True(aliased_image.flags & ImageFlagBits::GpuModified);
        }
    }
    if (aliased_images.empty()) {
        // Newer aliases were deleted since: skip the walk until one is modified again.
        image.newest_alias_tick = image.modification_tick;
        return;
    }
    const bool can_rescale = ImageCanRescale(image);
    if (any_rescaled) {
        if (can_rescale) {
            ScaleUp(image);
        } else {
            ScaleDown(image);
        }
    }
    SetModificationTick(slot_images, image, most_recent_tick);
    image.newest_alias_tick = most_recent_tick;
    image.storage_written_levels = 0;
    if (any_modified) {
        image.flags |= ImageFlagBits::GpuModified;
    }
    std::ranges::sort(aliased_images, [this](const AliasedImage* lhs, const AliasedImage* rhs) {
        const ImageBase& lhs_image = slot_images[lhs->id];
        const ImageBase& rhs_image = slot_images[rhs->id];
        return lhs_image.modification_tick < rhs_image.modification_tick;
    });
    const auto& resolution = Settings::values.resolution_info;
    for (const AliasedImage* const aliased : aliased_images) {
        if (!resolution.active || !any_rescaled) {
            CopyImage(image_id, aliased->id, aliased->copies);
            continue;
        }
        Image& aliased_image = slot_images[aliased->id];
        if (!can_rescale) {
            ScaleDown(aliased_image);
            CopyImage(image_id, aliased->id, aliased->copies);
            continue;
        }
        ScaleUp(aliased_image);
        CopyImage(image_id, aliased->id, aliased->copies);
    }
}

template <class P>
void TextureCache<P>::PrepareImage(ImageId image_id, bool is_modification, bool invalidate) {
    if (prepare_log != nullptr) {
        prepare_log->push_back(image_id);
        if (prepare_log_dry) {
            return;
        }
    }
    Image& image = slot_images[image_id];
    if (is_modification && !invalidate && !async_overlap_downloads.empty() &&
        WritebackWouldReload(image)) {
        // Its write-back landing would reload it, but not once the GPU has written to it: a
        // partial render now would keep guest memory's stale bytes where it doesn't draw. Land
        // the write-backs first (they mark it for the upload below).
        FinishAsyncOverlapDownloads(true);
    }
    if (invalidate) {
        image.flags &= ~(ImageFlagBits::CpuModified | ImageFlagBits::GpuModified);
        if (False(image.flags & ImageFlagBits::Tracked)) {
            TrackImage(image, image_id);
        }
    } else {
        RefreshContents(image, image_id);
        if (!image.aliased_images.empty()) {
            SynchronizeAliases(image_id);
        }
    }
    if (is_modification) {
        if constexpr (requires { &Image::BackTilesWhole; }) {
            if (True(image.flags & ImageFlagBits::SparseTiles) && image.SparseTilesShareZeroBlock()) {
                // The render would write the zeroed block all empty blocks share: each gets memory
                // of its own first, and the parts that hold data are loaded again.
                LOG_INFO(HW_GPU, "GPU write into sparse tiled image {:#x}; backing it whole",
                         image.gpu_addr);
                if (BackSparseTilesWhole(image)) {
                    image.dirty_ranges.clear();
                    RefreshSparseTiles(image);
                } else {
                    LOG_ERROR(HW_GPU,
                              "Sparse tiled image {:#x}: out of memory backing it whole, the GPU "
                              "write lands in the shared zeroed block",
                              image.gpu_addr);
                }
            }
        }
        if constexpr (requires { &Image::BindLayers; }) {
            if (True(image.flags & ImageFlagBits::SparseLayers)) {
                std::vector<u32> layers(static_cast<size_t>(image.info.resources.layers));
                std::iota(layers.begin(), layers.end(), 0U);
                // Layers over the image's memory cap stay unbacked (logged once).
                BindSparseLayers(image, layers);
            }
        }
        if (True(image.flags & ImageFlagBits::UnwrittenSparse) &&
            False(image.flags & ImageFlagBits::Remapped)) {
            // The GPU renders into it: these writes are lost, later uses get a backed image.
            LOG_WARNING(HW_GPU, "GPU write into unwritten sparse image {:#x}; replacing it",
                        image.gpu_addr);
            Tegra::Nx2SparseMarkWritten(image.gpu_addr, image.guest_size_bytes);
            image.flags |= ImageFlagBits::CpuModified | ImageFlagBits::Remapped;
            VideoCommon::Nx2FastDraw::Break(VideoCommon::Nx2FastDraw::Reason::Invalidation);
        }
        MarkModification(image);
        if (drawing_render_targets && image.first_written_frame == 0) {
            image.first_written_frame = frame_tick + 1;
        }
    }
    lru_cache.Touch(image.lru_index, frame_tick);
}

template <class P>
void TextureCache<P>::PrepareImageView(ImageViewId image_view_id, bool is_modification,
                                       bool invalidate) {
    if (!PreparesImage(slot_image_views, image_view_id)) {
        return;
    }
    PrepareImage(slot_image_views[image_view_id].image_id, is_modification, invalidate);
}

template <class P>
void TextureCache<P>::CopyImage(ImageId dst_id, ImageId src_id, std::vector<ImageCopy> copies) {
    Image& dst = slot_images[dst_id];
    Image& src = slot_images[src_id];
    if (True(dst.flags & ImageFlagBits::UnwrittenSparse) ||
        True(src.flags & ImageFlagBits::UnwrittenSparse)) {
        return;
    }
    if (dst.info.astc_opaque != src.info.astc_opaque) {
        // BC1 and BC3 blocks differ in size, so the host can't copy between them; both hold the
        // same guest data, which is uploaded from guest memory instead.
        return;
    }
    // As in BlitImage: land a parked deferred clear before the copy overwrites the image.
    runtime.FlushDeferredClear();
    const bool is_rescaled = True(src.flags & ImageFlagBits::Rescaled);
    if (is_rescaled) {
        ASSERT(True(dst.flags & ImageFlagBits::Rescaled));
        const bool both_2d{src.info.type == ImageType::e2D && dst.info.type == ImageType::e2D};
        const auto& resolution = Settings::values.resolution_info;
        for (auto& copy : copies) {
            copy.src_offset.x = resolution.ScaleUp(copy.src_offset.x);
            copy.dst_offset.x = resolution.ScaleUp(copy.dst_offset.x);
            copy.extent.width = resolution.ScaleUp(copy.extent.width);
            if (both_2d) {
                copy.src_offset.y = resolution.ScaleUp(copy.src_offset.y);
                copy.dst_offset.y = resolution.ScaleUp(copy.dst_offset.y);
                copy.extent.height = resolution.ScaleUp(copy.extent.height);
            }
        }
    }
    const auto dst_format_type = GetFormatType(dst.info.format);
    const auto src_format_type = GetFormatType(src.info.format);
    if (src_format_type == dst_format_type) {
        if constexpr (HAS_EMULATED_COPIES) {
            if (!runtime.CanImageBeCopied(dst, src)) {
                return runtime.EmulateCopyImage(dst, src, copies);
            }
        }
        return runtime.CopyImage(dst, src, copies);
    }
    UNIMPLEMENTED_IF(dst.info.type != ImageType::e2D);
    UNIMPLEMENTED_IF(src.info.type != ImageType::e2D);
    if (runtime.ShouldReinterpret(dst, src)) {
        return runtime.ReinterpretImage(dst, src, copies);
    }
    for (const ImageCopy& copy : MakeImageCopyLayers(copies)) {
        UNIMPLEMENTED_IF(copy.dst_subresource.num_layers != 1);
        UNIMPLEMENTED_IF(copy.src_subresource.num_layers != 1);
        UNIMPLEMENTED_IF(copy.src_offset != Offset3D{});
        UNIMPLEMENTED_IF(copy.dst_offset != Offset3D{});

        const SubresourceBase dst_base{
            .level = copy.dst_subresource.base_level,
            .layer = copy.dst_subresource.base_layer,
        };
        const SubresourceBase src_base{
            .level = copy.src_subresource.base_level,
            .layer = copy.src_subresource.base_layer,
        };
        const SubresourceExtent dst_extent{.levels = 1, .layers = 1};
        const SubresourceExtent src_extent{.levels = 1, .layers = 1};
        const SubresourceRange dst_range{.base = dst_base, .extent = dst_extent};
        const SubresourceRange src_range{.base = src_base, .extent = src_extent};
        PixelFormat dst_format = GetAliasConversionFormat(dst.info.format, src.info.format);
        const PixelFormat src_format = GetAliasConversionFormat(src.info.format, dst.info.format);
        if (GetFormatType(src.info.format) == SurfaceType::DepthStencil &&
            GetFormatType(dst_format) == SurfaceType::ColorTexture &&
            BytesPerBlock(dst_format) == 4) {
            dst_format = PixelFormat::A8B8G8R8_UNORM;
        }
        const ImageViewInfo dst_view_info(ImageViewType::e2D, dst_format, dst_range);
        const ImageViewInfo src_view_info(ImageViewType::e2D, src_format, src_range);
        const auto [dst_framebuffer_id, dst_view_id] = RenderTargetFromImage(dst_id, dst_view_info);
        Framebuffer* const dst_framebuffer = &slot_framebuffers[dst_framebuffer_id];
        const ImageViewId src_view_id = FindOrEmplaceImageView(src_id, src_view_info);
        ImageView& dst_view = slot_image_views[dst_view_id];
        ImageView& src_view = slot_image_views[src_view_id];
        [[maybe_unused]] const Extent3D expected_size{
            .width = (std::min)(dst_view.size.width, src_view.size.width),
            .height = (std::min)(dst_view.size.height, src_view.size.height),
            .depth = (std::min)(dst_view.size.depth, src_view.size.depth),
        };
        const Extent3D scaled_extent = [is_rescaled, expected_size]() {
            if (!is_rescaled) {
                return expected_size;
            }
            const auto& resolution = Settings::values.resolution_info;
            return Extent3D{
                .width = resolution.ScaleUp(expected_size.width),
                .height = resolution.ScaleUp(expected_size.height),
                .depth = expected_size.depth,
            };
        }();
        UNIMPLEMENTED_IF(copy.extent != scaled_extent);

        runtime.ConvertImage(dst_framebuffer, dst_view, src_view);
    }
}

template <class P>
void TextureCache<P>::BindRenderTarget(ImageViewId* old_id, ImageViewId new_id) {
    if (*old_id == new_id) {
        return;
    }
    if (new_id) {
        const ImageViewBase& old_view = slot_image_views[new_id];
        if (True(old_view.flags & ImageViewFlagBits::PreemtiveDownload) &&
            False(slot_images[old_view.image_id].flags & (ImageFlagBits::Unbacked | ImageFlagBits::TrimmedLevels))) {
            const PendingDownload new_download{true, 0, old_view.image_id};
            uncommitted_downloads.emplace_back(new_download);
        }
    }
    *old_id = new_id;
}

template <class P>
std::pair<FramebufferId, ImageViewId> TextureCache<P>::RenderTargetFromImage(
    ImageId image_id, const ImageViewInfo& view_info) {
    const ImageViewId view_id = FindOrEmplaceImageView(image_id, view_info);
    const ImageBase& image = slot_images[image_id];
    const bool is_rescaled = True(image.flags & ImageFlagBits::Rescaled);
    const bool is_color = GetFormatType(image.info.format) == SurfaceType::ColorTexture;
    const ImageViewId color_view_id = is_color ? view_id : ImageViewId{};
    const ImageViewId depth_view_id = is_color ? ImageViewId{} : view_id;
    Extent3D extent = MipSize(image.info.size, view_info.range.base.level);
    if (is_rescaled) {
        const auto& resolution = Settings::values.resolution_info;
        extent.width = resolution.ScaleUp(extent.width);
        if (image.info.type == ImageType::e2D) {
            extent.height = resolution.ScaleUp(extent.height);
        }
    }
    const u32 num_samples = image.info.num_samples;
    const auto [samples_x, samples_y] = SamplesLog2(num_samples);
    const FramebufferId framebuffer_id = GetFramebufferId(RenderTargets{
        .color_buffer_ids = {color_view_id},
        .depth_buffer_id = depth_view_id,
        .size = {extent.width >> samples_x, extent.height >> samples_y},
        .is_rescaled = is_rescaled,
    });
    return {framebuffer_id, view_id};
}

template <class P>
bool TextureCache<P>::IsFullClear(ImageViewId id) {
    if (!id) {
        return true;
    }
    const ImageViewBase& image_view = slot_image_views[id];
    const ImageBase& image = slot_images[image_view.image_id];
    const Extent3D size = image_view.size;
    if (image.info.resources.levels > 1 || image.info.resources.layers > 1) {
        // Images with multiple resources can't be cleared in a single call
        return false;
    }
    // Make sure the clear covers all texels in the subresource
    return ClearCovers(size.width, size.height);
}

template <class P>
bool TextureCache<P>::ClearWritesWhole(const ImageInfo& info, GPUVAddr gpu_addr) const {
    if (info.type != ImageType::e2D || info.resources.levels != 1 ||
        info.resources.layers != 1 || info.num_samples != 1) {
        return false;
    }
    const auto& regs = maxwell3d->regs;
    const auto& surface = regs.clear_surface;
    if (surface.layer != 0) {
        return false;
    }
    const u32 width = info.size.width;
    const u32 height = info.size.height;
    if (!ClearCovers(width, height)) {
        return false;
    }
    // The clear covers the framebuffer's render area, the smallest bound target.
    for (u32 index = 0; index < regs.rt_control.count && index < NUM_RT; ++index) {
        const auto& rt = regs.rt[index];
        if (rt.Address() != 0 && rt.format != Tegra::RenderTargetFormat::NONE &&
            (rt.width < width || rt.height < height)) {
            return false;
        }
    }
    if (regs.zeta_enable && regs.zeta.Address() != 0 &&
        (regs.zeta_size.width < width || regs.zeta_size.height < height)) {
        return false;
    }
    const bool is_depth_target = regs.zeta_enable && regs.zeta.Address() == gpu_addr;
    const bool full_stencil = surface.S && regs.stencil_front_mask == 0xFF;
    switch (VideoCore::Surface::GetFormatType(info.format)) {
    case VideoCore::Surface::SurfaceType::ColorTexture: {
        // OpenGL clears draw buffer RT, which writes target rt_control.Map(RT).
        const u32 index = surface.RT;
        return surface.R && surface.G && surface.B && surface.A && index < NUM_RT &&
               index < regs.rt_control.count && regs.rt_control.Map(index) == index &&
               regs.rt[index].Address() == gpu_addr;
    }
    case VideoCore::Surface::SurfaceType::Depth:
        return is_depth_target && surface.Z;
    case VideoCore::Surface::SurfaceType::Stencil:
        return is_depth_target && full_stencil;
    case VideoCore::Surface::SurfaceType::DepthStencil:
        return is_depth_target && surface.Z && full_stencil;
    default:
        return false;
    }
}

template <class P>
void TextureCache<P>::NoteClearedWhole() {
    // A clear that writes all of a target replaces its contents: write-backs noted before it are
    // no newer than what it holds (OverlapHasNewerData's smaller newer write-backs).
    const auto note = [&](ImageViewId id) {
        if (!PreparesImage(slot_image_views, id)) {
            return;
        }
        ImageBase& image = slot_images[slot_image_views[id].image_id];
        if (ClearWritesWhole(image.info, image.gpu_addr)) {
            image.load_tick = image.modification_tick;
        }
    };
    for (const ImageViewId id : render_targets.color_buffer_ids) {
        note(id);
    }
    note(render_targets.depth_buffer_id);
}

template <class P>
bool TextureCache<P>::ClearCovers(u32 width, u32 height) const {
    const auto& regs = maxwell3d->regs;
    // The render area is the surface clip's size.
    if (regs.surface_clip.width < width || regs.surface_clip.height < height) {
        return false;
    }
    if (regs.clear_control.use_scissor == 0) {
        return true;
    }
    // With a lower-left window origin, the clear's scissor is flipped around the surface clip's
    // height (GetScissorState).
    const auto& scissor = regs.scissor_test[0];
    const bool lower_left =
        regs.window_origin.mode != Tegra::Engines::Maxwell3D::Regs::WindowOrigin::Mode::UpperLeft;
    const s32 clip_height = static_cast<s32>(regs.surface_clip.height.Value());
    const s32 min_y = static_cast<s32>(scissor.min_y.Value());
    const s32 max_y = static_cast<s32>(scissor.max_y.Value());
    const s32 top = lower_left ? clip_height - max_y : min_y;
    const s32 bottom = lower_left ? clip_height - min_y : max_y;
    return scissor.min_x == 0 && top <= 0 && scissor.max_x >= width &&
           bottom >= static_cast<s32>(height);
}

template <class P>
void TextureCache<P>::CreateChannel(struct Tegra::Control::ChannelState& channel) {
    VideoCommon::ChannelSetupCaches<TextureCacheChannelInfo>::CreateChannel(channel);
    const auto it = channel_map.find(channel.bind_id);
    auto* this_state = &channel_storage[it->second];
    const auto& this_as_ref = address_spaces[channel.memory_manager->GetID()];
    this_state->gpu_page_table = &gpu_page_table_storage[this_as_ref.storage_id * 2];
    this_state->sparse_page_table = &gpu_page_table_storage[this_as_ref.storage_id * 2 + 1];
    LOG_INFO(HW_GPU, "Texture cache channel {} (slot {}): address space {}, storage {} of {}",
             channel.bind_id, it->second, channel.memory_manager->GetID(), this_as_ref.storage_id,
             address_spaces.size());
}

/// Bind a channel for execution.
template <class P>
void TextureCache<P>::OnGPUASRegister([[maybe_unused]] size_t map_id) {
    gpu_page_table_storage.emplace_back();
    gpu_page_table_storage.emplace_back();
}

} // namespace VideoCommon
