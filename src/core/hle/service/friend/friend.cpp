// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2025 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <queue>
#include <string_view>
#include <vector>
#include <fmt/format.h>
#include "common/logging.h"
#include "common/nextendo.h"
#include "common/uuid.h"
#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/acc/errors.h"
#include "core/hle/service/acc/nextendo_id_token.h"
#include "core/hle/service/friend/friend.h"
#include "core/hle/service/friend/friend_interface.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/nx2_stub_interface.h"
#include "core/hle/service/server_manager.h"

namespace Service::Friend {

namespace {

#pragma pack(push, 1)
struct UserPresenceImpl {
    Common::UUID user_id;
    s64 last_time_online_timestamp;
    u32 status;
    u8 same_presence_group_application;
    std::array<u8, 3> unknown;
    std::array<u8, 0xC0> app_key_value;
};
static_assert(sizeof(UserPresenceImpl) == 0xE0, "UserPresenceImpl has the wrong size");

struct FriendImpl {
    Common::UUID user_id;
    u64 network_user_id;
    std::array<char, 0x21> nickname;
    std::array<u8, 7> nickname_pad;
    UserPresenceImpl presence;
    u8 is_favourite;
    u8 is_new;
    std::array<u8, 6> unknown;
    u8 is_valid;
    std::array<u8, 0xD7> padding;
};
static_assert(sizeof(FriendImpl) == 0x200, "FriendImpl has the wrong size");
static_assert(offsetof(FriendImpl, presence) == 0x40);
static_assert(offsetof(FriendImpl, is_valid) == 0x128);

// Offsets as nnSdk's Profile accessors read them.
struct ProfileImpl {
    u64 network_user_id;
    std::array<char, 0x21> nickname;
    std::array<u8, 7> nickname_pad;
    std::array<char, 0xA0> image_url;
    u8 is_valid;
    std::array<u8, 0x2F> reserved;
};
static_assert(sizeof(ProfileImpl) == 0x100, "ProfileImpl has the wrong size");
static_assert(offsetof(ProfileImpl, image_url) == 0x30);
static_assert(offsetof(ProfileImpl, is_valid) == 0xD0);
#pragma pack(pop)

// A real friend list never has more than 300 entries.
constexpr std::size_t MaxRequestedIds = 300;

// These NPLN titles know their friends by NSA (NetworkServiceAccountId), not by Nextendo pid.
constexpr std::array<u64, 2> NsaIdTitles{
    0x0100C2500FC20000, // Splatoon 3
    0x010015100B514000, // Super Mario Bros. Wonder
};

// Friends are listed without their pictures, so each gets this plain grey 256x256 icon. Hardware
// always answers with one, and some titles abort on an empty icon.
constexpr std::array<u8, 667> DefaultFriendIcon{
    0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01,
    0x00, 0x01, 0x00, 0x00, 0xFF, 0xDB, 0x00, 0x43, 0x00, 0x05, 0x03, 0x04, 0x04, 0x04, 0x03, 0x05,
    0x04, 0x04, 0x04, 0x05, 0x05, 0x05, 0x06, 0x07, 0x0C, 0x08, 0x07, 0x07, 0x07, 0x07, 0x0F, 0x0B,
    0x0B, 0x09, 0x0C, 0x11, 0x0F, 0x12, 0x12, 0x11, 0x0F, 0x11, 0x11, 0x13, 0x16, 0x1C, 0x17, 0x13,
    0x14, 0x1A, 0x15, 0x11, 0x11, 0x18, 0x21, 0x18, 0x1A, 0x1D, 0x1D, 0x1F, 0x1F, 0x1F, 0x13, 0x17,
    0x22, 0x24, 0x22, 0x1E, 0x24, 0x1C, 0x1E, 0x1F, 0x1E, 0xFF, 0xDB, 0x00, 0x43, 0x01, 0x05, 0x05,
    0x05, 0x07, 0x06, 0x07, 0x0E, 0x08, 0x08, 0x0E, 0x1E, 0x14, 0x11, 0x14, 0x1E, 0x1E, 0x1E, 0x1E,
    0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E,
    0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E,
    0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0xFF, 0xC0,
    0x00, 0x11, 0x08, 0x01, 0x00, 0x01, 0x00, 0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11,
    0x01, 0xFF, 0xC4, 0x00, 0x15, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0xFF, 0xC4, 0x00, 0x14, 0x10, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xC4,
    0x00, 0x14, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xFF, 0xC4, 0x00, 0x14, 0x11, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xDA, 0x00, 0x0C, 0x03, 0x01,
    0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3F, 0x00, 0xB0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xFF, 0xD9};

// The friend list of the game's profile while Nextendo online play is on, read from the cache
// the frontend refreshes.
std::vector<Common::Nextendo::Friend> Friends(const Core::System& system) {
    if (!Common::Nextendo::OnlinePlayActive()) {
        return {};
    }
    return Common::Nextendo::CachedFriends(Account::Nextendo::CurrentProfile(system)).friends;
}

bool WantsNsaIds(Core::System& system) {
    return std::ranges::find(NsaIdTitles, system.GetApplicationProcessProgramID()) !=
           NsaIdTitles.end();
}

// Consoles announce their account id and emulators their pid; NPLN titles want the NSA for all.
u64 AdvertisedId(const Common::Nextendo::Friend& entry, bool want_nsa) {
    return (want_nsa || entry.console) && entry.nsa != 0 ? entry.nsa : entry.pid;
}

const Common::Nextendo::Friend* FindFriend(const std::vector<Common::Nextendo::Friend>& friends,
                                           u64 id) {
    const auto it = std::ranges::find_if(friends, [id](const Common::Nextendo::Friend& entry) {
        return entry.pid == id || (entry.nsa != 0 && entry.nsa == id);
    });
    return it == friends.end() ? nullptr : &*it;
}

// A Uid that stays the same for a friend across launches.
Common::UUID UidForId(u64 id) {
    std::array<u8, 16> raw{};
    constexpr u64 high = 0x1100000000000000ULL;
    std::memcpy(raw.data(), &id, sizeof(id));
    std::memcpy(raw.data() + 8, &high, sizeof(high));
    return Common::UUID{raw};
}

template <std::size_t N>
void CopyString(std::array<char, N>& out, std::string_view text) {
    std::memcpy(out.data(), text.data(), std::min(text.size(), N - 1));
}

// Records carry the id the caller asked about, or the title can't pair them with its query.
FriendImpl MakeFriend(const Common::Nextendo::Friend& entry, u64 id, u64 program_id,
                      const Common::UUID* owner) {
    FriendImpl out{};
    // nnSdk's Friend::GetProfileImage checks that this Uid is a local user.
    out.user_id = owner ? *owner : UidForId(id);
    out.network_user_id = id;
    CopyString(out.nickname, entry.name);
    // A friend's presence starts with the application they play and its presence group.
    const std::array<u64, 2> application{entry.app_id, entry.app_id};
    std::memcpy(&out.presence.user_id, application.data(), sizeof(application));
    out.presence.status = static_cast<u32>(entry.status);
    out.presence.last_time_online_timestamp = std::numeric_limits<s64>::max();
    out.presence.same_presence_group_application =
        entry.status > 0 && entry.app_id == program_id ? 1 : 0;
    std::memcpy(out.presence.app_key_value.data(), entry.app_field.data(),
                std::min(entry.app_field.size(), out.presence.app_key_value.size()));
    out.is_valid = 1;
    return out;
}

std::vector<u64> RequestedIds(HLERequestContext& ctx) {
    std::vector<u64> ids;
    if (ctx.CanReadBuffer(0)) {
        const auto raw = ctx.ReadBuffer(0);
        ids.resize(std::min(raw.size() / sizeof(u64), MaxRequestedIds));
        std::memcpy(ids.data(), raw.data(), ids.size() * sizeof(u64));
    }
    return ids;
}

} // namespace

class IFriendService final : public ServiceFramework<IFriendService> {
public:
    explicit IFriendService(Core::System& system_)
        : ServiceFramework{system_, "IFriendService"}, service_context{system, "IFriendService"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &IFriendService::GetCompletionEvent, "GetCompletionEvent"},
            {1, &IFriendService::Cancel, "Cancel"},
            {10100, &IFriendService::GetFriendListIds, "GetFriendListIds"},
            {10101, &IFriendService::GetFriendList, "GetFriendList"},
            {10102, &IFriendService::UpdateFriendInfo, "UpdateFriendInfo"},
            {10110, &IFriendService::GetFriendProfileImage, "GetFriendProfileImage"},
            {10111, &IFriendService::GetFriendProfileImage, "GetFriendProfileImageWithImageSize"}, // 18.0.0+
            {10120, &IFriendService::CheckFriendListAvailability, "CheckFriendListAvailability"},
            {10121, nullptr, "EnsureFriendListAvailable"},
            {10200, nullptr, "SendFriendRequestForApplication"},
            {10211, nullptr, "AddFacedFriendRequestForApplication"},
            {10400, &IFriendService::GetBlockedUserListIds, "GetBlockedUserListIds"},
            {10420, &IFriendService::CheckBlockedUserListAvailability, "CheckBlockedUserListAvailability"},
            {10421, nullptr, "EnsureBlockedUserListAvailable"},
            {10500, &IFriendService::GetProfileList, "GetProfileList"},
            {10501, &IFriendService::GetProfileList, "GetProfileListV2"}, // 18.0.0+
            {10600, nullptr, "DeclareOpenOnlinePlaySession"},
            {10601, &IFriendService::DeclareCloseOnlinePlaySession, "DeclareCloseOnlinePlaySession"},
            {10610, &IFriendService::UpdateUserPresence, "UpdateUserPresence"},
            {10700, &IFriendService::GetPlayHistoryRegistrationKey, "GetPlayHistoryRegistrationKey"},
            {10701, nullptr, "GetPlayHistoryRegistrationKeyWithNetworkServiceAccountId"},
            {10702, nullptr, "AddPlayHistory"},
            {11000, nullptr, "GetProfileImageUrl"},
            {11001, nullptr, "GetProfileImageUrlV2"}, // 18.0.0+
            {20100, &IFriendService::GetFriendCount, "GetFriendCount"},
            {20101, &IFriendService::GetNewlyFriendCount, "GetNewlyFriendCount"},
            {20102, &IFriendService::GetFriendDetailedInfo, "GetFriendDetailedInfo"},
            {20103, nullptr, "SyncFriendList"},
            {20104, &IFriendService::RequestSyncFriendList, "RequestSyncFriendList"},
            {20105, &IFriendService::GetFriendListForViewer, "GetFriendListForViewerV1"},
            {20106, &IFriendService::GetFriendDetailedInfo, "UpdateFriendInfoForViewerV1"},
            {20107, &IFriendService::GetFriendDetailedInfo, "GetFriendDetailedInfoV2"}, // 20.0.0+
            {20108, &IFriendService::GetFriendListForViewer, "GetFriendListForViewerV2"}, // 22.0.0+
            {20109, &IFriendService::GetFriendDetailedInfo, "UpdateFriendInfoForViewerV2"}, // 22.0.0+
            {20110, nullptr, "LoadFriendSettingV1"},
            {20111, nullptr, "LoadFriendSettingV2"}, // 22.0.0+
            {20200, &IFriendService::GetReceivedFriendRequestCount, "GetReceivedFriendRequestCount"},
            {20201, nullptr, "GetFriendRequestListV1"},
            {20202, nullptr, "GetFriendRequestListV2"}, // 20.0.0+
            {20203, nullptr, "GetFriendRequestReceivedNotificationCount"}, // 22.0.0+
            {20300, nullptr, "GetFriendCandidateList"},
            {20301, nullptr, "GetNintendoNetworkIdInfo"},
            {20302, nullptr, "GetSnsAccountLinkage"}, // 5.0.0-19.0.1
            {20303, nullptr, "GetSnsAccountProfile"}, // 5.0.0-19.0.1
            {20304, nullptr, "GetSnsAccountFriendList"}, // 5.0.0-19.0.1
            {20400, nullptr, "GetBlockedUserListV1"},
            {20401, nullptr, "SyncBlockedUserList"},
            {20402, nullptr, "GetBlockedUserListV2"}, // 20.0.0+
            {20500, nullptr, "GetProfileExtraListV1"},
            {20501, nullptr, "GetRelationship"},
            {20502, nullptr, "GetProfileExtraListV2"}, // 19.0.0+
            {20600, &IFriendService::GetUserPresenceView, "GetUserPresenceViewV1"},
            {20601, &IFriendService::GetUserPresenceView, "GetUserPresenceViewV2"}, // 19.0.0+
            {20700, nullptr, "GetPlayHistoryListV1"},
            {20701, &IFriendService::GetPlayHistoryStatistics, "GetPlayHistoryStatistics"},
            {20702, nullptr, "GetPlayHistoryListV2"}, // 19.0.0+
            {20800, &IFriendService::LoadUserSetting, "LoadUserSettingV1"},
            {20801, nullptr, "SyncUserSetting"},
            {20802, &IFriendService::LoadUserSetting, "LoadUserSettingV2"}, // 19.0.0+
            {20900, &IFriendService::RequestListSummaryOverlayNotification, "RequestListSummaryOverlayNotification"},
            {21000, nullptr, "GetExternalApplicationCatalog"},
            {22000, nullptr, "GetReceivedFriendInvitationListV1"},
            {22001, nullptr, "GetReceivedFriendInvitationDetailedInfoV1"},
            {22002, nullptr, "GetReceivedFriendInvitationListV2"}, // 19.0.0+
            {22003, nullptr, "GetReceivedFriendInvitationDetailedInfoV2"}, // 19.0.0+
            {22010, &IFriendService::GetReceivedFriendInvitationCountCache, "GetReceivedFriendInvitationCountCache"},
            {30100, nullptr, "DropFriendNewlyFlags"},
            {30101, nullptr, "DeleteFriend"},
            {30110, nullptr, "DropFriendNewlyFlag"},
            {30120, nullptr, "ChangeFriendFavoriteFlag"},
            {30121, nullptr, "ChangeFriendOnlineNotificationFlag"},
            {30130, nullptr, "SetFriendNote"}, // 22.0.0+
            {30131, nullptr, "RequestUploadPendingNote"}, // 22.0.0+
            {30190, nullptr, "RequestSyncLocalUpdates"}, // 22.0.0+
            {30200, nullptr, "SendFriendRequest"},
            {30201, nullptr, "SendFriendRequestWithApplicationInfoV1"},
            {30202, nullptr, "CancelFriendRequest"},
            {30203, nullptr, "AcceptFriendRequest"},
            {30204, nullptr, "RejectFriendRequest"},
            {30205, nullptr, "ReadFriendRequest"},
            {30210, nullptr, "GetFacedFriendRequestRegistrationKey"},
            {30211, nullptr, "AddFacedFriendRequest"},
            {30212, nullptr, "CancelFacedFriendRequest"},
            {30213, nullptr, "GetFacedFriendRequestProfileImage"},
            {30214, nullptr, "GetFacedFriendRequestProfileImageFromPath"},
            {30215, nullptr, "SendFriendRequestWithExternalApplicationCatalogId"},
            {30216, nullptr, "ResendFacedFriendRequest"},
            {30217, nullptr, "SendFriendRequestWithNintendoNetworkIdInfo"},
            {30218, nullptr, "SendFriendRequestWithApplicationInfoV2"}, // 20.0.0+
            {30300, nullptr, "GetSnsAccountLinkPageUrl"}, // 5.0.0-19.0.1
            {30301, nullptr, "UnlinkSnsAccount"}, // 5.0.0-19.0.1
            {30400, nullptr, "BlockUser"},
            {30401, nullptr, "BlockUserWithApplicationInfoV1"},
            {30402, nullptr, "UnblockUser"},
            {30403, nullptr, "BlockUserWithApplicationInfoV2"}, // 20.0.0+
            {30500, nullptr, "GetProfileExtraFromFriendCodeV1"},
            {30501, nullptr, "GetProfileExtraFromFriendCodeV2"}, // 19.0.0+
            {30700, nullptr, "DeletePlayHistory"},
            {30701, nullptr, "AddPlayHistoryWithApplication"}, // 19.0.0+
            {30810, nullptr, "ChangePresencePermission"},
            {30811, nullptr, "ChangeFriendRequestReception"},
            {30812, nullptr, "ChangePlayLogPermission"},
            {30820, nullptr, "IssueFriendCode"},
            {30830, nullptr, "ClearPlayLog"},
            {30900, nullptr, "SendFriendInvitationV1"},
            {30901, nullptr, "SendFriendInvitationV2"}, // 19.0.0+
            {30910, nullptr, "ReadFriendInvitation"},
            {30911, nullptr, "ReadAllFriendInvitations"},
            {31000, nullptr, "OpenUser"}, // 19.0.0+
            {40100, nullptr, "DeleteFriendListCache"},
            {40400, nullptr, "DeleteBlockedUserListCache"},
            {49900, nullptr, "DeleteNetworkServiceAccountCache"},
        };
        // clang-format on

        RegisterHandlers(functions);

        completion_event = service_context.CreateEvent("IFriendService:CompletionEvent");
    }

    ~IFriendService() override {
        service_context.CloseEvent(completion_event);
    }

private:
    enum class PresenceFilter : u32 {
        None = 0,
        Online = 1,
        OnlinePlay = 2,
        OnlineOrOnlinePlay = 3,
    };

    struct SizedFriendFilter {
        PresenceFilter presence;
        u8 is_favorite;
        u8 same_app;
        u8 same_app_played;
        u8 arbitrary_app_played;
        u64 group_id;
    };
    static_assert(sizeof(SizedFriendFilter) == 0x10, "SizedFriendFilter is an invalid size");

    struct FriendsUserSetting {
        Common::UUID uuid;
        u32 presence_permission;
        u32 play_log_permission;
        u64 friend_request_reception;
        char friend_code[0x20];
        u64 friend_code_next_issuable_time;
        u8 unk_x48[0x7B8];
    };
    static_assert(sizeof(FriendsUserSetting) == 0x800, "FriendsUserSetting is an invalid size");

    void GetCompletionEvent(HLERequestContext& ctx) {
        LOG_DEBUG(Service_Friend, "called");

        auto& readable_event = completion_event->GetReadableEvent();

        IPC::ResponseBuilder rb{ctx, 2, 1};
        rb.Push(readable_event.Signal(system.Kernel()));
        rb.PushCopyObjects(ctx, readable_event);
    }

    void Cancel(HLERequestContext& ctx) {
        LOG_DEBUG(Service_Friend, "(STUBBED) called.");

        // TODO (jarrodnorwell)

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // The cached friends that pass filter. Play history isn't kept, so the played-application
    // conditions don't narrow the list.
    std::vector<Common::Nextendo::Friend> FilteredFriends(const SizedFriendFilter& filter) {
        const u64 program_id = system.GetApplicationProcessProgramID();
        auto friends = Friends(system);
        std::erase_if(friends, [&filter, program_id](const Common::Nextendo::Friend& entry) {
            const bool presence =
                filter.presence == PresenceFilter::Online               ? entry.status == 1
                : filter.presence == PresenceFilter::OnlinePlay         ? entry.status == 2
                : filter.presence == PresenceFilter::OnlineOrOnlinePlay ? entry.status != 0
                                                                        : true;
            return !presence || (filter.is_favorite && !entry.favorite) ||
                   (filter.same_app && (entry.status == 0 || entry.app_id != program_id));
        });
        return friends;
    }

    void GetFriendListIds(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto offset = rp.Pop<u32>();
        rp.Skip(1, false); // Padding: the Uid that follows is 8-byte aligned
        [[maybe_unused]] const auto uuid = rp.PopRaw<Common::UUID>();
        const auto friends = FilteredFriends(rp.PopRaw<SizedFriendFilter>());
        const bool want_nsa = WantsNsaIds(system);
        std::vector<u64> ids;
        for (std::size_t i = offset;
             i < friends.size() && ids.size() < ctx.GetWriteBufferNumElements<u64>(); ++i) {
            ids.push_back(AdvertisedId(friends[i], want_nsa));
        }
        if (!ids.empty()) {
            ctx.WriteBuffer(ids);
        }
        LOG_DEBUG(Service_Friend, "called, offset={} -> {}", offset, ids.size());

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(static_cast<u32>(ids.size()));
    }

    void GetFriendList(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto offset = rp.Pop<u32>();
        rp.Skip(1, false); // Padding: the Uid that follows is 8-byte aligned
        const auto uuid = rp.PopRaw<Common::UUID>();
        const auto friends = FilteredFriends(rp.PopRaw<SizedFriendFilter>());
        const bool want_nsa = WantsNsaIds(system);
        const u64 program_id = system.GetApplicationProcessProgramID();
        std::vector<FriendImpl> list;
        for (std::size_t i = offset;
             i < friends.size() && list.size() < ctx.GetWriteBufferNumElements<FriendImpl>(); ++i) {
            list.push_back(
                MakeFriend(friends[i], AdvertisedId(friends[i], want_nsa), program_id, &uuid));
        }
        if (!list.empty()) {
            ctx.WriteBuffer(list);
        }
        LOG_DEBUG(Service_Friend, "called, offset={}, uuid=0x{} -> {}", offset, uuid.RawString(),
                  list.size());

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(static_cast<u32>(list.size()));
    }

    // One record per requested id, in order; an unknown id gets a zeroed (invalid) record.
    void WriteFriendInfo(HLERequestContext& ctx, const Common::UUID* owner) {
        const auto ids = RequestedIds(ctx);
        const auto friends = Friends(system);
        const u64 program_id = system.GetApplicationProcessProgramID();
        std::vector<FriendImpl> info(
            std::min(ids.size(), ctx.GetWriteBufferNumElements<FriendImpl>()));
        for (std::size_t i = 0; i < info.size(); ++i) {
            if (const auto* entry = FindFriend(friends, ids[i])) {
                info[i] = MakeFriend(*entry, ids[i], program_id, owner);
            }
        }
        if (!info.empty()) {
            ctx.WriteBuffer(info);
        }
        LOG_DEBUG(Service_Friend, "called, {} ids", ids.size());
    }

    void UpdateFriendInfo(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto uuid = rp.PopRaw<Common::UUID>();
        WriteFriendInfo(ctx, &uuid);

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetFriendDetailedInfo(HLERequestContext& ctx) {
        WriteFriendInfo(ctx, nullptr);

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetFriendProfileImage(HLERequestContext& ctx) {
        LOG_DEBUG(Service_Friend, "called");
        ctx.WriteBuffer(DefaultFriendIcon);

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(static_cast<u32>(DefaultFriendIcon.size()));
    }

    // Names the signed-in account and its friends; other ids get a zeroed record.
    void GetProfileList(HLERequestContext& ctx) {
        const auto ids = RequestedIds(ctx);
        const auto friends = Friends(system);
        const auto account =
            Account::Nextendo::LinkedAccount(Account::Nextendo::CurrentProfile(system));
        std::vector<ProfileImpl> profiles(
            std::min(ids.size(), ctx.GetWriteBufferNumElements<ProfileImpl>()));
        u32 resolved = 0;
        for (std::size_t i = 0; i < profiles.size(); ++i) {
            const u64 id = ids[i];
            const auto* entry = FindFriend(friends, id);
            // Animal Crossing knows the account by its BAAS id (NetworkServiceAccountId).
            const bool own = account && id != 0 && (account->pid == id || account->baas_nsa == id);
            if (!own && !entry) {
                continue;
            }
            auto& profile = profiles[i];
            profile.network_user_id = id;
            CopyString(profile.nickname, own ? account->username : entry->name);
            // The address a console gets for the picture; Nextendo's host redirect serves it.
            constexpr std::string_view host =
                "https://cdn-image-e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com/1";
            CopyString(
                profile.image_url,
                !own && entry->nsa != 0
                    ? fmt::format("{}/fr_{:016x}?pid={}", host, entry->nsa, entry->pid)
                    : fmt::format("{}/pid_{}?pid={}", host, id, own ? account->pid : entry->pid));
            profile.is_valid = 1;
            ++resolved;
        }
        if (!profiles.empty()) {
            ctx.WriteBuffer(profiles);
        }
        LOG_DEBUG(Service_Friend, "called, {} ids -> {}", ids.size(), resolved);

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(resolved);
    }

    void CheckFriendListAvailability(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto uuid{rp.PopRaw<Common::UUID>()};

        LOG_WARNING(Service_Friend, "(STUBBED) called, uuid=0x{}", uuid.RawString());

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(true);
    }

    void GetBlockedUserListIds(HLERequestContext& ctx) {
        // This is safe to stub, as there should be no adverse consequences from reporting no
        // blocked users.
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push<u32>(0); // Indicates there are no blocked users
    }

    void CheckBlockedUserListAvailability(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto uuid{rp.PopRaw<Common::UUID>()};

        LOG_WARNING(Service_Friend, "(STUBBED) called, uuid=0x{}", uuid.RawString());

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(true);
    }

    void DeclareCloseOnlinePlaySession(HLERequestContext& ctx) {
        // Stub used by Splatoon 2
        LOG_WARNING(Service_Friend, "(STUBBED) called");
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // Keeps the game's presence data, which the frontend sends to Nextendo with the presence.
    void UpdateUserPresence(HLERequestContext& ctx) {
        LOG_DEBUG(Service_Friend, "called");
        if (Common::Nextendo::OnlinePlayActive() &&
            ctx.GetReadBufferSize() >= sizeof(UserPresenceImpl)) {
            UserPresenceImpl presence{};
            std::memcpy(&presence, ctx.ReadBuffer().data(), sizeof(presence));
            Common::Nextendo::SetGameAppField(
                system.GetApplicationProcessProgramID(),
                std::string{reinterpret_cast<const char*>(presence.app_key_value.data()),
                            presence.app_key_value.size()});
        }
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetPlayHistoryRegistrationKey(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto local_play = rp.Pop<bool>();
        const auto uuid = rp.PopRaw<Common::UUID>();

        LOG_WARNING(Service_Friend, "(STUBBED) called, local_play={}, uuid=0x{}", local_play,
                    uuid.RawString());

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetFriendCount(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        [[maybe_unused]] const auto uuid = rp.PopRaw<Common::UUID>();
        const auto count = FilteredFriends(rp.PopRaw<SizedFriendFilter>()).size();
        LOG_DEBUG(Service_Friend, "called -> {}", count);

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(static_cast<u32>(count));
    }

    void GetNewlyFriendCount(HLERequestContext& ctx) {
        LOG_DEBUG(Service_Friend, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(0);
    }

    void RequestSyncFriendList(HLERequestContext& ctx) {
        LOG_DEBUG(Service_Friend, "(STUBBED) called.");

        // TODO (jarrodnorwell)

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetFriendListForViewer(HLERequestContext& ctx) {
        const auto friends = Friends(system);
        std::vector<u64> ids;
        for (const auto& entry : friends) {
            if (ids.size() >= ctx.GetWriteBufferNumElements<u64>()) {
                break;
            }
            ids.push_back(AdvertisedId(entry, false));
        }
        if (!ids.empty()) {
            ctx.WriteBuffer(ids);
        }
        LOG_DEBUG(Service_Friend, "called -> {}", ids.size());

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(static_cast<u32>(ids.size()));
    }

    void GetReceivedFriendRequestCount(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        [[maybe_unused]] const auto uuid = rp.PopRaw<Common::UUID>();

        LOG_DEBUG(Service_Friend, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push(0);
        rb.Push(0);
    }

    void GetUserPresenceView(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto uuid = rp.PopRaw<Common::UUID>();
        LOG_DEBUG(Service_Friend, "(STUBBED) called, uuid={}.", uuid.RawString());

        u8 buf[0xe0]{};
        ctx.WriteBuffer(buf);

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
    }

    void GetPlayHistoryStatistics(HLERequestContext& ctx) {
        LOG_ERROR(Service_Friend, "(STUBBED) called, check in out");

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void LoadUserSetting(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto uuid = rp.PopRaw<Common::UUID>();

        LOG_WARNING(Service_Friend, "(STUBBED) called");

        FriendsUserSetting setting{};
        setting.uuid = uuid;
        setting.presence_permission = 2;
        setting.play_log_permission = 5;
        setting.friend_request_reception = 1;
        setting.friend_code_next_issuable_time = 99999999999;
        std::string_view friend_code = "0000-0000-0000";
        const auto account =
            Account::Nextendo::LinkedAccount(Account::Nextendo::CurrentProfile(system));
        if (account && !account->friend_code.empty()) {
            // Nextendo codes read SW-0000-0000-0000; the console shows the digits.
            friend_code = account->friend_code;
            if (friend_code.starts_with("SW-")) {
                friend_code.remove_prefix(3);
            }
        }
        std::memcpy(setting.friend_code, friend_code.data(),
                    std::min(friend_code.size(), sizeof(setting.friend_code) - 1));
        ctx.WriteBuffer(setting);

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void RequestListSummaryOverlayNotification(HLERequestContext& ctx) {
        LOG_INFO(Service_Friend, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetReceivedFriendInvitationCountCache(HLERequestContext& ctx) {
        LOG_DEBUG(Service_Friend, "(STUBBED) called, check in out");

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(0);
    }

    KernelHelpers::ServiceContext service_context;

    Kernel::KEvent* completion_event;
};

class INotificationService final : public ServiceFramework<INotificationService> {
public:
    explicit INotificationService(Core::System& system_, Common::UUID uuid_)
        : ServiceFramework{system_, "INotificationService"}, uuid{uuid_},
          service_context{system_, "INotificationService"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &INotificationService::GetEvent, "GetEvent"},
            {1, &INotificationService::Clear, "Clear"},
            {2, &INotificationService::Pop, "Pop"}
        };
        // clang-format on

        RegisterHandlers(functions);

        notification_event = service_context.CreateEvent("INotificationService:NotifyEvent");
    }

    ~INotificationService() override {
        service_context.CloseEvent(notification_event);
    }

private:
    void GetEvent(HLERequestContext& ctx) {
        LOG_DEBUG(Service_Friend, "called");

        IPC::ResponseBuilder rb{ctx, 2, 1};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, notification_event->GetReadableEvent());
    }

    void Clear(HLERequestContext& ctx) {
        LOG_DEBUG(Service_Friend, "called");
        while (!notifications.empty()) {
            notifications.pop();
        }
        std::memset(&states, 0, sizeof(States));

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void Pop(HLERequestContext& ctx) {
        LOG_DEBUG(Service_Friend, "called");

        if (notifications.empty()) {
            LOG_ERROR(Service_Friend, "No notifications in queue!");
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(Account::ResultNoNotifications);
            return;
        }

        const auto notification = notifications.front();
        notifications.pop();

        switch (notification.notification_type) {
        case NotificationTypes::HasUpdatedFriendsList:
            states.has_updated_friends = false;
            break;
        case NotificationTypes::HasReceivedFriendRequest:
            states.has_received_friend_request = false;
            break;
        default:
            // HOS seems not have an error case for an unknown notification
            LOG_WARNING(Service_Friend, "Unknown notification {:08X}",
                        notification.notification_type);
            break;
        }

        IPC::ResponseBuilder rb{ctx, 6};
        rb.Push(ResultSuccess);
        rb.PushRaw<SizedNotificationInfo>(notification);
    }

    enum class NotificationTypes : u32 {
        HasUpdatedFriendsList = 0x65,
        HasReceivedFriendRequest = 0x1
    };

    struct SizedNotificationInfo {
        NotificationTypes notification_type;
        INSERT_PADDING_WORDS(
            1); // TODO(ogniK): This doesn't seem to be used within any IPC returns as of now
        u64_le account_id;
    };
    static_assert(sizeof(SizedNotificationInfo) == 0x10,
                  "SizedNotificationInfo is an incorrect size");

    struct States {
        bool has_updated_friends;
        bool has_received_friend_request;
    };

    Common::UUID uuid;
    KernelHelpers::ServiceContext service_context;

    Kernel::KEvent* notification_event;
    std::queue<SizedNotificationInfo> notifications;
    States states{};
};

void Module::Interface::CreateFriendService(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IFriendService>(ctx, system);
    LOG_DEBUG(Service_Friend, "called");
}

void Module::Interface::CreateNotificationService(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    auto uuid = rp.PopRaw<Common::UUID>();

    LOG_DEBUG(Service_Friend, "called, uuid=0x{}", uuid.RawString());

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<INotificationService>(ctx, system, uuid);
}

Module::Interface::Interface(std::shared_ptr<Module> module_, Core::System& system_, const char* name)
    : ServiceFramework{system_, name}, module{std::move(module_)}
{}

Module::Interface::~Interface() = default;

class IServiceForApplication final : public ServiceFramework<IServiceForApplication> {
public:
    explicit IServiceForApplication(Core::System& system_)
        : ServiceFramework{system_, "nd:app"}
    {
        static const FunctionInfo functions[] = {
            {0, nullptr, "GetReceivableNeighborInfoCountMax"},
            {10, nullptr, "IsNeighborDetectionEnabled"},
        };
        RegisterHandlers(functions);
    }
};

class IServiceForSystem final : public ServiceFramework<IServiceForSystem> {
public:
    explicit IServiceForSystem(Core::System& system_)
        : ServiceFramework{system_, "nd:sys"}
    {
        static const FunctionInfo functions[] = {
            {0, nullptr, "GetReceivableNeighborInfoCountMax"},
            {10, nullptr, "IsNeighborDetectionEnabled"},
            {200, nullptr, "SetSystemData"},
            {201, nullptr, "ClearSystemData"},
            {203, nullptr, "GetReceivableNeighborInfoCountForSystem"},
            {204, nullptr, "ReceiveNeighborInfoForSystem"},
            {205, nullptr, "SetSender"},
            {206, nullptr, "GetSender"},
            {207, nullptr, "CreateScannerForSystem"},
            {208, nullptr, "CreateReceiveEventHolderForSystem"},
            {223, nullptr, "EnableNeighborDetection"},
            {224, nullptr, "DisableNeighborDetection"},
            {226, nullptr, "EnablePowerSave"},
            {227, nullptr, "DisablePowerSave"},
            {228, nullptr, "IsPowerSaveEnabled"},
            {232, nullptr, "ClearBlockedUsers"},
            {233, nullptr, "GetBlockedUserCount"},
            {234, nullptr, "BlockUserByLocalUserId"},
            {235, nullptr, "BlockUserByNetworkUserId"},
            {236, nullptr, "UnblockUserByLocalUserId"},
            {237, nullptr, "UnblockUserByNetworkUserId"},
            {240, nullptr, "DeleteApplication"},
            {250, nullptr, "InitializeApplicationInfo"},
            {260, nullptr, "CreateAccountSystemSaveDataAccessSuppressor"},
            {300, nullptr, "AddReceivedNeighborInfoForSystemForDebug"},
            {301, nullptr, "GetSendDataForDebug"},
            {302, nullptr, "ClearReceiveCounterForDebug"},
            {303, nullptr, "GetNextReceiveCounterForDebug"},
            {304, nullptr, "ListBlockedUsersForDebug"},
            {305, nullptr, "RefreshSendDataIdForDebug"},
            {306, nullptr, "ReloadFwdbgSettingsForDebug"},
            {307, nullptr, "EnableApplicationForDebug"},
            {308, nullptr, "GetNextReceiveCountersForDebug"},
            {309, nullptr, "ListApplicationInfoForDebug"},
            {310, nullptr, "SetApplicationDataForDebug"},
            {400, nullptr, "GetNetworkUserId"},
            {401, nullptr, "DeleteNetworkUserId"},
        };
        RegisterHandlers(functions);
    }
};

// Session returned by chat:u 0 and 1. Command 1 hands back a handle the title waits on, so it gets
// an event that is never signalled. 2-4 attach to the room info, local camera image processing
// mode and local camera zoom change events (Switchbrew: no input, an event out); none of them
// changes outside a GameChat room, so they never fire. Everything else falls through to the nx2
// auto stub.
class IChatSession final : public ServiceFramework<IChatSession> {
public:
    explicit IChatSession(Core::System& system_)
        : ServiceFramework{system_, "nx2:chat:u::0"}, service_context{system_, "IChatSession"} {
        static const FunctionInfo functions[] = {
            {1, &IChatSession::GetEvent, "GetEvent (guessed)"},
            {2, &IChatSession::AttachToChangedEvent, "AttachToRoomInfoChangedEvent"},
            {3, &IChatSession::AttachToChangedEvent, "AttachToLocalCameraImageProcessingModeChangedEvent"},
            {4, &IChatSession::AttachToChangedEvent, "AttachToLocalCameraZoomMagnificationChangedEvent"},
        };
        RegisterHandlers(functions);
        event = service_context.CreateEvent("IChatSession:Event");
        static constexpr std::array<const char*, 3> changed_event_names{
            "IChatSession:RoomInfoChanged",
            "IChatSession:LocalCameraImageProcessingModeChanged",
            "IChatSession:LocalCameraZoomMagnificationChanged",
        };
        for (size_t i = 0; i < changed_events.size(); ++i) {
            changed_events[i] = service_context.CreateEvent(changed_event_names[i]);
        }
    }

    ~IChatSession() override {
        service_context.CloseEvent(event);
        for (auto* changed_event : changed_events) {
            service_context.CloseEvent(changed_event);
        }
    }

private:
    void GetEvent(HLERequestContext& ctx) {
        LOG_WARNING(Service_Friend, "(STUBBED) chat:u::0 1 [5]={:#x}", ctx.CommandBuffer()[5]);
        IPC::ResponseBuilder rb{ctx, 2, 1};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, event->GetReadableEvent());
    }

    void AttachToChangedEvent(HLERequestContext& ctx) {
        LOG_DEBUG(Service_Friend, "chat:u::0 {}", ctx.GetCommand());
        IPC::ResponseBuilder rb{ctx, 2, 1};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, changed_events[ctx.GetCommand() - 2]->GetReadableEvent());
    }

    KernelHelpers::ServiceContext service_context;
    Kernel::KEvent* event;
    std::array<Kernel::KEvent*, 3> changed_events{};
};

// Switch 2 GameChat (chat:u). 0 and 1 open the session the title keeps using. 2-4 return objects
// nothing here implements yet (layout customization, virtual camera, selective talk), so each
// gets an nx2 stub whose commands all succeed empty; MKW calls 3 at boot. Switchbrew leaves 0-4
// unnamed: 2-4 are named after what they return. 11 answers the empty room. Everything else falls
// through to the nx2 auto stub.
class IChatService final : public ServiceFramework<IChatService> {
public:
    explicit IChatService(Core::System& system_) : ServiceFramework{system_, "nx2:chat:u"} {
        static const FunctionInfo functions[] = {
            {0, &IChatService::OpenSession, "OpenSession"},
            {1, &IChatService::OpenSession, "OpenSession (u32)"},
            {2, &IChatService::OpenStubObject, "OpenUserLayoutCustomizationShimScopedObject"},
            {3, &IChatService::OpenStubObject, "OpenVirtualCameraShimScopedObject"},
            {4, &IChatService::OpenStubObject, "OpenUserSelectiveTalkShimScopedObject"},
            {11, &IChatService::GetRoomInfo, "GetRoomInfo"},
        };
        RegisterHandlers(functions);
    }

private:
    void OpenSession(HLERequestContext& ctx) {
        LOG_WARNING(Service_Friend, "(STUBBED) chat:u {}, returning a chat session", ctx.GetCommand());
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<IChatSession>(ctx, system);
    }

    // 11: the nn::chat::RoomInfo (0xF0 bytes) of the room the user chats in. Nobody is ever in a
    // room here, so it is all zeroes, ChattingUid (0x18) included. The auto stub left the title's
    // buffer as it was.
    void GetRoomInfo(HLERequestContext& ctx) {
        LOG_DEBUG(Service_Friend, "called");
        constexpr std::array<u8, 0xF0> room_info{};
        ctx.WriteBuffer(room_info);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void OpenStubObject(HLERequestContext& ctx) {
        static constexpr std::array<const char*, 3> names{
            "nx2:chat:IUserLayoutCustomizationShimScopedObject",
            "nx2:chat:IVirtualCameraShimScopedObject",
            "nx2:chat:IUserSelectiveTalkShimScopedObject",
        };
        const char* name = names[ctx.GetCommand() - 2];
        LOG_WARNING(Service_Friend, "(STUBBED) chat:u {}, returning {}", ctx.GetCommand(), name);
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<INx2StubInterface>(ctx, system, name);
    }
};

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);
    auto module = std::make_shared<Module>();

    server_manager->RegisterNamedService("nd:app", std::make_shared<IServiceForApplication>(system));
    server_manager->RegisterNamedService("nd:sys", std::make_shared<IServiceForSystem>(system));

    server_manager->RegisterNamedService("friend:a", std::make_shared<Friend>(module, system, "friend:a"));
    server_manager->RegisterNamedService("friend:m", std::make_shared<Friend>(module, system, "friend:m"));
    server_manager->RegisterNamedService("friend:s", std::make_shared<Friend>(module, system, "friend:s"));
    server_manager->RegisterNamedService("friend:u", std::make_shared<Friend>(module, system, "friend:u"));
    server_manager->RegisterNamedService("friend:v", std::make_shared<Friend>(module, system, "friend:v"));

    // Switch 2 GameChat. Titles block in sm until it exists, so register a stub for now.
    server_manager->RegisterNamedService("chat:u", std::make_shared<IChatService>(system));

    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::Friend
