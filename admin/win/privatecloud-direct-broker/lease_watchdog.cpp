// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "lease_watchdog.h"

#include <algorithm>

LeaseWatchdog::LeaseWatchdog(DWORD initialBudgetMs, HANDLE stopEvent)
{
    if (!initialBudgetMs || initialBudgetMs == INFINITE) {
        return;
    }
    _expiresAt = GetTickCount64() + initialBudgetMs;
    _wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    _revocation = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!_wake || !_revocation) {
        return;
    }
    if (stopEvent) {
        if (stopEvent == INVALID_HANDLE_VALUE
            || !DuplicateHandle(GetCurrentProcess(), stopEvent, GetCurrentProcess(), &_observed[0], 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            return;
        }
        _observedCount = 1;
        if (WaitForSingleObject(_observed[0], 0) != WAIT_TIMEOUT) {
            revokeLocked(GetTickCount64());
            return;
        }
    }
    _thread = CreateThread(nullptr, 0, &LeaseWatchdog::run, this, 0, nullptr);
}

LeaseWatchdog::~LeaseWatchdog()
{
    cancelAndJoin();
    for (DWORD index = 0; index != _observedCount; ++index) {
        CloseHandle(_observed[index]);
    }
    if (_revocation) {
        CloseHandle(_revocation);
    }
    if (_wake) {
        CloseHandle(_wake);
    }
}

bool LeaseWatchdog::ready() const
{
    return _thread != nullptr;
}

void LeaseWatchdog::revokeLocked(ULONGLONG now)
{
    if (_revoked || _canceled) {
        return;
    }
    _revoked = true;
    _terminateAt = std::min(_expiresAt, now + cleanupGraceMs);
    SetEvent(_revocation);
    SetEvent(_wake);
}

void LeaseWatchdog::refreshLocked(ULONGLONG now)
{
    if (_revoked || _canceled) {
        return;
    }
    if (now >= _expiresAt) {
        revokeLocked(now);
        return;
    }
    for (DWORD index = 0; index != _observedCount; ++index) {
        if (WaitForSingleObject(_observed[index], 0) != WAIT_TIMEOUT) {
            revokeLocked(now);
            return;
        }
    }
}

bool LeaseWatchdog::observe(HANDLE handle)
{
    if (!ready()) {
        return false;
    }
    HANDLE duplicate = nullptr;
    const auto duplicated = handle && handle != INVALID_HANDLE_VALUE
        && DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS);
    AcquireSRWLockExclusive(&_lock);
    const auto now = GetTickCount64();
    refreshLocked(now);
    const auto admitted = duplicated && !_revoked && !_canceled && _observedCount != maximumObservedHandles;
    if (admitted) {
        _observed[_observedCount++] = duplicate;
        duplicate = nullptr;
        refreshLocked(now);
    } else if (!duplicated || _observedCount == maximumObservedHandles) {
        revokeLocked(now);
    }
    const auto accepted = admitted && !_revoked;
    ReleaseSRWLockExclusive(&_lock);
    if (duplicate) {
        CloseHandle(duplicate);
    }
    SetEvent(_wake);
    return accepted;
}

bool LeaseWatchdog::renew(DWORD milliseconds)
{
    if (!ready()) {
        return false;
    }
    AcquireSRWLockExclusive(&_lock);
    const auto now = GetTickCount64();
    refreshLocked(now);
    if (!milliseconds || milliseconds == INFINITE) {
        revokeLocked(now);
    }
    const auto renewedAt = GetTickCount64();
    if (renewedAt >= _expiresAt) {
        revokeLocked(renewedAt);
    }
    const auto accepted = !_revoked && !_canceled;
    if (accepted) {
        _expiresAt = renewedAt + milliseconds;
    }
    ReleaseSRWLockExclusive(&_lock);
    SetEvent(_wake);
    return accepted;
}

bool LeaseWatchdog::revoked()
{
    if (!ready()) {
        return true;
    }
    AcquireSRWLockExclusive(&_lock);
    refreshLocked(GetTickCount64());
    const auto result = _revoked || _canceled;
    ReleaseSRWLockExclusive(&_lock);
    return result;
}

HANDLE LeaseWatchdog::revocationEvent() const
{
    return _revocation;
}

void LeaseWatchdog::cancelAndJoin()
{
    if (!_thread) {
        return;
    }
    AcquireSRWLockExclusive(&_lock);
    _canceled = true;
    ReleaseSRWLockExclusive(&_lock);
    SetEvent(_wake);
    WaitForSingleObject(_thread, INFINITE);
    CloseHandle(_thread);
    _thread = nullptr;
}

DWORD WINAPI LeaseWatchdog::run(void *context)
{
    static_cast<LeaseWatchdog *>(context)->watch();
    return 0;
}

void LeaseWatchdog::watch()
{
    while (true) {
        std::array<HANDLE, maximumObservedHandles + 1> handles{};
        handles[0] = _wake;
        auto count = DWORD{1};
        AcquireSRWLockExclusive(&_lock);
        const auto now = GetTickCount64();
        refreshLocked(now);
        if (_canceled) {
            ReleaseSRWLockExclusive(&_lock);
            return;
        }
        const auto end = _revoked ? _terminateAt : _expiresAt;
        if (now >= end) {
            ReleaseSRWLockExclusive(&_lock);
            TerminateProcess(GetCurrentProcess(), ERROR_TIMEOUT);
            return;
        }
        if (!_revoked) {
            for (DWORD index = 0; index != _observedCount; ++index) {
                handles[count++] = _observed[index];
            }
        }
        const auto remaining = static_cast<DWORD>(std::min<ULONGLONG>(end - now, INFINITE - 1));
        ReleaseSRWLockExclusive(&_lock);
        const auto waited = WaitForMultipleObjects(count, handles.data(), FALSE, remaining);
        if (waited != WAIT_OBJECT_0 && waited != WAIT_TIMEOUT) {
            AcquireSRWLockExclusive(&_lock);
            revokeLocked(GetTickCount64());
            ReleaseSRWLockExclusive(&_lock);
        }
    }
}
