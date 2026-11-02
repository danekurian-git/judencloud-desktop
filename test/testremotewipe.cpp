/*
 * SPDX-FileCopyrightText: 2019 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: CC0-1.0
 *
 * This software is in the public domain, furnished "as is", without technical
 * support, and with no warranty, express or implied, as to its usefulness for
 * any purpose.
 */

#include <qglobal.h>
#include <QtTest>

#include "remotewipe.h"
#include "accountmanager.h"

#include "folderman.h"
#include "account.h"
#include "accountstate.h"
#include "configfile.h"
#include "logger.h"

#include "testhelper.h"

#include "syncenginetestutils.h"

using namespace OCC;

class TestRemoteWipe : public QObject
{
    Q_OBJECT

    QTemporaryDir _configDir;

    AccountState *addTestAccount(const QString &user = QStringLiteral("remote-wipe-test-user"))
    {
        const auto account = Account::create();
        account->setUrl(Account::enforcedServerUrl().isEmpty() ? QUrl(QStringLiteral("https://cloud.example")) : Account::enforcedServerUrl());
        auto *credentials = new FakeCredentials(new FakeQNAM({}));
        credentials->setUserName(user);
        account->setCredentials(credentials);
        account->setDavUser(user);
        return AccountManager::instance()->addAccount(account);
    }

    std::unique_ptr<RemoteWipe> createCheck(const AccountPtr &account)
    {
        auto wipe = std::make_unique<RemoteWipe>(account);
        QObject::disconnect(wipe.get(), &RemoteWipe::authorized, nullptr, nullptr);
        wipe->_canWipeLocalFiles = true;
        wipe->_networkManager->deleteLater();
        wipe->_networkManager = new FakeQNAM({});
        wipe->_networkManager->setParent(wipe.get());
        return wipe;
    }

private Q_SLOTS:
    void initTestCase()
    {
        OCC::Logger::instance()->setLogFlush(true);
        OCC::Logger::instance()->setLogDebug(true);

        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(_configDir.isValid());
        ConfigFile::setConfDir(_configDir.path());
    }

    void cleanup()
    {
        const auto accounts = AccountManager::instance()->accounts();
        for (const auto &state : accounts) {
            AccountManager::instance()->removeAccountState(state.data());
        }
    }

    void testStrictAuthorization_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<int>("error");
        QTest::addColumn<bool>("authorized");
        QTest::newRow("true") << 200 << QByteArrayLiteral("{\"wipe\":true}") << 0 << true;
        QTest::newRow("false") << 200 << QByteArrayLiteral("{\"wipe\":false}") << 0 << false;
        QTest::newRow("missing") << 200 << QByteArrayLiteral("{}") << 0 << false;
        QTest::newRow("string") << 200 << QByteArrayLiteral("{\"wipe\":\"true\"}") << 0 << false;
        QTest::newRow("number") << 200 << QByteArrayLiteral("{\"wipe\":1}") << 0 << false;
        QTest::newRow("array") << 200 << QByteArrayLiteral("[{\"wipe\":true}]") << 0 << false;
        QTest::newRow("malformed") << 200 << QByteArrayLiteral("{\"wipe\":true") << 0 << false;
        QTest::newRow("created-is-not-authorization") << 201 << QByteArrayLiteral("{\"wipe\":true}") << 0 << false;
        QTest::newRow("redirect") << 302 << QByteArrayLiteral("{\"wipe\":true}") << 0 << false;
        QTest::newRow("revoked") << 401 << QByteArrayLiteral("{\"wipe\":true}") << 0 << false;
        QTest::newRow("forbidden") << 403 << QByteArrayLiteral("{\"wipe\":true}") << 0 << false;
        QTest::newRow("not-found") << 404 << QByteArrayLiteral("{\"wipe\":true}") << 0 << false;
        QTest::newRow("server-error") << 500 << QByteArrayLiteral("{\"wipe\":true}") << 0 << false;
        QTest::newRow("tls-error") << 200 << QByteArrayLiteral("{\"wipe\":true}") << int(QNetworkReply::SslHandshakeFailedError) << false;
    }

    void testStrictAuthorization()
    {
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        QFETCH(int, error);
        QFETCH(bool, authorized);
        auto *state = addTestAccount();
        auto wipe = createCheck(state->account());
        auto requests = 0;
        auto sentBody = QByteArray{};
        auto redirectPolicy = QVariant{};
        static_cast<FakeQNAM *>(wipe->_networkManager)->setOverride([&](auto op, const QNetworkRequest &request, QIODevice *device) -> QNetworkReply * {
            ++requests;
            sentBody = device->readAll();
            redirectPolicy = request.attribute(QNetworkRequest::RedirectPolicyAttribute);
            auto *reply = new FakeErrorReply(op, request, wipe.get(), status, body);
            reply->setError(QNetworkReply::NetworkError(error), QString{});
            return reply;
        });
        auto authorizedSpy = QSignalSpy(wipe.get(), &RemoteWipe::authorized);
        wipe->startCheckJobWithAppPassword(QStringLiteral("test&token=+%"));
        wipe->startCheckJobWithAppPassword(QStringLiteral("duplicate"));
        QTRY_VERIFY(!wipe->_networkReplyCheck);
        QCOMPARE(requests, 1);
        QCOMPARE(sentBody, QByteArrayLiteral("token=test%26token%3D%2B%25"));
        QCOMPARE(redirectPolicy.toInt(), int(QNetworkRequest::ManualRedirectPolicy));
        QCOMPARE(authorizedSpy.count(), authorized ? 1 : 0);
        if (authorized) {
            QCOMPARE(qvariant_cast<AccountState *>(authorizedSpy.at(0).at(0)), state);
            wipe->startCheckJobWithAppPassword(QStringLiteral("duplicate-after-authorization"));
            QCOMPARE(requests, 1);
        }
    }

    void testAuthorizationUsesAccountIdentity()
    {
        auto *first = addTestAccount();
        auto *second = addTestAccount();
        QCOMPARE(first->account()->displayName(), second->account()->displayName());
        auto wipe = createCheck(second->account());
        static_cast<FakeQNAM *>(wipe->_networkManager)->setOverride([&](auto op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            return new FakeJsonReply(op, request, wipe.get(), 200, QJsonDocument::fromJson(R"({"wipe":true})"));
        });
        auto authorizedSpy = QSignalSpy(wipe.get(), &RemoteWipe::authorized);
        wipe->startCheckJobWithAppPassword(QStringLiteral("synthetic-token"));
        QTRY_COMPARE(authorizedSpy.count(), 1);
        QCOMPARE(qvariant_cast<AccountState *>(authorizedSpy.at(0).at(0)), second);
    }

    void testRemovedOrChangedAccountCannotWipe_data()
    {
        QTest::addColumn<bool>("removeAccount");
        QTest::newRow("removed") << true;
        QTest::newRow("changed-user") << false;
    }

    void testRemovedOrChangedAccountCannotWipe()
    {
        QFETCH(bool, removeAccount);
        auto *state = addTestAccount();
        const auto account = state->account();
        auto wipe = createCheck(account);
        auto replyGuard = QPointer<QNetworkReply>{};
        static_cast<FakeQNAM *>(wipe->_networkManager)->setOverride([&](auto op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            auto *reply = new DelayedReply<FakeJsonReply>(30, op, request, wipe.get(), 200, QJsonDocument::fromJson(R"({"wipe":true})"));
            replyGuard = reply;
            return reply;
        });
        auto authorizedSpy = QSignalSpy(wipe.get(), &RemoteWipe::authorized);
        wipe->startCheckJobWithAppPassword(QStringLiteral("synthetic-token"));
        if (removeAccount) {
            AccountManager::instance()->removeAccountState(state);
        } else {
            account->setDavUser(QStringLiteral("changed-user"));
        }
        QTRY_VERIFY(!replyGuard);
        QCOMPARE(authorizedSpy.count(), 0);
    }

    void testCheckDeadlineAllowsRetry()
    {
        auto *state = addTestAccount();
        auto wipe = createCheck(state->account());
        wipe->_checkDeadline.setInterval(10);
        auto requests = 0;
        static_cast<FakeQNAM *>(wipe->_networkManager)->setOverride([&](auto op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            ++requests;
            return new FakeHangingReply(op, request, wipe.get());
        });
        auto authorizedSpy = QSignalSpy(wipe.get(), &RemoteWipe::authorized);
        wipe->startCheckJobWithAppPassword(QStringLiteral("synthetic-token"));
        QTRY_VERIFY(!wipe->_networkReplyCheck);
        wipe->startCheckJobWithAppPassword(QStringLiteral("synthetic-token"));
        QCOMPARE(requests, 2);
        QTRY_VERIFY(!wipe->_networkReplyCheck);
        QCOMPARE(authorizedSpy.count(), 0);
    }

    void testFailedWipeAndShutdownNeverAcknowledge()
    {
        auto *state = addTestAccount();
        auto wipe = createCheck(state->account());
        auto requests = 0;
        static_cast<FakeQNAM *>(wipe->_networkManager)->setOverride([&](auto op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            ++requests;
            return new FakeJsonReply(op, request, wipe.get(), 200, QJsonDocument::fromJson(R"({"wipe":true})"));
        });
        auto authorizedSpy = QSignalSpy(wipe.get(), &RemoteWipe::authorized);
        wipe->startCheckJobWithAppPassword(QStringLiteral("synthetic-token"));
        QTRY_COMPARE(authorizedSpy.count(), 1);
        wipe->slotWipeDone(state, false);
        wipe->notifyServerSuccess();
        QCOMPARE(requests, 1);
        AccountManager::instance()->shutdown();
        QCoreApplication::processEvents();
        QCOMPARE(requests, 1);
        QVERIFY(!wipe->_wipeCompleted);
    }

    void testSuccessfulWipeAcknowledgedOnce()
    {
        auto *state = addTestAccount();
        auto wipe = createCheck(state->account());
        auto checks = 0;
        auto acknowledgments = 0;
        static_cast<FakeQNAM *>(wipe->_networkManager)->setOverride([&](auto op, const QNetworkRequest &request, QIODevice *body) -> QNetworkReply * {
            if (request.url().path().endsWith(QStringLiteral("/check"))) {
                ++checks;
                return new FakeJsonReply(op, request, wipe.get(), 200, QJsonDocument::fromJson(R"({"wipe":true})"));
            }
            ++acknowledgments;
            if (body->readAll() != QByteArrayLiteral("token=synthetic-token")) {
                return new FakeErrorReply(op, request, wipe.get(), 400);
            }
            return new FakeJsonReply(op, request, wipe.get(), 200, QJsonDocument(QJsonObject{}));
        });
        auto authorizedSpy = QSignalSpy(wipe.get(), &RemoteWipe::authorized);
        wipe->startCheckJobWithAppPassword(QStringLiteral("synthetic-token"));
        QTRY_COMPARE(authorizedSpy.count(), 1);
        wipe->slotWipeDone(state, true);
        QTRY_VERIFY(wipe->_acknowledged);
        wipe->notifyServerSuccess();
        QCOMPARE(checks, 1);
        QCOMPARE(acknowledgments, 1);
        QVERIFY(AccountManager::instance()->accounts().isEmpty());
        QVERIFY(wipe->_appPassword.isEmpty());
    }

    void testAcknowledgmentFailureCanRetry_data()
    {
        QTest::addColumn<bool>("timeout");
        QTest::newRow("http-failure") << false;
        QTest::newRow("deadline") << true;
    }

    void testAcknowledgmentFailureCanRetry()
    {
        QFETCH(bool, timeout);
        auto *state = addTestAccount();
        auto wipe = createCheck(state->account());
        wipe->_successDeadline.setInterval(10);
        auto acknowledgments = 0;
        static_cast<FakeQNAM *>(wipe->_networkManager)->setOverride([&](auto op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            if (request.url().path().endsWith(QStringLiteral("/check"))) {
                return new FakeJsonReply(op, request, wipe.get(), 200, QJsonDocument::fromJson(R"({"wipe":true})"));
            }
            ++acknowledgments;
            if (acknowledgments == 1) {
                if (timeout) {
                    return new FakeHangingReply(op, request, wipe.get());
                }
                return new FakeErrorReply(op, request, wipe.get(), 503);
            }
            return new FakeJsonReply(op, request, wipe.get(), 200, QJsonDocument(QJsonObject{}));
        });
        auto authorizedSpy = QSignalSpy(wipe.get(), &RemoteWipe::authorized);
        wipe->startCheckJobWithAppPassword(QStringLiteral("synthetic-token"));
        QTRY_COMPARE(authorizedSpy.count(), 1);
        wipe->slotWipeDone(state, true);
        QTRY_VERIFY(!wipe->_networkReplySuccess);
        QVERIFY(!wipe->_acknowledged);
        QVERIFY(!wipe->_appPassword.isEmpty());
        wipe->notifyServerSuccess();
        QTRY_VERIFY(wipe->_acknowledged);
        QCOMPARE(acknowledgments, 2);
    }

    void testDestroyDuringCheck()
    {
        auto *state = addTestAccount();
        auto wipe = createCheck(state->account());
        auto replyGuard = QPointer<QNetworkReply>{};
        static_cast<FakeQNAM *>(wipe->_networkManager)->setOverride([&](auto op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            auto *reply = new FakeHangingReply(op, request, wipe->_networkManager);
            replyGuard = reply;
            return reply;
        });
        wipe->startCheckJobWithAppPassword(QStringLiteral("synthetic-token"));
        QVERIFY(replyGuard);
        wipe.reset();
        QVERIFY(!replyGuard);
        QCOMPARE(AccountManager::instance()->accounts().size(), 1);
    }

    void testRemoteWipe()
    {
        // RemoteWipe needs FolderMan for actually wiping local data
        auto folderMan = FolderMan::instance();
        QVERIFY(folderMan);

        // RemoteWipe also needs an account present in the AccountManager
        FakeFolder fakeFolder{FileInfo::A12_B12_C12_S12()};
        auto account = fakeFolder.account();
        auto accountState = AccountManager::instance()->addAccount(account);

        // retrieve the RemoteWipe instance created from the real AccountState,
        // and replace its QNetworkAccessManager with our own one for testing
        auto remoteWipe = FakeAccountState::remoteWipe(accountState);
        auto fakeQnam = new FakeQNAM({});
        remoteWipe->_networkManager->deleteLater();
        remoteWipe->_networkManager = fakeQnam;

        // let FolderMan know about our sync folder
        FolderMan::instance()->addFolder(accountState, folderDefinition(fakeFolder.localPath()));

        bool revokeAppPassword = false; // whether respond with 401 to requests
        bool doWipe = false; // whether a remote wipe should be done

        const auto fakeQnamOverride = [&](const QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *device) -> QNetworkReply * {
            Q_UNUSED(device)
            if (!revokeAppPassword) {
                qDebug() << "App password not revoked";
                return nullptr;
            }

            const auto requestUrl = request.url();
            const auto requestPath = requestUrl.path();

            if (op == QNetworkAccessManager::Operation::DeleteOperation && requestPath.endsWith("/ocs/v2.php/core/apppassword")) {
                qDebug() << "Responding success for app password deletion";
                // allow deletion of appPassword to succeed
                return new FakeJsonReply(op, request, this, 200);
            }

            if (doWipe) {
                qDebug() << "Wipe enabled";
                if (requestPath.endsWith("/index.php/core/wipe/check")) {
                    qDebug() << "Responding with wipe=true";
                    return new FakeJsonReply(op, request, this, 200, QJsonDocument::fromJson(R"({"wipe": true})"));
                } else if (requestPath.endsWith("/index.php/core/wipe/success")) {
                    qDebug() << "Responding with successful wipe";
                    return new FakeJsonReply(op, request, this, 200, QJsonDocument::fromJson(R"({})"));
                }
            }

            qDebug() << "Responding with unauthorised";
            auto errorReply = new FakeErrorReply(op, request, this, 401);
            errorReply->setError(QNetworkReply::AuthenticationRequiredError, QLatin1String("Unauthorised"));

            return errorReply;
        };
        fakeFolder.setServerOverride(fakeQnamOverride);
        fakeQnam->setOverride(fakeQnamOverride);

        const auto localFolderExists = [&fakeFolder]() -> bool {
            return QDir(fakeFolder.localPath()).exists();
        };

        // initial sync to ensure we've had a working connection
        qDebug() << "Test: Initial sync works";
        QVERIFY(fakeFolder.syncOnce());

        // just revoking the app password -> no remote wipe should be done
        qDebug() << "Test: App password revoked, no remote wipe triggered";
        revokeAppPassword = true;
        QVERIFY(!fakeFolder.syncOnce());
        // `Account` will try to retrieve the password from the keychain,
        // however during testing the password received from it will be empty.
        // An empty password will not perform the wipe check at all.
        // Therefore: call the slot which `Account` connects its
        // `appPasswordRetrieved` signal directly on the remoteWipe instance
        remoteWipe->startCheckJobWithAppPassword("password");
        QTest::qWait(500); // wait a bit to process events
        // ensure the account was not removed and the sync folder is still present
        QCOMPARE(AccountManager::instance()->accounts().size(), 1);
        QVERIFY2(localFolderExists(), "Local sync folder should exist as no wipe was requested");

        // hack for test: close the journal db of FakeFolder, as FolderMan::addFolder creates its own
        // as long as the test DB is open, removing files will break on e.g. Windows
        fakeFolder.syncJournal().close();

        // server tells us to wipe the local data
        qDebug() << "Test: Server tells us to remote wipe";
        doWipe = true;
        // ensure folder exists before performing the wipe
        QVERIFY2(localFolderExists(), "Local sync folder should exist before wiping");
        remoteWipe->startCheckJobWithAppPassword("password");
        QTest::qWait(500); // wait a bit to process events
        // account should now be gone
        QCOMPARE(AccountManager::instance()->accounts().size(), 0);
        // local folder should now be gone
        QVERIFY2(!localFolderExists(), "Local sync folder should be removed after wiping");
    }
};

QTEST_GUILESS_MAIN(TestRemoteWipe)
#include "testremotewipe.moc"
