// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/httplib.h"
#include "common/logging.h"
#include "common/nextendo.h"
#include "common/npln_patch_catalog.h"
#include "common/npln_patch_manifest.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Common::NplnPatchCatalog {
namespace {

using Clock = std::chrono::steady_clock;

// citron-nextendo's verification key for nextendo.network's signed NPLN catalogue (public).
constexpr std::string_view PublicKey = "LAI+Pi+GZtj3p4DpNM5ORBraw1cbZCVSMFgUmAVWNeA=";
constexpr auto RefreshInterval = std::chrono::minutes(15);
constexpr auto PrepareTimeout = std::chrono::seconds(10);

struct State {
    std::mutex mutex;
    std::once_flag initialized;
    PatchState patches;
    TitleIndexState index;
    // When the index was last fetched or confirmed; a failed fetch leaves it, so the next launch
    // tries again.
    std::optional<Clock::time_point> index_checked;
    std::map<std::string, Clock::time_point> next_refresh;
};

State& Cache() {
    static State state;
    return state;
}

s64 Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::filesystem::path CacheDir() {
    return FS::GetEdenPath(FS::EdenPath::CacheDir) / "nextendo-npln";
}

std::string TargetName(u64 title_id, std::string_view build_id) {
    return fmt::format("{:016x}-{}", title_id, build_id);
}

std::string ReadDocument(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::string body(MaxDocument + 1, '\0');
    input.read(body.data(), static_cast<std::streamsize>(body.size()));
    body.resize(static_cast<std::size_t>(input.gcount()));
    return body;
}

std::vector<std::filesystem::path> Documents(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> paths;
    std::error_code error;
    for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end;
         it.increment(error)) {
        if (it->is_regular_file(error) && it->path().extension() == ".json") {
            paths.push_back(it->path());
        }
    }
    return paths;
}

// Every kept envelope is verified again on load; nothing unsigned is ever trusted. Earlier
// builds stay available even when a newer catalogue no longer lists them.
void LoadCache() {
    auto& state = Cache();
    for (const auto& path : Documents(CacheDir() / "verified")) {
        try {
            const auto manifest = VerifyManifest(ReadDocument(path), PublicKey, Now(), true);
            std::scoped_lock lock(state.mutex);
            state.patches.Remember(manifest);
        } catch (const std::exception& error) {
            LOG_WARNING(WebService, "Nextendo: NPLN patch cache {} rejected: {}",
                        path.filename().string(), error.what());
        }
    }

    // Replays signed discovery in revision order, so partial lists keep known titles.
    std::vector<TitleIndex> indexes;
    for (const auto& path : Documents(CacheDir() / "title-index")) {
        try {
            indexes.push_back(VerifyTitleIndex(ReadDocument(path), PublicKey, Now(), true));
        } catch (const std::exception& error) {
            LOG_WARNING(WebService, "Nextendo: NPLN title index cache {} rejected: {}",
                        path.filename().string(), error.what());
        }
    }
    std::sort(indexes.begin(), indexes.end(),
              [](const auto& a, const auto& b) { return a.revision < b.revision; });
    std::scoped_lock lock(state.mutex);
    for (const auto& index : indexes) {
        try {
            state.index.Remember(index);
        } catch (const std::exception& error) {
            LOG_WARNING(WebService, "Nextendo: NPLN title index cache rejected: {}", error.what());
        }
    }
}

// A failed write only costs the offline copy; the verified document is used either way.
void SaveDocument(const std::filesystem::path& path, const std::string& envelope) {
    try {
        auto temporary = path;
        temporary += ".tmp";
        std::filesystem::create_directories(path.parent_path());
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output.write(envelope.data(), static_cast<std::streamsize>(envelope.size()));
            output.flush();
            if (!output) {
                throw std::runtime_error("cannot write the file");
            }
        }
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error("cannot replace the file");
        }
#else
        std::filesystem::rename(temporary, path);
#endif
    } catch (const std::exception& error) {
        LOG_WARNING(WebService, "Nextendo: NPLN cache {} not saved: {}", path.filename().string(),
                    error.what());
    }
}

struct Response {
    int status{};
    std::string etag;
    std::string body;
};

// Downloads over HTTPS from nextendo.network, never following a redirect, within the deadline
// and MaxDocument bytes.
std::optional<Response> Get(const std::string& path, const std::string& tag,
                            Clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    if (left <= std::chrono::milliseconds::zero() || !Nextendo::TakeRequestSlot()) {
        return std::nullopt;
    }
    httplib::Client client{std::string{Nextendo::ApiBase}};
    Nextendo::ConfigureClient(client, 5);
    client.set_max_timeout(left);
    client.set_follow_location(false);
    httplib::Headers headers{{"User-Agent", "Lichii"},
                             {"Accept", "application/json"},
                             {"Cache-Control", "no-cache"}};
    if (!tag.empty()) {
        headers.emplace("If-None-Match", tag);
    }
    Response response;
    const auto result = client.Get(path, headers, [&](const char* data, std::size_t length) {
        if (length > MaxDocument - response.body.size()) {
            return false;
        }
        response.body.append(data, length);
        return true;
    });
    if (!result) {
        return std::nullopt;
    }
    response.status = result->status;
    response.etag = result->get_header_value("ETag");
    return response;
}

void DownloadTitleIndex(State& state, Clock::time_point deadline) {
    std::string tag;
    {
        std::scoped_lock lock(state.mutex);
        tag = state.index.Tag();
    }
    const auto response = Get("/service-catalog/v1/npln-title-index", tag, deadline);
    if (response && response->status == 304 && !tag.empty() && response->etag == tag) {
        std::scoped_lock lock(state.mutex);
        state.index_checked = Clock::now();
        return;
    }
    if (!response || response->status != 200) {
        throw std::runtime_error("title index download unavailable");
    }
    const auto index = VerifyTitleIndex(response->body, PublicKey, Now());
    if (response->etag != index.etag) {
        throw std::runtime_error("title index response tag mismatch");
    }
    {
        std::scoped_lock lock(state.mutex);
        state.index.Remember(index);
        state.index_checked = Clock::now();
    }
    SaveDocument(CacheDir() / "title-index" / fmt::format("revision-{}.json", index.revision),
                 response->body);
    LOG_INFO(WebService, "Nextendo: NPLN title index verified, revision {}, {} titles",
             index.revision, index.titles.size());
}

// Returns once the server answered for this build: true for a verified patch, new or unchanged,
// and false when the build has none.
bool Download(State& state, u64 title_id, const std::string& build_id,
              Clock::time_point deadline) {
    std::string tag;
    {
        std::scoped_lock lock(state.mutex);
        tag = state.patches.Tag(title_id, build_id);
    }
    const auto response = Get(
        fmt::format("/service-catalog/v1/npln-patches/{:016x}/{}", title_id, build_id), tag,
        deadline);
    if (response && response->status == 404) {
        return false;
    }
    if (response && response->status == 304 && !tag.empty() && response->etag == tag) {
        return true;
    }
    if (!response || response->status != 200) {
        throw std::runtime_error("patch download unavailable");
    }
    const auto manifest = VerifyBuildManifest(response->body, PublicKey, Now(), title_id, build_id);
    if (response->etag != manifest.etag) {
        throw std::runtime_error("patch response tag mismatch");
    }
    {
        std::scoped_lock lock(state.mutex);
        if (manifest.revision < state.patches.Revision(title_id, build_id)) {
            throw std::runtime_error("patch catalog revision rollback rejected");
        }
        state.patches.Remember(manifest);
    }
    SaveDocument(CacheDir() / "verified" /
                     fmt::format("{}-revision-{}.json", TargetName(title_id, build_id),
                                 manifest.revision),
                 response->body);
    LOG_INFO(WebService, "Nextendo: NPLN patch {} v{} verified for build {}",
             manifest.patches[0].id, manifest.patches[0].version, build_id);
    return true;
}

// Whether this build was last answered RefreshInterval ago, here or in an earlier run. An earlier
// run's answer that had a patch only counts when that patch is cached, so a patch whose save
// failed is downloaded again.
bool RefreshDue(State& state, const std::string& target, bool patch_cached) {
    const auto now = Clock::now();
    if (!state.next_refresh.contains(target)) {
        std::ifstream last_check(CacheDir() / "checks" / (target + ".txt"));
        s64 checked_at{};
        int had_patch{};
        const s64 wall_now = Now();
        const s64 interval =
            std::chrono::duration_cast<std::chrono::seconds>(RefreshInterval).count();
        if (last_check >> checked_at >> had_patch && (had_patch == 0 || patch_cached) &&
            checked_at > 0 && checked_at <= wall_now && wall_now - checked_at < interval) {
            state.next_refresh[target] =
                now + std::chrono::seconds(interval - (wall_now - checked_at));
        }
    }
    return now >= state.next_refresh[target];
}

void RecordCheck(State& state, const std::string& target, bool has_patch) {
    {
        std::scoped_lock lock(state.mutex);
        state.next_refresh[target] = Clock::now() + RefreshInterval;
    }
    const auto checks = CacheDir() / "checks" / (target + ".txt");
    std::error_code error;
    std::filesystem::create_directories(checks.parent_path(), error);
    std::ofstream last_check(checks, std::ios::trunc);
    last_check << Now() << ' ' << (has_patch ? 1 : 0) << '\n';
}

void Prepare(State& state, u64 title_id,
             const std::function<std::optional<std::string>()>& read_build_id) {
    const auto deadline = Clock::now() + PrepareTimeout;
    std::call_once(state.initialized, LoadCache);

    bool index_due;
    {
        std::scoped_lock lock(state.mutex);
        index_due = !state.index_checked || Clock::now() - *state.index_checked >= RefreshInterval;
    }
    if (index_due) {
        try {
            DownloadTitleIndex(state, deadline);
        } catch (const std::exception& error) {
            LOG_WARNING(WebService, "Nextendo: NPLN title index check failed, keeping the verified "
                                    "index: {}",
                        error.what());
        }
    }

    // A title the index stops listing keeps its verified patches until a signed entry withdraws
    // them, so its builds are still refreshed.
    {
        std::scoped_lock lock(state.mutex);
        if (!state.index.HasTitle(title_id) && !state.patches.HasTitle(title_id)) {
            return;
        }
    }
    const auto build_id = read_build_id();
    if (!build_id || build_id->size() != 64 ||
        build_id->find_first_not_of("0123456789ABCDEF") != std::string::npos) {
        LOG_WARNING(WebService, "Nextendo: no main build ID to check the NPLN patch with");
        return;
    }

    const auto target = TargetName(title_id, *build_id);
    bool refresh;
    {
        std::scoped_lock lock(state.mutex);
        const auto cached = state.patches.Cached(title_id, *build_id);
        refresh = !(cached && cached->final) && RefreshDue(state, target, cached != nullptr);
    }
    if (refresh) {
        try {
            RecordCheck(state, target, Download(state, title_id, *build_id, deadline));
        } catch (const std::exception& error) {
            LOG_WARNING(WebService, "Nextendo: NPLN patch refresh for {} failed, keeping verified "
                                    "patches: {}",
                        target, error.what());
        }
    }

    std::scoped_lock lock(state.mutex);
    state.patches.BeginGame(title_id);
    LOG_INFO(WebService, "Nextendo: NPLN patches fixed for this launch, catalog revision {}",
             state.patches.LatestRevision());
}

} // namespace

void PrepareForGame(u64 title_id,
                    const std::function<std::optional<std::string>()>& read_build_id) {
    auto& state = Cache();
    {
        std::scoped_lock lock(state.mutex);
        state.patches.EndGame();
    }
    if (!Nextendo::OnlinePlayActive()) {
        return;
    }
    try {
        Prepare(state, title_id, read_build_id);
    } catch (const std::exception& error) {
        LOG_WARNING(WebService, "Nextendo: NPLN patch check failed: {}", error.what());
    }
}

std::optional<AppliedPatch> Apply(u64 title_id, std::string_view build_id, std::string_view module,
                                  std::span<u8> section) {
    if (!Nextendo::OnlinePlayActive()) {
        return std::nullopt;
    }
    auto& state = Cache();
    std::scoped_lock lock(state.mutex);
    const auto patch = state.patches.Find(title_id, build_id, module);
    if (!patch) {
        return std::nullopt;
    }
    if (!patch->enabled) {
        LOG_WARNING(WebService, "Nextendo: NPLN patch {} withdrawn by the signed catalog",
                    patch->id);
        return std::nullopt;
    }
    if (!CheckGuards(*patch, section)) {
        LOG_ERROR(WebService, "Nextendo: NPLN patch {} rejected, the original instructions do not "
                              "match",
                  patch->id);
        return std::nullopt;
    }
    ApplyGuards(*patch, section);
    return AppliedPatch{patch->id, patch->version};
}

} // namespace Common::NplnPatchCatalog
