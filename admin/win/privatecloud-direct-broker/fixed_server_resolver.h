// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <vector>

namespace fixed_server_resolver
{
inline constexpr DWORD timeoutMs = 3000;
struct Result {
    std::vector<IN_ADDR> addresses;
    int error = 0;
};

// Native operations can be replaced in deterministic tests. No hostname or
// service override is exposed: every query is for the fixed server's HTTPS port.
struct Api {
    decltype(&GetAddrInfoExW) begin = &GetAddrInfoExW;
    decltype(&GetAddrInfoExCancel) cancel = &GetAddrInfoExCancel;
    decltype(&WaitForMultipleObjects) wait = &WaitForMultipleObjects;
    decltype(&FreeAddrInfoExW) freeAnswer = &FreeAddrInfoExW;
};
Result resolve(HANDLE stopEvent = nullptr, const Api &api = {});
}
