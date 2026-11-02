// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "fixed_server_resolver.h"
#include <cstdio>
#include <cwchar>

namespace
{
enum class Scenario {
    ImmediateSuccess,
    ImmediateError,
    PendingSuccess,
    PendingError,
    Timeout,
    Stop,
    CancelRace,
    LateCompletion,
    WaitFailure,
    EmptyAnswer,
};
Scenario scenario;
OVERLAPPED *pendingOverlapped = nullptr;
LPLOOKUPSERVICE_COMPLETION_ROUTINE pendingCallback = nullptr;
ADDRINFOEXW **pendingAnswer = nullptr;
sockaddr_in address{};
ADDRINFOEXW answer{};
int failures = 0, checks = 0, begins = 0, cancels = 0, freed = 0;

void check(bool passed, const char *name)
{
    ++checks;
    failures += !passed;
    std::printf("%s %s\n", passed ? "PASS" : "FAIL", name);
}

void complete(DWORD error)
{
    *pendingAnswer = &answer;
    pendingCallback(error, 0, pendingOverlapped);
    pendingAnswer = nullptr;
    pendingCallback = nullptr;
    pendingOverlapped = nullptr;
}

INT WSAAPI begin(PCWSTR name,
                 PCWSTR service,
                 DWORD nameSpace,
                 LPGUID provider,
                 const ADDRINFOEXW *hints,
                 PADDRINFOEXW *result,
                 timeval *timeout,
                 LPOVERLAPPED overlapped,
                 LPLOOKUPSERVICE_COMPLETION_ROUTINE callback,
                 LPHANDLE cancellation)
{
    ++begins;
    check(std::wcscmp(name, L"cloud.monocloud.online") == 0 && std::wcscmp(service, L"443") == 0 && nameSpace == NS_DNS && !provider,
          "resolver always requests fixed server HTTPS in DNS namespace");
    check(hints && hints->ai_family == AF_INET && hints->ai_socktype == SOCK_STREAM && hints->ai_protocol == IPPROTO_TCP && !hints->ai_flags && !hints->ai_addr
              && !hints->ai_addrlen && !hints->ai_canonname && !hints->ai_next && !hints->ai_blob && !hints->ai_bloblen && !hints->ai_provider,
          "IPv4 TCP hints initialize reserved fields");
    check(!timeout && overlapped && !overlapped->hEvent && callback && cancellation, "regression: timeout is not passed to synchronous GetAddrInfoExW");
    *cancellation = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(1));
    if (scenario == Scenario::ImmediateSuccess) {
        *result = &answer;
        return 0;
    }
    if (scenario == Scenario::ImmediateError) {
        return WSAEINVAL;
    }
    if (scenario == Scenario::EmptyAnswer) {
        return 0;
    }
    pendingOverlapped = overlapped;
    pendingCallback = callback;
    pendingAnswer = result;
    return WSA_IO_PENDING;
}

DWORD WINAPI wait(DWORD count, const HANDLE *events, BOOL all, DWORD milliseconds)
{
    check((count == 1 || count == 2) && events && events[0] && !all && milliseconds == fixed_server_resolver::timeoutMs,
          "wait is bounded and includes an owned completion event");
    switch (scenario) {
    case Scenario::PendingSuccess:
        complete(0);
        return WAIT_OBJECT_0;
    case Scenario::PendingError:
        complete(WSAHOST_NOT_FOUND);
        return WAIT_OBJECT_0;
    case Scenario::Timeout:
    case Scenario::CancelRace:
    case Scenario::LateCompletion:
        return WAIT_TIMEOUT;
    case Scenario::Stop:
        check(count == 2 && events[1], "service stop event is included");
        SetEvent(events[1]);
        return WAIT_OBJECT_0 + 1;
    case Scenario::WaitFailure:
        SetLastError(ERROR_INVALID_HANDLE);
        return WAIT_FAILED;
    default:
        return WAIT_OBJECT_0;
    }
}

INT WSAAPI cancel(LPHANDLE cancellation)
{
    ++cancels;
    check(cancellation && *cancellation, "cancellation uses the returned native request handle");
    if (scenario == Scenario::LateCompletion) {
        return 0;
    }
    complete(scenario == Scenario::CancelRace ? 0 : WSA_E_CANCELLED);
    return scenario == Scenario::CancelRace ? WSA_INVALID_HANDLE : 0;
}

VOID WSAAPI freeAnswer(PADDRINFOEXW value)
{
    check(value == &answer, "only the returned answer is released");
    ++freed;
}

void reset(Scenario value)
{
    scenario = value;
    begins = cancels = freed = 0;
    answer = {};
    address = {};
    address.sin_family = AF_INET;
    address.sin_addr.S_un.S_addr = htonl(0x01010101);
    answer.ai_family = AF_INET;
    answer.ai_addr = reinterpret_cast<sockaddr *>(&address);
    answer.ai_addrlen = sizeof(address);
}
}

int main()
{
    const fixed_server_resolver::Api api{&begin, &cancel, &wait, &freeAnswer};
    for (const auto item : {Scenario::ImmediateSuccess, Scenario::PendingSuccess}) {
        reset(item);
        const auto result = fixed_server_resolver::resolve(nullptr, api);
        check(!result.error && result.addresses.size() == 1 && result.addresses.front().S_un.S_addr == address.sin_addr.S_un.S_addr,
              "immediate and pending success return resolved address");
        check(freed == 1 && cancels == 0, "success releases exactly once without cancellation");
    }
    reset(Scenario::ImmediateError);
    check(fixed_server_resolver::resolve(nullptr, api).error == WSAEINVAL && cancels == 0 && freed == 0, "exact synchronous Winsock error survives");
    reset(Scenario::PendingError);
    check(fixed_server_resolver::resolve(nullptr, api).error == WSAHOST_NOT_FOUND && cancels == 0 && freed == 1, "exact asynchronous Winsock error survives");
    for (const auto item : {Scenario::Timeout, Scenario::CancelRace}) {
        reset(item);
        const auto result = fixed_server_resolver::resolve(nullptr, api);
        check(result.error == WSAETIMEDOUT && result.addresses.empty() && cancels == 1 && freed == 1,
              "timeout refuses even a raced successful answer and cleans up");
    }
    reset(Scenario::Stop);
    const auto stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    check(stop != nullptr, "stop fixture event created");
    check(fixed_server_resolver::resolve(stop, api).error == WSA_E_CANCELLED && cancels == 1 && freed == 1, "service stop cancels pending DNS");
    const auto previousBegins = begins;
    check(fixed_server_resolver::resolve(stop, api).error == WSA_E_CANCELLED && begins == previousBegins, "already stopped service does not start DNS");
    CloseHandle(stop);
    reset(Scenario::LateCompletion);
    check(fixed_server_resolver::resolve(nullptr, api).error == WSAETIMEDOUT && cancels == 1 && freed == 0,
          "late completion retains request ownership after return");
    check(fixed_server_resolver::resolve(nullptr, api).error == WSAEINPROGRESS && begins == 1, "stalled completion cannot accumulate native requests");
    complete(WSA_E_CANCELLED);
    check(freed == 1, "late callback safely releases retained request");
    reset(Scenario::ImmediateSuccess);
    check(!fixed_server_resolver::resolve(nullptr, api).error, "new request admitted after late callback cleanup");
    reset(Scenario::WaitFailure);
    check(fixed_server_resolver::resolve(nullptr, api).error == ERROR_INVALID_HANDLE && cancels == 1 && freed == 1,
          "wait failure cancels without losing Windows error");
    reset(Scenario::EmptyAnswer);
    check(fixed_server_resolver::resolve(nullptr, api).error == WSANO_DATA, "successful lookup without IPv4 answer has precise error");
    std::printf("Resolver: %d checks, %d failures. Fake DNS API; no network, WFP, service or registry changes.\n", checks, failures);
    return failures ? 1 : 0;
}
