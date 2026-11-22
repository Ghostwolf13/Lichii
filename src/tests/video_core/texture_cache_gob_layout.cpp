// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "common/page_table.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/memory.h"
#include "tests/core/system_test_context.h"
#include "video_core/control/channel_state.h"
#include "video_core/gpu.h"
#include "video_core/renderer_null/null_rasterizer.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/textures/decoders.h"

namespace {

struct TestAsyncBuffer {
    std::span<u8> mapped_span;
};

struct TestRuntime {
    TestAsyncBuffer DownloadStagingBuffer(size_t size, bool deferred = false) {
        REQUIRE(deferred);
        staging.resize(size);
        ++downloads;
        return {staging};
    }
    void FreeDeferredStagingBuffer(TestAsyncBuffer&) {
        staging.clear();
    }
    u64 CurrentTick() const { return 1; }
    bool IsFree(u64 tick) const { return completed_tick >= tick; }
    void FlushDeferredClear() {}
    void Finish() { FAIL("A texture lookup must not wait for the GPU"); }

    bool HasBrokenTextureViewFormats() const {
        return false;
    }
    bool HasNativeBgr() const {
        return false;
    }
    std::vector<u8> staging;
    u64 completed_tick{};
    u32 downloads{};
};

struct TestImage : VideoCommon::ImageBase {
    TestImage(TestRuntime&, const VideoCommon::ImageInfo& image_info, GPUVAddr gpu_address,
              DAddr cpu_address)
        : ImageBase{image_info, gpu_address, cpu_address} {}
    explicit TestImage(const VideoCommon::NullImageParams& params) : ImageBase{params} {}
    TestImage(TestImage&&) = default;
    TestImage& operator=(TestImage&&) = default;
    void DownloadMemory(const TestAsyncBuffer& map,
                        std::span<const VideoCommon::BufferImageCopy>) {
        REQUIRE(map.mapped_span.size() == gpu_bytes.size());
        std::ranges::copy(gpu_bytes, map.mapped_span.begin());
    }
    std::vector<u8> gpu_bytes;
};

struct TestImageView : VideoCommon::ImageViewBase {
    TestImageView(TestRuntime&, const VideoCommon::ImageViewInfo& info, VideoCommon::ImageId id,
                  TestImage& image, const Common::SlotVector<TestImage>&)
        : ImageViewBase{info, image.info, id, image.gpu_addr} {}
    TestImageView(TestRuntime&, const VideoCommon::ImageInfo& info,
                  const VideoCommon::ImageViewInfo& view_info, GPUVAddr addr)
        : ImageViewBase{info, view_info, addr} {}
    TestImageView(TestRuntime&, const VideoCommon::ImageInfo& info,
                  const VideoCommon::ImageViewInfo& view_info)
        : ImageViewBase{info, view_info, 0} {}
    TestImageView(TestRuntime&, const VideoCommon::NullImageViewParams& params)
        : ImageViewBase{params} {}
    TestImageView(TestImageView&&) = default;
    TestImageView& operator=(TestImageView&&) = default;
};

struct TestSampler {
    TestSampler(TestRuntime&, const Tegra::Texture::TSCEntry&) {}
};

struct TestImageAlloc {
    std::vector<VideoCommon::ImageId> images;
};

struct TestFramebuffer {};
struct TestBufferType {};

struct Policy {
    using Runtime = TestRuntime;
    using Image = TestImage;
    using ImageAlloc = TestImageAlloc;
    using ImageView = TestImageView;
    using Sampler = TestSampler;
    using Framebuffer = TestFramebuffer;
    using AsyncBuffer = TestAsyncBuffer;
    using BufferType = TestBufferType;
    static constexpr bool ENABLE_VALIDATION = false;
    static constexpr bool FRAMEBUFFER_BLITS = false;
    static constexpr bool HAS_EMULATED_COPIES = false;
    static constexpr bool HAS_DEVICE_MEMORY_INFO = false;
    static constexpr bool IMPLEMENTS_ASYNC_DOWNLOADS = false;
    static constexpr bool HAS_MSAA_DOWNLOADS = false;
};

using Cache = VideoCommon::TextureCache<Policy>;

VideoCommon::ImageInfo MakeInfo() {
    VideoCommon::ImageInfo info{};
    info.type = VideoCommon::ImageType::e2D;
    info.format = VideoCore::Surface::PixelFormat::A8B8G8R8_UNORM;
    info.size = {64, 64, 1};
    info.resources = {1, 1};
    info.block = {0, 0, 0};
    info.num_samples = 1;
    return info;
}

} // namespace

namespace VideoCommon {

struct TextureCacheTestAccess {
    template <class C, class F>
    static void ForEachSparseSegment(C& cache, ImageBase& image, F&& func) {
        cache.ForEachSparseSegment(image, std::forward<F>(func));
    }
    template <class C>
    static ImageId Insert(C& cache, const ImageInfo& info, GPUVAddr gpu_addr, DAddr cpu_addr) {
        const auto id = cache.slot_images.insert(cache.runtime, info, gpu_addr, cpu_addr);
        cache.RegisterImage(id);
        return id;
    }
    template <class C>
    static ImageId Find(C& cache, const ImageInfo& info, GPUVAddr gpu_addr) {
        bool retry = false;
        return cache.FindImage(info, gpu_addr, RelaxedOptions{}, &retry);
    }
    template <class C>
    static void WriteGpu(C& cache, ImageId id, std::vector<u8> bytes) {
        auto& image = cache.slot_images[id];
        image.gpu_bytes = std::move(bytes);
        image.flags &= ~ImageFlagBits::CpuModified;
        cache.MarkModification(image);
    }
    template <class C>
    static void PollWritebacks(C& cache) {
        cache.FinishAsyncOverlapDownloads(false);
    }
    template <class C>
    static const ImageInfo& Info(C& cache, ImageId id) {
        return cache.slot_images[id].info;
    }
};

} // namespace VideoCommon

namespace {

using Access = VideoCommon::TextureCacheTestAccess;

struct Fixture {
    ScopedSystemTestContext context;
    Core::System system;
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager device{physical};
    Tegra::GPU gpu{system, false, false};
    Null::RasterizerNull rasterizer{gpu};
    TestRuntime runtime;
    std::unique_ptr<Cache> cache;
    std::unique_ptr<Kernel::KProcess> process;
    Core::Asid asid{};
    std::shared_ptr<Tegra::MemoryManager> memory;
    Tegra::Control::ChannelState state{0};
    u8* bytes{};

    Fixture() {
        system.Initialize();
        process = std::make_unique<Kernel::KProcess>(system.Kernel());
        auto& table = process->GetPageTable().GetImpl();
        table.Resize(24, 12);
        bytes = physical.GetPointerFromRaw<u8>(0x10000);
        std::memset(bytes, 0, 0x40000);
        for (u64 page = 0; page < 64; ++page) {
            table.entries.GetAndFault(0x10 + page)
                .Store(false, Common::PageType::Memory, 1,
                       reinterpret_cast<uintptr_t>(bytes) - 0x10000);
        }
        process->GetMemory().SetCurrentPageTable(*process);
        asid = device.RegisterProcess(&process->GetMemory());
        device.BindInterface(&rasterizer);
        device.Map(0x10000, 0x10000, 0x40000, asid);
        memory = std::make_shared<Tegra::MemoryManager>(system, device, 32, 1ULL << 31, 16);
        memory->BindRasterizer(&rasterizer);
        memory->EnableNativeTiling();
        memory->Map(0x100000, 0x10000, 0x10000, Tegra::PTEKind::PITCH, true);
        state.memory_manager = memory.get();
        state.payload.emplace(system, *memory, state);
        cache = std::make_unique<Cache>(runtime, device);
        cache->CreateChannel(state);
        cache->BindToChannel(0);
    }
    ~Fixture() {
        cache.reset();
        memory.reset();
        device.Unmap(0x10000, 0x40000);
        device.UnregisterProcess(asid);
        system.Kernel().UnregisterKernelObject(process.get());
    }
};

} // namespace

TEST_CASE("Sparse texture iteration skips segments that cannot be translated",
          "[video_core][texture_cache][sparse_segments]") {
    Fixture f;
    constexpr GPUVAddr gpu_addr = 0x200000;
    constexpr size_t page_size = 0x1000;
    constexpr DAddr invalid_addr = DAddr{1} << Tegra::MaxwellDeviceMemoryManager::AS_BITS;
    constexpr DAddr valid_addr = 0x20000;
    f.memory->Map(gpu_addr, invalid_addr, page_size, Tegra::PTEKind::PITCH, false);
    f.memory->Map(gpu_addr + page_size, valid_addr, page_size, Tegra::PTEKind::PITCH, false);

    VideoCommon::ImageBase image{MakeInfo(), gpu_addr, valid_addr};
    REQUIRE(f.memory->GetSubmappedRange(gpu_addr, image.guest_size_bytes).size() == 2);
    REQUIRE_FALSE(f.memory->GpuToCpuAddress(gpu_addr));

    size_t visited{};
    Access::ForEachSparseSegment(*f.cache, image, [&](GPUVAddr address, DAddr device, size_t size) {
        ++visited;
        CHECK(address == gpu_addr + page_size);
        CHECK(device == valid_addr);
        CHECK(size == page_size);
    });
    CHECK(visited == 1);
}

TEST_CASE("FindImage keeps GOB and native tiling images apart at one address",
          "[video_core][texture_cache][gob_layout]") {
    Fixture f;
    constexpr GPUVAddr gpu_addr = 0x100000;
    constexpr DAddr cpu_addr = 0x10000;

    auto gob = MakeInfo();
    gob.format = VideoCore::Surface::PixelFormat::BC7_UNORM;
    gob.maxwell_gob_layout = true;

    auto native = gob;
    native.maxwell_gob_layout = false;
    native.native_tiling = true;

    const auto gob_id = Access::Insert(*f.cache, gob, gpu_addr, cpu_addr);
    REQUIRE(gob_id != VideoCommon::ImageId{});
    CHECK(Access::Info(*f.cache, gob_id).maxwell_gob_layout);
    CHECK_FALSE(Access::Info(*f.cache, gob_id).native_tiling);

    // Same address and dimensions, but the other GOB order: a different image, not a subresource.
    CHECK(Access::Find(*f.cache, native, gpu_addr) == VideoCommon::ImageId{});
    // The matching layout still resolves to the cached image.
    CHECK(Access::Find(*f.cache, gob, gpu_addr) == gob_id);

    const auto native_id = Access::Insert(*f.cache, native, gpu_addr, cpu_addr);
    REQUIRE(native_id != VideoCommon::ImageId{});
    REQUIRE(native_id != gob_id);
    CHECK_FALSE(Access::Info(*f.cache, native_id).maxwell_gob_layout);
    CHECK(Access::Info(*f.cache, native_id).native_tiling);

    // With both cached, each descriptor resolves only to its own layout.
    CHECK(Access::Find(*f.cache, native, gpu_addr) == native_id);
    CHECK(Access::Find(*f.cache, gob, gpu_addr) == gob_id);
}

TEST_CASE("Partial texture transfers and writeback use the image GOB order",
          "[video_core][texture_cache][gob_layout]") {
    using namespace VideoCommon;
    using Tegra::Texture::SwizzleTexture;

    Fixture f;
    constexpr GPUVAddr gpu_addr = 0x100000;
    auto info = MakeInfo();
    Common::ScratchBuffer<u8> scratch;
    for (const bool compressed : {false, true}) {
        info.format = compressed ? PixelFormat::BC7_UNORM : PixelFormat::A8B8G8R8_UNORM;
        const u32 tiles = compressed ? 16 : 64;
        const u32 bytes_per_tile = compressed ? 16 : 4;
        const auto guest_size = CalculateGuestSizeInBytes(info);
        std::vector<u8> linear(CalculateUnswizzledSizeBytes(info));
        for (size_t i = 0; i < linear.size(); ++i) {
            linear[i] = static_cast<u8>(i ^ (i >> 8U));
        }
        const auto downloads = FullDownloadCopies(info);
        for (const bool native : {false, true}) {
            CAPTURE(compressed, native);
            info.native_tiling = native;
            std::vector<u8> expected(guest_size);
            SwizzleTexture(expected, linear, bytes_per_tile, tiles, tiles, 1, 0, 0, 0, native);
            f.memory->WriteBlockUnsafe(gpu_addr, expected.data(), expected.size());

            const std::array dirty{std::pair<u64, u64>{0, 512}};
            const auto rows = PlanUnswizzleImageRows(gpu_addr, info, dirty);
            REQUIRE(rows.layout_valid);
            REQUIRE(rows.runs.size() == 1);
            REQUIRE(rows.total_host_bytes == 512);
            std::vector<u8> row_output(rows.total_host_bytes);
            ExecuteUnswizzlePlan(*f.memory, rows, row_output);
            for (size_t y = 0; y < 8; ++y) {
                CHECK(std::equal(row_output.begin() + y * 64, row_output.begin() + (y + 1) * 64,
                                 linear.begin() + y * 256));
            }
            if (!compressed) {
                const std::array rects{LevelRect{.level = 0, .x = 0, .y = 0, .width = 16, .height = 8}};
                std::vector<u8> rect_output(UnswizzledRectsBytes(info, rects));
                REQUIRE(UnswizzleImageRects(*f.memory, gpu_addr, info, rects, rect_output).size() == 1);
                CHECK(rect_output == row_output);
            }

            std::vector<u8> actual(guest_size);
            f.memory->WriteBlockUnsafe(gpu_addr, actual.data(), actual.size());
            SwizzleImage(*f.memory, gpu_addr, info, downloads, linear, scratch);
            f.memory->ReadBlockUnsafe(gpu_addr, actual.data(), actual.size());
            CHECK(actual == expected);
            CHECK(actual[native ? 0x40 : 0x20] == linear[16]);
        }
    }
}

TEST_CASE("Hinted BC7 lookup waits for GPU-written native overlap bytes",
          "[video_core][texture_cache][gob_layout]") {
    using namespace VideoCommon;

    Fixture f;
    constexpr GPUVAddr gpu_addr = 0x100000;
    constexpr DAddr cpu_addr = 0x10000;
    auto native = MakeInfo();
    native.native_tiling = true;
    auto gob = native;
    gob.format = PixelFormat::BC7_UNORM;
    gob.native_tiling = false;
    gob.maxwell_gob_layout = true;
    const auto target = Access::Insert(*f.cache, native, gpu_addr, cpu_addr);
    const auto hinted = Access::Insert(*f.cache, gob, gpu_addr, cpu_addr);
    std::vector<u8> written(CalculateUnswizzledSizeBytes(native));
    written[16] = 0x5a;
    Access::WriteGpu(*f.cache, target, std::move(written));

    SECTION("An in-flight writeback preserves intervening guest writes") {
        REQUIRE(Access::Find(*f.cache, gob, gpu_addr) == NULL_IMAGE_ID);
        CHECK(f.runtime.downloads == 1);
        CHECK(f.bytes[0x40] == 0);
        CHECK(Access::Find(*f.cache, gob, gpu_addr) == NULL_IMAGE_ID);
        CHECK(f.runtime.downloads == 1);

        f.bytes[0x41] = 0x73;
        f.cache->WriteMemory(cpu_addr + 0x41, 1);
        f.runtime.completed_tick = 1;
        Access::PollWritebacks(*f.cache);
        CHECK(f.bytes[0x40] == 0x5a);
        CHECK(f.bytes[0x41] == 0x73);
        CHECK(f.bytes[0x20] == 0);
        CHECK(Access::Find(*f.cache, gob, gpu_addr) == hinted);
        CHECK(f.runtime.downloads == 1);
    }

    SECTION("Only a full guest overwrite releases the view before staging") {
        f.bytes[0x41] = 0x73;
        f.cache->WriteMemory(cpu_addr + 0x41, 1);
        CHECK(Access::Find(*f.cache, gob, gpu_addr) == NULL_IMAGE_ID);
        CHECK(f.runtime.downloads == 0);
        CHECK(f.bytes[0x40] == 0);
        CHECK(f.bytes[0x41] == 0x73);

        const auto guest_size = CalculateGuestSizeInBytes(native);
        const std::vector<u8> replacement(guest_size, 0x73);
        f.memory->WriteBlockUnsafe(gpu_addr, replacement.data(), replacement.size());
        f.cache->WriteMemory(cpu_addr, guest_size);

        CHECK(Access::Find(*f.cache, gob, gpu_addr) == hinted);
        CHECK(f.runtime.downloads == 0);
        CHECK(f.bytes[0x40] == 0x73);
    }
}
