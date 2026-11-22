// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <condition_variable>
#include <mutex>

#include "core/hle/service/am/am_types.h"

namespace Core {
class System;
}

namespace Service {
class Process;
}

namespace Service::AM {

class WindowSystem;

enum class LaunchType {
    FrontendInitiated,
    ApplicationInitiated,
};

struct FrontendAppletParameters {
    ProgramId program_id{};
    AppletId applet_id{};
    AppletType applet_type{};
    LaunchType launch_type{};
    s32 program_index{};
    s32 previous_program_index{-1};

    constexpr bool UsesNativeHomeBootstrap(ProgramId process_program_id,
                                            bool process_is_switch2) const {
        constexpr ProgramId NativeHomeProgramId = 0x0400000000001000ULL;
        return process_is_switch2 && applet_type == AppletType::SystemApplet &&
               applet_id == AppletId::QLaunch &&
               program_id == NativeHomeProgramId && process_program_id == NativeHomeProgramId;
    }
};

class AppletManager {
public:
    explicit AppletManager(Core::System& system);
    ~AppletManager();

    void CreateAndInsertByFrontendAppletParameters(std::unique_ptr<Process> process,
                                                   const FrontendAppletParameters& params);
    void RequestExit();
    /// At shutdown: releases the process AM never took (the boot closed before AM started), while
    /// the kernel that owns it still runs. The next boot would otherwise free it into its own.
    void DropPendingProcess();
    void OperationModeChanged();
    /// A HOME button press from the frontend (hotkey), as the controller's HOME button sends.
    void PressHomeButton();

public:
    void SetWindowSystem(WindowSystem* window_system);
    [[nodiscard]] WindowSystem* GetWindowSystem() const { return m_window_system; }

private:
    Core::System& m_system;

    std::mutex m_lock;
    std::condition_variable m_cv;

    WindowSystem* m_window_system{};
    bool m_docked{}; // console mode the applets were last told about

    FrontendAppletParameters m_pending_parameters{};
    std::unique_ptr<Process> m_pending_process{};
};

} // namespace Service::AM
