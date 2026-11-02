// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Windows network headers require the Winsock and Win32 types first.
// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
// clang-format on

enum class PolicyState {
    NoBypassNeeded,
    RecognizedTailscale,
    Untrusted,
};

struct PolicyProof {
    PolicyState state = PolicyState::Untrusted;
    GUID provider{};
    GUID sublayer{};
    NET_LUID tunnel{};
    const char *reason = "unrecognized-policy";
    const char *stage = "policy-assessment";
    DWORD nativeError = 0;
};

// 0/1 and 16 did not establish a distinct effective priority in the isolated
// Windows 11 proof; 256 did. Never substitute a maximum-weight exemption.
inline constexpr UINT16 privateCloudPermitWeight = 256;
inline constexpr UINT16 privateCloudNextPriorityClass = 512;

// BFE may normalize requested 256 to 257. The complete effective class is
// accepted; neither the old ineffective class nor a stronger class is valid.
constexpr bool privateCloudPermitWeightMatches(UINT16 weight)
{
    return weight >= privateCloudPermitWeight && weight < privateCloudNextPriorityClass;
}

// Read-only, conservative recognition. Never adds, removes, or updates WFP objects.
PolicyProof inspectPolicy(HANDLE engine, IN_ADDR server, const GUID &ownSublayer);
