// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace broker_pipe_lifecycle
{
inline constexpr DWORD interactiveAccess = FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
std::wstring securityDescriptor(const std::wstring &serviceSid);

// Retain the first instance across disconnect/reconnect. An old client may keep
// its disconnected handle open; creating a new FIRST_PIPE_INSTANCE can then fail.
class Server
{
public:
    explicit Server(const wchar_t *name, SECURITY_ATTRIBUTES *security);
    ~Server();
    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;
    [[nodiscard]] HANDLE handle() const
    {
        return _pipe;
    }
    [[nodiscard]] DWORD error() const
    {
        return _error;
    }
    [[nodiscard]] bool valid() const
    {
        return _pipe != INVALID_HANDLE_VALUE;
    }
    bool accept(HANDLE stop, DWORD timeout);
    bool disconnect();

private:
    HANDLE _pipe = INVALID_HANDLE_VALUE;
    DWORD _error = ERROR_SUCCESS;
};
}
