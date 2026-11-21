// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <span>
#include "common/socket_types.h"

namespace Network {
inline bool IsRoomLocalBind(IPv4Address bound, IPv4Address own, std::span<const IPv4Address> local,
                            std::span<const IPv4Address> peers) {
    if (own == IPv4Address{})
        return false;
    if (bound == IPv4Address{} || bound == own)
        return true;
    if (bound[0] == 127 || bound[0] >= 224)
        return false;
    if (std::find(peers.begin(), peers.end(), bound) != peers.end())
        return false;
    return std::find(local.begin(), local.end(), bound) != local.end();
}
} // namespace Network
