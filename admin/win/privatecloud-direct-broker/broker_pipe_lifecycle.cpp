// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "broker_pipe_lifecycle.h"

#include <cwchar>

namespace broker_pipe_lifecycle
{
std::wstring securityDescriptor(const std::wstring &serviceSid)
{
    wchar_t mask[11]{};
    swprintf_s(mask, L"0x%08lx", interactiveAccess);
    return std::wstring(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)") + (serviceSid.empty() ? L"" : L"(A;;GA;;;" + serviceSid + L")") + L"(A;;" + mask + L";;;IU)";
}

Server::Server(const wchar_t *name, SECURITY_ATTRIBUTES *security)
{
    _pipe = CreateNamedPipeW(name,
                             PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                             PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                             1,
                             4096,
                             4096,
                             0,
                             security);
    if (!valid()) {
        _error = GetLastError();
    }
}

Server::~Server()
{
    if (valid()) {
        CloseHandle(_pipe);
    }
}

bool Server::accept(HANDLE stop, DWORD timeout)
{
    const auto event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) {
        _error = GetLastError();
        return false;
    }
    OVERLAPPED operation{};
    operation.hEvent = event;
    const auto connected = ConnectNamedPipe(_pipe, &operation);
    _error = connected ? ERROR_SUCCESS : GetLastError();
    if (_error == ERROR_PIPE_CONNECTED) {
        _error = ERROR_SUCCESS;
    } else if (_error == ERROR_IO_PENDING) {
        const HANDLE waits[] = {event, stop};
        const auto waited = WaitForMultipleObjects(stop ? 2 : 1, waits, FALSE, timeout);
        DWORD ignored = 0;
        if (waited == WAIT_OBJECT_0) {
            _error = GetOverlappedResult(_pipe, &operation, &ignored, FALSE) ? ERROR_SUCCESS : GetLastError();
        } else {
            _error = waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : waited == WAIT_OBJECT_0 + 1 ? ERROR_OPERATION_ABORTED : GetLastError();
            CancelIoEx(_pipe, &operation);
            // Drain cancellation before releasing the stack OVERLAPPED/event.
            GetOverlappedResult(_pipe, &operation, &ignored, TRUE);
        }
    }
    CloseHandle(event);
    return _error == ERROR_SUCCESS;
}

bool Server::disconnect()
{
    const auto disconnected = DisconnectNamedPipe(_pipe);
    _error = disconnected ? ERROR_SUCCESS : GetLastError();
    return disconnected != FALSE;
}
}
