// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "lease_watchdog.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{
constexpr DWORD childWaitMs = 5000;
constexpr DWORD completedChild = 17;

std::wstring executablePath()
{
    auto path = std::vector<wchar_t>(32768);
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    return length && length < path.size() ? std::wstring(path.data(), length) : std::wstring();
}

bool spawn(const wchar_t *mode, PROCESS_INFORMATION &process)
{
    const auto executable = executablePath();
    if (executable.empty()) {
        return false;
    }
    auto command = L"\"" + executable + L"\" " + mode;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    return CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
}

bool runChild(const wchar_t *mode, DWORD expected, ULONGLONG maximumElapsed = childWaitMs)
{
    const auto started = GetTickCount64();
    PROCESS_INFORMATION process{};
    if (!spawn(mode, process)) {
        return false;
    }
    CloseHandle(process.hThread);
    auto code = DWORD{0};
    const auto completed = WaitForSingleObject(process.hProcess, childWaitMs) == WAIT_OBJECT_0;
    const auto read = completed && GetExitCodeProcess(process.hProcess, &code);
    if (!completed) {
        TerminateProcess(process.hProcess, ERROR_PROCESS_ABORTED);
        WaitForSingleObject(process.hProcess, childWaitMs);
    }
    CloseHandle(process.hProcess);
    const auto elapsed = GetTickCount64() - started;
    const auto passed = read && code == expected && elapsed <= maximumElapsed;
    std::printf("%ls: %s (exit %lu expected %lu, elapsed %llu ms)\n", mode, passed ? "PASS" : "FAIL", code, expected, elapsed);
    return passed;
}

int timedChild()
{
    LeaseWatchdog watchdog(100);
    if (!watchdog.ready()) {
        return 1;
    }
    // Simulates stalled native inspection; no WFP, DNS or sockets are used.
    Sleep(INFINITE);
    return 1;
}

int renewedChild()
{
    LeaseWatchdog watchdog(500);
    if (!watchdog.ready() || !watchdog.renew(2000)) {
        return 1;
    }
    Sleep(700);
    return !watchdog.revoked() ? completedChild : 1;
}

int lateRenewalChild()
{
    LeaseWatchdog watchdog(100);
    if (!watchdog.ready()) {
        return 1;
    }
    Sleep(300);
    // This cannot save a process whose original deadline has expired.
    return watchdog.renew(2000) ? completedChild : 1;
}

int revokedChild(bool capGrace)
{
    const auto event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) {
        return 1;
    }
    LeaseWatchdog watchdog(capGrace ? 300 : 5000);
    const auto observing = watchdog.ready() && watchdog.observe(event);
    SetEvent(event);
    CloseHandle(event);
    if (!observing || WaitForSingleObject(watchdog.revocationEvent(), 2000) != WAIT_OBJECT_0 || !watchdog.revoked() || watchdog.renew(5000)) {
        return 1;
    }
    Sleep(INFINITE);
    return 1;
}

int clientExitChild()
{
    PROCESS_INFORMATION process{};
    if (!spawn(L"--exit-child", process)) {
        return 1;
    }
    LeaseWatchdog watchdog(5000);
    const auto observing = watchdog.ready() && watchdog.observe(process.hProcess);
    // The watchdog must keep observing its duplicate after the originals close.
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (!observing) {
        return 1;
    }
    Sleep(INFINITE);
    return 1;
}

int canceledChild()
{
    DWORD before = 0;
    if (!GetProcessHandleCount(GetCurrentProcess(), &before)) {
        return 1;
    }
    for (auto iteration = 0; iteration != 20; ++iteration) {
        const auto stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        const auto policy = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop || !policy) {
            if (stop) {
                CloseHandle(stop);
            }
            if (policy) {
                CloseHandle(policy);
            }
            return 1;
        }
        LeaseWatchdog watchdog(10000, stop);
        const auto observing = watchdog.ready() && watchdog.observe(policy);
        CloseHandle(stop);
        CloseHandle(policy);
        if (!observing || !watchdog.renew(100)) {
            return 1;
        }
        watchdog.cancelAndJoin();
        if (watchdog.renew(10000) || !watchdog.revoked()) {
            return 1;
        }
    }
    DWORD after = 0;
    if (!GetProcessHandleCount(GetCurrentProcess(), &after) || before != after) {
        return 1;
    }
    Sleep(300);
    return completedChild;
}

int admissionFailureChild()
{
    LeaseWatchdog zero(0);
    LeaseWatchdog unbounded(INFINITE);
    LeaseWatchdog invalidStop(1000, INVALID_HANDLE_VALUE);
    if (zero.ready() || unbounded.ready() || invalidStop.ready()) {
        return 1;
    }
    LeaseWatchdog watchdog(5000);
    if (!watchdog.ready() || watchdog.observe(INVALID_HANDLE_VALUE) || !watchdog.revoked() || watchdog.renew(5000)) {
        return 1;
    }
    watchdog.cancelAndJoin();
    return completedChild;
}
} // namespace

int wmain(int argc, wchar_t **argv)
{
    if (argc == 2) {
        const auto mode = std::wstring(argv[1]);
        if (mode == L"--timeout-child") {
            return timedChild();
        }
        if (mode == L"--renew-child") {
            return renewedChild();
        }
        if (mode == L"--late-renewal-child") {
            return lateRenewalChild();
        }
        if (mode == L"--revoke-child") {
            return revokedChild(false);
        }
        if (mode == L"--capped-grace-child") {
            return revokedChild(true);
        }
        if (mode == L"--client-exit-child") {
            return clientExitChild();
        }
        if (mode == L"--exit-child") {
            Sleep(100);
            return 0;
        }
        if (mode == L"--cancel-child") {
            return canceledChild();
        }
        if (mode == L"--admission-failure-child") {
            return admissionFailureChild();
        }
        return 1;
    }
    if (argc != 1) {
        return 1;
    }
    auto passed = runChild(L"--timeout-child", ERROR_TIMEOUT);
    passed &= runChild(L"--renew-child", completedChild);
    passed &= runChild(L"--late-renewal-child", ERROR_TIMEOUT);
    passed &= runChild(L"--revoke-child", ERROR_TIMEOUT, 2500);
    passed &= runChild(L"--capped-grace-child", ERROR_TIMEOUT, 900);
    passed &= runChild(L"--client-exit-child", ERROR_TIMEOUT, 2500);
    passed &= runChild(L"--cancel-child", completedChild);
    passed &= runChild(L"--admission-failure-child", completedChild);
    return passed ? 0 : 1;
}
