/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "privateclouddrivecontroller.h"
#include "privateclouddrive.h"

#include <QCoreApplication>
#include <QDir>
#include <QtConcurrentRun>
#include <utility>

namespace OCC
{
namespace
{
constexpr auto recoveryDelayMs = 5000;
constexpr auto preparationDeadlineMs = 15000;
constexpr auto maximumAttempts = 3;

PrivateCloudDriveController::OperationResult updateDrive(const QString &previousPath, const QString &nextPath, const QString &accountIdentity)
{
    if (!nextPath.isEmpty()) {
        QString error;
        const auto expectedPath = accountIdentity.isEmpty() ? QString() : PrivateCloudDrive::backingPath(accountIdentity, &error);
        if (error.isEmpty()
            && (expectedPath.isEmpty()
                || QDir::cleanPath(QDir::fromNativeSeparators(expectedPath)).compare(QDir::cleanPath(QDir::fromNativeSeparators(nextPath)), Qt::CaseInsensitive)
                    != 0)) {
            error = QCoreApplication::translate("PrivateCloudDriveController", "The configured sync folder does not match this privateCloud account.");
        }
        if (!error.isEmpty()) {
            return {error, previousPath};
        }
    }
    if (!previousPath.isEmpty() && previousPath != nextPath) {
        const auto error = PrivateCloudDrive::unmountChecked(previousPath);
        if (!error.isEmpty()) {
            return {error, previousPath};
        }
    }
    if (nextPath.isEmpty()) {
        return {};
    }
    const auto error = PrivateCloudDrive::mount(nextPath);
    if (error.isEmpty()) {
        return {{}, nextPath};
    }
    // A failed mount can still have created an alias before a later registry or
    // label operation failed. Keep its cleanup tracked until removal succeeds.
    const auto cleanupError = PrivateCloudDrive::unmountChecked(nextPath);
    return cleanupError.isEmpty() ? PrivateCloudDriveController::OperationResult{error, {}}
                                  : PrivateCloudDriveController::OperationResult{error + QLatin1Char('\n') + cleanupError, nextPath};
}
}

PrivateCloudDriveController::PrivateCloudDriveController(QObject *parent)
    : PrivateCloudDriveController(updateDrive, recoveryDelayMs, preparationDeadlineMs, parent)
{
}

PrivateCloudDriveController::PrivateCloudDriveController(Operation operation, int retryDelayMs, int deadlineMs, QObject *parent)
    : QObject(parent)
    , _operation(std::move(operation))
    , _retryDelayMs(retryDelayMs)
{
    _retry.setSingleShot(true);
    _deadline.setSingleShot(true);
    _deadline.setInterval(deadlineMs);
    connect(&_retry, &QTimer::timeout, this, &PrivateCloudDriveController::start);
    connect(&_watcher, &QFutureWatcher<OperationResult>::finished, this, &PrivateCloudDriveController::finish);
    connect(&_deadline, &QTimer::timeout, this, [this] {
        _error = tr("Windows is taking longer than expected to prepare the drive. Recovery is continuing in the background.");
        Q_EMIT changed();
    });
}

void PrivateCloudDriveController::prepare(const QString &path, bool force, const QString &accountIdentity)
{
    if (_desiredPath == path && _desiredIdentity == accountIdentity
        && (_running || (!force && (_retry.isActive() || (path == _mountedPath && _error.isEmpty()))))) {
        return;
    }
    _desiredPath = path;
    _desiredIdentity = accountIdentity;
    ++_generation;
    _retry.stop();
    _error.clear();
    _attempts = 0;
    if (!_running) {
        start();
    } else {
        Q_EMIT changed();
    }
}

void PrivateCloudDriveController::release(const QString &ownedPath)
{
    if (!_running && _mountedPath.isEmpty()) {
        _mountedPath = ownedPath;
    }
    prepare({});
}

QString PrivateCloudDriveController::readyPath() const
{
    return !_running && _error.isEmpty() && _desiredPath == _mountedPath ? _mountedPath : QString();
}

QString PrivateCloudDriveController::error() const
{
    return _error;
}

bool PrivateCloudDriveController::isBusy() const
{
    return _running;
}

bool PrivateCloudDriveController::isReleased() const
{
    return !_running && !_retry.isActive() && _desiredPath.isEmpty() && _mountedPath.isEmpty() && _error.isEmpty();
}

void PrivateCloudDriveController::start()
{
    if (_running) {
        return;
    }
    if (_desiredPath.isEmpty() && _mountedPath.isEmpty()) {
        Q_EMIT changed();
        return;
    }
    _running = true;
    _runningPath = _desiredPath;
    _runningIdentity = _desiredIdentity;
    _runningGeneration = _generation;
    ++_attempts;
    _deadline.start();
    _watcher.setFuture(QtConcurrent::run(_operation, _mountedPath, _runningPath, _runningIdentity));
    Q_EMIT changed();
}

void PrivateCloudDriveController::finish()
{
    _deadline.stop();
    _running = false;
    const auto result = _watcher.result();
    _mountedPath = result.ownedPath;
    if (_runningGeneration != _generation) {
        // An old account may have mounted while sign-out was requested. Remove
        // its alias before reporting the new request as complete.
        _error.clear();
        start();
        return;
    }
    _error = result.error;
    if (!_error.isEmpty() && (!_desiredPath.isEmpty() || !_mountedPath.isEmpty()) && _attempts < maximumAttempts) {
        _retry.start(_retryDelayMs * _attempts);
    }
    Q_EMIT changed();
}
}
