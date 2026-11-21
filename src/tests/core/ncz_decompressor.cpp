// SPDX-FileCopyrightText: Copyright 2026 Lichii contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include "core/crypto/aes_util.h"
#include "core/crypto/key_manager.h"
#include "core/crypto/ncz_cache.h"
#include "core/crypto/ncz_decompressor.h"
#include "core/file_sys/vfs/vfs_vector.h"
#ifdef _WIN32
#include <windows.h>

#include <psapi.h>
#endif

namespace {
constexpr std::size_t Header = 0x4000;
constexpr std::size_t Payload = Header + 16 + 64;
struct Fixture {
    std::shared_ptr<FileSys::VectorVfsFile> source;
    std::vector<u8> expected;
};
void Put(std::vector<u8>& out, std::size_t offset, u64 value, std::size_t count = 8);
Fixture MakeSolid(std::size_t size = 0x3007, u64 crypto = 1);
void CheckRead(const FileSys::VirtualFile& file, const std::vector<u8>& expected,
               std::size_t offset, std::size_t length);
std::vector<u8> WideWindow(const std::vector<u8>& bytes, std::size_t payload = Payload);
} // namespace

TEST_CASE("NCZ discovery accepts solid headers without decoding payloads", "[ncz]") {
    class CountingFile final : public FileSys::VectorVfsFile {
    public:
        using FileSys::VectorVfsFile::VectorVfsFile;
        std::size_t Read(u8* buffer, std::size_t length, std::size_t offset) const override {
            bytes_read += length;
            return FileSys::VectorVfsFile::Read(buffer, length, offset);
        }
        mutable std::size_t bytes_read{};
    };
    auto fixture = MakeSolid(0x100007);
    auto source = std::make_shared<CountingFile>(fixture.source->ReadAllBytes(), "synthetic.ncz");
    REQUIRE(Core::Crypto::IsSupportedNCZ(source));
    CHECK(source->bytes_read <= 256);
    CHECK_FALSE(Core::Crypto::IsBlockCompressedNCZ(source));
    auto bytes = source->ReadAllBytes();
    bytes[Payload] ^= 0xff;
    CHECK_FALSE(
        Core::Crypto::IsSupportedNCZ(std::make_shared<FileSys::VectorVfsFile>(std::move(bytes))));
}

TEST_CASE("NCZBLOCK playback remains supported", "[ncz]") {
    auto fixture = MakeSolid(20000);
    auto bytes = fixture.source->ReadAllBytes();
    bytes.resize(Payload + 24 + 8);
    const std::array<u8, 8> magic{'N', 'C', 'Z', 'B', 'L', 'O', 'C', 'K'};
    std::copy(magic.begin(), magic.end(), bytes.begin() + Payload);
    bytes[Payload + 8] = 2;
    bytes[Payload + 9] = 1;
    bytes[Payload + 11] = 14;
    Put(bytes, Payload + 12, 2, 4);
    Put(bytes, Payload + 16, 20000);
    for (unsigned i = 0; i < 2; ++i) {
        const std::size_t offset = i * 16384;
        const std::size_t length = std::min<std::size_t>(16384, 20000 - offset);
        std::vector<u8> block(ZSTD_compressBound(length));
        const auto encoded = ZSTD_compress(block.data(), block.size(),
                                           fixture.expected.data() + Header + offset, length, 3);
        REQUIRE_FALSE(ZSTD_isError(encoded));
        Put(bytes, Payload + 24 + 4 * i, encoded, 4);
        bytes.insert(bytes.end(), block.begin(), block.begin() + encoded);
    }
    SECTION("valid blocks") {
        auto source = std::make_shared<FileSys::VectorVfsFile>(std::move(bytes));
        REQUIRE(Core::Crypto::IsSupportedNCZ(source));
        REQUIRE(Core::Crypto::IsBlockCompressedNCZ(source));
        auto file = Core::Crypto::DecompressNCZ(source, "synthetic.nca");
        REQUIRE(file);
        CheckRead(file, fixture.expected, Header + 16380, 100);
        CheckRead(file, fixture.expected, Header + 1, 40);
        CheckRead(file, fixture.expected, Header + 19990, 40);
    }
    SECTION("invalid block exponent") {
        bytes[Payload + 11] = 255;
        CHECK_FALSE(Core::Crypto::IsSupportedNCZ(
            std::make_shared<FileSys::VectorVfsFile>(std::move(bytes))));
    }
    SECTION("truncated block-size table") {
        Put(bytes, Payload + 12, 0x7fffffff, 4);
        CHECK_FALSE(Core::Crypto::IsSupportedNCZ(
            std::make_shared<FileSys::VectorVfsFile>(std::move(bytes))));
    }
    SECTION("block count that does not match the decompressed size") {
        // With 4 GiB blocks, 20000 bytes fit in one block; the second entry made the reader
        // allocate a whole 4 GiB block for the first one.
        bytes[Payload + 11] = 32;
        CHECK_FALSE(Core::Crypto::IsSupportedNCZ(
            std::make_shared<FileSys::VectorVfsFile>(std::move(bytes))));
    }
    SECTION("decompressed size that needs more blocks than the table holds") {
        Put(bytes, Payload + 16, 40000);
        CHECK_FALSE(Core::Crypto::IsSupportedNCZ(
            std::make_shared<FileSys::VectorVfsFile>(std::move(bytes))));
    }
    SECTION("section that overflows or starts before the file") {
        // Block mode used to skip the solid stream's section checks: these opened as files of
        // about 2^63 bytes, or with a section before the header.
        const std::array<std::pair<u64, u64>, 3> bad{{
            {0x7FFFFFFFFFFFFFF0, 0x100},
            {static_cast<u64>(-0x1000), 0x5000},
            {Header, 0x7FFFFFFFFFFF0000},
        }};
        for (const auto& [offset, size] : bad) {
            auto copy = bytes;
            Put(copy, Header + 16, offset);
            Put(copy, Header + 24, size);
            auto source = std::make_shared<FileSys::VectorVfsFile>(std::move(copy));
            CHECK_FALSE(Core::Crypto::IsSupportedNCZ(source));
            CHECK(Core::Crypto::DecompressNCZ(source, "synthetic.nca") == nullptr);
        }
    }
}

namespace {
void Put(std::vector<u8>& out, std::size_t offset, u64 value, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        out[offset + i] = static_cast<u8>(value >> (i * 8));
    }
}

u8 Pattern(std::size_t offset) {
    return static_cast<u8>(offset * 37 + 11);
}

Fixture MakeSolid(std::size_t size, u64 crypto) {
    std::vector<u8> plain(size);
    for (std::size_t i = 0; i < size; ++i) {
        plain[i] = Pattern(i);
    }
    std::vector<u8> bytes(Payload, 0);
    std::fill_n(bytes.begin(), Header, 0xa5);
    const std::array<u8, 8> magic{'N', 'C', 'Z', 'S', 'E', 'C', 'T', 'N'};
    std::copy(magic.begin(), magic.end(), bytes.begin() + Header);
    Put(bytes, Header + 8, 1);
    Put(bytes, Header + 16, Header);
    Put(bytes, Header + 24, size);
    Put(bytes, Header + 32, crypto);
    const Core::Crypto::Key128 key{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    Core::Crypto::Key128 counter{0x12, 0x34, 0x56, 0x78};
    std::copy(key.begin(), key.end(), bytes.begin() + Header + 48);
    std::copy(counter.begin(), counter.end(), bytes.begin() + Header + 64);
    std::vector<u8> compressed(ZSTD_compressBound(size));
    const auto encoded = ZSTD_compress(compressed.data(), compressed.size(), plain.data(), size, 3);
    REQUIRE_FALSE(ZSTD_isError(encoded));
    compressed.resize(encoded);
    bytes.insert(bytes.end(), compressed.begin(), compressed.end());
    std::vector<u8> expected(Header, 0xa5);
    if (crypto == 3 || crypto == 4) {
        // Fixture ciphertext is generated as one continuous AES-CTR operation, independent
        // of the decoder's chunking, cache, and per-read counter calculation.
        counter[14] = 4;
        Core::Crypto::AESCipher<Core::Crypto::Key128> cipher(key, Core::Crypto::Mode::CTR);
        cipher.SetIV(counter);
        cipher.Transcode(plain.data(), plain.size(), plain.data(), Core::Crypto::Op::Encrypt);
    }
    expected.insert(expected.end(), plain.begin(), plain.end());
    return {std::make_shared<FileSys::VectorVfsFile>(std::move(bytes), "synthetic.ncz"),
            std::move(expected)};
}

void CheckRead(const FileSys::VirtualFile& file, const std::vector<u8>& expected,
               std::size_t offset, std::size_t length) {
    std::vector<u8> out(length, 0xcc);
    const auto want = offset >= expected.size() ? 0 : std::min(length, expected.size() - offset);
    REQUIRE(file->Read(out.data(), length, offset) == want);
    CHECK(std::equal(out.begin(), out.begin() + want,
                     expected.begin() + std::min(offset, expected.size())));
}

// Keeps the genuine blocks, but declares an unknown-size frame with a 128 MiB window.
std::vector<u8> WideWindow(const std::vector<u8>& bytes, std::size_t payload) {
    const auto header_length =
        ZSTD_frameHeaderSize(bytes.data() + payload, bytes.size() - payload);
    REQUIRE_FALSE(ZSTD_isError(header_length));
    std::vector<u8> patched(bytes.begin(), bytes.begin() + payload);
    const std::array<u8, 6> header{0x28, 0xb5, 0x2f, 0xfd, 0x00, 0x88};
    patched.insert(patched.end(), header.begin(), header.end());
    patched.insert(patched.end(), bytes.begin() + payload + header_length, bytes.end());
    return patched;
}
} // namespace

TEST_CASE("Solid NCZ playback supports backward and unaligned random reads", "[ncz]") {
    auto fixture = MakeSolid(0x210007, 3);
    auto file = Core::Crypto::DecompressNCZ(fixture.source, "synthetic.nca");
    REQUIRE(file);
    CHECK(file->GetSize() == fixture.expected.size());
    for (auto offset : {Header + 0x200003, Header + 3, Header - 3, Header + 0x100001,
                        std::size_t{0}, fixture.expected.size() - 1}) {
        CheckRead(file, fixture.expected, offset, 33);
    }
    CheckRead(file, fixture.expected, file->GetSize(), 8);
}

TEST_CASE("Solid NCZ installation preserves exact bytes", "[ncz]") {
    auto fixture = MakeSolid(0x130007, 4);
    auto file = Core::Crypto::DecompressNCZForInstall(fixture.source, "synthetic.nca");
    REQUIRE(file);
    CheckRead(file, fixture.expected, 0, fixture.expected.size());
}

TEST_CASE("Solid NCZ rejects negative and overflowing section bounds", "[ncz]") {
    auto fixture = MakeSolid();
    auto bytes = fixture.source->ReadAllBytes();
    SECTION("negative offset") {
        Put(bytes, Header + 16, std::numeric_limits<u64>::max());
    }
    SECTION("negative size") {
        Put(bytes, Header + 24, std::numeric_limits<u64>::max());
    }
    SECTION("overflowing end") {
        Put(bytes, Header + 24, std::numeric_limits<s64>::max());
    }
    SECTION("unsupported crypto") {
        Put(bytes, Header + 32, 2);
    }
    CHECK_FALSE(Core::Crypto::DecompressNCZForInstall(
        std::make_shared<FileSys::VectorVfsFile>(std::move(bytes)), "synthetic.nca"));
}

TEST_CASE("Solid NCZ rejects corrupt or truncated streams and retains failure", "[ncz]") {
    auto fixture = MakeSolid();
    auto bytes = fixture.source->ReadAllBytes();
    SECTION("truncated frame") {
        bytes.pop_back();
    }
    SECTION("invalid magic") {
        bytes[Payload] ^= 0xff;
    }
    SECTION("declared output too long") {
        Put(bytes, Header + 24, 0x3017);
    }
    SECTION("declared output too short") {
        Put(bytes, Header + 24, 0x3000);
    }
    SECTION("trailing compressed input") {
        bytes.push_back(0);
    }
    auto file = Core::Crypto::DecompressNCZForInstall(
        std::make_shared<FileSys::VectorVfsFile>(std::move(bytes)), "synthetic.nca");
    if (file) {
        std::array<u8, 16> out{};
        REQUIRE(file->Read(out.data(), out.size(), file->GetSize() - out.size()) == 0);
        CHECK(file->Read(out.data(), out.size(), 0) == 0);
    }
}

TEST_CASE("Solid NCZ bounds the Zstandard window", "[ncz]") {
    auto fixture = MakeSolid();
    auto bytes = fixture.source->ReadAllBytes();
    bytes.resize(Payload);
    // A standard frame with a 256 MiB window, above this reader's 128 MiB limit.
    const std::array<u8, 9> frame{0x28, 0xb5, 0x2f, 0xfd, 0x00, 0x90, 0x01, 0x00, 0x00};
    bytes.insert(bytes.end(), frame.begin(), frame.end());
    auto file = Core::Crypto::DecompressNCZForInstall(
        std::make_shared<FileSys::VectorVfsFile>(std::move(bytes)), "synthetic.nca");
    if (file) {
        std::array<u8, 1> out{};
        CHECK(file->Read(out.data(), 1, Header) == 0);
    }
}

TEST_CASE("Concurrent solid NCZ reads keep the stream and cache coherent", "[ncz]") {
    auto fixture = MakeSolid(0x300007, 3);
    auto file = Core::Crypto::DecompressNCZ(fixture.source, "synthetic.nca");
    REQUIRE(file);
    std::vector<std::future<bool>> readers;
    for (std::size_t index = 0; index < 8; ++index) {
        readers.push_back(std::async(std::launch::async, [&, index] {
            const auto offset = Header + index * 0x60001;
            std::array<u8, 127> out{};
            return file->Read(out.data(), out.size(), offset) == out.size() &&
                   std::equal(out.begin(), out.end(), fixture.expected.begin() + offset);
        }));
    }
    for (auto& reader : readers) {
        CHECK(reader.get());
    }
}

TEST_CASE("Solid NCZ cache IO faults invalidate cached prefixes", "[ncz]") {
    struct Control {
        bool fail_read{};
        bool fail_write{};
    };
    class FaultCache final : public Core::Crypto::NczCache {
    public:
        explicit FaultCache(std::shared_ptr<Control> control_) : control(std::move(control_)) {}
        bool Write(std::span<const u8> bytes, u64 offset) override {
            if (control->fail_write) {
                Core::Crypto::NczCache::Write(bytes.first(bytes.size() / 2), offset);
                return false;
            }
            return Core::Crypto::NczCache::Write(bytes, offset);
        }
        std::size_t Read(std::span<u8> bytes, u64 offset) override {
            const auto count = Core::Crypto::NczCache::Read(bytes, offset);
            return control->fail_read && count > 0 ? count - 1 : count;
        }

    private:
        std::shared_ptr<Control> control;
    };
    auto control = std::make_shared<Control>();
    auto fixture = MakeSolid(0x300007);
    auto owned = std::make_unique<FaultCache>(control);
    const auto* cache = owned.get();
    auto file = Core::Crypto::DecompressNCZ(fixture.source, "synthetic.nca", std::move(owned));
    REQUIRE(file);
    std::array<u8, 7> out{};
    REQUIRE(file->Read(out.data(), out.size(), Header) == out.size());
    SECTION("partial cache write after a valid prefix") {
        control->fail_write = true;
    }
    SECTION("short cache read after a valid prefix") {
        control->fail_read = true;
    }
    REQUIRE(file->Read(out.data(), out.size(), Header + 0x200000) == 0);
    CHECK_FALSE(cache->IsOpen());
    CHECK_FALSE(file->IsReadable());
    CHECK(file->Read(out.data(), out.size(), 0) == 0);
    CHECK(file->Read(out.data(), out.size(), Header) == 0);
}

TEST_CASE("Solid NCZ handles unaligned encryption section transitions", "[ncz]") {
    auto fixture = MakeSolid(0x2007);
    auto bytes = fixture.source->ReadAllBytes();
    bytes.insert(bytes.begin() + Payload, 64, 0);
    Put(bytes, Header + 8, 2);
    Put(bytes, Header + 24, 17);
    Put(bytes, Payload, Header + 17);
    Put(bytes, Payload + 8, 0x2007 - 17);
    Put(bytes, Payload + 16, 3);
    Core::Crypto::Key128 key{};
    key.fill(0x2b);
    std::copy(key.begin(), key.end(), bytes.begin() + Payload + 32);
    Core::Crypto::Key128 iv{};
    iv[14] = 4;
    iv[15] = 1; // (0x4000 + 17) / 16.
    Core::Crypto::AESCipher<Core::Crypto::Key128> cipher(key, Core::Crypto::Mode::CTR);
    cipher.SetIV(iv);
    std::vector<u8> reference(1, 0);
    reference.insert(reference.end(), fixture.expected.begin() + Header + 17,
                     fixture.expected.end());
    cipher.Transcode(reference.data(), reference.size(), reference.data(),
                     Core::Crypto::Op::Encrypt);
    std::copy(reference.begin() + 1, reference.end(), fixture.expected.begin() + Header + 17);
    SECTION("valid sections") {
        auto file = Core::Crypto::DecompressNCZ(
            std::make_shared<FileSys::VectorVfsFile>(std::move(bytes)), "synthetic.nca");
        REQUIRE(file);
        CheckRead(file, fixture.expected, Header + 7, 100);
        CheckRead(file, fixture.expected, Header + 0x2000, 7);
        CheckRead(file, fixture.expected, Header + 15, 4);
    }
    SECTION("overlapping sections") {
        Put(bytes, Payload, Header + 16);
        CHECK_FALSE(Core::Crypto::DecompressNCZ(
            std::make_shared<FileSys::VectorVfsFile>(std::move(bytes)), "synthetic.nca"));
    }
    SECTION("missing bytes between sections") {
        Put(bytes, Payload, Header + 18);
        CHECK_FALSE(Core::Crypto::DecompressNCZ(
            std::make_shared<FileSys::VectorVfsFile>(std::move(bytes)), "synthetic.nca"));
    }
    SECTION("future empty section cannot consume bytes with another key") {
        bytes.insert(bytes.begin() + Payload, 64, 0);
        Put(bytes, Header + 8, 3);
        Put(bytes, Payload, Header + 0x1000);
        Put(bytes, Payload + 16, 3);
        CHECK_FALSE(Core::Crypto::DecompressNCZ(
            std::make_shared<FileSys::VectorVfsFile>(std::move(bytes)), "synthetic.nca"));
    }
}

TEST_CASE("Solid NCZ idle readers give up their decoder budget", "[ncz][resource]") {
    auto fixture = MakeSolid(2 * 1024 * 1024);
    auto source =
        std::make_shared<FileSys::VectorVfsFile>(WideWindow(fixture.source->ReadAllBytes()));
    // The budget holds three 128 MiB windows, so later readers release earlier ones.
    std::vector<FileSys::VirtualFile> readers;
    for (std::size_t i = 0; i < 8; ++i) {
        readers.push_back(Core::Crypto::DecompressNCZ(source, "synthetic.nca"));
        REQUIRE(readers.back());
        CheckRead(readers.back(), fixture.expected, Header + i, 1);
    }
    for (std::size_t i = 0; i < readers.size(); ++i) {
        CHECK(readers[i]->IsReadable());
        CheckRead(readers[i], fixture.expected, fixture.expected.size() - 1000 - i, 1000);
    }
}

TEST_CASE("A released solid NCZ reader resumes with identical bytes", "[ncz][resource]") {
    class StartCountingFile final : public FileSys::VectorVfsFile {
    public:
        using FileSys::VectorVfsFile::VectorVfsFile;
        std::size_t Read(u8* buffer, std::size_t length, std::size_t offset) const override {
            starts += offset == Payload + 64;
            return FileSys::VectorVfsFile::Read(buffer, length, offset);
        }
        mutable std::size_t starts{};
    };
    // A plain section ending unaligned, then a CTR section, so the frontier lands mid-block.
    constexpr std::size_t size = 0x200007;
    constexpr std::size_t split = 0x100011;
    auto fixture = MakeSolid(size);
    auto bytes = fixture.source->ReadAllBytes();
    bytes.insert(bytes.begin() + Payload, 64, 0);
    Put(bytes, Header + 8, 2);
    Put(bytes, Header + 24, split);
    Put(bytes, Payload, Header + split);
    Put(bytes, Payload + 8, size - split);
    Put(bytes, Payload + 16, 3);
    Core::Crypto::Key128 key{};
    key.fill(0x2b);
    std::copy(key.begin(), key.end(), bytes.begin() + Payload + 32);
    Core::Crypto::Key128 iv{};
    iv[13] = 0x01;
    iv[14] = 0x04;
    iv[15] = 0x01; // (0x4000 + 0x100011) / 16.
    Core::Crypto::AESCipher<Core::Crypto::Key128> cipher(key, Core::Crypto::Mode::CTR);
    cipher.SetIV(iv);
    std::vector<u8> reference(1, 0);
    reference.insert(reference.end(), fixture.expected.begin() + Header + split,
                     fixture.expected.end());
    cipher.Transcode(reference.data(), reference.size(), reference.data(),
                     Core::Crypto::Op::Encrypt);
    std::copy(reference.begin() + 1, reference.end(), fixture.expected.begin() + Header + split);
    bytes = WideWindow(bytes, Payload + 64);

    auto counted = std::make_shared<StartCountingFile>(bytes, "synthetic.ncz");
    auto file = Core::Crypto::DecompressNCZ(counted, "synthetic.nca");
    REQUIRE(file);
    const auto opened = counted->starts;
    CheckRead(file, fixture.expected, Header + 0x100000, 8);
    REQUIRE(counted->starts == opened + 1);

    auto source = std::make_shared<FileSys::VectorVfsFile>(std::move(bytes));
    std::vector<FileSys::VirtualFile> others;
    for (std::size_t i = 0; i < 3; ++i) {
        others.push_back(Core::Crypto::DecompressNCZ(source, "synthetic.nca"));
        REQUIRE(others.back());
        CheckRead(others.back(), fixture.expected, Header, 1);
    }
    CHECK(file->IsReadable());
    CheckRead(file, fixture.expected, Header + 3, split - 3);
    CHECK(counted->starts == opened + 1);
    CheckRead(file, fixture.expected, Header + split - 9, 0x100);
    CHECK(counted->starts == opened + 2);
    CheckRead(file, fixture.expected, fixture.expected.size() - 7, 7);
}

TEST_CASE("A solid NCZ reader recovers once a full disk or the decoder budget clears",
          "[ncz][resource]") {
    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        bool full{};
        bool hold{};
        std::size_t held{};
        std::size_t writes{};
    };
    // Fails every write while the disk is full, and holds payload writes (and so the decoder
    // of the reader that makes them) while hold is set.
    class GateCache final : public Core::Crypto::NczCache {
    public:
        explicit GateCache(std::shared_ptr<Gate> gate_) : gate(std::move(gate_)) {}
        bool Write(std::span<const u8> bytes, u64 offset) override {
            {
                std::unique_lock lock{gate->mutex};
                ++gate->writes;
                if (offset >= Header && gate->hold) {
                    ++gate->held;
                    gate->changed.notify_all();
                    gate->changed.wait(lock, [this] { return !gate->hold; });
                }
                if (gate->full)
                    return false;
            }
            return Core::Crypto::NczCache::Write(bytes, offset);
        }

    private:
        std::shared_ptr<Gate> gate;
    };
    auto gate = std::make_shared<Gate>();

    SECTION("full disk") {
        auto fixture = MakeSolid(0x300007);
        auto file = Core::Crypto::DecompressNCZ(fixture.source, "synthetic.nca",
                                                std::make_unique<GateCache>(gate));
        REQUIRE(file);
        CheckRead(file, fixture.expected, Header, 7);
        gate->full = true;
        std::array<u8, 7> out{};
        REQUIRE(file->Read(out.data(), out.size(), Header + 0x200000) == 0);
        // The first read after the failure tries again at once, later ones wait.
        auto writes = gate->writes;
        CHECK(file->Read(out.data(), out.size(), Header) == 0);
        CHECK(gate->writes > writes);
        writes = gate->writes;
        CHECK(file->Read(out.data(), out.size(), Header) == 0);
        CHECK(gate->writes == writes);
        gate->full = false;
        std::this_thread::sleep_for(std::chrono::milliseconds{300});
        CHECK_FALSE(file->IsReadable());
        CheckRead(file, fixture.expected, Header + 0x200000, 7);
        CHECK(file->IsReadable());
        CheckRead(file, fixture.expected, Header, 7);
    }

    SECTION("decoder budget held by decoding readers") {
        auto fixture = MakeSolid(2 * 1024 * 1024);
        auto source =
            std::make_shared<FileSys::VectorVfsFile>(WideWindow(fixture.source->ReadAllBytes()));
        // The budget holds three 128 MiB windows; these three hold them while they decode.
        std::vector<FileSys::VirtualFile> busy;
        for (std::size_t i = 0; i < 3; ++i) {
            busy.push_back(Core::Crypto::DecompressNCZ(source, "synthetic.nca",
                                                       std::make_unique<GateCache>(gate)));
            REQUIRE(busy.back());
        }
        auto file = Core::Crypto::DecompressNCZ(source, "synthetic.nca");
        REQUIRE(file);
        gate->hold = true;
        std::vector<std::future<std::size_t>> decoding;
        struct Release {
            Gate& gate;
            void operator()() const {
                std::scoped_lock lock{gate.mutex};
                gate.hold = false;
                gate.changed.notify_all();
            }
            ~Release() {
                (*this)();
            }
        } release{*gate};
        for (const auto& reader : busy) {
            decoding.push_back(std::async(std::launch::async, [reader] {
                std::array<u8, 1> out{};
                return reader->Read(out.data(), out.size(), Header);
            }));
        }
        {
            std::unique_lock lock{gate->mutex};
            REQUIRE(gate->changed.wait_for(lock, std::chrono::seconds{10},
                                           [&] { return gate->held == busy.size(); }));
        }
        std::array<u8, 1> out{};
        REQUIRE(file->Read(out.data(), out.size(), Header) == 0);
        release();
        for (auto& reader : decoding) {
            CHECK(reader.get() == out.size());
        }
        CHECK_FALSE(file->IsReadable());
        CheckRead(file, fixture.expected, Header, 1);
        CHECK(file->IsReadable());
        CheckRead(file, fixture.expected, fixture.expected.size() - 1000, 1000);
    }
}

TEST_CASE("A solid NCZ reader gives up on a header it cannot read or cache", "[ncz][resource]") {
    struct Disk {
        std::atomic<bool> full{};
        std::atomic<std::size_t> writes{};
    };
    class DiskCache final : public Core::Crypto::NczCache {
    public:
        explicit DiskCache(std::shared_ptr<Disk> disk_) : disk(std::move(disk_)) {}
        bool Write(std::span<const u8> bytes, u64 offset) override {
            ++disk->writes;
            return !disk->full && Core::Crypto::NczCache::Write(bytes, offset);
        }

    private:
        std::shared_ptr<Disk> disk;
    };
    auto disk = std::make_shared<Disk>();
    auto fixture = MakeSolid(0x300007);
    const auto bytes = fixture.source->ReadAllBytes();
    auto file = Core::Crypto::DecompressNCZ(fixture.source, "synthetic.nca",
                                            std::make_unique<DiskCache>(disk));
    REQUIRE(file);
    CheckRead(file, fixture.expected, Header, 7);
    disk->full = true;
    std::array<u8, 7> out{};
    REQUIRE(file->Read(out.data(), out.size(), Header + 0x200000) == 0);
    // When the reader would try again if it hadn't given up.
    std::chrono::milliseconds next_try{};

    SECTION("cache that keeps refusing the header") {
        // Each try waits out the backoff of the one before: 0, 250, 500 and 1000 ms.
        for (const auto wait : {0, 300, 600, 1100}) {
            std::this_thread::sleep_for(std::chrono::milliseconds{wait});
            const auto writes = disk->writes.load();
            CHECK(file->Read(out.data(), out.size(), 0) == 0);
            CHECK(disk->writes == writes + 1);
        }
        next_try = std::chrono::milliseconds{2100};
    }
    SECTION("source shorter than the header") {
        disk->full = false;
        fixture.source->Resize(Header / 2);
        CHECK(file->Read(out.data(), out.size(), 0) == 0);
        fixture.source->Assign(bytes);
        next_try = std::chrono::milliseconds{300};
    }
    disk->full = false;
    std::this_thread::sleep_for(next_try);
    const auto writes = disk->writes.load();
    CHECK_FALSE(file->IsReadable());
    CHECK(file->Read(out.data(), out.size(), 0) == 0);
    CHECK(disk->writes == writes);
    auto reopened = Core::Crypto::DecompressNCZ(fixture.source, "synthetic.nca");
    REQUIRE(reopened);
    CheckRead(reopened, fixture.expected, Header + 0x200000, 7);
}

#ifdef _WIN32
TEST_CASE("Solid NCZ large decoded data does not become private process memory",
          "[ncz][resource]") {
    constexpr std::size_t size = 192 * 1024 * 1024;
    auto fixture = MakeSolid(16);
    auto bytes = fixture.source->ReadAllBytes();
    bytes.resize(Payload);
    Put(bytes, Header + 24, size);
    auto* compressor = ZSTD_createCStream();
    REQUIRE(compressor);
    REQUIRE_FALSE(ZSTD_isError(ZSTD_initCStream(compressor, 3)));
    std::vector<u8> input(64 * 1024);
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = Pattern(i);
    std::vector<u8> output(ZSTD_CStreamOutSize());
    for (std::size_t pos = 0; pos < size; pos += input.size()) {
        ZSTD_inBuffer in{input.data(), input.size(), 0};
        const auto directive = pos + input.size() == size ? ZSTD_e_end : ZSTD_e_continue;
        std::size_t remaining;
        do {
            ZSTD_outBuffer out{output.data(), output.size(), 0};
            remaining = ZSTD_compressStream2(compressor, &out, &in, directive);
            REQUIRE_FALSE(ZSTD_isError(remaining));
            bytes.insert(bytes.end(), output.begin(), output.begin() + out.pos);
        } while (in.pos < in.size || (directive == ZSTD_e_end && remaining != 0));
    }
    ZSTD_freeCStream(compressor);
    auto file = Core::Crypto::DecompressNCZ(
        std::make_shared<FileSys::VectorVfsFile>(std::move(bytes)), "synthetic.nca");
    REQUIRE(file);
    PROCESS_MEMORY_COUNTERS_EX before{};
    before.cb = sizeof(before);
    REQUIRE(GetProcessMemoryInfo(
        GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&before), sizeof(before)));
    std::array<u8, 37> out{};
    REQUIRE(file->Read(out.data(), out.size(), Header + size - out.size()) == out.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        CHECK(out[i] == Pattern(size - out.size() + i));
    }
    PROCESS_MEMORY_COUNTERS_EX after{};
    after.cb = sizeof(after);
    REQUIRE(GetProcessMemoryInfo(
        GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&after), sizeof(after)));
    CHECK(after.PrivateUsage < before.PrivateUsage + 64 * 1024 * 1024);
}
#endif
