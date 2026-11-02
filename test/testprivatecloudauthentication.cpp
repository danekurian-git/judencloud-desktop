/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "account.h"
#include "creds/privatecloudauthentication.h"
#include "creds/webflowcredentials.h"
#include "httplogger.h"
#include "syncenginetestutils.h"

#include <QJsonObject>
#include <QLoggingCategory>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <QUrlQuery>

using namespace OCC;

namespace OCC
{
class PrivateCloudAuthenticationTestAccess
{
public:
    static void setCredentialsFactory(PrivateCloudAuthentication &authentication,
                                      std::function<AbstractCredentials *(const QString &, const QString &)> factory)
    {
        authentication._credentialsFactory = std::move(factory);
    }
    static void setTimeout(PrivateCloudAuthentication &authentication, int milliseconds)
    {
        authentication._deadline.setInterval(milliseconds);
    }
};
}

class PrivateCloudTestCredentials : public WebFlowCredentials
{
public:
    PrivateCloudTestCredentials(const QString &user, const QString &password, std::function<QNetworkAccessManager *()> factory, int *persistCount)
        : WebFlowCredentials(user, password)
        , _factory(std::move(factory))
        , _persistCount(persistCount)
    {
    }
    QNetworkAccessManager *createQNAM() const override
    {
        return _factory();
    }
    void persist() override
    {
        ++*_persistCount;
    }

private:
    std::function<QNetworkAccessManager *()> _factory;
    int *_persistCount;
};

class TestPrivateCloudAuthentication : public QObject
{
    Q_OBJECT
    QStringList _paths;
    QStringList _davPasswords;
    QString _responseUser;
    int _identityStatus = 200;
    int _tokenStatus = 200;
    int _davStatus = 207;
    int _persistCount = 0;
    bool _hang = false;
    bool _malformedToken = false;
    bool _ocsHeadersPresent = true;

    AccountPtr candidate()
    {
        const auto account = Account::create();
        account->setUrl(Account::enforcedServerUrl().isEmpty() ? QUrl(QStringLiteral("https://cloud.example")) : Account::enforcedServerUrl());
        return account;
    }

    void configure(PrivateCloudAuthentication &authentication, const AccountPtr &account)
    {
        PrivateCloudAuthenticationTestAccess::setCredentialsFactory(authentication, [this, account](const QString &user, const QString &password) {
            return new PrivateCloudTestCredentials(
                user,
                password,
                [this, account = account.data()] {
                    auto *manager = new FakeQNAM({});
                    manager->setOverride(
                        [this, account, manager](QNetworkAccessManager::Operation operation, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
                            const auto path = request.url().path();
                            _paths.append(path);
                            if (_hang) {
                                return new FakeHangingReply(operation, request, manager);
                            }
                            if (path.endsWith(QStringLiteral("/status.php"))) {
                                return new FakeJsonReply(
                                    operation,
                                    request,
                                    manager,
                                    200,
                                    QJsonDocument(QJsonObject{{QStringLiteral("installed"), true}, {QStringLiteral("version"), QStringLiteral("35.0.0")}}));
                            }
                            if (path.contains(QStringLiteral("/ocs/"))) {
                                _ocsHeadersPresent = _ocsHeadersPresent && request.rawHeader("OCS-APIREQUEST") == "true"
                                    && QUrlQuery(request.url()).queryItemValue(QStringLiteral("format")) == QStringLiteral("json");
                                const auto isIdentity = path.endsWith(QStringLiteral("/cloud/user"));
                                const auto status = isIdentity ? _identityStatus : _tokenStatus;
                                if (status != 200) {
                                    return new FakeErrorReply(operation, request, manager, status);
                                }
                                const auto data = isIdentity
                                    ? QJsonObject{{QStringLiteral("id"), _responseUser}, {QStringLiteral("display-name"), QStringLiteral("Test User")}}
                                    : QJsonObject{{QStringLiteral("apppassword"), _malformedToken ? QString{} : QStringLiteral("generated-token")}};
                                return new FakeJsonReply(
                                    operation,
                                    request,
                                    manager,
                                    200,
                                    QJsonDocument(
                                        QJsonObject{{QStringLiteral("ocs"),
                                                     QJsonObject{{QStringLiteral("meta"), QJsonObject{{QStringLiteral("statuscode"), isIdentity ? 100 : 200}}},
                                                                 {QStringLiteral("data"), data}}}}));
                            }
                            _davPasswords.append(account->credentials()->password());
                            if (_davStatus != 207) {
                                return new FakeErrorReply(operation, request, manager, _davStatus);
                            }
                            return new FakePropfindReply(
                                QByteArrayLiteral("<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\"><d:response>"
                                                  "<d:href>/</d:href><d:propstat><d:prop><d:getlastmodified>Wed, 16 Sep 2026 12:00:00 GMT</d:getlastmodified>"
                                                  "</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response></d:multistatus>"),
                                operation,
                                request,
                                manager);
                        });
                    return manager;
                },
                &_persistCount);
        });
    }

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }
    void init()
    {
        _paths.clear();
        _davPasswords.clear();
        _responseUser = QStringLiteral("canonical-user");
        _identityStatus = 200;
        _tokenStatus = 200;
        _davStatus = 207;
        _persistCount = 0;
        _hang = false;
        _malformedToken = false;
        _ocsHeadersPresent = true;
    }

    void authenticatesExplicitUser_data()
    {
        QTest::addColumn<int>("tokenStatus");
        QTest::addColumn<QString>("expectedPassword");
        QTest::newRow("account-password") << 200 << QStringLiteral("generated-token");
        QTest::newRow("existing-app-password") << 403 << QStringLiteral("entered-password");
    }
    void authenticatesExplicitUser()
    {
        QFETCH(int, tokenStatus);
        QFETCH(QString, expectedPassword);
        _tokenStatus = tokenStatus;
        const auto account = candidate();
        PrivateCloudAuthentication authentication(account);
        configure(authentication, account);
        QSignalSpy success(&authentication, &PrivateCloudAuthentication::succeeded);
        QSignalSpy failure(&authentication, &PrivateCloudAuthentication::failed);
        authentication.start(QStringLiteral("login@example.com"), QStringLiteral("entered-password"));
        QTRY_COMPARE(success.count(), 1);
        QCOMPARE(failure.count(), 0);
        QCOMPARE(account->davUser(), QStringLiteral("canonical-user"));
        QCOMPARE(account->credentials()->user(), QStringLiteral("login@example.com"));
        QCOMPARE(account->credentialSetting(QStringLiteral("user")).toString(), QStringLiteral("login@example.com"));
        QCOMPARE(_davPasswords, QStringList{expectedPassword});
        QCOMPARE(account->credentials()->password(), expectedPassword);
        QVERIFY(_ocsHeadersPresent);
        QCOMPARE(_paths.size(), 4);
        QVERIFY(_paths.last().contains(QStringLiteral("canonical-user")));
        QCOMPARE(_persistCount, 0);
    }

    void rejectsInvalidCredentialsAndDifferentAccount_data()
    {
        QTest::addColumn<int>("identityStatus");
        QTest::addColumn<QString>("expectedUser");
        QTest::newRow("wrong-password") << 401 << QString{};
        QTest::newRow("two-factor-required") << 403 << QString{};
        QTest::newRow("different-account") << 200 << QStringLiteral("another-user");
    }
    void rejectsInvalidCredentialsAndDifferentAccount()
    {
        QFETCH(int, identityStatus);
        QFETCH(QString, expectedUser);
        _identityStatus = identityStatus;
        const auto account = candidate();
        PrivateCloudAuthentication authentication(account);
        configure(authentication, account);
        QSignalSpy success(&authentication, &PrivateCloudAuthentication::succeeded);
        QSignalSpy failure(&authentication, &PrivateCloudAuthentication::failed);
        authentication.start(QStringLiteral("user"), QStringLiteral("entered-password"), expectedUser);
        QTRY_COMPARE(failure.count(), 1);
        QCOMPARE(success.count(), 0);
        QCOMPARE(_paths.size(), 2);
        QVERIFY(!failure.first().first().toString().isEmpty());
        QCOMPARE(account->credentials()->authType(), QStringLiteral("dummy"));
        QCOMPARE(_persistCount, 0);
    }

    void rejectsMalformedTokenAndDavFailure_data()
    {
        QTest::addColumn<bool>("malformedToken");
        QTest::addColumn<int>("davStatus");
        QTest::newRow("empty-token") << true << 207;
        QTest::newRow("dav-forbidden") << false << 403;
        QTest::newRow("dav-not-found") << false << 404;
    }
    void rejectsMalformedTokenAndDavFailure()
    {
        QFETCH(bool, malformedToken);
        QFETCH(int, davStatus);
        _malformedToken = malformedToken;
        _davStatus = davStatus;
        const auto account = candidate();
        PrivateCloudAuthentication authentication(account);
        configure(authentication, account);
        QSignalSpy success(&authentication, &PrivateCloudAuthentication::succeeded);
        QSignalSpy failure(&authentication, &PrivateCloudAuthentication::failed);
        authentication.start(QStringLiteral("user"), QStringLiteral("entered-password"));
        QTRY_COMPARE(failure.count(), 1);
        QCOMPARE(success.count(), 0);
        QCOMPARE(_persistCount, 0);
    }

    void cancelsAndRetriesWithoutStaleCompletion()
    {
        const auto account = candidate();
        PrivateCloudAuthentication authentication(account);
        configure(authentication, account);
        QSignalSpy success(&authentication, &PrivateCloudAuthentication::succeeded);
        QSignalSpy failure(&authentication, &PrivateCloudAuthentication::failed);
        authentication.start(QStringLiteral("first"), QStringLiteral("first-password"));
        authentication.cancel();
        QCoreApplication::processEvents();
        QCOMPARE(success.count(), 0);
        QCOMPARE(failure.count(), 0);
        authentication.start(QStringLiteral("second"), QStringLiteral("second-password"));
        QTRY_COMPARE(success.count(), 1);
        QCOMPARE(account->credentials()->user(), QStringLiteral("second"));
        QCOMPARE(failure.count(), 0);
    }

    void retryInvalidatesQueuedFailure()
    {
        const auto account = candidate();
        PrivateCloudAuthentication authentication(account);
        configure(authentication, account);
        QSignalSpy success(&authentication, &PrivateCloudAuthentication::succeeded);
        QSignalSpy failure(&authentication, &PrivateCloudAuthentication::failed);
        authentication.start(QStringLiteral("first"), QString{});
        authentication.start(QStringLiteral("second"), QStringLiteral("entered-password"));
        QTRY_COMPARE(success.count(), 1);
        QCOMPARE(failure.count(), 0);
        QCOMPARE(account->credentials()->user(), QStringLiteral("second"));
    }

    void timeoutClearsCredentials()
    {
        _hang = true;
        const auto account = candidate();
        PrivateCloudAuthentication authentication(account);
        configure(authentication, account);
        PrivateCloudAuthenticationTestAccess::setTimeout(authentication, 10);
        QSignalSpy failure(&authentication, &PrivateCloudAuthentication::failed);
        authentication.start(QStringLiteral("user"), QStringLiteral("entered-password"));
        QTRY_COMPARE(failure.count(), 1);
        QVERIFY(failure.first().first().toString().contains(QStringLiteral("timed out")));
        QCOMPARE(account->credentials()->authType(), QStringLiteral("dummy"));
        QCOMPARE(_persistCount, 0);
    }

    void appPasswordEndpointIsExcludedFromHttpLogs()
    {
        static QStringList capturedMessages;
        capturedMessages.clear();
        const auto oldHandler = qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
            capturedMessages.append(message);
        });
        QLoggingCategory::setFilterRules(QStringLiteral("sync.httplogger.info=true"));
        const auto restoreLogging = qScopeGuard([oldHandler] {
            qInstallMessageHandler(oldHandler);
            QLoggingCategory::setFilterRules(QString{});
        });
        QNetworkRequest request(QUrl(QStringLiteral("https://cloud.example/ocs/v2.php/core/getapppassword?format=json")));
        auto *reply = new FakeJsonReply(QNetworkAccessManager::GetOperation,
                                        request,
                                        this,
                                        200,
                                        QJsonDocument(QJsonObject{{QStringLiteral("apppassword"), QStringLiteral("secret-token-should-never-log")}}));
        reply->setRawHeader("Content-Type", "application/json");
        HttpLogger::logRequest(reply, QNetworkAccessManager::GetOperation, nullptr);
        QTRY_VERIFY(reply->isFinished());
        QVERIFY(!capturedMessages.join(QLatin1Char('\n')).contains(QStringLiteral("secret-token-should-never-log")));
        delete reply;
    }

    void sensitiveHttpHeadersAreRedacted_data()
    {
        QTest::addColumn<QByteArray>("header");
        QTest::addColumn<bool>("responseHeader");
        QTest::newRow("authorization") << QByteArray("Authorization") << false;
        QTest::newRow("lowercase-authorization") << QByteArray("authorization") << false;
        QTest::newRow("mixedcase-authorization") << QByteArray("aUtHoRiZaTiOn") << false;
        QTest::newRow("proxy-authorization") << QByteArray("Proxy-Authorization") << false;
        QTest::newRow("mixedcase-proxy-authorization") << QByteArray("pRoXy-AuThOrIzAtIoN") << false;
        QTest::newRow("cookie") << QByteArray("Cookie") << false;
        QTest::newRow("lowercase-cookie") << QByteArray("cookie") << false;
        QTest::newRow("set-cookie") << QByteArray("Set-Cookie") << true;
        QTest::newRow("mixedcase-set-cookie") << QByteArray("sEt-CoOkIe") << true;
        QTest::newRow("response-authorization") << QByteArray("authorization") << true;
    }

    void sensitiveHttpHeadersAreRedacted()
    {
        QFETCH(QByteArray, header);
        QFETCH(bool, responseHeader);
        static QStringList capturedMessages;
        capturedMessages.clear();
        const auto oldHandler = qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
            capturedMessages.append(message);
        });
        QLoggingCategory::setFilterRules(QStringLiteral("sync.httplogger.info=true"));
        const auto restoreLogging = qScopeGuard([oldHandler] {
            qInstallMessageHandler(oldHandler);
            QLoggingCategory::setFilterRules(QString{});
        });
        const auto secret = QByteArray("sensitive-header-value-never-log");
        QNetworkRequest request(QUrl(QStringLiteral("https://cloud.example/status.php")));
        if (!responseHeader) {
            request.setRawHeader(header, "Basic " + secret);
        }
        auto *reply = new FakeJsonReply(QNetworkAccessManager::GetOperation, request, this, 200, QJsonDocument(QJsonObject{{"ok", true}}));
        if (responseHeader) {
            reply->setRawHeader(header, "Basic " + secret);
        }
        reply->setRawHeader("Content-Type", "application/json");
        HttpLogger::logRequest(reply, QNetworkAccessManager::GetOperation, nullptr);
        QTRY_VERIFY(reply->isFinished());
        const auto messages = capturedMessages.join(QLatin1Char('\n'));
        QVERIFY(messages.contains(QString::fromLatin1(header), Qt::CaseInsensitive));
        QVERIFY(!messages.contains(QString::fromLatin1(secret)));
        QVERIFY(messages.contains(QStringLiteral("[redacted]")));
        delete reply;
    }

    void requiresUsernameAndPassword()
    {
        const auto account = candidate();
        PrivateCloudAuthentication authentication(account);
        configure(authentication, account);
        QSignalSpy failure(&authentication, &PrivateCloudAuthentication::failed);
        authentication.start(QStringLiteral("user"), QString{});
        QTRY_COMPARE(failure.count(), 1);
        QVERIFY(_paths.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestPrivateCloudAuthentication)
#include "testprivatecloudauthentication.moc"
