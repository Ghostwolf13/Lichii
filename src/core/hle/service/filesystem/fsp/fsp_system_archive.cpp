// SPDX-FileCopyrightText: Copyright 2026 Lichii contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <utility>

#include "core/core.h"
#include "core/file_sys/errors.h"
#include "core/file_sys/romfs_factory.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/filesystem/fsp/fsp_system_archive.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/service.h"
#include "core/hle/service/system_data_view.h"

namespace Service::FileSystem {
namespace {

template <typename Derived>
class ReadOnlyFspService : public ServiceFramework<Derived> {
public:
    Result HandleSyncRequest(Kernel::KServerSession& session, HLERequestContext& ctx) override {
        const auto type = ctx.GetCommandType();
        if (type == IPC::CommandType::Close || type == IPC::CommandType::TIPC_Close) {
            return ServiceFrameworkBase::HandleSyncRequest(session, ctx);
        }

        const auto* process = ctx.GetThread().GetOwnerProcess();
        const bool request = type == IPC::CommandType::Request ||
                             type == IPC::CommandType::RequestWithContext;
        const bool control = type == IPC::CommandType::Control ||
                             type == IPC::CommandType::ControlWithContext;
        if (owner_process_id != 0 && process && process->GetProcessId() == owner_process_id &&
            ((request && this->handlers.contains(ctx.GetCommand())) ||
             (control && ctx.GetCommand() <= 4))) {
            return ServiceFrameworkBase::HandleSyncRequest(session, ctx);
        }

        IPC::ResponseBuilder reply{ctx, 2};
        reply.Push(FileSys::ResultPermissionDenied);
        if (this->system.IsPoweredOn()) {
            ctx.WriteToOutgoingCommandBuffer();
        }
        return ResultSuccess;
    }

protected:
    ReadOnlyFspService(Core::System& system, const char* service_name, u64 owner)
        : ServiceFramework<Derived>{system, service_name}, owner_process_id{owner} {}

    const u64 owner_process_id;
};

class ReadOnlySystemArchive final : public ReadOnlyFspService<ReadOnlySystemArchive> {
public:
    ReadOnlySystemArchive(Core::System& system_, std::shared_ptr<const FileSys::VfsFile> archive,
                          u64 owner)
        : ReadOnlyFspService{system_, "IStorage", owner}, backend{std::move(archive)} {
        static const FunctionInfo functions[] = {
            {0, D<&ReadOnlySystemArchive::Read>, "Read"},
            {4, D<&ReadOnlySystemArchive::GetSize>, "GetSize"},
        };
        RegisterHandlers(functions);
    }

private:
    Result Read(
        OutBuffer<BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure> out_bytes,
        s64 offset, s64 length) {
        R_UNLESS(length >= 0 && static_cast<u64>(length) <= out_bytes.size(),
                 FileSys::ResultInvalidSize);
        const auto size = backend->GetSize();
        R_UNLESS(offset >= 0 && static_cast<u64>(offset) <= size, FileSys::ResultInvalidOffset);
        R_UNLESS(static_cast<u64>(length) <= size - static_cast<u64>(offset),
                 FileSys::ResultOutOfRange);
        R_SUCCEED_IF(length == 0);
        R_UNLESS(backend->Read(out_bytes.data(), length, offset) == static_cast<u64>(length),
                 FileSys::ResultOutOfRange);
        R_SUCCEED();
    }

    Result GetSize(Out<u64> out_size) {
        *out_size = backend->GetSize();
        R_SUCCEED();
    }

    const std::shared_ptr<const FileSys::VfsFile> backend;
};

class ReadOnlySystemArchiveFsp final : public ReadOnlyFspService<ReadOnlySystemArchiveFsp> {
public:
    ReadOnlySystemArchiveFsp(Core::System& system_,
                             std::shared_ptr<const SystemDataView> isolated_system_data,
                             u64 owner)
        : ReadOnlyFspService{system_, "fsp-srv", owner}, system_data{std::move(isolated_system_data)} {
        static const FunctionInfo functions[] = {
            {202, D<&ReadOnlySystemArchiveFsp::OpenDataStorageByDataId>, "OpenDataStorageByDataId"},
            {210, D<&ReadOnlySystemArchiveFsp::SetCurrentProcess>, "SetCurrentProcess"},
        };
        RegisterHandlers(functions);
    }

private:
    Result SetCurrentProcess(ClientProcessId pid) {
        process_bound = system_data && pid && *pid == owner_process_id;
        R_UNLESS(process_bound, FileSys::ResultPermissionDenied);
        R_SUCCEED();
    }

    Result OpenDataStorageByDataId(OutInterface<ReadOnlySystemArchive> out_storage,
                                   FileSys::StorageId storage, u32 unknown, u64 data_id) {
        R_UNLESS(process_bound && storage == FileSys::StorageId::NandSystem,
                 FileSys::ResultPermissionDenied);
        const auto archive = system_data->OpenNativeDataArchive(data_id);
        R_UNLESS(archive, FileSys::ResultTargetNotFound);
        *out_storage = std::make_shared<ReadOnlySystemArchive>(system, archive, owner_process_id);
        R_SUCCEED();
    }

    const std::shared_ptr<const SystemDataView> system_data;
    bool process_bound{};
};

} // namespace

std::shared_ptr<SessionRequestHandler> CreateReadOnlySystemArchiveFsp(
    Core::System& system, std::shared_ptr<const SystemDataView> isolated_system_data,
    u64 owner_process_id) {
    return std::make_shared<ReadOnlySystemArchiveFsp>(system, std::move(isolated_system_data),
                                                      owner_process_id);
}

} // namespace Service::FileSystem
