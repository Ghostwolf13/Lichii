// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

namespace Core {
class System;
}

namespace Service {
class ServiceFrameworkBase;

namespace PlayReport {

std::shared_ptr<ServiceFrameworkBase> CreateService(const char* name, Core::System& system);
void LoopProcess(Core::System& system);

} // namespace PlayReport
} // namespace Service
