/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <functional>

namespace OCC
{
// Keeps the event loop available while outstanding file operations stop.
class PrivateCloudAccountSwitch : public QObject
{
    Q_OBJECT
public:
    explicit PrivateCloudAccountSwitch(QObject *parent = nullptr);
    void start(std::function<bool()> operationsPending, int timeoutMs = 30000);
    void suspendSyncScheduling(bool enabled, std::function<void(bool)> setEnabled);
    void restoreSyncScheduling();
    void discardSyncScheduling();
    void cancel();
    [[nodiscard]] bool isRunning() const;

Q_SIGNALS:
    void readyToRemove();
    void timedOut();

private:
    void check();
    void stopWaiting();
    QTimer _timer;
    QElapsedTimer _elapsed;
    std::function<bool()> _operationsPending;
    std::function<void()> _restoreSyncScheduling;
    int _timeoutMs = 0;
};
}
