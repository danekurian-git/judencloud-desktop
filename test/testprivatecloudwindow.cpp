/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "accountmanager.h"
#include "configfile.h"
#include "privatecloudaccountswitch.h"
#include "privateclouddrive.h"
#include "privatecloudwindow.h"
#include "syncenginetestutils.h"
#include "theme.h"
#include "tray/usermodel.h"
#include <QTemporaryDir>

#include <QAction>
#include <QClipboard>
#include <QDir>
#include <QFontDatabase>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <QTabWidget>
#include <QToolButton>
#include <QTreeView>
#include <QtTest>

using namespace OCC;

class TestPrivateCloudWindow : public QObject
{
    Q_OBJECT
    QTemporaryDir _configuration;

public:
    TestPrivateCloudWindow()
    {
        Q_INIT_RESOURCE(resources);
        Q_INIT_RESOURCE(theme);
    }

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(_configuration.isValid());
        QVERIFY(ConfigFile::setConfDir(_configuration.path()));
#ifdef Q_OS_WIN
        const auto windowsDirectory = qEnvironmentVariable("WINDIR", QStringLiteral("C:/Windows"));
        QVERIFY(QFontDatabase::addApplicationFont(QDir(windowsDirectory).filePath(QStringLiteral("Fonts/segoeui.ttf"))) >= 0);
        QGuiApplication::setFont(QFont(QStringLiteral("Segoe UI"), 10));
#endif
    }

    void testPrivateCloudIconHasTransparentBackground()
    {
        if (!PrivateCloudDrive::isEnabled()) {
            QSKIP("The privateCloud icon is enabled only in privateCloud builds.");
        }
        const auto image = Theme::instance()->applicationIcon().pixmap(64, 64).toImage();
        QVERIFY(!image.isNull());
        QVERIFY(image.hasAlphaChannel());
        QCOMPARE(image.pixelColor(0, 0).alpha(), 0);
        QCOMPARE(image.pixelColor(image.width() - 1, 0).alpha(), 0);
        QCOMPARE(image.pixelColor(0, image.height() - 1).alpha(), 0);
        QCOMPARE(image.pixelColor(image.width() - 1, image.height() - 1).alpha(), 0);
        bool whiteCloudNearCenter = false;
        for (int y = image.height() / 4; y < 3 * image.height() / 4; ++y) {
            for (int x = image.width() / 4; x < 3 * image.width() / 4; ++x) {
                const auto color = image.pixelColor(x, y);
                whiteCloudNearCenter = whiteCloudNearCenter || (color.alpha() > 200 && color.red() > 200 && color.green() > 200 && color.blue() > 200);
            }
        }
        QVERIFY(whiteCloudNearCenter);
    }

    void testSignedOutActions()
    {
        PrivateCloudWindow window;
        window.setStatus({}, QStringLiteral("J:/"), PrivateCloudWindow::Status::SignedOut, {}, false);
        auto *openDrive = window.findChild<QPushButton *>(QStringLiteral("privateCloudOpenDrive"));
        auto *retry = window.findChild<QPushButton *>(QStringLiteral("privateCloudRetry"));
        auto *signOut = window.findChild<QAction *>(QStringLiteral("privateCloudSignOut"));
        auto *change = window.findChild<QAction *>(QStringLiteral("privateCloudChangeAccount"));
        QVERIFY(openDrive && retry && signOut && change);
        QVERIFY(!openDrive->isEnabled());
        QVERIFY(!signOut->isEnabled());
        QVERIFY(!change->isEnabled());
        QCOMPARE(retry->text(), QStringLiteral("Sign in"));
        QSignalSpy signIn(&window, &PrivateCloudWindow::retryRequested);
        QSignalSpy opened(&window, &PrivateCloudWindow::openDriveRequested);
        retry->click();
        openDrive->click();
        QCOMPARE(signIn.count(), 1);
        QCOMPARE(opened.count(), 0);
    }

    void testReadyDoesNotOfferRetry()
    {
        PrivateCloudWindow window;
        window.setStatus(QStringLiteral("alice"), QStringLiteral("J:/"), PrivateCloudWindow::Status::Ready, {}, true);
        auto *retry = window.findChild<QPushButton *>(QStringLiteral("privateCloudRetry"));
        QVERIFY(retry);
        QVERIFY(retry->isHidden());
        QVERIFY(!retry->isEnabled());
        QCOMPARE(window.findChild<QPushButton *>(QStringLiteral("privateCloudOpenDrive"))->text(), QStringLiteral("Open privateCloud (J:)"));
    }

    void testFullDiagnosticsAreAvailable()
    {
        PrivateCloudWindow window;
        const QString diagnostic = QStringLiteral("Could not update your files.\n") + QString(1000, u'x') + QStringLiteral("\nlast filename");
        window.setStatus(QStringLiteral("alice"), QStringLiteral("J:/"), PrivateCloudWindow::Status::NeedsAttention, diagnostic, true);
        auto *details = window.findChild<QPlainTextEdit *>(QStringLiteral("privateCloudFullDetails"));
        QVERIFY(details);
        QCOMPARE(details->toPlainText(), diagnostic);
        QVERIFY(details->isReadOnly());
        auto *toggle = window.findChild<QToolButton *>(QStringLiteral("privateCloudDetailsToggle"));
        auto *panel = window.findChild<QWidget *>(QStringLiteral("privateCloudDetailsPanel"));
        auto *copy = window.findChild<QToolButton *>(QStringLiteral("privateCloudCopyDetails"));
        QVERIFY(toggle && panel && copy);
        QVERIFY(panel->isHidden());
        toggle->click();
        QVERIFY(!panel->isHidden());
        const auto originalClipboard = QApplication::clipboard()->text();
        const auto restoreClipboard = qScopeGuard([originalClipboard] {
            QApplication::clipboard()->setText(originalClipboard);
        });
        copy->click();
        QCOMPARE(QApplication::clipboard()->text(), diagnostic);
        window.setStatus(QStringLiteral("alice"), QStringLiteral("J:/"), PrivateCloudWindow::Status::NeedsAttention, diagnostic, true);
        QVERIFY(toggle->isChecked());
        QCOMPARE(details->toPlainText(), diagnostic);
        window.setStatus(QStringLiteral("bob"), QStringLiteral("J:/"), PrivateCloudWindow::Status::NeedsAttention, QStringLiteral("New account error"), true);
        QVERIFY(!toggle->isChecked());
        QVERIFY(panel->isHidden());
        QCOMPARE(details->toPlainText(), QStringLiteral("New account error"));
        window.setStatus(QStringLiteral("bob"), QStringLiteral("J:/"), PrivateCloudWindow::Status::Ready, {}, true);
        QVERIFY(toggle->isHidden());
        QVERIFY(panel->isHidden());
        QVERIFY(details->toPlainText().isEmpty());
        QVERIFY(!copy->isEnabled());
    }

    void testConnectionStates_data()
    {
        QTest::addColumn<PrivateCloudWindow::Status>("status");
        QTest::addColumn<QString>("label");
        QTest::addColumn<QString>("action");
        QTest::newRow("connecting") << PrivateCloudWindow::Status::Connecting << QStringLiteral("Connecting") << QString();
        QTest::newRow("ready") << PrivateCloudWindow::Status::Ready << QStringLiteral("Ready") << QString();
        QTest::newRow("syncing") << PrivateCloudWindow::Status::Syncing << QStringLiteral("Syncing") << QString();
        QTest::newRow("checking") << PrivateCloudWindow::Status::Checking << QStringLiteral("Checking for changes") << QString();
        QTest::newRow("paused") << PrivateCloudWindow::Status::Paused << QStringLiteral("Paused") << QStringLiteral("Resume");
        QTest::newRow("error") << PrivateCloudWindow::Status::NeedsAttention << QStringLiteral("Needs attention") << QStringLiteral("Retry");
        QTest::newRow("offline") << PrivateCloudWindow::Status::Offline << QStringLiteral("Offline") << QStringLiteral("Reconnect");
        QTest::newRow("authentication") << PrivateCloudWindow::Status::AuthenticationRequired << QStringLiteral("Sign in required")
                                        << QStringLiteral("Sign in");
        QTest::newRow("conflicts") << PrivateCloudWindow::Status::Conflicts << QStringLiteral("Files need review") << QStringLiteral("Review files");
    }

    void testConnectionStates()
    {
        QFETCH(PrivateCloudWindow::Status, status);
        QFETCH(QString, label);
        QFETCH(QString, action);
        PrivateCloudWindow window;
        window.setStatus(QStringLiteral("alice"), QStringLiteral("X:/"), status, {}, true);
        QCOMPARE(window.findChild<QLabel *>(QStringLiteral("privateCloudStatus"))->text(), label);
        QCOMPARE(window.statusSummary(), label);
        QCOMPARE(window.findChild<QLabel *>(QStringLiteral("privateCloudAccount"))->text(), QStringLiteral("alice"));
        QCOMPARE(window.findChild<QLabel *>(QStringLiteral("privateCloudDrive"))->text(), QStringLiteral("privateCloud (X:)"));
        QVERIFY(!window.findChild<QLabel *>(QStringLiteral("privateCloudDetail"))->text().isEmpty());
        auto *retry = window.findChild<QPushButton *>(QStringLiteral("privateCloudRetry"));
        QCOMPARE(retry->text(), action);
        QCOMPARE(retry->isHidden(), action.isEmpty());
        QCOMPARE(retry->isEnabled(), !action.isEmpty());
        QSignalSpy recovery(&window, &PrivateCloudWindow::retryRequested);
        QSignalSpy review(&window, &PrivateCloudWindow::reviewFilesRequested);
        retry->click();
        QCOMPARE(recovery.count(), !action.isEmpty() && status != PrivateCloudWindow::Status::Conflicts ? 1 : 0);
        QCOMPARE(review.count(), status == PrivateCloudWindow::Status::Conflicts ? 1 : 0);
    }

    void testEssentialActions()
    {
        PrivateCloudWindow window;
        window.setStatus(QStringLiteral("alice"), QStringLiteral("J:/"), PrivateCloudWindow::Status::Ready, {}, true);
        QSignalSpy openDrive(&window, &PrivateCloudWindow::openDriveRequested);
        QSignalSpy retry(&window, &PrivateCloudWindow::retryRequested);
        QSignalSpy signOut(&window, &PrivateCloudWindow::signOutRequested);
        QSignalSpy changeAccount(&window, &PrivateCloudWindow::changeAccountRequested);
        QSignalSpy quit(&window, &PrivateCloudWindow::quitRequested);
        window.findChild<QPushButton *>(QStringLiteral("privateCloudOpenDrive"))->click();
        window.findChild<QPushButton *>(QStringLiteral("privateCloudRetry"))->click();
        auto *options = window.findChild<QToolButton *>(QStringLiteral("privateCloudOptions"));
        QVERIFY(options && options->menu());
        QCOMPARE(options->accessibleName(), QStringLiteral("More options"));
        for (auto *action : options->menu()->actions()) {
            if (!action->isSeparator()) {
                action->trigger();
            }
        }
        QCOMPARE(openDrive.count(), 1);
        QCOMPARE(retry.count(), 0);
        QCOMPARE(signOut.count(), 1);
        QCOMPARE(changeAccount.count(), 1);
        QCOMPARE(quit.count(), 1);
        QCOMPARE(window.findChildren<QPushButton *>().size(), 2);
        QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("privateCloudSignOut")));
        QVERIFY(window.findChildren<QTabWidget *>().isEmpty());
        QVERIFY(window.findChildren<QTreeView *>().isEmpty());
        QVERIFY(window.findChildren<QLineEdit *>().isEmpty());
    }

    void testErrorsStayPlainAndBounded()
    {
        PrivateCloudWindow window;
        const QString detail = QStringLiteral("<b>Server unavailable</b> ") + QString(1000, u'x') + QStringLiteral("\nsecond filename\nthird filename");
        window.setStatus(QStringLiteral("alice"), QStringLiteral("J:/"), PrivateCloudWindow::Status::NeedsAttention, detail, false);
        auto *label = window.findChild<QLabel *>(QStringLiteral("privateCloudDetail"));
        QCOMPARE(label->textFormat(), Qt::PlainText);
        QVERIFY(label->text().startsWith(QStringLiteral("<b>Server unavailable</b>")));
        QVERIFY(label->text().size() <= 320);
        QVERIFY(!label->text().contains(QStringLiteral("second filename")));
        QCOMPARE(window.statusSummary(), QStringLiteral("Needs attention"));
        QCOMPARE(window.findChild<QPlainTextEdit *>(QStringLiteral("privateCloudFullDetails"))->toPlainText(), detail);
        QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("privateCloudOpenDrive"))->isEnabled());
        window.setStatus(QStringLiteral("alice"), QStringLiteral("J:/"), PrivateCloudWindow::Status::Ready, {}, true);
        QVERIFY(!label->text().contains(QStringLiteral("Server unavailable")));
    }

    void testOperationWaitServicesQueuedCallbacks()
    {
        PrivateCloudAccountSwitch wait;
        bool pending = true;
        QSignalSpy ready(&wait, &PrivateCloudAccountSwitch::readyToRemove);
        wait.start([&pending] {
            return pending;
        });
        QTimer::singleShot(0, &wait, [&pending] {
            pending = false;
        });
        QTRY_COMPARE(ready.count(), 1);
        QVERIFY(!wait.isRunning());
    }

    void testAccountChangeWaitsForOperations()
    {
        PrivateCloudAccountSwitch change;
        bool pending = true;
        QSignalSpy ready(&change, &PrivateCloudAccountSwitch::readyToRemove);
        QSignalSpy timeout(&change, &PrivateCloudAccountSwitch::timedOut);
        change.start([&pending] {
            return pending;
        });
        QVERIFY(change.isRunning());
        QCOMPARE(ready.count(), 0);
        change.start([] {
            return false;
        });
        QCOMPARE(ready.count(), 0); // Repeated clicks cannot bypass the wait.
        pending = false;
        QTRY_COMPARE(ready.count(), 1);
        QVERIFY(!change.isRunning());
        QCOMPARE(timeout.count(), 0);
    }

    void testAccountChangeTimeoutKeepsConnection()
    {
        PrivateCloudAccountSwitch change;
        QSignalSpy ready(&change, &PrivateCloudAccountSwitch::readyToRemove);
        QSignalSpy timeout(&change, &PrivateCloudAccountSwitch::timedOut);
        change.start(
            [] {
                return true;
            },
            1);
        QTRY_COMPARE(timeout.count(), 1);
        QCOMPARE(ready.count(), 0);
        QVERIFY(!change.isRunning());
        change.start([] {
            return false;
        });
        QCOMPARE(ready.count(), 1);
    }

    void testAccountChangeTimeoutRestoresScheduling_data()
    {
        QTest::addColumn<bool>("previouslyEnabled");
        QTest::newRow("running-session") << true;
        QTest::newRow("already-suspended-session") << false;
    }

    void testAccountChangeTimeoutRestoresScheduling()
    {
        QFETCH(bool, previouslyEnabled);
        PrivateCloudAccountSwitch change;
        bool enabled = previouslyEnabled;
        QList<bool> schedulingChanges;
        const auto setEnabled = [&](bool value) {
            enabled = value;
            schedulingChanges.append(value);
        };
        change.suspendSyncScheduling(enabled, setEnabled);
        QVERIFY(!enabled);
        change.suspendSyncScheduling(enabled, setEnabled); // Repeated requests retain the original state.
        QCOMPARE(schedulingChanges.size(), 1);
        QSignalSpy timeout(&change, &PrivateCloudAccountSwitch::timedOut);
        change.start(
            [] {
                return true;
            },
            1);
        QTRY_COMPARE(timeout.count(), 1);
        QCOMPARE(enabled, previouslyEnabled);
        QCOMPARE(schedulingChanges, QList<bool>({false, previouslyEnabled}));
        change.restoreSyncScheduling();
        QCOMPARE(schedulingChanges.size(), 2); // Recovery happens only once.
    }

    void testRetainingAccountAfterDrainRestoresScheduling()
    {
        PrivateCloudAccountSwitch change;
        bool enabled = true;
        change.suspendSyncScheduling(enabled, [&](bool value) {
            enabled = value;
        });
        QSignalSpy ready(&change, &PrivateCloudAccountSwitch::readyToRemove);
        change.start([] {
            return false;
        });
        QCOMPARE(ready.count(), 1);
        QVERIFY(!enabled); // Keep scheduling stopped until account identity is checked.
        change.restoreSyncScheduling();
        QVERIFY(enabled);
    }

    void testCommittedTeardownDoesNotResumeOldSession()
    {
        PrivateCloudAccountSwitch change;
        bool enabled = true;
        change.suspendSyncScheduling(enabled, [&](bool value) {
            enabled = value;
        });
        connect(&change, &PrivateCloudAccountSwitch::readyToRemove, &change, &PrivateCloudAccountSwitch::discardSyncScheduling);
        change.start([] {
            return false;
        });
        change.cancel();
        QVERIFY(!enabled);
        enabled = true; // A new account starts a separate session.
        change.suspendSyncScheduling(enabled, [&](bool value) {
            enabled = value;
        });
        change.start([] {
            return true;
        });
        change.cancel();
        QVERIFY(enabled);
    }

    void testAccountChangeCanBeCancelled()
    {
        PrivateCloudAccountSwitch change;
        bool enabled = true;
        change.suspendSyncScheduling(enabled, [&](bool value) {
            enabled = value;
        });
        QSignalSpy ready(&change, &PrivateCloudAccountSwitch::readyToRemove);
        QSignalSpy timeout(&change, &PrivateCloudAccountSwitch::timedOut);
        change.start(
            [] {
                return true;
            },
            1);
        change.cancel();
        QVERIFY(!change.isRunning());
        QVERIFY(enabled);
        QTest::qWait(150);
        QCOMPARE(ready.count(), 0);
        QCOMPARE(timeout.count(), 0);
    }

    void testDisconnectingDisablesConflictingActions()
    {
        PrivateCloudWindow window;
        window.setStatus(QStringLiteral("alice"), QStringLiteral("X:/"), PrivateCloudWindow::Status::Connecting, {}, true);
        window.setBusy(true);
        for (auto *button : window.findChildren<QPushButton *>()) {
            QVERIFY(!button->isEnabled());
        }
        QVERIFY(!window.findChild<QAction *>(QStringLiteral("privateCloudSignOut"))->isEnabled());
        QVERIFY(!window.findChild<QAction *>(QStringLiteral("privateCloudChangeAccount"))->isEnabled());
        QVERIFY(window.findChild<QAction *>(QStringLiteral("privateCloudQuit"))->isEnabled());
        window.setStatus(QStringLiteral("alice"), QStringLiteral("X:/"), PrivateCloudWindow::Status::NeedsAttention, {}, false);
        QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("privateCloudRetry"))->isEnabled());
        window.setBusy(false);
        QVERIFY(window.findChild<QPushButton *>(QStringLiteral("privateCloudRetry"))->isEnabled());
        QVERIFY(window.findChild<QAction *>(QStringLiteral("privateCloudChangeAccount"))->isEnabled());
    }

    void testBusyPreservesDetailsAndRestoresStateActions()
    {
        PrivateCloudWindow window;
        window.setStatus(QStringLiteral("alice"),
                         QStringLiteral("J:/"),
                         PrivateCloudWindow::Status::NeedsAttention,
                         QStringLiteral("Finishing a file operation"),
                         true);
        window.setBusy(true);
        QVERIFY(window.findChild<QToolButton *>(QStringLiteral("privateCloudDetailsToggle"))->isEnabled());
        QVERIFY(window.findChild<QToolButton *>(QStringLiteral("privateCloudCopyDetails"))->isEnabled());
        window.setStatus(QStringLiteral("alice"), QStringLiteral("J:/"), PrivateCloudWindow::Status::Ready, {}, true);
        QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("privateCloudOpenDrive"))->isEnabled());
        QVERIFY(!window.findChild<QAction *>(QStringLiteral("privateCloudSignOut"))->isEnabled());
        window.setBusy(false);
        QVERIFY(window.findChild<QPushButton *>(QStringLiteral("privateCloudOpenDrive"))->isEnabled());
        QVERIFY(window.findChild<QAction *>(QStringLiteral("privateCloudSignOut"))->isEnabled());
        QVERIFY(window.findChild<QPushButton *>(QStringLiteral("privateCloudRetry"))->isHidden());
    }

    void testConflictsCannotReviewAnUnavailableDrive()
    {
        PrivateCloudWindow window;
        window.setStatus(QStringLiteral("alice"), QStringLiteral("J:/"), PrivateCloudWindow::Status::Conflicts, {}, false);
        auto *review = window.findChild<QPushButton *>(QStringLiteral("privateCloudRetry"));
        QVERIFY(!review->isEnabled());
        QSignalSpy reviewed(&window, &PrivateCloudWindow::reviewFilesRequested);
        review->click();
        QCOMPARE(reviewed.count(), 0);
        window.setStatus(QStringLiteral("alice"), QStringLiteral("J:/"), PrivateCloudWindow::Status::Conflicts, {}, true);
        QVERIFY(review->isEnabled());
        review->click();
        QCOMPARE(reviewed.count(), 1);
    }

    void testChangingAccountRemovesStaleUserModel()
    {
        auto *users = UserModel::instance();
        QCOMPARE(users->count(), 0);
        QCOMPARE(AccountManager::instance()->accounts().size(), 0);
        const auto account = Account::create();
        const auto url = Account::enforcedServerUrl().isEmpty() ? QUrl(QStringLiteral("https://example.test")) : Account::enforcedServerUrl();
        QVERIFY(account->setUrl(url));
        account->setDavUser(QStringLiteral("ui-test-account"));
        auto *network = new FakeQNAM({});
        network->setOverride([network](QNetworkAccessManager::Operation operation, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            return new FakeErrorReply(operation, request, network, 404);
        });
        account->setCredentials(new FakeCredentials{network});
        auto *state = AccountManager::instance()->addAccount(account);
        QVERIFY(state);
        const auto row = users->findUserIdForAccount(state);
        QVERIFY(row >= 0);
        QPointer<User> oldUser = users->user(row);
        QVERIFY(oldUser);
        QSignalSpy removed(users, &QAbstractItemModel::rowsRemoved);
        users->removeAccountConnection(row);
        QCOMPARE(removed.count(), 1);
        QCOMPARE(users->count(), 0);
        QVERIFY(AccountManager::instance()->accounts().isEmpty());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(oldUser.isNull());
        users->removeAccountConnection(row);
        QCOMPARE(removed.count(), 1);
    }

    void testScreenshots()
    {
        const auto outputDirectory = qEnvironmentVariable("PRIVATECLOUD_UI_SCREENSHOT_DIR");
        if (outputDirectory.isEmpty()) {
            QSKIP("Set PRIVATECLOUD_UI_SCREENSHOT_DIR to capture the isolated ready, paused, signed-out and error states.");
        }
        QVERIFY(QDir().mkpath(outputDirectory));
        PrivateCloudWindow window;
        const auto capture = [&](const QString &fileName, PrivateCloudWindow::Status status, const QString &detail, bool available) {
            window.setStatus(QStringLiteral("Alice Example"), QStringLiteral("J:/"), status, detail, available);
            window.show();
            QTest::qWait(50);
            return window.grab().save(QDir(outputDirectory).filePath(fileName));
        };
        QVERIFY(capture(QStringLiteral("privateCloud-ready.png"), PrivateCloudWindow::Status::Ready, {}, true));
        QVERIFY(capture(QStringLiteral("privateCloud-paused.png"), PrivateCloudWindow::Status::Paused, {}, true));
        QVERIFY(capture(QStringLiteral("privateCloud-offline.png"), PrivateCloudWindow::Status::Offline, {}, true));
        QVERIFY(capture(QStringLiteral("privateCloud-authentication.png"), PrivateCloudWindow::Status::AuthenticationRequired, {}, false));
        QVERIFY(capture(QStringLiteral("privateCloud-checking.png"), PrivateCloudWindow::Status::Checking, {}, true));
        QVERIFY(capture(QStringLiteral("privateCloud-conflicts.png"), PrivateCloudWindow::Status::Conflicts, {}, true));
        QVERIFY(capture(QStringLiteral("privateCloud-signed-out.png"), PrivateCloudWindow::Status::SignedOut, {}, false));
        QVERIFY(capture(QStringLiteral("privateCloud-error.png"),
                        PrivateCloudWindow::Status::NeedsAttention,
                        QStringLiteral("Cannot connect to privateCloud. Check your internet connection, then select Retry."),
                        false));
        window.findChild<QToolButton *>(QStringLiteral("privateCloudDetailsToggle"))->click();
        QTest::qWait(50);
        QVERIFY(window.grab().save(QDir(outputDirectory).filePath(QStringLiteral("privateCloud-expanded-details.png"))));
    }
};

QTEST_MAIN(TestPrivateCloudWindow)
#include "testprivatecloudwindow.moc"
