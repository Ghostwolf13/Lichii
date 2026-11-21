// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

#include "common/common_types.h"
#include "video_core/gpu_split.h"
#include "video_core/native_qmd.h"
#include "video_core/nx2.h"
#include "video_core/nx2_drain_check.h"

namespace VideoCommon {

/// The QMD scheduling of every channel (Tegra::NativeQmdScheduler), run on the split GPU thread's
/// front as its back will run it, on a table of its own: what the launches release is known when
/// they are sent, for the front's view of the semaphore words (GpuSplit::NoteSemaphoreWrite).
///
/// The table is the back's from the start: an engine's front takes it over with the back drained
/// when it takes the engine's macros over (TakeOver). It stays the back's as long as the front
/// reads the descriptors the back will read. The front reads one when it sends a signal, the back
/// when it runs it, both from guest memory without the GPU caches. The front takes a descriptor:
/// - from the signal's own inline QMD, which the back writes right before;
/// - from memory, once the back ran every write sent there (GpuSplit::MemoryWritesTarget: the
///   writes placed in its granule and the unplaced ones), on pages GPU work never wrote
///   (Nx2MayBeGpuWritten);
/// - from the inline QMD a LOAD_INLINE_QMD_DATA still queued writes there, when that is the last
///   write sent there (NoteInlineWrite);
/// - from memory with the inline uploads still queued that rewrite part of it on top, when those
///   are the last writes sent there and the back ran every write before them (NoteUpload).
/// No change of the GPU's mappings may be queued either (GpuSplit::LastMappingChange). Memory holds
/// what the back finds there only while the back has not run the signal's entry yet: the front
/// sends and values it under a GpuSplit::PublishHold.
///
/// Where the back has yet to run a write the front can't read past (into a QMD the signal reads,
/// or anywhere, or to the mappings), the signal waits, with what it changed in the table taken
/// back (NativeQmdScheduler::BeginAttempt), and every signal sent after it waits behind it: the
/// table stays the back's as of the signals valued. The front values the waiting ones in order
/// once the back ran what the first waits for (CatchUp). Late, the back may have run the signal
/// and what followed it, so memory holds what the back read only where nothing sent after the
/// signal writes the QMD, and no release of the waiting signals writes a QMD read late. Their
/// releases are placed at their signals' positions, not noted. Where the front can't read what
/// the back read at all (a QMD on a GPU-written page, written after the signal, released into,
/// or the mappings changed after it), it loses the table: it stops valuing, and takes the back's
/// table over the next time the back has run everything sent, between commands or at a QMD
/// method (MaybeTakeOver).
///
/// `Memory`, the channel's GPU address space as the back reads it, has:
/// - bool IsMapped(GPUVAddr, u64), void Read(GPUVAddr, void*, u64), u32 ReadWord(GPUVAddr) (as
///   MemoryManager::Read<u32>), bool InSyncpointShim(GPUVAddr, u64);
/// - std::optional<u64> Device(GPUVAddr), bool GpuWritten(u64 device, u64 size);
/// - u64 WritesTarget(const GpuSplit&, GPUVAddr, u64), void NoteWrite(GpuSplit&, GPUVAddr, u64),
///   void NoteWriteAt(GpuSplit&, GPUVAddr, u64 size, u64 position), and for the word of a one-word
///   release u64 OtherWritesTarget(const GpuSplit&, GPUVAddr) and
///   void NoteSemaphoreRelease(GpuSplit&, GPUVAddr);
/// - const void* Id(): the address space's, the same for every Memory of it.
class FrontQmdView {
public:
    using Descriptor = Tegra::NativeQmdScheduler::Descriptor;

    /// The view the compute engines of every channel feed: they share the back's table.
    static FrontQmdView& Get() {
        static FrontQmdView view;
        return view;
    }

    /// The back ran everything sent and runs nothing (the split drained, or it waits for work):
    /// takes over the table `back` shares with the other engines.
    void TakeOver(const Tegra::NativeQmdScheduler& back) {
        Count(g_acquire_stats.qmd_takeovers);
        scheduler.CopyStates(back);
        synced = true;
        ClearWaiting();
    }

    /// The front, between commands and before QMD methods: takes the back's table over if the
    /// view lost it or signals wait, and the back has run everything sent.
    void MaybeTakeOver(const GpuSplit& split, const Tegra::NativeQmdScheduler& back) {
        if (!Current() && split.Ran(split.Position())) {
            TakeOver(back);
        }
    }
    void MaybeTakeOver(const GpuSplit& split) {
        MaybeTakeOver(split, back_table);
    }

    /// The engines schedule QMDs the front does not see (the split stopped).
    void Lose() {
        synced = false;
        inline_writes_count = 0;
        ClearWaiting();
        checks.clear();
    }

    [[nodiscard]] bool Synced() const {
        return synced;
    }
    /// Whether the table is the back's as of everything sent: synced, and no signal waits.
    [[nodiscard]] bool Current() const {
        return synced && waiting.empty();
    }

    /// The longest inline upload the view keeps (NoteUpload).
    static constexpr u64 MaxUploadBytes = 2048;

    /// A compute engine's front sent all of an inline upload of one linear line: once the back ran
    /// the entries before `position`, `bytes` are in guest memory from `address` of the address
    /// space `memory` (Memory::Id). Until then the front reads a QMD they cover from here, with
    /// memory under the rest, if the upload is the last write sent there (ReadPatched).
    /// `prior`: the last write sent over the upload's granules before it (GpuSplit::
    /// MemoryWritesTarget as its data began).
    void NoteUpload(const void* memory, GPUVAddr address, std::span<const u8> bytes, u64 position,
                    u64 prior) {
        if (bytes.empty() || bytes.size() > MaxUploadBytes) {
            return;
        }
        KeptUpload& kept = uploads[uploads_count % uploads.size()];
        ++uploads_count;
        kept.memory = memory;
        kept.address = address;
        kept.size = bytes.size();
        kept.position = position;
        kept.prior = prior;
        std::memcpy(kept.bytes.data(), bytes.data(), bytes.size());
    }

    /// LOAD_INLINE_QMD_DATA was just sent: running it, the back writes `qmd` to the QMD at
    /// `device`. Until then the front reads it from here (ReadDescriptor).
    void NoteInlineWrite(const GpuSplit& split, u64 device, const Descriptor& qmd) {
        InlineWrite& written = inline_writes[inline_writes_count % inline_writes.size()];
        ++inline_writes_count;
        written.device = device;
        written.position = split.Position();
        written.qmd = qmd;
        written.overwritten = false;
    }

    /// The scheduling of PCAS action `argument` on the QMD at `address` that the back runs at the
    /// entry just sent (KeplerCompute::ProcessNativeMethod); `inline_qmd`: the QMD it writes there
    /// right before. Notes what the launches it runs release, or has the signal wait. The entry
    /// must still be unpublished (GpuSplit::PublishHold): the back must not have run it.
    template <typename Memory>
    void Signal(GpuSplit& split, const Memory& memory, GPUVAddr address, u32 argument,
                const Descriptor* inline_qmd) {
        split.NoteCacheWrite(); // The dispatches' shader stores
        const u64 position = split.Position();
        if (Nx2DrainCheck::g_enabled) {
            checks.emplace_back(); // This signal's, valued if Attempt values it now
        }
        if (synced) {
            CatchUp(split, memory);
        }
        if (!synced) {
            Count(g_acquire_stats.qmd_unvalued);
            split.NoteUnnotedReleases();
            return;
        }
        if (!waiting.empty()) {
            // Behind a signal still waiting.
            Wait(split, position, memory.Id(), address, argument, inline_qmd);
            return;
        }
        const Outcome outcome =
            Attempt(split, memory, position, address, argument, inline_qmd, false);
        if (outcome.valued) {
            Count(g_acquire_stats.qmd_valued);
            return;
        }
        if (outcome.retry_after) {
            retry_after = *outcome.retry_after;
            Wait(split, position, memory.Id(), address, argument, inline_qmd);
            return;
        }
        LoseTable(outcome.loss);
        split.NoteUnnotedReleases();
    }

    /// EDEN_NX2_DRAIN_CHECK: what the view read and noted for the signals of the entry just sent,
    /// and its table if that is the back's as of the entry (Nx2DrainCheck::Check).
    struct Checks {
        std::vector<Nx2DrainCheck::QmdSignal> signals;
        std::shared_ptr<const Tegra::NativeQmdScheduler> table;
    };
    [[nodiscard]] std::optional<Checks> TakeChecks() {
        if (checks.empty()) {
            return std::nullopt;
        }
        Checks taken{std::move(checks), nullptr};
        checks.clear();
        if (Current()) {
            auto table =
                std::make_shared<Tegra::NativeQmdScheduler>(Tegra::NativeQmdScheduler::OwnTable{});
            table->CopyStates(scheduler);
            taken.table = std::move(table);
        }
        return taken;
    }

private:
    struct Release {
        GPUVAddr address;
        u32 payload;
        u32 flags;
    };

    /// An inline QMD a LOAD_INLINE_QMD_DATA sent writes (NoteInlineWrite).
    struct InlineWrite {
        u64 device{};
        u64 position{};
        Descriptor qmd{};
        bool overwritten{}; ///< A release of the same entry writes into it after
    };

    /// A signal sent at `position` that waits to be valued (`inline_qmd` if `has_inline`).
    struct WaitingSignal {
        u64 position;
        const void* memory; ///< Memory::Id of the address space it reads
        GPUVAddr address;
        u32 argument;
        bool has_inline;
        Descriptor inline_qmd;
    };

    /// An inline upload (NoteUpload) the back has not run yet.
    struct KeptUpload {
        const void* memory{};
        GPUVAddr address{};
        u64 size{};
        u64 position{};
        u64 prior{}; ///< The last write over its granules before it
        std::array<u8, MaxUploadBytes> bytes{};
    };

    /// How an attempt to value a signal ended: valued, or taken back, to try again once the back
    /// ran `retry_after`, or for good (the view loses its table).
    struct Outcome {
        bool valued;
        std::optional<u64> retry_after;
        QmdLoss loss{}; ///< Why, if for good
    };

    /// Signals that wait, at most: a back that far behind leaves the view to its next takeover.
    static constexpr size_t MaxWaiting = 4096;

    /// The signal just sent at `position` waits, first or behind the others. None of its releases
    /// is noted: they are placed once it is valued.
    void Wait(GpuSplit& split, u64 position, const void* memory_id, GPUVAddr address, u32 argument,
              const Descriptor* inline_qmd) {
        split.NoteUnnotedReleases();
        if (waiting.size() >= MaxWaiting) {
            LoseTable(QmdLoss::TooMany);
            ClearWaiting();
            return;
        }
        Count(g_acquire_stats.qmd_waits);
        WaitingSignal& signal = waiting.emplace_back();
        signal.position = position;
        signal.memory = memory_id;
        signal.address = address;
        signal.argument = argument;
        signal.has_inline = inline_qmd != nullptr;
        if (inline_qmd != nullptr) {
            signal.inline_qmd = *inline_qmd;
        }
    }

    void ClearWaiting() {
        waiting.clear();
        late_reads.clear();
        retry_after = 0;
    }

    /// The view can't read what the back read: it stops valuing until the next takeover.
    void LoseTable(QmdLoss loss) {
        synced = false;
        Count(g_acquire_stats.qmd_losses[static_cast<size_t>(loss)]);
    }

    /// Values the waiting signals that read `memory` in order, from the first, as long as the back
    /// ran what the first waits for.
    template <typename Memory>
    void CatchUp(GpuSplit& split, const Memory& memory) {
        while (!waiting.empty()) {
            const WaitingSignal& first = waiting.front();
            if (first.memory != memory.Id() || !split.Ran(retry_after)) {
                return;
            }
            const Outcome outcome =
                Attempt(split, memory, first.position, first.address, first.argument,
                        first.has_inline ? &first.inline_qmd : nullptr, true);
            if (outcome.retry_after) {
                retry_after = *outcome.retry_after;
                return;
            }
            if (!outcome.valued) {
                LoseTable(outcome.loss);
                ClearWaiting();
                return;
            }
            Count(g_acquire_stats.qmd_late);
            waiting.pop_front();
            retry_after = 0;
        }
        // Every signal sent before the reads made late is valued: none of their releases waits.
        late_reads.clear();
    }

    /// Runs the signal sent at `position` on the table as the back runs it, and notes what its
    /// launches release, or places it if `late`. Takes it back where the front can't read a QMD
    /// as the back reads it.
    template <typename Memory>
    Outcome Attempt(GpuSplit& split, const Memory& memory, u64 position, GPUVAddr address,
                    u32 argument, const Descriptor* inline_qmd, bool late) {
        // The back finds the mappings every change sent before the signal made, and none after it.
        const u64 mapping_change = split.LastMappingChange();
        if (mapping_change > position) {
            return {false, std::nullopt, QmdLoss::Mappings};
        }
        if (!split.Ran(mapping_change)) {
            return {false, mapping_change};
        }
        releases.clear();
        attempt_reads.clear();
        checked_reads.clear();
        std::optional<Outcome> failure;
        const auto read = [&](u64 qmd_address, Descriptor& descriptor) {
            if (!memory.IsMapped(qmd_address, sizeof(descriptor))) {
                return false;
            }
            if (inline_qmd != nullptr && qmd_address == address) {
                descriptor = *inline_qmd;
            } else if (const auto unread =
                           ReadDescriptor(split, memory, qmd_address, position, descriptor)) {
                failure = unread;
                return false; // Stops the scheduling, which is taken back
            }
            // A release this signal ran before writes into the QMD.
            if (ReleasedInto(memory, qmd_address)) {
                failure = Outcome{false, std::nullopt, QmdLoss::ReleasedInto};
                return false;
            }
            attempt_reads.push_back(qmd_address);
            if (Nx2DrainCheck::g_enabled && !late) {
                checked_reads.push_back({qmd_address, descriptor});
            }
            return true;
        };
        const auto release = [&](u64 destination, u32 payload, u32 flags) {
            const bool four_words = (flags >> 30) == 0;
            if (!memory.IsMapped(destination, four_words ? 16 : sizeof(u32))) {
                return false;
            }
            releases.push_back({destination, payload, flags});
            return true;
        };
        scheduler.BeginAttempt();
        scheduler.Signal(address, argument, read, release, [](const Descriptor&) { return true; });
        if (!failure && late && ReleasesIntoLateReads(memory)) {
            failure = Outcome{false, std::nullopt, QmdLoss::ReleasedInto};
        }
        if (failure) {
            scheduler.RollbackAttempt();
            return *failure;
        }
        scheduler.CommitAttempt();
        if (late) {
            for (const u64 read_address : attempt_reads) {
                if (const auto device = memory.Device(read_address)) {
                    late_reads.insert(*device >> 8);
                }
            }
            for (const Release& done : releases) {
                memory.NoteWriteAt(split, done.address, (done.flags >> 30) == 0 ? 16 : 4, position);
            }
        } else {
            if (Nx2DrainCheck::g_enabled && !checks.empty()) {
                checks.back().valued = true;
                checks.back().reads = std::move(checked_reads);
            }
            for (const Release& noted : releases) {
                NoteRelease(split, memory, noted.address, noted.payload, noted.flags);
            }
        }
        return {true, std::nullopt};
    }

    /// A release writes at most 16 bytes at its address (four words).
    static constexpr u64 ReleaseBytes = 16;

    /// Calls `func(device, size)` for the device memory of each piece of what a release at
    /// `address` may write, a piece per 4 KiB GPU page, until it returns true; whether it did.
    /// Releases and QMDs are compared there: the same device memory can be mapped at several GPU
    /// addresses (NX2 maps CPU ranges twice). Where no device memory is mapped, nothing is written.
    template <typename Memory, typename Func>
    [[nodiscard]] static bool AnyReleasedPiece(const Memory& memory, GPUVAddr address,
                                               Func&& func) {
        constexpr GPUVAddr PageSize = 0x1000;
        const GPUVAddr end = address + ReleaseBytes;
        for (GPUVAddr begin = address; begin < end;) {
            const GPUVAddr piece_end = (std::min)((begin & ~(PageSize - 1)) + PageSize, end);
            const auto device = memory.Device(begin);
            if (device && func(*device, piece_end - begin)) {
                return true;
            }
            begin = piece_end;
        }
        return false;
    }

    /// Whether a release the attempt ran writes into the QMD at `qmd_address`, through any GPU
    /// address. A QMD (256 bytes at a 256-byte boundary) lies in one GPU page.
    template <typename Memory>
    [[nodiscard]] bool ReleasedInto(const Memory& memory, GPUVAddr qmd_address) const {
        if (releases.empty()) {
            return false;
        }
        const auto qmd = memory.Device(qmd_address);
        if (!qmd) {
            return false; // No device memory there: no release writes what is read
        }
        const u64 qmd_end = *qmd + sizeof(Descriptor);
        for (const Release& release : releases) {
            if (AnyReleasedPiece(memory, release.address, [&](u64 device, u64 size) {
                    return device < qmd_end && *qmd < device + size;
                })) {
                return true;
            }
        }
        return false;
    }

    /// Late: whether a release of the signal writes a QMD it read, or one a signal valued late
    /// before read (late_reads). The back may have run the release before the front read that QMD.
    template <typename Memory>
    [[nodiscard]] bool ReleasesIntoLateReads(const Memory& memory) const {
        for (const u64 read_address : attempt_reads) {
            if (ReleasedInto(memory, read_address)) {
                return true;
            }
        }
        for (const Release& done : releases) {
            if (AnyReleasedPiece(memory, done.address, [this](u64 device, u64 size) {
                    return late_reads.contains(device >> 8) ||
                           late_reads.contains((device + size - 1) >> 8);
                })) {
                return true;
            }
        }
        return false;
    }

    /// Reads into `descriptor` the QMD at `qmd_address` as the back read it, or will, running the
    /// signal sent at `position`, or tells why the front can't.
    template <typename Memory>
    [[nodiscard]] std::optional<Outcome> ReadDescriptor(const GpuSplit& split, const Memory& memory,
                                                        GPUVAddr qmd_address, u64 position,
                                                        Descriptor& descriptor) const {
        const auto device = memory.Device(qmd_address);
        // As for GPU-written command lists and CheckWithoutDrain: a dispatch still queued that is
        // the first to write this page is not marked yet (nx2.h, g_nx2_gpu_written_pages).
        if (!device || memory.GpuWritten(*device, sizeof(descriptor))) {
            return Outcome{false, std::nullopt, QmdLoss::Unreadable};
        }
        // The last write sent there, placed or not (the inline write covers the whole QMD).
        const u64 target = memory.WritesTarget(split, qmd_address, sizeof(descriptor));
        if (target > position) {
            // Sent after the signal (it waited): memory may hold it, the back read what was before.
            return Outcome{false, std::nullopt, QmdLoss::WrittenAfter};
        }
        if (split.Ran(target)) {
            // Read only once Ran saw the back past the write: the back stores its position after
            // the writes before it. A read before that look may be older than the write. The back
            // can run a write published shortly before the signal (to an idle back, after a few
            // words) while the front reads, and store its position before the front looks.
            memory.Read(qmd_address, descriptor.data(), sizeof(descriptor));
            return std::nullopt;
        }
        if (const InlineWrite* written = QueuedInlineWrite(split, *device);
            written != nullptr && written->position == target) {
            descriptor = written->qmd;
            Count(g_acquire_stats.qmd_inline_reads);
            return std::nullopt;
        }
        // Inline uploads still queued wrote it last (games rewrite a few words of a QMD between
        // dispatches).
        if (ReadPatched(split, memory, qmd_address, target, descriptor)) {
            Count(g_acquire_stats.qmd_patched_reads);
            return std::nullopt;
        }
        return Outcome{false, target};
    }

    /// The QMD at `qmd_address`, whose last write (at `target`) is a kept upload over part or all
    /// of it, as the back will find it: walking back from that upload through the uploads that
    /// were each the last write over the QMD's granule before the next, until one covers the whole
    /// QMD or the writes before the first ran (memory holds those), then those uploads' words on
    /// top in the order sent. A QMD is one 256-byte granule (its address is the register shifted
    /// by 8), so `prior` covers all of it.
    template <typename Memory>
    [[nodiscard]] bool ReadPatched(const GpuSplit& split, const Memory& memory,
                                   GPUVAddr qmd_address, u64 target, Descriptor& descriptor) const {
        const GPUVAddr qmd_end = qmd_address + sizeof(Descriptor);
        std::array<const KeptUpload*, 8> chain{};
        size_t count = 0;
        bool covered = false;
        for (u64 last = target; count < chain.size();) {
            const KeptUpload* upload = PartUpload(split, memory.Id(), qmd_address, last);
            if (upload == nullptr) {
                return false;
            }
            chain[count++] = upload;
            covered = upload->address <= qmd_address && upload->address + upload->size >= qmd_end;
            if (covered || split.Ran(upload->prior)) {
                break;
            }
            last = upload->prior;
        }
        if (count == 0 || !(covered || split.Ran(chain[count - 1]->prior))) {
            return false;
        }
        if (!covered) {
            memory.Read(qmd_address, descriptor.data(), sizeof(descriptor));
        }
        auto* const bytes = reinterpret_cast<u8*>(descriptor.data());
        for (size_t i = count; i-- > 0;) {
            const KeptUpload& upload = *chain[i];
            const GPUVAddr begin = (std::max)(upload.address, qmd_address);
            const GPUVAddr end = (std::min<GPUVAddr>)(upload.address + upload.size, qmd_end);
            std::memcpy(bytes + (begin - qmd_address),
                        upload.bytes.data() + (begin - upload.address), end - begin);
        }
        return true;
    }

    /// The kept upload the back has not run yet that wrote last (at `target`) over part of the
    /// QMD at `qmd_address`.
    [[nodiscard]] const KeptUpload* PartUpload(const GpuSplit& split, const void* memory,
                                               GPUVAddr qmd_address, u64 target) const {
        const size_t count = (std::min)(uploads_count, uploads.size());
        for (size_t i = 1; i <= count; ++i) {
            const KeptUpload& kept = uploads[(uploads_count - i) % uploads.size()];
            if (split.Ran(kept.position)) {
                break; // The back wrote it, and the older ones
            }
            if (kept.memory == memory && kept.position == target &&
                kept.address < qmd_address + sizeof(Descriptor) &&
                qmd_address < kept.address + kept.size) {
                return &kept;
            }
        }
        return nullptr;
    }

    /// The last inline QMD sent to the QMD at `device` that the back has not written yet.
    [[nodiscard]] const InlineWrite* QueuedInlineWrite(const GpuSplit& split, u64 device) const {
        const size_t count = (std::min)(inline_writes_count, inline_writes.size());
        for (size_t i = 1; i <= count; ++i) {
            const InlineWrite& written =
                inline_writes[(inline_writes_count - i) % inline_writes.size()];
            if (split.Ran(written.position)) {
                break; // The back wrote it, and the older ones
            }
            if (written.device == device) {
                return written.overwritten ? nullptr : &written;
            }
        }
        return nullptr;
    }

    /// A release the back runs with the entry just sent, as ProcessNativeMethod's: what the word
    /// holds once the back ran it.
    template <typename Memory>
    void NoteRelease(GpuSplit& split, const Memory& memory, GPUVAddr address, u32 payload,
                     u32 flags) {
        const bool four_words = (flags >> 30) == 0;
        const bool reduction = (flags & (1U << 26)) != 0;
        const bool maximum = !reduction && !four_words && (flags & (1U << 19)) != 0;
        const u64 size = four_words ? 16 : sizeof(u32);
        const auto device = memory.Device(address);
        // Plain and four-word releases are fence-flagged reports; reductions and maxima run in
        // SignalFence operations (KeplerCompute::ProcessNativeMethod).
        const auto landing =
            reduction || maximum ? GpuSplit::FenceLanding() : GpuSplit::Landing::Fenced;
        // The word a reduction or a maximum combines the payload with, before this write.
        Base base{0, QmdBase::Unknown};
        if (device && (reduction || maximum)) {
            base = WordAt(split, memory, address, *device, landing == GpuSplit::Landing::Fenced);
        }
        if (four_words) {
            memory.NoteWrite(split, address, size);
        } else {
            memory.NoteSemaphoreRelease(split, address);
        }
        if (!device) {
            return;
        }
        // An inline QMD this entry wrote before is no longer what memory holds there.
        for (size_t i = 1; i <= (std::min)(inline_writes_count, inline_writes.size()); ++i) {
            InlineWrite& written = inline_writes[(inline_writes_count - i) % inline_writes.size()];
            if (written.position != split.Position()) {
                break;
            }
            if (written.device < *device + size && *device < written.device + sizeof(Descriptor)) {
                written.overwritten = true;
            }
        }
        const auto note = [&](u64 word, u32 value, bool known, bool at_least) {
            split.NoteSemaphoreWrite(word, value, known, at_least, landing);
            if (Nx2DrainCheck::g_enabled && !checks.empty()) {
                checks.back().releases.push_back({word, value, known, at_least});
            }
        };
        if (reduction) {
            Count(g_acquire_stats.qmd_reduction_bases[static_cast<size_t>(base.kind)]);
            const u32 op = (flags >> 20) & 7;
            const bool is_signed = ((flags >> 24) & 1) != 0;
            // Over a lower bound, only the unsigned MIN and MAX give one: an ADD wraps. Over an
            // exact base the value is the back's, wrapped as it wraps.
            const bool keeps_bound = !is_signed && (op == 1 || op == 2);
            if (base.Exact() || (keeps_bound && base.kind == QmdBase::LowerBound)) {
                note(*device, Tegra::ReduceSemaphore(op, is_signed, base.value, payload), true,
                     !base.Exact());
            } else {
                note(*device, 0, false, false);
            }
        } else if (four_words) {
            // The payload as 64 bits, a timestamp after it.
            note(*device, payload, true, false);
            note(*device + 4, 0, true, false);
            split.NoteUnknownWrites(*device + 8, 8, landing);
        } else if (maximum) {
            // Whatever the word held, it holds the payload at least.
            const bool bound = base.Exact() || base.kind == QmdBase::LowerBound;
            note(*device, bound ? (std::max)(base.value, payload) : payload, true, !base.Exact());
        } else {
            note(*device, payload, true, false);
        }
    }

    /// A word the back finds: `value`, exactly or at least (`kind`).
    struct Base {
        u32 value;
        QmdBase kind;

        [[nodiscard]] bool Exact() const {
            return kind == QmdBase::ExactNote || kind == QmdBase::ExactMemory;
        }
    };

    /// The word the back finds at `address` (`device`) when it runs the entry just sent, as
    /// far as the front knows it; `fenced`: the write combining with it lands as a fence
    /// operation. Exactly:
    /// - the last release noted there, if it lands before this write (as the back runs it, or as
    ///   an earlier fence operation) and nothing sent since may have written over it but noted
    ///   releases to other words (OtherWritesTarget: the note map knows those word by word);
    /// - memory, if every write sent there landed: no note there is queued (QueuedSemaphoreWriteAt
    ///   keeps a fenced one until it landed), the back ran the other writes sent there, GPU work
    ///   never wrote the page, and no release the front did not note may land yet. The shape of
    ///   jpkto's MemoryHoldsBase (#1703), with the unnoted releases always guarded.
    /// Memory gives a lower bound only while a fenced release the back ran there, or one the
    /// front did not note, may land over it yet. The entry itself is held unpublished
    /// (PublishHold): an idle back that ran it before this read would leave its own increment in
    /// memory, and the front would count it twice.
    template <typename Memory>
    [[nodiscard]] Base WordAt(const GpuSplit& split, const Memory& memory, GPUVAddr address,
                              u64 device, bool fenced) const {
        // The back reads the syncpoint shim from the syncpoints.
        if (memory.InSyncpointShim(address, sizeof(u32)) ||
            Tegra::Nx2MirrorShadow::Get().Diverts(device)) {
            return {0, QmdBase::Diverted};
        }
        const u64 target = memory.OtherWritesTarget(split, address);
        const auto queued = split.QueuedSemaphoreWriteAt(device);
        // A fenced release the back ran may not have landed yet, and the back reads memory: from
        // there, as below.
        if (queued && !(queued->fenced && split.Ran(queued->position))) {
            if (queued->known && queued->position >= target && (!queued->fenced || fenced)) {
                return {queued->payload,
                        queued->at_least ? QmdBase::LowerBound : QmdBase::ExactNote};
            }
            return {0, QmdBase::Unknown};
        }
        if (memory.GpuWritten(device, sizeof(u32)) || !split.Ran(target)) {
            return {0, QmdBase::Unknown};
        }
        const bool exact = !queued && split.UnnotedReleasesLanded();
        return {memory.ReadWord(address), exact ? QmdBase::ExactMemory : QmdBase::LowerBound};
    }

    Tegra::NativeQmdScheduler scheduler{Tegra::NativeQmdScheduler::OwnTable{}};
    /// The table the engines of every channel share, which the back's scheduling uses.
    Tegra::NativeQmdScheduler back_table;
    std::vector<Release> releases;
    std::vector<u64> attempt_reads; ///< The QMDs the attempt read
    /// EDEN_NX2_DRAIN_CHECK: the descriptors the attempt read, and the signals of the entry being
    /// sent (TakeChecks).
    std::vector<Nx2DrainCheck::QmdRead> checked_reads;
    std::vector<Nx2DrainCheck::QmdSignal> checks;
    bool synced{};
    /// Signals that wait to be valued, oldest first; the first may be once the back ran
    /// retry_after. late_reads: the device granules of the QMDs read late since the waiting began.
    std::deque<WaitingSignal> waiting;
    u64 retry_after{};
    std::unordered_set<u64> late_reads;
    /// The last inline QMDs sent, oldest first from inline_writes_count on: a back a millisecond
    /// behind has some ten to write, a few times that in long frames.
    std::array<InlineWrite, 256> inline_writes{};
    size_t inline_writes_count{};
    /// The last inline uploads sent (NoteUpload), oldest first from uploads_count on: about as
    /// many as the back runs behind the front (one patch per dispatch), so that the walk back
    /// through a QMD's patches (ReadPatched) finds the older ones still here.
    std::array<KeptUpload, 64> uploads{};
    size_t uploads_count{};
};

} // namespace VideoCommon
