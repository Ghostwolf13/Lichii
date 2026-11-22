// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Core {
class System;
}

namespace Service {
class SystemDataView;

namespace NS {
void LoopProcess(Core::System& system, const SystemDataView* isolated_system_data);

} // namespace NS
} // namespace Service
