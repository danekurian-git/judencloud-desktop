/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "privatecloudstatus.h"
#include "account.h"

#include <QCoreApplication>
#include <QLocale>

namespace OCC
{
namespace
{
QString firstNonEmpty(const QStringList &values)
{
    for (const auto &value : values) {
        if (!value.trimmed().isEmpty()) {
            return value.trimmed();
        }
    }
    return {};
}

QString withDiagnostics(const QString &summary, QStringList diagnostics)
{
    diagnostics.removeAll(QString());
    diagnostics.removeDuplicates();
    return diagnostics.isEmpty() ? summary : summary + QLatin1Char('\n') + diagnostics.join(QLatin1Char('\n'));
}

QString transferDetail(const PrivateCloudStatus::ProgressSnapshot &progress)
{
    QString action;
    if (progress.uploading > 0 && progress.downloading > 0) {
        action = QCoreApplication::translate("PrivateCloudStatus", "Uploading and downloading files.");
    } else if (progress.uploading > 0) {
        //: %n is the number of files currently uploading, not the entire sync queue.
        action = QCoreApplication::translate("PrivateCloudStatus", "Uploading files (%n active).", nullptr, progress.uploading);
    } else if (progress.downloading > 0) {
        //: %n is the number of files currently downloading, not the entire sync queue.
        action = QCoreApplication::translate("PrivateCloudStatus", "Downloading files (%n active).", nullptr, progress.downloading);
    } else {
        action = QCoreApplication::translate("PrivateCloudStatus", "Updating your files.");
    }
    if (progress.totalFiles <= 0) {
        return action;
    }
    const auto completed = qBound<qint64>(0, progress.completedFiles, progress.totalFiles);
    //: %1 is an action such as "Uploading 2 files."; %2 is completed items and %3 is total items in this sync run.
    auto detail = QCoreApplication::translate("PrivateCloudStatus", "%1 %2 of %3 items complete.")
                      .arg(action, QLocale().toString(completed), QLocale().toString(progress.totalFiles));
    if (progress.remainingBytes > 0 && (progress.uploading > 0 || progress.downloading > 0)) {
        //: %1 is the previous progress sentence and %2 is a formatted amount of data, for example "12 MiB".
        detail = QCoreApplication::translate("PrivateCloudStatus", "%1 %2 left to transfer.").arg(detail, QLocale().formattedDataSize(progress.remainingBytes));
    }
    return detail;
}
}

PrivateCloudStatus::ProgressSnapshot PrivateCloudStatus::captureProgress(const ProgressInfo &progress)
{
    ProgressSnapshot snapshot;
    snapshot.available = true;
    snapshot.phase = progress.status();
    snapshot.totalFiles = qMax<qint64>(0, progress.totalFiles());
    snapshot.completedFiles = qBound<qint64>(0, progress.completedFiles(), snapshot.totalFiles);
    snapshot.remainingBytes = qMax<qint64>(0, progress.totalSize() - progress.completedSize());
    for (const auto &item : progress._currentItems) {
        if (!ProgressInfo::isSizeDependent(item._item)) {
            continue;
        }
        if (item._item._direction == SyncFileItem::Up) {
            ++snapshot.uploading;
        } else if (item._item._direction == SyncFileItem::Down) {
            ++snapshot.downloading;
        }
    }
    return snapshot;
}

void PrivateCloudStatus::populateAccountIdentity(Input &input, const AccountPtr &account)
{
    input.hasAccount = !account.isNull();
    input.displayName = account ? account->davDisplayName() : QString();
    input.username = account && account->credentials() ? account->credentials()->user() : QString();
    input.canonicalUser = account ? account->davUser() : QString();
}

bool PrivateCloudStatus::shouldRetrySync(AccountState::State connectionState, SyncResult::Status syncStatus, int errorCount)
{
    return connectionState == AccountState::Connected && (syncStatus == SyncResult::Error || syncStatus == SyncResult::SetupError || errorCount > 0);
}

PrivateCloudStatus::Output PrivateCloudStatus::present(const Input &input)
{
    using Status = PrivateCloudWindow::Status;
    Output output;
    if (!input.hasAccount) {
        return output;
    }
    output.accountName = firstNonEmpty({input.displayName, input.username, input.canonicalUser});
    output.driveAvailable = input.driveAvailable && input.driveError.isEmpty() && input.connectionState != AccountState::SignedOut
        && input.connectionState != AccountState::AskingCredentials && !input.busy && !input.drivePreparing;
    output.busy = input.busy;
    if (input.busy) {
        output.status = Status::Connecting;
        output.detail = input.busyDetail;
        return output;
    }
    if (input.connectionState == AccountState::SignedOut) {
        output.status = Status::SignedOut;
        return output;
    }
    if (input.connectionState == AccountState::AskingCredentials) {
        output.status = Status::AuthenticationRequired;
        output.detail = withDiagnostics(QCoreApplication::translate("PrivateCloudStatus", "Sign in again to reconnect your drive."), input.connectionErrors);
        return output;
    }
    if (input.directConnectionPending) {
        if (input.directConnectionError.isEmpty()) {
            output.status = Status::Connecting;
            output.detail = QCoreApplication::translate("PrivateCloudStatus", "Preparing your direct connection.");
        } else {
            output.status = Status::Offline;
            output.detail = withDiagnostics(
                QCoreApplication::translate("PrivateCloudStatus",
                                            "Your direct connection is unavailable. Reconnecting automatically. Downloaded files remain available."),
                {input.directConnectionError});
        }
        return output;
    }
    if (!input.driveError.isEmpty()) {
        output.status = Status::NeedsAttention;
        output.detail =
            withDiagnostics(QCoreApplication::translate("PrivateCloudStatus", "Your drive could not be prepared. Open Details for more information."),
                            {input.driveError});
        return output;
    }
    if (input.drivePreparing) {
        output.status = Status::Connecting;
        output.detail = QCoreApplication::translate("PrivateCloudStatus", "Preparing your drive.");
        return output;
    }
    switch (input.connectionState) {
    case AccountState::NetworkError:
    case AccountState::ServiceUnavailable:
        output.status = Status::Offline;
        output.detail = withDiagnostics(QCoreApplication::translate("PrivateCloudStatus", "Reconnecting automatically. Downloaded files remain available."),
                                        input.connectionErrors);
        return output;
    case AccountState::MaintenanceMode:
        output.status = Status::Offline;
        output.detail = withDiagnostics(QCoreApplication::translate("PrivateCloudStatus", "The server is undergoing maintenance. Reconnecting automatically."),
                                        input.connectionErrors);
        return output;
    case AccountState::RedirectDetected:
        output.status = Status::Offline;
        output.detail = withDiagnostics(QCoreApplication::translate("PrivateCloudStatus", "This network may require sign-in. Check your internet connection."),
                                        input.connectionErrors);
        return output;
    case AccountState::ConfigurationError:
    case AccountState::NeedToSignTermsOfService:
        output.status = Status::NeedsAttention;
        output.detail =
            withDiagnostics(input.connectionState == AccountState::NeedToSignTermsOfService
                                ? QCoreApplication::translate("PrivateCloudStatus", "Sign in to the server in your browser to review its terms of service.")
                                : QCoreApplication::translate("PrivateCloudStatus", "The server connection needs attention. Contact your administrator."),
                            input.connectionErrors);
        return output;
    case AccountState::Disconnected:
        output.status = Status::Connecting;
        return output;
    case AccountState::Connected:
        break;
    case AccountState::SignedOut:
    case AccountState::AskingCredentials:
        Q_UNREACHABLE();
    }
    if (!input.folderConfigured || input.setupPending) {
        output.status = Status::NeedsAttention;
        output.detail = QCoreApplication::translate("PrivateCloudStatus", "Drive setup is incomplete. Select Retry to continue.");
    } else if (input.paused) {
        output.status = Status::Paused;
    } else if (input.syncStatus == SyncResult::Error || input.syncStatus == SyncResult::SetupError || input.errorCount > 0) {
        output.status = Status::NeedsAttention;
        QString summary;
        if (input.errorCount > 0) {
            //: %n is the number of files with synchronization errors.
            summary =
                QCoreApplication::translate("PrivateCloudStatus", "Files needing attention: %n. Open Details for more information.", nullptr, input.errorCount);
        } else {
            summary = QCoreApplication::translate("PrivateCloudStatus", "Your files could not finish updating. Open Details for more information.");
        }
        auto diagnostics = input.syncErrors;
        diagnostics.append(input.firstItemError);
        output.detail = withDiagnostics(summary, diagnostics);
    } else if (input.conflictCount > 0) {
        output.status = Status::Conflicts;
        //: %n is the number of unresolved file conflicts. Both local and server versions have been preserved.
        output.detail =
            QCoreApplication::translate("PrivateCloudStatus", "Conflicting files: %n. Both versions were kept in your drive.", nullptr, input.conflictCount);
    } else if (input.syncRunning || input.syncStatus == SyncResult::SyncPrepare || input.syncStatus == SyncResult::SyncRunning) {
        if (input.syncStatus == SyncResult::SyncPrepare || !input.progress.available || input.progress.phase == ProgressInfo::Starting
            || input.progress.phase == ProgressInfo::Discovery || input.progress.phase == ProgressInfo::Reconcile) {
            output.status = Status::Checking;
            output.detail = QCoreApplication::translate("PrivateCloudStatus", "Checking for changes. You can keep using your drive.");
        } else {
            output.status = Status::Syncing;
            output.detail = transferDetail(input.progress);
        }
    } else if (output.driveAvailable) {
        output.status = Status::Ready;
    } else {
        output.status = Status::Connecting;
        output.detail = QCoreApplication::translate("PrivateCloudStatus", "Preparing your drive.");
    }
    return output;
}

}
