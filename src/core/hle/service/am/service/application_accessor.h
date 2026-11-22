// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/uuid.h"
#include "core/hle/service/am/am_types.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/glue/glue_manager.h"
#include "core/hle/service/service.h"

namespace Service::AM {

struct Applet;
class ILibraryAppletAccessor;
class IStorage;
class WindowSystem;

class IApplicationAccessor final : public ServiceFramework<IApplicationAccessor> {
public:
    explicit IApplicationAccessor(Core::System& system_, std::shared_ptr<Applet> applet,
                                  WindowSystem& window_system);
    ~IApplicationAccessor() override;

private:
    Result IsCompleted(Out<bool> out_is_completed);
    Result Start();
    Result RequestExit();
    Result Terminate();
    Result GetResult();
    Result GetAppletStateChangedEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result PushLaunchParameter(LaunchParameterKind kind, SharedPointer<IStorage> storage);
    Result GetApplicationControlProperty(OutBuffer<BufferAttr_HipcMapAlias> out_control_property);
    Result SetUsers(bool enable, InArray<Common::UUID, BufferAttr_HipcMapAlias> user_ids);
    Result GetCurrentLibraryApplet(Out<SharedPointer<ILibraryAppletAccessor>> out_accessor);
    Result TerminateAllLibraryApplets();
    Result AreAnyLibraryAppletsLeft(Out<bool> out_is_any_left);
    Result GetApplicationId(Out<u64> out_application_id);
    Result GetApplicationLaunchProperty(Out<Glue::ApplicationLaunchProperty> out_launch_property);
    Result GetDesirableUids(Out<s32> out_count,
                            OutArray<Common::UUID, BufferAttr_HipcMapAlias> out_user_ids);
    Result SetApplicationAttribute(InBuffer<BufferAttr_HipcMapAlias> attribute);
    Result HasSaveDataAccessPermission(Out<bool> out_has_permission, u64 application_id);
    Result RequestForApplicationToGetForeground();
    Result CheckRightsEnvironmentAvailable(Out<bool> out_is_available);
    Result GetNsRightsEnvironmentHandle(Out<u64> out_handle);
    Result ReportApplicationExitTimeout();

    WindowSystem& m_window_system;
    const std::shared_ptr<Applet> m_applet;
};

} // namespace Service::AM
