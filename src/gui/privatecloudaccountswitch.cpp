/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "privatecloudaccountswitch.h"

#include <utility>

namespace OCC
{
PrivateCloudAccountSwitch::PrivateCloudAccountSwitch(QObject *parent)
    : QObject(parent)
{
    constexpr int operationPollIntervalMs = 100;
    _timer.setInterval(operationPollIntervalMs);
    connect(&_timer, &QTimer::timeout, this, &PrivateCloudAccountSwitch::check);
}

void PrivateCloudAccountSwitch::start(std::function<bool()> operationsPending, int timeoutMs)
{
    if (isRunning()) {
        return;
    }
    _operationsPending = std::move(operationsPending);
    _timeoutMs = timeoutMs;
    _elapsed.start();
    _timer.start();
    check();
}

bool PrivateCloudAccountSwitch::isRunning() const
{
    return static_cast<bool>(_operationsPending);
}

void PrivateCloudAccountSwitch::suspendSyncScheduling(bool enabled, std::function<void(bool)> setEnabled)
{
    if (_restoreSyncScheduling) {
        return;
    }
    _restoreSyncScheduling = [enabled, setEnabled] {
        setEnabled(enabled);
    };
    setEnabled(false);
}

void PrivateCloudAccountSwitch::restoreSyncScheduling()
{
    if (const auto restore = std::exchange(_restoreSyncScheduling, {})) {
        restore();
    }
}

void PrivateCloudAccountSwitch::discardSyncScheduling()
{
    _restoreSyncScheduling = {};
}

void PrivateCloudAccountSwitch::cancel()
{
    stopWaiting();
    restoreSyncScheduling();
}

void PrivateCloudAccountSwitch::stopWaiting()
{
    _timer.stop();
    _operationsPending = {};
}

void PrivateCloudAccountSwitch::check()
{
    if (!isRunning()) {
        return;
    }
    if (!_operationsPending()) {
        stopWaiting();
        Q_EMIT readyToRemove();
    } else if (_elapsed.elapsed() >= _timeoutMs) {
        cancel();
        Q_EMIT timedOut();
    }
}
}
