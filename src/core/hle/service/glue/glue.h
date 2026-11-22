// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

namespace Core {
class System;
} // namespace Core

namespace Service::Glue {

namespace Time {
class TimeZoneBinary;
}

void LoopProcess(Core::System& system, std::shared_ptr<Time::TimeZoneBinary> isolated_time_zone);

} // namespace Service::Glue
