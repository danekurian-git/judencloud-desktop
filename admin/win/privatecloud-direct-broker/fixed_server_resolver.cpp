// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "fixed_server_resolver.h"
#include <atomic>
#include <memory>
#include <new>

namespace fixed_server_resolver
{
namespace
{
std::atomic_flag pending = ATOMIC_FLAG_INIT;

struct Operation {
    OVERLAPPED overlapped{};
    ADDRINFOEXW hints{};
    ADDRINFOEXW *answer = nullptr;
    HANDLE event = nullptr;
    HANDLE cancellation = nullptr;
    Api api;
    std::atomic<unsigned> references{1};
    std::atomic<int> error{WSA_IO_PENDING};
    bool winsockStarted = false;

    ~Operation()
    {
        if (answer) {
            api.freeAnswer(answer);
        }
        if (event) {
            CloseHandle(event);
        }
        // An outstanding callback retains its own Winsock reference, even if
        // the diagnostic or service caller has already returned and cleaned up.
        if (winsockStarted) {
            WSACleanup();
        }
        pending.clear(std::memory_order_release);
    }

    void release()
    {
        if (references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }

    static void CALLBACK complete(DWORD status, DWORD, OVERLAPPED *overlapped)
    {
        auto *operation = CONTAINING_RECORD(overlapped, Operation, overlapped);
        operation->error.store(static_cast<int>(status), std::memory_order_release);
        SetEvent(operation->event);
        operation->release();
    }
};

struct Release {
    void operator()(Operation *operation) const
    {
        operation->release();
    }
};
}

Result resolve(HANDLE stopEvent, const Api &api)
{
    if (stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
        return {{}, WSA_E_CANCELLED};
    }
    if (pending.test_and_set(std::memory_order_acquire)) {
        return {{}, WSAEINPROGRESS};
    }
    std::unique_ptr<Operation, Release> operation(new (std::nothrow) Operation);
    if (!operation) {
        pending.clear(std::memory_order_release);
        return {{}, WSA_NOT_ENOUGH_MEMORY};
    }
    operation->api = api;
    WSADATA data{};
    const auto startup = WSAStartup(MAKEWORD(2, 2), &data);
    if (startup) {
        return {{}, startup};
    }
    operation->winsockStarted = true;
    operation->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!operation->event) {
        return {{}, static_cast<int>(GetLastError())};
    }
    operation->hints.ai_family = AF_INET;
    operation->hints.ai_socktype = SOCK_STREAM;
    operation->hints.ai_protocol = IPPROTO_TCP;
    operation->references.fetch_add(1, std::memory_order_relaxed);
    // The old synchronous timeout argument returned WSAEINVAL immediately on
    // Windows. Use the documented overlapped/callback form; the caller owns the
    // timeout. A callback requires OVERLAPPED::hEvent to remain null.
    const auto start = api.begin(L"cloud.monocloud.online",
                                 L"443",
                                 NS_DNS,
                                 nullptr,
                                 &operation->hints,
                                 &operation->answer,
                                 nullptr,
                                 &operation->overlapped,
                                 &Operation::complete,
                                 &operation->cancellation);
    if (start != WSA_IO_PENDING) {
        // Winsock invokes the callback only for WSA_IO_PENDING. Immediate
        // success and errors use the same ownership completion path here.
        Operation::complete(static_cast<DWORD>(start), 0, &operation->overlapped);
    }
    const HANDLE events[] = {operation->event, stopEvent};
    const auto waited = api.wait(stopEvent ? 2 : 1, events, FALSE, timeoutMs);
    if (waited != WAIT_OBJECT_0 || (stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0)) {
        const auto error = waited == WAIT_TIMEOUT ? WSAETIMEDOUT : waited == WAIT_FAILED ? static_cast<int>(GetLastError()) : WSA_E_CANCELLED;
        if (start == WSA_IO_PENDING) {
            // Cancellation may race completion. Either way the callback owns
            // its reference until it finishes, so no unbounded drain is needed.
            api.cancel(&operation->cancellation);
        }
        return {{}, error};
    }
    const auto error = operation->error.load(std::memory_order_acquire);
    if (error) {
        return {{}, error};
    }
    Result result;
    for (auto *entry = operation->answer; entry; entry = entry->ai_next) {
        if (entry->ai_family == AF_INET && entry->ai_addr && entry->ai_addrlen >= sizeof(sockaddr_in)) {
            result.addresses.push_back(reinterpret_cast<const sockaddr_in *>(entry->ai_addr)->sin_addr);
        }
    }
    if (result.addresses.empty()) {
        result.error = WSANO_DATA;
    }
    return result;
}
}
