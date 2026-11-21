// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>
#include <string>
#include <string_view>

#include "common/common_types.h"
#include "video_core/macro_turing.h"

namespace VideoCommon::MmeCapture {

inline bool active{};

void StartSession();
void Record(std::string line);
void RecordEntry(std::string line);
void RecordWrite(size_t address_space, const void* engine, std::string_view source,
                 std::string_view bank, u32 method, u32 value, u32 code_cursor, u32 bind_cursor);
void Fail(size_t address_space, const void* engine, std::string_view bank, u32 method, u32 slot,
          u32 start, const Tegra::TuringMacroInterpreter::Result& result, u32 last_pc,
          std::span<const u32> code, std::span<const u32> parameters);

size_t EngineId(const void* engine);

} // namespace VideoCommon::MmeCapture
