/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QTimer>
#include <functional>

namespace OCC
{
// Serializes native drive work away from the GUI and discards obsolete results.
class PrivateCloudDriveController : public QObject
{
    Q_OBJECT
public:
    struct OperationResult {
        QString error;
        // Retained until the native backend confirms this mapping was removed.
        QString ownedPath;
    };
    using Operation = std::function<OperationResult(const QString &previousPath, const QString &nextPath, const QString &accountIdentity)>;
    explicit PrivateCloudDriveController(QObject *parent = nullptr);
    PrivateCloudDriveController(Operation operation, int retryDelayMs, int deadlineMs, QObject *parent = nullptr);
    void prepare(const QString &path, bool force = false, const QString &accountIdentity = {});
    void release(const QString &ownedPath = {});
    [[nodiscard]] QString readyPath() const;
    [[nodiscard]] QString error() const;
    [[nodiscard]] bool isBusy() const;
    [[nodiscard]] bool isReleased() const;

Q_SIGNALS:
    void changed();

private:
    void start();
    void finish();
    Operation _operation;
    QFutureWatcher<OperationResult> _watcher;
    QTimer _retry;
    QTimer _deadline;
    QString _desiredPath;
    QString _mountedPath;
    QString _runningPath;
    QString _desiredIdentity;
    QString _runningIdentity;
    QString _error;
    quint64 _generation = 0;
    quint64 _runningGeneration = 0;
    int _attempts = 0;
    int _retryDelayMs;
    bool _running = false;
};
}
