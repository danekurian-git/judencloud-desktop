/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "privateclouddrivecontroller.h"
#include <QSemaphore>
#include <QtTest>
#include <atomic>

using namespace OCC;

class TestPrivateCloudDriveController : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void prepareDoesNotBlockGui()
    {
        QSemaphore gate;
        PrivateCloudDriveController controller(
            [&](const QString &, const QString &next, const QString &) {
                gate.tryAcquire(1, 5000);
                return PrivateCloudDriveController::OperationResult{{}, next};
            },
            10,
            1000);
        controller.prepare(QStringLiteral("C:/test/account"));
        QVERIFY(controller.isBusy());
        QVERIFY(controller.readyPath().isEmpty());
        auto eventHandled = false;
        QTimer::singleShot(0, [&] {
            eventHandled = true;
        });
        QTRY_VERIFY(eventHandled);
        gate.release();
        QTRY_VERIFY(!controller.isBusy());
        QCOMPARE(controller.readyPath(), QStringLiteral("C:/test/account"));
    }

    void releaseCleansUpLateMount()
    {
        QSemaphore gate;
        QString previousDuringRelease;
        std::atomic<int> calls = 0;
        PrivateCloudDriveController controller(
            [&](const QString &previous, const QString &next, const QString &) {
                ++calls;
                if (!next.isEmpty()) {
                    gate.tryAcquire(1, 5000);
                } else {
                    previousDuringRelease = previous;
                }
                return PrivateCloudDriveController::OperationResult{{}, next};
            },
            10,
            1000);
        controller.prepare(QStringLiteral("C:/test/old-account"));
        controller.release();
        gate.release();
        QTRY_VERIFY(!controller.isBusy());
        QCOMPARE(calls.load(), 2);
        QCOMPARE(previousDuringRelease, QStringLiteral("C:/test/old-account"));
        QVERIFY(controller.readyPath().isEmpty());
        QVERIFY(controller.isReleased());
    }

    void retriesAreBoundedAndManualRetryRestarts()
    {
        std::atomic<int> calls = 0;
        std::atomic<bool> fail = true;
        PrivateCloudDriveController controller(
            [&](const QString &, const QString &next, const QString &) {
                ++calls;
                return PrivateCloudDriveController::OperationResult{fail ? QStringLiteral("J: occupied") : QString(), fail ? QString() : next};
            },
            10,
            1000);
        controller.prepare(QStringLiteral("C:/test/account"));
        QTRY_COMPARE(calls.load(), 3);
        QTRY_VERIFY(!controller.isBusy());
        QTest::qWait(80);
        QCOMPARE(calls.load(), 3);
        QCOMPARE(controller.error(), QStringLiteral("J: occupied"));
        QVERIFY(controller.readyPath().isEmpty());
        fail = false;
        controller.prepare(QStringLiteral("C:/test/account"), true);
        QTRY_VERIFY(!controller.isBusy());
        QCOMPARE(calls.load(), 4);
        QCOMPARE(controller.readyPath(), QStringLiteral("C:/test/account"));
    }

    void timeoutKeepsWorkTrackedUntilCompletion()
    {
        QSemaphore gate;
        PrivateCloudDriveController controller(
            [&](const QString &, const QString &next, const QString &) {
                gate.tryAcquire(1, 5000);
                return PrivateCloudDriveController::OperationResult{{}, next};
            },
            10,
            20);
        controller.prepare(QStringLiteral("C:/test/account"));
        QTRY_VERIFY(!controller.error().isEmpty());
        QVERIFY(controller.isBusy());
        gate.release();
        QTRY_VERIFY(!controller.isBusy());
        QVERIFY(controller.error().isEmpty());
        QCOMPARE(controller.readyPath(), QStringLiteral("C:/test/account"));
    }

    void releaseCancelsScheduledRetry()
    {
        std::atomic<int> mounts = 0;
        std::atomic<int> releases = 0;
        PrivateCloudDriveController controller(
            [&](const QString &, const QString &next, const QString &) {
                if (next.isEmpty()) {
                    ++releases;
                    return PrivateCloudDriveController::OperationResult{};
                }
                ++mounts;
                return PrivateCloudDriveController::OperationResult{QStringLiteral("Unavailable"), next};
            },
            100,
            1000);
        controller.prepare(QStringLiteral("C:/test/account"));
        QTRY_VERIFY(!controller.error().isEmpty());
        controller.release();
        QTRY_VERIFY(!controller.isBusy());
        QTest::qWait(150);
        QCOMPARE(mounts.load(), 1);
        QCOMPARE(releases.load(), 1);
        QVERIFY(controller.readyPath().isEmpty());
        QVERIFY(controller.isReleased());
    }

    void detachFailureRetainsOldOwnership()
    {
        const auto oldPath = QStringLiteral("C:/test/old");
        const auto newPath = QStringLiteral("C:/test/new");
        QString previousDuringRelease;
        std::atomic<int> detachAttempts = 0;
        PrivateCloudDriveController controller(
            [&](const QString &previous, const QString &next, const QString &) {
                if (next == newPath) {
                    ++detachAttempts;
                    return PrivateCloudDriveController::OperationResult{QStringLiteral("Cannot remove old mapping"), previous};
                }
                if (next.isEmpty()) {
                    previousDuringRelease = previous;
                }
                return PrivateCloudDriveController::OperationResult{{}, next};
            },
            10,
            1000);
        controller.prepare(oldPath);
        QTRY_COMPARE(controller.readyPath(), oldPath);
        controller.prepare(newPath);
        QTRY_COMPARE(detachAttempts.load(), 3);
        QTRY_VERIFY(!controller.isBusy());
        QVERIFY(controller.readyPath().isEmpty());
        QVERIFY(!controller.isReleased());
        controller.release();
        QTRY_VERIFY(controller.isReleased());
        QCOMPARE(previousDuringRelease, oldPath);
    }

    void releaseFailureRemainsBlockedAndCanRetry()
    {
        const auto path = QStringLiteral("C:/test/account");
        std::atomic<int> releases = 0;
        std::atomic<bool> failRelease = true;
        QString lastReleasePath;
        PrivateCloudDriveController controller(
            [&](const QString &previous, const QString &next, const QString &) {
                if (next.isEmpty()) {
                    ++releases;
                    lastReleasePath = previous;
                    return failRelease ? PrivateCloudDriveController::OperationResult{QStringLiteral("Windows still owns the mapping"), previous}
                                       : PrivateCloudDriveController::OperationResult{};
                }
                return PrivateCloudDriveController::OperationResult{{}, next};
            },
            10,
            1000);
        controller.prepare(path);
        QTRY_COMPARE(controller.readyPath(), path);
        controller.release();
        QVERIFY(!controller.isReleased());
        QTRY_COMPARE(releases.load(), 3);
        QTRY_VERIFY(!controller.isBusy());
        QVERIFY(!controller.isReleased());
        QTest::qWait(80);
        QCOMPARE(releases.load(), 3);
        failRelease = false;
        controller.release();
        QTRY_VERIFY(controller.isReleased());
        QCOMPARE(releases.load(), 4);
        QCOMPARE(lastReleasePath, path);
    }

    void failedLateMountRetainsCleanupResponsibility()
    {
        QSemaphore gate;
        QString releasedPath;
        const auto path = QStringLiteral("C:/test/partial");
        PrivateCloudDriveController controller(
            [&](const QString &previous, const QString &next, const QString &) {
                if (!next.isEmpty()) {
                    gate.tryAcquire(1, 5000);
                    return PrivateCloudDriveController::OperationResult{QStringLiteral("Label and rollback failed"), next};
                }
                releasedPath = previous;
                return PrivateCloudDriveController::OperationResult{};
            },
            10,
            1000);
        controller.prepare(path);
        controller.release();
        QVERIFY(!controller.isReleased());
        gate.release();
        QTRY_VERIFY(controller.isReleased());
        QCOMPARE(releasedPath, path);
    }

    void identityChangesInvalidateRunningRequest()
    {
        QSemaphore gate;
        QStringList identities;
        const auto path = QStringLiteral("C:/test/account");
        PrivateCloudDriveController controller(
            [&](const QString &, const QString &next, const QString &identity) {
                identities.append(identity);
                if (identity == QStringLiteral("old")) {
                    gate.tryAcquire(1, 5000);
                }
                return PrivateCloudDriveController::OperationResult{{}, next};
            },
            10,
            1000);
        controller.prepare(path, false, QStringLiteral("old"));
        controller.prepare(path, false, QStringLiteral("new"));
        gate.release();
        QTRY_COMPARE(controller.readyPath(), path);
        QCOMPARE(identities, QStringList({QStringLiteral("old"), QStringLiteral("new")}));
    }

    void releaseCanAdoptPreviousProcessMapping()
    {
        QString releasedPath;
        PrivateCloudDriveController controller(
            [&](const QString &previous, const QString &, const QString &) {
                releasedPath = previous;
                return PrivateCloudDriveController::OperationResult{};
            },
            10,
            1000);
        QVERIFY(controller.isReleased());
        controller.release(QStringLiteral("C:/test/recovered"));
        QTRY_VERIFY(controller.isReleased());
        QCOMPARE(releasedPath, QStringLiteral("C:/test/recovered"));
    }

    void lateTimeoutDoesNotBlockAlreadyCleanRelease()
    {
        QSemaphore gate;
        PrivateCloudDriveController controller(
            [&](const QString &, const QString &, const QString &) {
                gate.tryAcquire(1, 5000);
                return PrivateCloudDriveController::OperationResult{QStringLiteral("Mount failed without creating a mapping"), {}};
            },
            10,
            20);
        controller.prepare(QStringLiteral("C:/test/absent"));
        controller.release();
        QTRY_VERIFY(!controller.error().isEmpty());
        gate.release();
        QTRY_VERIFY(controller.isReleased());
        QVERIFY(controller.error().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestPrivateCloudDriveController)
#include "testprivateclouddrivecontroller.moc"
