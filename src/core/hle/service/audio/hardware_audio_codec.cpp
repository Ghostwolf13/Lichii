// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <vector>

#include "common/logging.h"
#include "core/core.h"
#include "core/hle/kernel/k_auto_object.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_page_group.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_transfer_memory.h"
#include "core/hle/service/audio/errors.h"
#include "core/hle/service/audio/hardware_audio_codec.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/memory.h"

namespace Service::Audio {
namespace {
constexpr u64 Align(u64 value, u64 alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

bool Contains(u64 total, u64 offset, u64 size) {
    return offset <= total && size <= total - offset;
}

bool Overlaps(u64 a, u64 a_size, u64 b, u64 b_size) {
    return a <= b ? b - a < a_size : a - b < b_size;
}

void ReplyError(HLERequestContext& ctx, Result result) {
    IPC::ResponseBuilder reply{ctx, 2};
    reply.Push(result);
}

// EDEN_NX2_HWACOD_TRACE=1 logs instance lifecycles and, about once a second, each manager's tasks
// by instance, so a stream that stops getting input shows up while the others keep going.
bool TraceEnabled() {
    static const bool enabled = std::getenv("EDEN_NX2_HWACOD_TRACE") != nullptr;
    return enabled;
}

struct InstanceTrace {
    u32 tasks, resets, errors;
    u64 total_tasks;
};
}

namespace Hwacod {

Result ValidateRequest(std::span<const u32> words, bool domain, RequestShape shape,
                       u32& scalar_offset) {
    R_UNLESS(words.size() >= 2, ResultInvalidArgument);
    IPC::CommandHeader header{};
    std::memcpy(&header, words.data(), sizeof(header));
    R_UNLESS(header.type == IPC::CommandType::Request ||
                 header.type == IPC::CommandType::RequestWithContext, ResultInvalidArgument);
    R_UNLESS(header.num_buf_b_descriptors == 0 && header.num_buf_w_descriptors == 0 &&
                 header.buf_c_descriptor_flags == IPC::CommandHeader::BufferDescriptorCFlag::Disabled,
             ResultInvalidArgument);
    u32 offset = 2;
    if (header.enable_handle_descriptor) {
        R_UNLESS(words.size() > offset, ResultInvalidArgument);
        IPC::HandleDescriptorHeader handles{};
        handles.raw_high = words[offset++];
        R_UNLESS(handles.send_current_pid == 0 && handles.num_handles_to_move == 0 &&
                     handles.num_handles_to_copy == shape.copy_handles, ResultInvalidHandle);
        offset += shape.copy_handles;
    } else {
        R_UNLESS(shape.copy_handles == 0, ResultInvalidHandle);
    }
    const u32 nx = header.num_buf_x_descriptors;
    const u32 na = header.num_buf_a_descriptors;
    if (shape.buffer == InputBuffer::None) {
        R_UNLESS(nx == 0 && na == 0, ResultInvalidArgument);
    } else if (shape.buffer == InputBuffer::Parameter) {
        R_UNLESS(nx == 1 && na == 0, ResultInvalidArgument);
    } else {
        R_UNLESS(nx <= 1 && na <= 1 && nx + na > 0, ResultInvalidArgument);
    }
    R_UNLESS(offset + 2 * nx + 3 * na <= words.size(), ResultInvalidArgument);
    IPC::BufferDescriptorX x{};
    IPC::BufferDescriptorABW a{};
    if (nx) {
        std::memcpy(&x, words.data() + offset, sizeof(x));
    }
    if (na) {
        std::memcpy(&a, words.data() + offset + 2 * nx, sizeof(a));
    }
    if (shape.buffer == InputBuffer::Parameter) {
        R_UNLESS(x.Size() == 0x120, ResultInvalidArgument);
    } else if (shape.buffer == InputBuffer::Tasks) {
        R_UNLESS(!(x.Size() && a.Size()) && (nx || a.Size()), ResultInvalidArgument);
        const u64 size = a.Size() ? a.Size() : x.Size();
        R_UNLESS(size % sizeof(Task) == 0 && size <= 65536 * sizeof(Task), ResultInvalidArgument);
    }
    offset += 2 * nx + 3 * na;
    const u64 raw_end = u64{offset} + header.data_size.Value();
    R_UNLESS(raw_end <= words.size(), ResultInvalidArgument);
    offset = static_cast<u32>(Align(offset, 4));
    if (domain) {
        R_UNLESS(offset + 4 <= raw_end, ResultInvalidArgument);
        IPC::DomainMessageHeader domain_header{};
        std::memcpy(&domain_header, words.data() + offset, sizeof(domain_header));
        R_UNLESS(domain_header.command == IPC::DomainMessageHeader::CommandType::SendMessage &&
                     domain_header.input_object_count == 0 &&
                     domain_header.size == 16 + shape.scalar_bytes, ResultInvalidArgument);
        offset += 4;
    }
    R_UNLESS(offset + 4 + (shape.scalar_bytes + 3) / 4 <= raw_end, ResultInvalidArgument);
    R_UNLESS(words[offset] == 0x49434653 && words[offset + 1] <= 1, ResultInvalidArgument);
    scalar_offset = offset + 4;
    R_UNLESS(raw_end - scalar_offset - (shape.scalar_bytes + 3) / 4 <= 4, ResultInvalidArgument);
    R_SUCCEED();
}

Result ParseParameters(std::span<const u8> bytes, AudioCore::OpusDecoder::CodecConfig& config) {
    R_UNLESS(bytes.size() == 0x120, ResultInvalidArgument);
    u64 kind{};
    s32 rate{}, channels{}, streams{}, coupled{};
    s64 duration{};
    std::memcpy(&kind, bytes.data(), 8);
    std::memcpy(&rate, bytes.data() + 8, 4);
    std::memcpy(&channels, bytes.data() + 12, 4);
    std::memcpy(&duration, bytes.data() + 16, 8);
    std::memcpy(&streams, bytes.data() + 24, 4);
    std::memcpy(&coupled, bytes.data() + 28, 4);
    R_UNLESS(kind != 0, ResultNotSupported);
    R_UNLESS(kind == 1 || kind == 2, ResultInvalidArgument);
    R_UNLESS(rate == 8000 || rate == 12000 || rate == 16000 || rate == 24000 || rate == 48000,
             ResultInvalidOpusSampleRate);
    R_UNLESS(channels > 0 && channels <= (kind == 1 ? 2 : 255), ResultInvalidOpusChannelCount);
    const s64 micros = duration / 1000;
    R_UNLESS(micros == 2500 || micros == 5000 || micros == 10000 || micros == 20000,
             ResultInvalidArgument);
    config = {};
    config.kind = static_cast<AudioCore::OpusDecoder::CodecKind>(kind);
    config.rate = static_cast<u32>(rate);
    config.channels = static_cast<u32>(channels);
    config.frame_samples = static_cast<u32>((u64{config.rate} * duration + 999999999) / 1000000000);
    if (kind == 2) {
        R_UNLESS(streams > 0 && streams <= 255 && coupled >= 0 && coupled <= streams &&
                     streams + coupled <= channels, ResultInvalidArgument);
        config.streams = static_cast<u32>(streams);
        config.coupled = static_cast<u32>(coupled);
        std::memcpy(config.mapping.data(), bytes.data() + 0x20, config.mapping.size());
        for (u32 i = 0; i < config.channels; ++i) {
            R_UNLESS(config.mapping[i] == 255 || config.mapping[i] < streams + coupled,
                     ResultInvalidArgument);
        }
    }
    u32 required{};
    R_RETURN(AudioCore::OpusDecoder::HostDecoder::RequiredBytes(config, required));
}

Result GetManagerSize(u32 io_count, u32& bytes) {
    R_UNLESS(io_count > 0 && io_count <= 65536, ResultInvalidArgument);
    bytes = static_cast<u32>(Align(sizeof(ManagerState) + u64{io_count} * sizeof(IoLease), 64));
    R_SUCCEED();
}

Result ValidateLayout(const SharedMemoryWire& l, u64 transfer_size, u64 process_address,
                      u64 process_size, u64 properties) {
    R_UNLESS(l.version == 1, ResultInvalidRevision);
    R_UNLESS(std::ranges::all_of(l.reserved, [](u32 value) { return value == 0; }) &&
                 (properties & 7) != 0, ResultInvalidArgument);
    R_UNLESS(l.instance_count > 0 && l.instance_count <= 65536 && l.instance_stride > 0 &&
                 l.instance_stride % 64 == 0 && l.input_stride >= 0x600 &&
                 l.input_stride % 64 == 0 && l.output_stride > 0 && l.output_stride % 64 == 0,
             ResultInvalidArgument);
    u32 manager_bytes{};
    R_TRY(GetManagerSize(l.io_count, manager_bytes));
    R_UNLESS(transfer_size > 0 && transfer_size % 4096 == 0 && process_size > 0 &&
                 process_size % 4096 == 0 && process_address % 4096 == 0 &&
                 process_size <= std::numeric_limits<u64>::max() - process_address &&
                 l.transfer_size <= transfer_size && l.process_size <= process_size &&
                 l.transfer_size % 4096 == 0 && l.process_size % 4096 == 0,
             ResultInvalidAddressInfo);
    const u64 queue_bytes = Align(0xc0 + u64{l.io_count} * 0x40, 4096);
    R_UNLESS(l.manager_size >= manager_bytes &&
                 l.instances_size >= u64{l.instance_count} * l.instance_stride &&
                 l.io_size >= u64{l.io_count} * (u64{l.input_stride} + l.output_stride) &&
                 l.transfer_queue_size >= queue_bytes && l.completion_size >= queue_bytes,
             ResultBufferTooSmall);
    const std::array<std::pair<u32, u32>, 3> transfer_regions{{
        {l.manager_offset, l.manager_size}, {l.instances_offset, l.instances_size},
        {l.transfer_queue_offset, l.transfer_queue_size}}};
    for (size_t i = 0; i < transfer_regions.size(); ++i) {
        const auto [offset, size] = transfer_regions[i];
        R_UNLESS(offset % (i == 2 ? 4096 : 64) == 0 &&
                     Contains(l.transfer_size, offset, size), ResultInvalidAddressInfo);
        for (size_t j = 0; j < i; ++j) {
            R_UNLESS(!Overlaps(offset, size, transfer_regions[j].first, transfer_regions[j].second),
                     ResultInvalidAddressInfo);
        }
    }
    R_UNLESS(l.io_offset % 64 == 0 && l.completion_offset % 4096 == 0 &&
                 Contains(l.process_size, l.io_offset, l.io_size) &&
                 Contains(l.process_size, l.completion_offset, l.completion_size) &&
                 !Overlaps(l.io_offset, l.io_size, l.completion_offset, l.completion_size),
             ResultInvalidAddressInfo);
    R_SUCCEED();
}

Completion MakeCompletion(const Task& task, Result result, u32 bytes, s64 duration_ns) {
    return {task.tag, task.instance, task.io, task.sequence, std::max<s64>(0, duration_ns),
            result.IsSuccess() ? bytes : 0, result.raw, {}};
}

Result BatchState::Initialize(u32 io_count, u16 tag) {
    R_UNLESS(!storage && tag != 0, ResultInvalidArgument);
    R_TRY(GetManagerSize(io_count, storage_bytes));
    storage = std::make_unique<u8[]>(storage_bytes);
    new (storage.get()) ManagerState{1, tag, 0, 0, io_count, 0, 0, 0, {}};
    for (u32 i = 0; i < io_count; ++i) {
        new (storage.get() + sizeof(ManagerState) + sizeof(IoLease) * i) IoLease{};
    }
    R_SUCCEED();
}

ManagerState& BatchState::State() {
    return *reinterpret_cast<ManagerState*>(storage.get());
}

const ManagerState& BatchState::State() const {
    return *reinterpret_cast<const ManagerState*>(storage.get());
}
std::span<IoLease> BatchState::Leases() {
    return {reinterpret_cast<IoLease*>(storage.get() + sizeof(ManagerState)), State().io_count};
}
std::span<const IoLease> BatchState::Leases() const {
    return {reinterpret_cast<const IoLease*>(storage.get() + sizeof(ManagerState)), State().io_count};
}
u32 BatchState::StorageBytes() const {
    return storage_bytes;
}

Result BatchState::Reclaim(u32 consumer, u32 producer) {
    auto& s = State();
    const u32 capacity = s.io_count + 1;
    R_UNLESS(consumer < capacity && producer == s.write_index, ResultInvalidArgument);
    const u32 consumed = (consumer + capacity - s.reclaimed_read_index) % capacity;
    R_UNLESS(consumed <= s.outstanding, ResultInvalidArgument);
    u32 count{};
    for (const auto& lease : Leases()) {
        if (lease.occupied &&
            (lease.completion_slot + capacity - s.reclaimed_read_index) % capacity < consumed) {
            ++count;
        }
    }
    R_UNLESS(count == consumed, ResultInvalidArgument);
    for (auto& lease : Leases()) {
        if (lease.occupied &&
            (lease.completion_slot + capacity - s.reclaimed_read_index) % capacity < consumed) {
            lease = {};
        }
    }
    s.reclaimed_read_index = consumer;
    s.outstanding -= consumed;
    R_SUCCEED();
}

Result BatchState::ValidateTasks(std::span<const Task> tasks, std::span<const u8> live_instances,
                                 u32 input_stride) const {
    const auto& s = State();
    R_UNLESS(tasks.size() <= s.io_count - s.outstanding, ResultBufferCountReached);
    std::vector<u8> seen(s.io_count);
    for (const auto& task : tasks) {
        R_UNLESS(task.tag == s.tag && task.instance < live_instances.size() &&
                     live_instances[task.instance] && task.io < s.io_count &&
                     task.input_bytes <= input_stride && task.reset <= 1 && task.notify <= 1 &&
                     task.reserved == 0, ResultInvalidArgument);
        R_UNLESS(!Leases()[task.io].occupied && !seen[task.io], ResultBufferCountReached);
        seen[task.io] = 1;
    }
    R_SUCCEED();
}

void BatchState::Commit(const Task& task) {
    auto& s = State();
    Leases()[task.io] = {1, s.write_index, task.instance, task.sequence, 0};
    s.write_index = (s.write_index + 1) % (s.io_count + 1);
    ++s.outstanding;
}

bool BatchState::HasOutstanding(u32 instance) const {
    return std::ranges::any_of(Leases(), [instance](const IoLease& lease) {
        return lease.occupied && lease.instance == instance;
    });
}
}

bool GuardHardwareAudioCodecRequest(HLERequestContext& ctx, Hwacod::RequestShape shape) {
    u32 scalar_offset{};
    const auto result = Hwacod::ValidateRequest(
        {ctx.CommandBuffer(), IPC::COMMAND_BUFFER_LENGTH}, ctx.GetManager()->IsDomain(), shape,
        scalar_offset);
    if (result.IsFailure() || scalar_offset != ctx.GetDataPayloadOffset() + 2) {
        ReplyError(ctx, result.IsFailure() ? result : ResultInvalidArgument);
        return false;
    }
    u64 address{}, size{};
    if (shape.buffer != Hwacod::InputBuffer::None) {
        const auto& a = ctx.BufferDescriptorA();
        const auto& x = ctx.BufferDescriptorX();
        if (!a.empty() && a[0].Size() != 0) {
            if (!x.empty() && x[0].Size() != 0) {
                ReplyError(ctx, ResultInvalidArgument);
                return false;
            }
            address = a[0].Address();
            size = a[0].Size();
        } else if (!x.empty()) {
            address = x[0].Address();
            size = x[0].Size();
        }
        const bool valid_size = shape.buffer == Hwacod::InputBuffer::Parameter
                                    ? size == 0x120
                                    : size % sizeof(Hwacod::Task) == 0 && size <= 65536 * sizeof(Hwacod::Task);
        if (!valid_size || size > std::numeric_limits<u64>::max() - address ||
            (size && !ctx.GetMemory().IsValidVirtualAddressRange(address, size))) {
            ReplyError(ctx, ResultInvalidArgument);
            return false;
        }
    }
    return true;
}

Result HardwareAudioCodecTags::Acquire(u16& tag) {
    std::scoped_lock lock{mutex};
    for (u32 i = 0; i < 65535; ++i) {
        const u16 candidate = next;
        next = next == 65535 ? 1 : static_cast<u16>(next + 1);
        if (!live[candidate]) {
            live.set(candidate);
            tag = candidate;
            R_SUCCEED();
        }
    }
    R_THROW(ResultOutOfSessions);
}

void HardwareAudioCodecTags::Release(u16 tag) {
    std::scoped_lock lock{mutex};
    live.reset(tag);
}

struct IHardwareAudioCodecManagerSession::Impl {
    Impl(Core::System& system_, std::shared_ptr<HardwareAudioCodecTags> tags_)
        : system{system_}, tags{std::move(tags_)}, transfer{system.Kernel()},
          process{system.Kernel()}, context{system, "hwacod"} {}

    ~Impl() {
        instances.clear();
        context.CloseEvent(event);
        if (locked) {
            const auto result = process->GetPageTable().UnlockForTransferMemory(address, size, *pages);
            if (result.IsFailure()) {
                LOG_ERROR(Service_Audio, "hwacod unlock failed: {:#x}", result.raw);
            }
            pages->Close(system.Kernel());
        }
        if (pages) {
            pages->Finalize();
        }
        if (tag) {
            tags->Release(tag);
        }
    }

    Result Reclaim() {
        R_UNLESS(!faulted, ResultOperationFailed);
        u32 consumer{}, producer{};
        R_UNLESS(process->GetMemory().Read32Acquire(ring + 0x44, consumer) &&
                     process->GetMemory().Read32Acquire(ring + 4, producer), ResultInvalidAddressInfo);
        R_RETURN(batch.Reclaim(consumer, producer));
    }

    Result Fault() {
        faulted = true;
        event->Signal(system.Kernel());
        LOG_ERROR(Service_Audio, "hwacod manager {} lost completion publication", tag);
        R_THROW(ResultOperationFailed);
    }

    Core::System& system;
    std::shared_ptr<HardwareAudioCodecTags> tags;
    Kernel::KScopedAutoObject<Kernel::KTransferMemory> transfer;
    Kernel::KScopedAutoObject<Kernel::KProcess> process;
    KernelHelpers::ServiceContext context;
    Kernel::KEvent* event{};
    std::optional<Kernel::KPageGroup> pages;
    bool locked{}, faulted{};
    u16 tag{};
    u32 logged_batches{}, logged_errors{};
    std::vector<InstanceTrace> trace;
    u32 trace_batches{};
    std::chrono::steady_clock::time_point trace_start;
    u64 address{}, size{}, ring{};
    Hwacod::SharedMemoryWire layout{};
    Hwacod::BatchState batch;
    std::vector<std::unique_ptr<AudioCore::OpusDecoder::HostDecoder>> instances;
    std::vector<u8> live;
};

IHardwareAudioCodecManagerSession::IHardwareAudioCodecManagerSession(
    Core::System& system_, std::shared_ptr<HardwareAudioCodecTags> tags)
    : ServiceFramework{system_, "IHardwareAudioCodecManagerSession"},
      impl{std::make_unique<Impl>(system_, std::move(tags))} {
    static const FunctionInfo functions[] = {
        {0, &IHardwareAudioCodecManagerSession::Dispatch, "InitializeInstance"},
        {1, &IHardwareAudioCodecManagerSession::Dispatch, "FinalizeInstance"},
        {10, &IHardwareAudioCodecManagerSession::Dispatch, "Execute"},
    };
    RegisterHandlers(functions);
}

IHardwareAudioCodecManagerSession::~IHardwareAudioCodecManagerSession() = default;

Result IHardwareAudioCodecManagerSession::Initialize(
    const Hwacod::SharedMemoryWire& layout, u64 transfer_size, u64 process_address,
    u64 process_size, u64 properties, Kernel::KTransferMemory* transfer, Kernel::KProcess* process) {
    R_UNLESS(transfer && process && transfer->IsInitialized() && process->IsInitialized() &&
                 !process->IsTerminated(), ResultInvalidHandle);
    R_TRY(Hwacod::ValidateLayout(layout, transfer_size, process_address, process_size, properties));
    R_UNLESS(transfer_size <= transfer->GetSize() &&
                 process->GetPageTable().Contains(process_address, process_size), ResultInvalidAddressInfo);
    R_UNLESS(transfer->GetOwner() != process ||
                 !Overlaps(GetInteger(transfer->GetSourceAddress()), transfer->GetSize(),
                           process_address, process_size), ResultInvalidAddressInfo);
    auto& p = *impl;
    p.transfer.SetObject(transfer);
    p.process.SetObject(process);
    p.address = process_address;
    p.size = process_size;
    p.layout = layout;
    p.pages.emplace(system.Kernel(), process->GetPageTable().GetBlockInfoManager());
    R_TRY(process->GetPageTable().LockForTransferMemory(
        &*p.pages, process_address, process_size, Kernel::KMemoryPermission::UserReadWrite));
    p.locked = true;
    for (u64 current = process_address; current < process_address + process_size;) {
        Kernel::KMemoryInfo info{};
        Kernel::Svc::PageInfo page_info{};
        R_TRY(process->GetPageTable().QueryInfo(&info, &page_info, current));
        R_UNLESS(info.m_state == Kernel::KMemoryState::Normal && info.m_size > 0 &&
                     info.m_address <= current && current - info.m_address < info.m_size &&
                     info.m_size <= std::numeric_limits<u64>::max() - info.m_address,
                 ResultInvalidAddressInfo);
        current = info.m_address + info.m_size;
    }
    R_TRY(p.tags->Acquire(p.tag));
    R_TRY(p.batch.Initialize(layout.io_count, p.tag));
    p.instances.resize(layout.instance_count);
    p.live.resize(layout.instance_count);
    if (TraceEnabled()) {
        p.trace.resize(layout.instance_count);
        p.trace_start = std::chrono::steady_clock::now();
    }
    p.event = p.context.CreateEvent("hwacod completion");
    R_UNLESS(p.event, ResultOutOfSessions);
    auto& memory = process->GetMemory();
    p.ring = process_address + layout.completion_offset;
    u32 consumer{};
    R_UNLESS(memory.Read32Acquire(p.ring + 0x44, consumer) && consumer == 0, ResultInvalidArgument);
    const std::array<u8, 64> server_line{};
    R_UNLESS(memory.WriteBlock(p.ring, server_line.data(), server_line.size()), ResultInvalidAddressInfo);
    const Hwacod::Completion empty{};
    for (u32 i = 0; i <= layout.io_count; ++i) {
        R_UNLESS(memory.WriteBlock(p.ring + 0x80 + u64{i} * sizeof(empty), &empty, sizeof(empty)),
                 ResultInvalidAddressInfo);
    }
    LOG_INFO(Service_Audio,
             "hwacod opened tag={} instances={} io={} state_stride={} input_stride={} output_stride={} tmem={} process={}",
             p.tag, layout.instance_count, layout.io_count, layout.instance_stride,
             layout.input_stride, layout.output_stride, transfer_size, process_size);
    R_SUCCEED();
}

u16 IHardwareAudioCodecManagerSession::GetTag() const {
    return impl->tag;
}
Kernel::KReadableEvent* IHardwareAudioCodecManagerSession::GetEvent() {
    return &impl->event->GetReadableEvent();
}

void IHardwareAudioCodecManagerSession::Dispatch(HLERequestContext& ctx) {
    try {
        switch (ctx.GetCommand()) {
        case 0:
            if (GuardHardwareAudioCodecRequest(ctx, {4, 0, Hwacod::InputBuffer::Parameter})) {
                CmifReplyWrap<true, &IHardwareAudioCodecManagerSession::InitializeInstance>(ctx);
            }
            break;
        case 1:
            if (GuardHardwareAudioCodecRequest(ctx, {4, 0, Hwacod::InputBuffer::None})) {
                CmifReplyWrap<true, &IHardwareAudioCodecManagerSession::FinalizeInstance>(ctx);
            }
            break;
        case 10:
            if (GuardHardwareAudioCodecRequest(ctx, {0, 0, Hwacod::InputBuffer::Tasks})) {
                CmifReplyWrap<true, &IHardwareAudioCodecManagerSession::Execute>(ctx);
            }
            break;
        }
    } catch (const std::bad_alloc&) {
        ReplyError(ctx, ResultLibOpusAllocFail);
    }
}

Result IHardwareAudioCodecManagerSession::InitializeInstance(
    u32 id, InBuffer<BufferAttr_HipcPointer> bytes) {
    auto& p = *impl;
    R_UNLESS(!p.faulted, ResultOperationFailed);
    R_UNLESS(id < p.instances.size() && !p.instances[id], ResultInvalidArgument);
    AudioCore::OpusDecoder::CodecConfig config{};
    R_TRY(Hwacod::ParseParameters(bytes, config));
    R_UNLESS(Align(u64{config.frame_samples} * config.channels * sizeof(s16), 64) <= p.layout.output_stride,
             ResultBufferTooSmall);
    auto decoder = std::make_unique<AudioCore::OpusDecoder::HostDecoder>();
    R_TRY(decoder->Initialize(config, p.layout.instance_stride));
    p.instances[id] = std::move(decoder);
    p.live[id] = 1;
    ++p.batch.State().live_instances;
    if (!p.trace.empty()) {
        p.trace[id].total_tasks = 0;
        LOG_INFO(Service_Audio,
                 "hwacod tag={} instance {} initialized: kind {}, {} Hz, {} channels, {} samples per "
                 "frame, {} streams ({} coupled)",
                 p.tag, id, static_cast<u32>(config.kind), config.rate, config.channels,
                 config.frame_samples, config.streams, config.coupled);
    }
    R_SUCCEED();
}

Result IHardwareAudioCodecManagerSession::FinalizeInstance(u32 id) {
    auto& p = *impl;
    R_TRY(p.Reclaim());
    R_UNLESS(id < p.instances.size() && p.instances[id], ResultInvalidArgument);
    R_UNLESS(!p.batch.HasOutstanding(id), ResultBufferCountReached);
    p.instances[id].reset();
    p.live[id] = 0;
    --p.batch.State().live_instances;
    if (!p.trace.empty()) {
        LOG_INFO(Service_Audio, "hwacod tag={} instance {} finalized after {} tasks", p.tag, id,
                 p.trace[id].total_tasks);
    }
    R_SUCCEED();
}

Result IHardwareAudioCodecManagerSession::Execute(InBuffer<BufferAttr_HipcAutoSelect> bytes) {
    auto& p = *impl;
    R_TRY(p.Reclaim());
    R_UNLESS(bytes.size() % sizeof(Hwacod::Task) == 0 &&
                 bytes.size() / sizeof(Hwacod::Task) <= p.layout.io_count, ResultInvalidArgument);
    std::vector<Hwacod::Task> tasks(bytes.size() / sizeof(Hwacod::Task));
    if (!bytes.empty()) {
        std::memcpy(tasks.data(), bytes.data(), bytes.size());
    }
    R_TRY(p.batch.ValidateTasks(tasks, p.live, p.layout.input_stride));
    R_SUCCEED_IF(tasks.empty());
    auto& memory = p.process->GetMemory();
    std::vector<std::vector<u8>> packets(tasks.size());
    std::vector<s16> pcm(p.layout.output_stride / sizeof(s16));
    for (size_t i = 0; i < tasks.size(); ++i) {
        const auto& task = tasks[i];
        packets[i].resize(task.input_bytes);
        const u64 input = p.address + p.layout.io_offset +
                          u64{task.io} * (u64{p.layout.input_stride} + p.layout.output_stride);
        R_UNLESS(memory.ReadBlock(input, packets[i].data(), packets[i].size()), ResultInvalidAddressInfo);
    }
    R_TRY(p.Reclaim());
    R_TRY(p.batch.ValidateTasks(tasks, p.live, p.layout.input_stride));
    u32 successful{}, failed{};
    u64 output_total{};
    for (size_t i = 0; i < tasks.size(); ++i) {
        const auto& task = tasks[i];
        auto& decoder = *p.instances[task.instance];
        u32 samples{};
        const auto start = std::chrono::steady_clock::now();
        const auto result = decoder.Decode(packets[i], pcm, task.reset != 0, samples);
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count();
        const u32 output_bytes = result.IsSuccess() ? samples * decoder.GetConfig().channels * sizeof(s16) : 0;
        const u64 output = p.address + p.layout.io_offset +
                           u64{task.io} * (u64{p.layout.input_stride} + p.layout.output_stride) + p.layout.input_stride;
        const auto completion = Hwacod::MakeCompletion(task, result, output_bytes, elapsed);
        const u32 current = p.batch.State().write_index;
        const u32 next = (current + 1) % (p.layout.io_count + 1);
        if ((output_bytes && !memory.WriteBlock(output, pcm.data(), output_bytes)) ||
            !memory.WriteBlock(p.ring + 0x80 + u64{current} * sizeof(completion), &completion, sizeof(completion)) ||
            !memory.WriteExclusive32(p.ring + 4, next, current)) {
            R_RETURN(p.Fault());
        }
        p.batch.Commit(task);
        if (task.notify) {
            p.event->Signal(system.Kernel());
        }
        if (!p.trace.empty()) {
            auto& trace = p.trace[task.instance];
            ++trace.tasks;
            ++trace.total_tasks;
            trace.resets += task.reset;
            trace.errors += result.IsFailure() ? 1 : 0;
        }
        if (result.IsSuccess()) {
            ++successful;
            output_total += output_bytes;
        } else {
            ++failed;
            if (p.logged_errors < 4) {
                ++p.logged_errors;
                LOG_INFO(Service_Audio, "hwacod decode error tag={} instance={} io={} result={:#x}",
                         p.tag, task.instance, task.io, result.raw);
            }
        }
    }
    if (p.logged_batches < 3) {
        ++p.logged_batches;
        LOG_INFO(Service_Audio, "hwacod completed tag={} tasks={} decoded={} errors={} pcm_bytes={} producer={}",
                 p.tag, tasks.size(), successful, failed, output_total, p.batch.State().write_index);
    }
    if (!p.trace.empty()) {
        ++p.trace_batches;
        const auto now = std::chrono::steady_clock::now();
        if (now - p.trace_start >= std::chrono::seconds{1}) {
            std::string active;
            u32 window_tasks{}, window_errors{};
            for (size_t id = 0; id < p.trace.size(); ++id) {
                auto& trace = p.trace[id];
                if (trace.tasks == 0) {
                    continue;
                }
                window_tasks += trace.tasks;
                window_errors += trace.errors;
                fmt::format_to(std::back_inserter(active), " {}:{}", id, trace.tasks);
                if (trace.resets) {
                    fmt::format_to(std::back_inserter(active), "r{}", trace.resets);
                }
                if (trace.errors) {
                    fmt::format_to(std::back_inserter(active), "e{}", trace.errors);
                }
                trace.tasks = trace.resets = trace.errors = 0;
            }
            const auto& state = p.batch.State();
            LOG_INFO(Service_Audio,
                     "hwacod tag={} in {:.1f} s: {} batches, {} tasks, {} errors, {} live instances, "
                     "{} completions not consumed; instance:tasks{}",
                     p.tag, std::chrono::duration<double>(now - p.trace_start).count(),
                     p.trace_batches, window_tasks, window_errors, state.live_instances,
                     state.outstanding, active);
            p.trace_batches = 0;
            p.trace_start = now;
        }
    }
    R_SUCCEED();
}

IClientSession::IClientSession(Core::System& system_, std::shared_ptr<HardwareAudioCodecTags> tags_)
    : ServiceFramework{system_, "IClientSession"}, tags{std::move(tags_)} {
    static const FunctionInfo functions[] = {
        {0, &IClientSession::Dispatch, "GetInstanceSize"},
        {1, &IClientSession::Dispatch, "GetManagerSize"},
        {10, &IClientSession::Dispatch, "OpenManager"},
    };
    RegisterHandlers(functions);
}

IClientSession::~IClientSession() = default;

void IClientSession::Dispatch(HLERequestContext& ctx) {
    try {
        switch (ctx.GetCommand()) {
        case 0:
            if (GuardHardwareAudioCodecRequest(ctx, {0, 0, Hwacod::InputBuffer::Parameter})) {
                CmifReplyWrap<true, &IClientSession::GetInstanceSize>(ctx);
            }
            break;
        case 1:
            if (GuardHardwareAudioCodecRequest(ctx, {4, 0, Hwacod::InputBuffer::None})) {
                CmifReplyWrap<true, &IClientSession::GetManagerSize>(ctx);
            }
            break;
        case 10:
            if (GuardHardwareAudioCodecRequest(ctx, {0x80, 2, Hwacod::InputBuffer::None})) {
                CmifReplyWrap<true, &IClientSession::OpenManager>(ctx);
            }
            break;
        }
    } catch (const std::bad_alloc&) {
        ReplyError(ctx, ResultLibOpusAllocFail);
    }
}

Result IClientSession::GetInstanceSize(Out<u32> bytes, InBuffer<BufferAttr_HipcPointer> parameters) {
    AudioCore::OpusDecoder::CodecConfig config{};
    R_TRY(Hwacod::ParseParameters(parameters, config));
    R_RETURN(AudioCore::OpusDecoder::HostDecoder::RequiredBytes(config, *bytes));
}

Result IClientSession::GetManagerSize(Out<u32> bytes, u32 io_count) {
    R_RETURN(Hwacod::GetManagerSize(io_count, *bytes));
}

Result IClientSession::OpenManager(
    Out<u32> tag, OutCopyHandle<Kernel::KReadableEvent> event,
    Out<SharedPointer<IHardwareAudioCodecManagerSession>> child, Hwacod::SharedMemoryWire layout,
    u64 transfer_size, u64 process_address, u64 process_size, u64 properties,
    InCopyHandle<Kernel::KTransferMemory> transfer, InCopyHandle<Kernel::KProcess> process) {
    auto manager = std::make_shared<IHardwareAudioCodecManagerSession>(system, tags);
    R_TRY(manager->Initialize(layout, transfer_size, process_address, process_size, properties,
                              transfer.Get(), process.Get()));
    *tag = manager->GetTag();
    *event = manager->GetEvent();
    *child = std::move(manager);
    R_SUCCEED();
}

}
