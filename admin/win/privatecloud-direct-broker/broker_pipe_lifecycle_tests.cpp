// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "broker_pipe_lifecycle.h"

#include <sddl.h>

#include <cstdio>
#include <string>
#include <thread>

namespace
{
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle()
    {
        if (value && value != INVALID_HANDLE_VALUE) {
            CloseHandle(value);
        }
    }
};

HANDLE openClient(const std::wstring &name)
{
    return CreateFileW(name.c_str(),
                       FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE,
                       0,
                       nullptr,
                       OPEN_EXISTING,
                       FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                       nullptr);
}

DWORD clientAccess(const std::wstring &name, const std::wstring &sddl, DWORD desired, bool verifyServer = false)
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
        return GetLastError();
    }
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    broker_pipe_lifecycle::Server server(name.c_str(), &attributes);
    LocalFree(descriptor);
    if (!server.valid()) {
        return server.error();
    }
    Handle client{
        CreateFileW(name.c_str(), desired, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr)};
    if (client.value == INVALID_HANDLE_VALUE) {
        return GetLastError();
    }
    if (verifyServer) {
        ULONG serverPid = 0;
        if (!GetNamedPipeServerProcessId(client.value, &serverPid)) {
            return GetLastError();
        }
        if (serverPid != GetCurrentProcessId()) {
            return ERROR_INVALID_DATA;
        }
    }
    return ERROR_SUCCESS;
}
}

int main()
{
    int checks = 0, failed = 0;
    const auto check = [&](bool value, const char *name) {
        ++checks;
        if (!value) {
            ++failed;
        }
        std::printf("%s %s\n", value ? "PASS" : "FAIL", name);
    };
    const auto name = L"\\\\.\\pipe\\privateCloud-pipe-lifecycle-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    Handle token{nullptr};
    TOKEN_ELEVATION elevation{};
    DWORD needed = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)
        || !GetTokenInformation(token.value, TokenElevation, &elevation, sizeof(elevation), &needed) || elevation.TokenIsElevated) {
        std::puts("FAIL pipe security matrix requires an ordinary unelevated interactive user.");
        return 1;
    }
    constexpr auto exactRights = FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE;
    const auto originalDescriptor = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x00100003;;;IU)";
    const auto productionDescriptor = broker_pipe_lifecycle::securityDescriptor(L"");
    check(broker_pipe_lifecycle::interactiveAccess == 0x00100083, "production interactive mask excludes creation and control rights");
    const auto original = clientAccess(name + L"-old", originalDescriptor, exactRights);
    const auto readOnly = clientAccess(name + L"-read", originalDescriptor, FILE_READ_DATA);
    const auto attributeAcl = clientAccess(name + L"-acl", productionDescriptor, exactRights);
    const auto attributeBoth = clientAccess(name + L"-both", productionDescriptor, exactRights | FILE_READ_ATTRIBUTES);
    std::printf("PIPE_ACCESS_MATRIX original=%lu readOnly=%lu readAttributesAcl=%lu readAttributesAclAndRequest=%lu\n",
                original,
                readOnly,
                attributeAcl,
                attributeBoth);
    check(original == ERROR_ACCESS_DENIED && readOnly == ERROR_ACCESS_DENIED, "minimal original IU descriptor reproduces client access denial");
    check(attributeAcl == ERROR_SUCCESS, "adding only IU read-attributes admits exact original GUI rights");
    check(attributeBoth == ERROR_SUCCESS, "IU read-attributes also supports explicit client attribute access");
    const auto identity = clientAccess(name + L"-identity", productionDescriptor, exactRights, true);
    std::printf("PIPE_SERVER_IDENTITY exactGuiRights error=%lu\n", identity);
    check(identity == ERROR_SUCCESS, "production descriptor and exact GUI rights verify the fixture server PID");
    Handle stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!stop.value) {
        return 1;
    }
    broker_pipe_lifecycle::Server server(name.c_str(), nullptr);
    check(server.valid(), "first fixture instance created");
    if (!server.valid()) {
        return 1;
    }
    const auto originalHandle = server.handle();
    {
        broker_pipe_lifecycle::Server squat(name.c_str(), nullptr);
        std::printf("FIRST_INSTANCE_REFUSAL initial valid=%d error=%lu\n", squat.valid() ? 1 : 0, squat.error());
        check(!squat.valid(), "another first instance cannot take the name");
    }
    Handle first{openClient(name)};
    check(first.value != INVALID_HANDLE_VALUE && server.accept(stop.value, 1000), "client arriving before accept is admitted");
    check(server.disconnect(), "server disconnects first client");
    // Keep first.value alive throughout reconnect: this was the problematic lifetime.
    check(server.handle() == originalHandle, "first server handle retained after disconnect");
    Handle second;
    std::thread connector([&] {
        Sleep(30);
        second.value = openClient(name);
    });
    const auto accepted = server.accept(stop.value, 2000);
    connector.join();
    check(accepted && second.value != INVALID_HANDLE_VALUE, "pending reconnect succeeds while old client handle remains open");
    ULONG serverPid = 0;
    check(GetNamedPipeServerProcessId(second.value, &serverPid) && serverPid == GetCurrentProcessId(),
          "reconnected client retains the original server identity");
    {
        broker_pipe_lifecycle::Server squat(name.c_str(), nullptr);
        std::printf("FIRST_INSTANCE_REFUSAL reconnect valid=%d error=%lu\n", squat.valid() ? 1 : 0, squat.error());
        check(!squat.valid(), "first-instance anti-squatting remains enforced after reconnect");
    }
    DWORD bytes = 0;
    check(!PeekNamedPipe(first.value, nullptr, 0, nullptr, &bytes, nullptr), "old client handle cannot access the new session");
    check(server.disconnect(), "second client disconnects");
    const auto begin = GetTickCount64();
    check(!server.accept(stop.value, 20) && server.error() == ERROR_TIMEOUT && GetTickCount64() - begin < 1000, "idle accept deadline is bounded");
    // A cancelled pending connect is returned to the disconnected state before reuse.
    server.disconnect();
    SetEvent(stop.value);
    check(!server.accept(stop.value, 2000) && server.error() == ERROR_OPERATION_ABORTED, "stop event interrupts pending accept");
    std::printf("%d pipe lifecycle checks, %d failures; isolated named pipes only.\n", checks, failed);
    return failed ? 1 : 0;
}
