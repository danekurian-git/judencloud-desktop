/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "accountstate.h"
#include "privatecloudwindow.h"
#include "progressdispatcher.h"
#include "syncresult.h"

namespace OCC
{

// A value snapshot keeps presentation independent of account and folder lifetimes.
class PrivateCloudStatus final
{
public:
    struct ProgressSnapshot {
        bool available = false;
        ProgressInfo::Status phase = ProgressInfo::Starting;
        qint64 totalFiles = 0;
        qint64 completedFiles = 0;
        qint64 remainingBytes = 0;
        int uploading = 0;
        int downloading = 0;
    };

    struct Input {
        bool hasAccount = false;
        QString displayName;
        QString username;
        QString canonicalUser;
        AccountState::State connectionState = AccountState::Disconnected;
        QStringList connectionErrors;
        bool directConnectionPending = false;
        QString directConnectionError;
        QString driveError;
        bool driveAvailable = false;
        bool drivePreparing = false;
        bool folderConfigured = false;
        bool setupPending = false;
        bool paused = false;
        bool syncRunning = false;
        bool busy = false;
        QString busyDetail;
        SyncResult::Status syncStatus = SyncResult::Undefined;
        int conflictCount = 0;
        int errorCount = 0;
        QStringList syncErrors;
        QString firstItemError;
        ProgressSnapshot progress;
    };

    struct Output {
        QString accountName;
        PrivateCloudWindow::Status status = PrivateCloudWindow::Status::SignedOut;
        QString detail;
        bool driveAvailable = false;
        bool busy = false;
    };

    [[nodiscard]] static ProgressSnapshot captureProgress(const ProgressInfo &progress);
    static void populateAccountIdentity(Input &input, const AccountPtr &account);
    [[nodiscard]] static bool shouldRetrySync(AccountState::State connectionState, SyncResult::Status syncStatus, int errorCount);
    [[nodiscard]] static Output present(const Input &input);
};

}
