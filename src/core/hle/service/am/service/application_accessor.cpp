// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/result.h"
#include "core/hle/service/am/am_results.h"
#include "core/hle/service/am/am_types.h"
#include "core/hle/service/am/applet.h"
#include "core/hle/service/am/applet_data_broker.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/frontend/applets.h"
#include "core/hle/service/am/service/application_accessor.h"
#include "core/hle/service/am/service/library_applet_accessor.h"
#include "core/hle/service/am/service/storage.h"
#include "core/hle/service/am/window_system.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/glue/glue_manager.h"

namespace Service::AM {

IApplicationAccessor::IApplicationAccessor(Core::System& system_, std::shared_ptr<Applet> applet,
                                           WindowSystem& window_system)
    : ServiceFramework{system_, "IApplicationAccessor"}, m_window_system(window_system),
      m_applet(std::move(applet)) {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&IApplicationAccessor::GetAppletStateChangedEvent>, "GetAppletStateChangedEvent"},
        {1, D<&IApplicationAccessor::IsCompleted>, "IsCompleted"},
        {10, D<&IApplicationAccessor::Start>, "Start"},
        {20, D<&IApplicationAccessor::RequestExit>, "RequestExit"},
        {25, D<&IApplicationAccessor::Terminate>, "Terminate"},
        {30, D<&IApplicationAccessor::GetResult>, "GetResult"},
        {101, D<&IApplicationAccessor::RequestForApplicationToGetForeground>, "RequestForApplicationToGetForeground"},
        {110, D<&IApplicationAccessor::TerminateAllLibraryApplets>, "TerminateAllLibraryApplets"},
        {111, D<&IApplicationAccessor::AreAnyLibraryAppletsLeft>, "AreAnyLibraryAppletsLeft"},
        {112, D<&IApplicationAccessor::GetCurrentLibraryApplet>, "GetCurrentLibraryApplet"},
        {120, D<&IApplicationAccessor::GetApplicationId>, "GetApplicationId"},
        {121, D<&IApplicationAccessor::PushLaunchParameter>, "PushLaunchParameter"},
        {122, D<&IApplicationAccessor::GetApplicationControlProperty>, "GetApplicationControlProperty"},
        {123, D<&IApplicationAccessor::GetApplicationLaunchProperty>, "GetApplicationLaunchProperty"},
        {124, nullptr, "GetApplicationLaunchRequestInfo"},
        {130, D<&IApplicationAccessor::SetUsers>, "SetUsers"},
        {131, D<&IApplicationAccessor::CheckRightsEnvironmentAvailable>, "CheckRightsEnvironmentAvailable"},
        {132, D<&IApplicationAccessor::GetNsRightsEnvironmentHandle>, "GetNsRightsEnvironmentHandle"},
        {140, D<&IApplicationAccessor::GetDesirableUids>, "GetDesirableUids"},
        {150, D<&IApplicationAccessor::ReportApplicationExitTimeout>, "ReportApplicationExitTimeout"},
        {160, D<&IApplicationAccessor::SetApplicationAttribute>, "SetApplicationAttribute"},
        {170, D<&IApplicationAccessor::HasSaveDataAccessPermission>, "HasSaveDataAccessPermission"},
        {180, nullptr, "PushToFriendInvitationStorageChannel"},
        {190, nullptr, "PushToNotificationStorageChannel"},
        {200, nullptr, "RequestApplicationSoftReset"},
        {201, nullptr, "RestartApplicationTimer"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

IApplicationAccessor::~IApplicationAccessor() = default;

Result IApplicationAccessor::Start() {
    LOG_INFO(Service_AM, "called");
    m_applet->process->Run();
    R_SUCCEED();
}

Result IApplicationAccessor::RequestExit() {
    LOG_INFO(Service_AM, "called");

    std::scoped_lock lk{m_applet->lock};
    if (m_applet->exit_locked) {
        m_applet->lifecycle_manager.RequestExit(system.Kernel());
        m_applet->UpdateSuspensionStateLocked(true);
    } else {
        m_applet->process->Terminate();
    }

    R_SUCCEED();
}

Result IApplicationAccessor::Terminate() {
    LOG_INFO(Service_AM, "called");
    m_applet->process->Terminate();
    R_SUCCEED();
}

Result IApplicationAccessor::GetResult() {
    LOG_INFO(Service_AM, "called");
    std::scoped_lock lk{m_applet->lock};
    R_RETURN(m_applet->terminate_result);
}

Result IApplicationAccessor::IsCompleted(Out<bool> out_is_completed) {
    LOG_DEBUG(Service_AM, "called");
    std::scoped_lock lk{m_applet->lock};
    *out_is_completed = m_applet->is_completed;
    R_SUCCEED();
}

Result IApplicationAccessor::GetAppletStateChangedEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_INFO(Service_AM, "called");
    *out_event = m_applet->state_changed_event.GetHandle();
    R_SUCCEED();
}

Result IApplicationAccessor::PushLaunchParameter(LaunchParameterKind kind,
                                                 SharedPointer<IStorage> storage) {
    LOG_INFO(Service_AM, "called, kind={}", kind);

    switch (kind) {
    case LaunchParameterKind::AccountPreselectedUser:
        m_applet->preselected_user_launch_parameter.push_back(storage->GetData());
        R_SUCCEED();
    default:
        R_THROW(ResultUnknown);
    }
}

Result IApplicationAccessor::GetApplicationControlProperty(
    OutBuffer<BufferAttr_HipcMapAlias> out_control_property) {
    LOG_INFO(Service_AM, "called");

    std::vector<u8> nacp;
    R_TRY(system.GetARPManager().GetControlProperty(&nacp, m_applet->program_id));

    std::memcpy(out_control_property.data(), nacp.data(),
                (std::min)(out_control_property.size(), nacp.size()));

    R_SUCCEED();
}

Result IApplicationAccessor::SetUsers(bool enable,
                                      InArray<Common::UUID, BufferAttr_HipcMapAlias> user_ids) {
    LOG_INFO(Service_AM, "called, enable={} user_id_count={}", enable, user_ids.size());
    R_SUCCEED();
}

Result IApplicationAccessor::GetCurrentLibraryApplet(
    Out<SharedPointer<ILibraryAppletAccessor>> out_accessor) {
    LOG_INFO(Service_AM, "(STUBBED) called");
    *out_accessor = nullptr;
    R_SUCCEED();
}

Result IApplicationAccessor::TerminateAllLibraryApplets() {
    LOG_INFO(Service_AM, "called");
    std::list<std::shared_ptr<Applet>> child_applets;
    {
        std::scoped_lock lk{m_applet->lock};
        child_applets = m_applet->child_applets;
    }
    for (const auto& child_applet : child_applets) {
        {
            std::scoped_lock lk{child_applet->lock};
            child_applet->process->Terminate();
            child_applet->terminate_result = AM::ResultLibraryAppletTerminated;
        }
        if (child_applet->frontend) {
            child_applet->frontend->RequestExit();
        }
    }
    R_SUCCEED();
}

Result IApplicationAccessor::AreAnyLibraryAppletsLeft(Out<bool> out_is_any_left) {
    LOG_DEBUG(Service_AM, "called");
    std::scoped_lock lk{m_applet->lock};
    *out_is_any_left = !m_applet->child_applets.empty();
    R_SUCCEED();
}

Result IApplicationAccessor::GetApplicationId(Out<u64> out_application_id) {
    LOG_DEBUG(Service_AM, "called");
    *out_application_id = m_applet->program_id;
    R_SUCCEED();
}

Result IApplicationAccessor::GetApplicationLaunchProperty(
    Out<Glue::ApplicationLaunchProperty> out_launch_property) {
    LOG_DEBUG(Service_AM, "called");
    R_RETURN(system.GetARPManager().GetLaunchProperty(out_launch_property.Get(),
                                                      m_applet->program_id));
}

Result IApplicationAccessor::GetDesirableUids(
    Out<s32> out_count, OutArray<Common::UUID, BufferAttr_HipcMapAlias> out_user_ids) {
    LOG_DEBUG(Service_AM, "(STUBBED) called");
    *out_count = 0;
    R_SUCCEED();
}

Result IApplicationAccessor::SetApplicationAttribute(
    InBuffer<BufferAttr_HipcMapAlias> attribute) {
    LOG_WARNING(Service_AM, "(STUBBED) called, size={}", attribute.size());
    R_SUCCEED();
}

Result IApplicationAccessor::HasSaveDataAccessPermission(Out<bool> out_has_permission,
                                                         u64 application_id) {
    LOG_DEBUG(Service_AM, "(STUBBED) called, application_id={:016X}", application_id);
    *out_has_permission = true;
    R_SUCCEED();
}

Result IApplicationAccessor::RequestForApplicationToGetForeground() {
    LOG_INFO(Service_AM, "called");
    m_window_system.RequestApplicationToGetForeground();
    R_SUCCEED();
}

Result IApplicationAccessor::CheckRightsEnvironmentAvailable(Out<bool> out_is_available) {
    LOG_WARNING(Service_AM, "(STUBBED) called");
    *out_is_available = true;
    R_SUCCEED();
}

Result IApplicationAccessor::GetNsRightsEnvironmentHandle(Out<u64> out_handle) {
    LOG_WARNING(Service_AM, "(STUBBED) called");
    *out_handle = 0xdeadbeef;
    R_SUCCEED();
}

Result IApplicationAccessor::ReportApplicationExitTimeout() {
    LOG_ERROR(Service_AM, "called");
    R_SUCCEED();
}

} // namespace Service::AM
