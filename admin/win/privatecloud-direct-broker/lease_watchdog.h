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

#include <array>

// Declare before the owned WFP lease/session: normal destruction must release
// that session before canceling this independent process deadline.
class LeaseWatchdog
{
public:
    static constexpr DWORD cleanupGraceMs = 1000;
    static constexpr DWORD maximumObservedHandles = 3;

    explicit LeaseWatchdog(DWORD initialBudgetMs, HANDLE stopEvent = nullptr);
    ~LeaseWatchdog();

    LeaseWatchdog(const LeaseWatchdog &) = delete;
    LeaseWatchdog &operator=(const LeaseWatchdog &) = delete;
    LeaseWatchdog(LeaseWatchdog &&) = delete;
    LeaseWatchdog &operator=(LeaseWatchdog &&) = delete;

    [[nodiscard]] bool ready() const;
    // Duplicates a process or manual-reset-event synchronization handle.
    // Originals may be closed afterward.
    // Handles remain observed until destruction; failed admission revokes.
    [[nodiscard]] bool observe(HANDLE handle);
    // No duration can revive an expired, revoked, or canceled watchdog.
    [[nodiscard]] bool renew(DWORD milliseconds);
    [[nodiscard]] bool revoked();
    [[nodiscard]] HANDLE revocationEvent() const;
    // Caller must first destroy its lease/session, including on error paths.
    void cancelAndJoin();

private:
    static DWORD WINAPI run(void *context);
    void watch();
    void refreshLocked(ULONGLONG now);
    void revokeLocked(ULONGLONG now);

    SRWLOCK _lock = SRWLOCK_INIT;
    HANDLE _wake = nullptr;
    HANDLE _revocation = nullptr;
    HANDLE _thread = nullptr;
    std::array<HANDLE, maximumObservedHandles> _observed{};
    DWORD _observedCount = 0;
    ULONGLONG _expiresAt = 0;
    ULONGLONG _terminateAt = 0;
    bool _revoked = false;
    bool _canceled = false;
};
