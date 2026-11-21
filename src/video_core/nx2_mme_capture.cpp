// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "common/logging.h"
#include "video_core/nx2_mme_capture.h"

namespace VideoCommon::MmeCapture {
namespace {

constexpr size_t HistoryBytes = 8 * 1024 * 1024;
constexpr size_t RecentEntries = 16;
constexpr size_t MaxParameters = 256;

struct Capture {
    std::filesystem::path path;
    std::string history;
    size_t dropped{};
    std::deque<std::string> entries;
    std::vector<const void*> engines;
};

Capture capture;

void Window(std::ofstream& out, std::string_view name, u32 pc, std::span<const u32> code) {
    const size_t count = code.size() / 3;
    if (pc >= count) {
        out << fmt::format("{} pc={:#x} outside RAM ({} instructions)\n", name, pc, count);
        return;
    }
    const size_t first = pc >= 4 ? pc - 4 : 0;
    const size_t end = std::min<size_t>(count, static_cast<size_t>(pc) + 5);
    out << fmt::format("{} window at {:#x} (words in upload order)\n", name, pc);
    for (size_t i = first; i < end; ++i) {
        out << fmt::format("  {:#06x}: {:08x}\n", i, fmt::join(code.subspan(i * 3, 3), " "));
    }
}

} // namespace

void StartSession() {
    const char* path = std::getenv("EDEN_NX2_MME_STARTUP_CAPTURE");
    capture = {};
    active = path != nullptr && *path != '\0';
    if (active) {
        capture.path = path;
    }
}

size_t EngineId(const void* engine) {
    const auto it = std::ranges::find(capture.engines, engine);
    if (it != capture.engines.end()) {
        return static_cast<size_t>(it - capture.engines.begin());
    }
    capture.engines.push_back(engine);
    return capture.engines.size() - 1;
}

void Record(std::string line) {
    if (capture.dropped != 0 || line.size() + 1 > HistoryBytes - capture.history.size()) {
        ++capture.dropped;
        return;
    }
    capture.history += line;
    capture.history += '\n';
}

void RecordEntry(std::string line) {
    if (capture.entries.size() == RecentEntries) {
        capture.entries.pop_front();
    }
    capture.entries.push_back(std::move(line));
}

void RecordWrite(size_t address_space, const void* engine, std::string_view source,
                 std::string_view bank, u32 method, u32 value, u32 code_cursor, u32 bind_cursor) {
    Record(fmt::format("write as={} engine={} source={} bank={} method={:#x} arg={:#010x} "
                       "code_word={:#x} bind_slot={:#x}",
                       address_space, EngineId(engine), source, bank, method, value, code_cursor,
                       bind_cursor));
}

void Fail(size_t address_space, const void* engine, std::string_view bank, u32 method, u32 slot,
          u32 start, const Tegra::TuringMacroInterpreter::Result& result, u32 last_pc,
          std::span<const u32> code, std::span<const u32> parameters) {
    const size_t engine_id = EngineId(engine);
    active = false;
    const auto state = std::exchange(capture, {});
    std::error_code error;
    if (std::filesystem::exists(state.path, error) || error) {
        LOG_WARNING(HW_GPU, "MME startup capture: output already exists or is inaccessible");
        return;
    }
    std::ofstream out{state.path};
    out << "Lichii MME startup capture v1 (game data; share privately)\n";
    out << fmt::format("first compute failure: as={} engine={} bank={} method={:#x} slot={:#x}"
                       " start={:#x} pc={:#x} consumed={} fifo_words={} error={}\n",
                       address_space, engine_id, bank, method, slot, start, result.pc,
                       result.consumed, parameters.size(), result.error);
    if (last_pc != ~0U) {
        out << fmt::format("last executed PC: {:#x}\n", last_pc);
    }
    const auto printed = parameters.first(std::min(parameters.size(), MaxParameters));
    out << fmt::format("FIFO at failure (showing {} of {} words): {:08x}\n", printed.size(),
                       parameters.size(), fmt::join(printed, " "));
    Window(out, "start", start, code);
    if (last_pc != ~0U && last_pc != start && last_pc != result.pc) {
        Window(out, "last executed", last_pc, code);
    }
    if (result.pc != start) {
        Window(out, "failure", result.pc, code);
    }
    out << fmt::format("\nMME writes and channel binds ({} records omitted):\n", state.dropped);
    out << state.history;
    out << "\nMost recent GPFIFO parse inputs (first 128 words per input):\n";
    for (const auto& entry : state.entries) {
        out << entry << '\n';
    }
    out.close();
    if (!out) {
        LOG_WARNING(HW_GPU, "MME startup capture: could not write complete output");
        return;
    }
    LOG_INFO(HW_GPU, "MME startup capture saved. The file contains game data; share privately.");
}

} // namespace VideoCommon::MmeCapture
