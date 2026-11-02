/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "account.h"
#include "creds/webflowcredentials.h"
#include "privatecloudstatus.h"

#include <QtTest>

using namespace OCC;

class TestPrivateCloudStatus : public QObject
{
    Q_OBJECT
    using Status = PrivateCloudWindow::Status;

    static PrivateCloudStatus::Input connected()
    {
        PrivateCloudStatus::Input input;
        input.hasAccount = true;
        input.displayName = QStringLiteral("Alice Example");
        input.username = QStringLiteral("alice");
        input.canonicalUser = QStringLiteral("canonical-account-id");
        input.connectionState = AccountState::Connected;
        input.folderConfigured = true;
        input.driveAvailable = true;
        input.syncStatus = SyncResult::Success;
        return input;
    }

private Q_SLOTS:
    void realAccountUsesServerDisplayNameThenLoginThenCanonicalUser()
    {
        const auto account = Account::create();
        account->setDavDisplayName(QStringLiteral("Alice Example"));
        account->setDavUser(QStringLiteral("canonical-account-id"));
        account->setCredentials(new WebFlowCredentials(QStringLiteral("alice-login"), QStringLiteral("test-token")));
        auto input = connected();
        PrivateCloudStatus::populateAccountIdentity(input, account);
        QVERIFY(input.hasAccount);
        QCOMPARE(input.displayName, QStringLiteral("Alice Example"));
        QCOMPARE(input.username, QStringLiteral("alice-login"));
        QCOMPARE(input.canonicalUser, QStringLiteral("canonical-account-id"));
        QCOMPARE(PrivateCloudStatus::present(input).accountName, QStringLiteral("Alice Example"));
        account->setDavDisplayName({});
        PrivateCloudStatus::populateAccountIdentity(input, account);
        QCOMPARE(PrivateCloudStatus::present(input).accountName, QStringLiteral("alice-login"));
        const auto accountWithoutCredentials = Account::create();
        accountWithoutCredentials->setDavUser(QStringLiteral("fallback-canonical-user"));
        PrivateCloudStatus::populateAccountIdentity(input, accountWithoutCredentials);
        QVERIFY(input.displayName.isEmpty());
        QVERIFY(input.username.isEmpty());
        QCOMPARE(PrivateCloudStatus::present(input).accountName, QStringLiteral("fallback-canonical-user"));
        PrivateCloudStatus::populateAccountIdentity(input, {});
        QVERIFY(!input.hasAccount);
        QVERIFY(input.displayName.isEmpty());
        QVERIFY(input.username.isEmpty());
        QVERIFY(input.canonicalUser.isEmpty());
        QCOMPARE(PrivateCloudStatus::present(input).status, Status::SignedOut);
    }

    void retrySyncOnlyForConnectedFailures_data()
    {
        QTest::addColumn<int>("connectionState");
        QTest::addColumn<int>("syncStatus");
        QTest::addColumn<int>("errorCount");
        QTest::addColumn<bool>("expected");
        QTest::newRow("sync-error") << int(AccountState::Connected) << int(SyncResult::Error) << 0 << true;
        QTest::newRow("setup-error") << int(AccountState::Connected) << int(SyncResult::SetupError) << 0 << true;
        QTest::newRow("file-errors") << int(AccountState::Connected) << int(SyncResult::Problem) << 2 << true;
        QTest::newRow("healthy") << int(AccountState::Connected) << int(SyncResult::Success) << 0 << false;
        QTest::newRow("checking") << int(AccountState::Connected) << int(SyncResult::SyncPrepare) << 0 << false;
        QTest::newRow("offline-error") << int(AccountState::NetworkError) << int(SyncResult::Error) << 2 << false;
        QTest::newRow("connecting-error") << int(AccountState::Disconnected) << int(SyncResult::SetupError) << 2 << false;
        QTest::newRow("authentication-error") << int(AccountState::AskingCredentials) << int(SyncResult::Error) << 1 << false;
        QTest::newRow("signed-out") << int(AccountState::SignedOut) << int(SyncResult::Error) << 1 << false;
    }

    void retrySyncOnlyForConnectedFailures()
    {
        QFETCH(int, connectionState);
        QFETCH(int, syncStatus);
        QFETCH(int, errorCount);
        QFETCH(bool, expected);
        QCOMPARE(
            PrivateCloudStatus::shouldRetrySync(static_cast<AccountState::State>(connectionState), static_cast<SyncResult::Status>(syncStatus), errorCount),
            expected);
    }

    void accountNamesPreferFriendlyIdentity()
    {
        auto input = connected();
        QCOMPARE(PrivateCloudStatus::present(input).accountName, QStringLiteral("Alice Example"));
        input.displayName = QStringLiteral("  ");
        QCOMPARE(PrivateCloudStatus::present(input).accountName, QStringLiteral("alice"));
        input.username.clear();
        QCOMPARE(PrivateCloudStatus::present(input).accountName, QStringLiteral("canonical-account-id"));
        input.hasAccount = false;
        const auto output = PrivateCloudStatus::present(input);
        QVERIFY(output.accountName.isEmpty());
        QCOMPARE(output.status, Status::SignedOut);
        QVERIFY(!output.driveAvailable);
    }

    void connectionStates_data()
    {
        QTest::addColumn<int>("connectionState");
        QTest::addColumn<int>("expectedStatus");
        QTest::addColumn<bool>("driveAvailable");
        QTest::newRow("ready") << int(AccountState::Connected) << int(Status::Ready) << true;
        QTest::newRow("connecting") << int(AccountState::Disconnected) << int(Status::Connecting) << true;
        QTest::newRow("signed-out") << int(AccountState::SignedOut) << int(Status::SignedOut) << false;
        QTest::newRow("authentication") << int(AccountState::AskingCredentials) << int(Status::AuthenticationRequired) << false;
        QTest::newRow("network") << int(AccountState::NetworkError) << int(Status::Offline) << true;
        QTest::newRow("service") << int(AccountState::ServiceUnavailable) << int(Status::Offline) << true;
        QTest::newRow("maintenance") << int(AccountState::MaintenanceMode) << int(Status::Offline) << true;
        QTest::newRow("captive-portal") << int(AccountState::RedirectDetected) << int(Status::Offline) << true;
        QTest::newRow("configuration") << int(AccountState::ConfigurationError) << int(Status::NeedsAttention) << true;
        QTest::newRow("terms") << int(AccountState::NeedToSignTermsOfService) << int(Status::NeedsAttention) << true;
    }

    void connectionStates()
    {
        QFETCH(int, connectionState);
        QFETCH(int, expectedStatus);
        QFETCH(bool, driveAvailable);
        auto input = connected();
        input.connectionState = static_cast<AccountState::State>(connectionState);
        const auto output = PrivateCloudStatus::present(input);
        QCOMPARE(int(output.status), expectedStatus);
        QCOMPARE(output.driveAvailable, driveAvailable);
    }

    void offlineKeepsTechnicalErrorsInDetails()
    {
        auto input = connected();
        input.connectionState = AccountState::NetworkError;
        input.connectionErrors = {QStringLiteral("Network stack diagnostic"), QStringLiteral("Second diagnostic")};
        const auto output = PrivateCloudStatus::present(input);
        QVERIFY(output.detail.startsWith(QStringLiteral("Reconnecting automatically.")));
        QVERIFY(output.detail.contains(QStringLiteral("\nNetwork stack diagnostic\nSecond diagnostic")));
        QVERIFY(output.driveAvailable);
    }

    void pendingDirectConnectionReplacesStaleReadyAndKeepsCachedDrive()
    {
        auto input = connected();
        QCOMPARE(PrivateCloudStatus::present(input).status, Status::Ready);
        input.directConnectionPending = true;
        auto output = PrivateCloudStatus::present(input);
        QCOMPARE(output.status, Status::Connecting);
        QCOMPARE(output.detail, QStringLiteral("Preparing your direct connection."));
        QVERIFY(output.driveAvailable);
        QVERIFY(!output.busy);
        input.driveAvailable = false;
        QVERIFY(!PrivateCloudStatus::present(input).driveAvailable);
        input.driveAvailable = true;
        input.directConnectionPending = false;
        input.directConnectionError = QStringLiteral("A stale diagnostic must not hide a recovered connection.");
        output = PrivateCloudStatus::present(input);
        QCOMPARE(output.status, Status::Ready);
        QVERIFY(output.detail.isEmpty());
        QVERIFY(output.driveAvailable);
    }

    void directConnectionFailureIsOfflineWithItsOwnDiagnostic()
    {
        auto input = connected();
        input.directConnectionPending = true;
        input.directConnectionError = QStringLiteral("The direct connection broker refused the lease (route_unavailable).");
        input.connectionErrors = {QStringLiteral("Stale server diagnostic")};
        input.syncStatus = SyncResult::SetupError;
        auto output = PrivateCloudStatus::present(input);
        QCOMPARE(output.status, Status::Offline);
        QVERIFY(output.detail.startsWith(QStringLiteral("Your direct connection is unavailable. Reconnecting automatically.")));
        QVERIFY(output.detail.contains(QStringLiteral("Downloaded files remain available.")));
        QVERIFY(output.detail.endsWith(QLatin1Char('\n') + input.directConnectionError));
        QVERIFY(!output.detail.contains(input.connectionErrors.first()));
        QVERIFY(output.driveAvailable);
        input.driveAvailable = false;
        output = PrivateCloudStatus::present(input);
        QCOMPARE(output.status, Status::Offline);
        QVERIFY(!output.driveAvailable);
    }

    void accountAndTeardownStatesTakePriorityOverDirectConnection()
    {
        auto input = connected();
        input.directConnectionPending = true;
        input.directConnectionError = QStringLiteral("Broker unavailable.");
        input.connectionState = AccountState::SignedOut;
        auto output = PrivateCloudStatus::present(input);
        QCOMPARE(output.status, Status::SignedOut);
        QVERIFY(!output.driveAvailable);
        QVERIFY(!output.detail.contains(input.directConnectionError));
        input.connectionState = AccountState::AskingCredentials;
        input.connectionErrors = {QStringLiteral("Sign-in diagnostic")};
        output = PrivateCloudStatus::present(input);
        QCOMPARE(output.status, Status::AuthenticationRequired);
        QVERIFY(output.detail.contains(input.connectionErrors.first()));
        QVERIFY(!output.detail.contains(input.directConnectionError));
        QVERIFY(!output.driveAvailable);
        input.busy = true;
        input.busyDetail = QStringLiteral("Finishing file operations.");
        output = PrivateCloudStatus::present(input);
        QCOMPARE(output.status, Status::Connecting);
        QCOMPARE(output.detail, input.busyDetail);
        QVERIFY(output.busy);
        QVERIFY(!output.driveAvailable);
        input.hasAccount = false;
        QCOMPARE(PrivateCloudStatus::present(input).status, Status::SignedOut);
    }

    void driveAndAccountOperationPriority()
    {
        auto input = connected();
        input.drivePreparing = true;
        auto output = PrivateCloudStatus::present(input);
        QCOMPARE(output.status, Status::Connecting);
        QCOMPARE(output.detail, QStringLiteral("Preparing your drive."));
        QVERIFY(!output.busy);
        QVERIFY(!output.driveAvailable);
        input.driveError = QStringLiteral("J: is occupied.");
        output = PrivateCloudStatus::present(input);
        QCOMPARE(output.status, Status::NeedsAttention);
        QVERIFY(output.detail.endsWith(QStringLiteral("\nJ: is occupied.")));
        input.connectionState = AccountState::AskingCredentials;
        QCOMPARE(PrivateCloudStatus::present(input).status, Status::AuthenticationRequired);
        input.busy = true;
        input.busyDetail = QStringLiteral("Finishing file operations.");
        output = PrivateCloudStatus::present(input);
        QCOMPARE(output.status, Status::Connecting);
        QCOMPARE(output.detail, input.busyDetail);
        QVERIFY(output.busy);
        QVERIFY(!output.driveAvailable);
    }

    void conflictsAndErrorsHaveDistinctRecovery()
    {
        auto input = connected();
        input.conflictCount = 2;
        auto output = PrivateCloudStatus::present(input);
        QCOMPARE(output.status, Status::Conflicts);
        QVERIFY(output.detail.contains(QStringLiteral("Both versions were kept")));
        QVERIFY(output.driveAvailable);
        input.errorCount = 1;
        input.syncErrors = {QStringLiteral("First technical error"), QStringLiteral("Second technical error")};
        input.firstItemError = input.syncErrors.first();
        output = PrivateCloudStatus::present(input);
        QCOMPARE(output.status, Status::NeedsAttention);
        QVERIFY(output.detail.contains(QStringLiteral("Open Details")));
        QCOMPARE(output.detail.count(QStringLiteral("First technical error")), 1);
        QVERIFY(output.detail.contains(QStringLiteral("Second technical error")));
        input.errorCount = 0;
        input.syncErrors.clear();
        input.firstItemError.clear();
        input.conflictCount = 0;
        input.syncStatus = SyncResult::SetupError;
        output = PrivateCloudStatus::present(input);
        QCOMPARE(output.status, Status::NeedsAttention);
        QVERIFY(!output.detail.isEmpty());
    }

    void missingPausedAndPreparingFolders()
    {
        auto input = connected();
        input.folderConfigured = false;
        QCOMPARE(PrivateCloudStatus::present(input).status, Status::NeedsAttention);
        input.folderConfigured = true;
        input.setupPending = true;
        QCOMPARE(PrivateCloudStatus::present(input).status, Status::NeedsAttention);
        input.setupPending = false;
        input.paused = true;
        QCOMPARE(PrivateCloudStatus::present(input).status, Status::Paused);
        input.paused = false;
        input.syncStatus = SyncResult::SyncPrepare;
        QCOMPARE(PrivateCloudStatus::present(input).status, Status::Checking);
    }

    void discoveryAndStaleProgress()
    {
        auto input = connected();
        input.syncRunning = true;
        input.progress.available = true;
        for (const auto phase : {ProgressInfo::Starting, ProgressInfo::Discovery, ProgressInfo::Reconcile}) {
            input.progress.phase = phase;
            QCOMPARE(PrivateCloudStatus::present(input).status, Status::Checking);
        }
        input.progress.phase = ProgressInfo::Propagation;
        input.progress.uploading = 1;
        QCOMPARE(PrivateCloudStatus::present(input).status, Status::Syncing);
        input.syncRunning = false;
        QCOMPARE(PrivateCloudStatus::present(input).status, Status::Ready);
    }

    void snapshotCountsRealTransfersAndExcludesPlaceholders()
    {
        ProgressInfo progress;
        progress._status = ProgressInfo::Propagation;
        auto upload = SyncFileItem();
        upload._file = QStringLiteral("upload.txt");
        upload._instruction = CSYNC_INSTRUCTION_NEW;
        upload._type = ItemTypeFile;
        upload._direction = SyncFileItem::Up;
        upload._size = 100;
        progress.adjustTotalsForFile(upload);
        progress.setProgressItem(upload, 25);
        auto download = upload;
        download._file = QStringLiteral("download.txt");
        download._direction = SyncFileItem::Down;
        download._size = 200;
        progress.adjustTotalsForFile(download);
        progress.setProgressItem(download, 50);
        auto placeholder = download;
        placeholder._file = QStringLiteral("online-only.txt");
        placeholder._type = ItemTypeVirtualFile;
        progress.adjustTotalsForFile(placeholder);
        progress.setProgressItem(placeholder, 0);
        const auto snapshot = PrivateCloudStatus::captureProgress(progress);
        QVERIFY(snapshot.available);
        QCOMPARE(snapshot.phase, ProgressInfo::Propagation);
        QCOMPARE(snapshot.uploading, 1);
        QCOMPARE(snapshot.downloading, 1);
        QCOMPARE(snapshot.totalFiles, 3);
        QCOMPARE(snapshot.completedFiles, 0);
        QCOMPARE(snapshot.remainingBytes, 225);
        auto input = connected();
        input.syncRunning = true;
        input.progress = snapshot;
        auto output = PrivateCloudStatus::present(input);
        QVERIFY(output.detail.startsWith(QStringLiteral("Uploading and downloading")));
        QVERIFY(output.detail.contains(QStringLiteral("0 of 3 items complete")));
        progress.setProgressComplete(upload);
        input.progress = PrivateCloudStatus::captureProgress(progress);
        output = PrivateCloudStatus::present(input);
        QVERIFY(output.detail.startsWith(QStringLiteral("Downloading")));
        QCOMPARE(input.progress.completedFiles, 1);
        QCOMPARE(input.progress.remainingBytes, 150);
    }

    void metadataUpdatesDoNotClaimDownloads()
    {
        auto input = connected();
        input.syncRunning = true;
        input.progress.available = true;
        input.progress.phase = ProgressInfo::Propagation;
        input.progress.totalFiles = 2;
        input.progress.completedFiles = 8;
        const auto output = PrivateCloudStatus::present(input);
        QCOMPARE(output.status, Status::Syncing);
        QCOMPARE(output.detail, QStringLiteral("Updating your files. 2 of 2 items complete."));
    }
};

QTEST_GUILESS_MAIN(TestPrivateCloudStatus)
#include "testprivatecloudstatus.moc"
