/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "accessmanager.h"
#include "account.h"
#include "creds/dummycredentials.h"
#include "creds/httpcredentialsgui.h"
#include "creds/privatecloudauthentication.h"
#include "creds/webflowcredentials.h"
#include "privatecloudconnecttestserver.h"
#include "privatecloudtransport.h"
#include "pushnotifications.h"

#include <QDir>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace OCC;

class TestPrivateCloudTransport : public QObject
{
    Q_OBJECT
    const QUrl _serverUrl{QStringLiteral("https://cloud.monocloud.online")};

    static QNetworkRequest requestWithTrust(const QUrl &url, const QSslCertificate &certificate)
    {
        auto request = QNetworkRequest(url);
        auto ssl = request.sslConfiguration();
        ssl.addCaCertificate(certificate);
        ssl.setPeerVerifyMode(QSslSocket::VerifyPeer);
        request.setSslConfiguration(ssl);
        return request;
    }

    static bool provision(PrivateCloudConnectTestServer &server)
    {
        return !server.certificate.isNull() && !server.key.isNull() && server.listen(QHostAddress::LocalHost)
            && PrivateCloudTransport::setEndpoint(server.serverPort(), QStringLiteral("privatecloud-session"), QStringLiteral("test-session-nonce"));
    }

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(!PrivateCloudTransport::isRequired());
        AccessManager generic;
        const auto ordinaryProxy = QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"), 12345);
        generic.setProxy(ordinaryProxy);
        PrivateCloudTransport::apply(&generic);
        QCOMPARE(generic.proxy(), ordinaryProxy);
        QVERIFY(PrivateCloudTransport::allows(QUrl(QStringLiteral("http://other.example"))));
        PrivateCloudTransport::enableRequired();
    }

    void init()
    {
        PrivateCloudTransport::clearEndpoint();
        if (Account::enforcedServerUrl() != _serverUrl) {
            QSKIP("Requires the compiled privateCloud fixed server policy.");
        }
    }

    void cleanup()
    {
        PrivateCloudTransport::clearEndpoint();
    }

    void unavailableFailsLocally()
    {
        AccessManager manager;
        manager.setProxy(QNetworkProxy::NoProxy);
        const auto reply = manager.get(QNetworkRequest(_serverUrl));
        QSignalSpy finished(reply, &QNetworkReply::finished);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(reply->error(), QNetworkReply::ProxyConnectionRefusedError);
        QCOMPARE(reply->url(), _serverUrl);
        QVERIFY(!PrivateCloudTransport::setEndpoint(0, QStringLiteral("user"), QStringLiteral("nonce")));
        QVERIFY(!PrivateCloudTransport::isReady());
    }

    void onlyFixedDestination_data()
    {
        QTest::addColumn<QUrl>("url");
        for (const auto text : {"http://cloud.monocloud.online/",
                                "https://cloud.monocloud.online:444/",
                                "https://other.example/",
                                "https://cloud.monocloud.online.other.example/",
                                "https://user:password@cloud.monocloud.online/",
                                "https://127.0.0.1/"}) {
            QTest::newRow(text) << QUrl(QString::fromLatin1(text));
        }
    }

    void onlyFixedDestination()
    {
        QFETCH(QUrl, url);
        AccessManager manager;
        const auto reply = manager.get(QNetworkRequest(url));
        QSignalSpy finished(reply, &QNetworkReply::finished);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(reply->error(), QNetworkReply::ContentAccessDenied);
        QVERIFY(!PrivateCloudTransport::allows(url));
    }

    void localResourcePolicy_data()
    {
        QTest::addColumn<QUrl>("url");
        QTest::addColumn<bool>("allowed");
        QTest::newRow("local-file") << QUrl::fromLocalFile(QDir::tempPath() + QStringLiteral("/privatecloud-resource.txt")) << true;
        QTest::newRow("embedded-resource") << QUrl(QStringLiteral("qrc:/theme/privatecloud.svg")) << true;
        QTest::newRow("inline-data") << QUrl(QStringLiteral("data:text/plain,privateCloud")) << true;
        QTest::newRow("remote-file-host") << QUrl(QStringLiteral("file://server.invalid/share/file")) << false;
        QTest::newRow("unc-path") << QUrl::fromLocalFile(QStringLiteral("//server.invalid/share/file")) << false;
        QTest::newRow("unc-empty-authority") << QUrl(QStringLiteral("file:////server.invalid/share/file")) << false;
        QTest::newRow("loopback-share") << QUrl(QStringLiteral("file://localhost/share/file")) << false;
        QTest::newRow("relative-file") << QUrl(QStringLiteral("file:relative-file")) << false;
        QTest::newRow("remote-https") << _serverUrl << false;
#ifdef Q_OS_WIN
        QTest::newRow("drive-relative") << QUrl(QStringLiteral("file:C:relative-file")) << false;
        QTest::newRow("device-path") << QUrl::fromLocalFile(QStringLiteral("//?/C:/privatecloud-resource.txt")) << false;
        QTest::newRow("encoded-device-path") << QUrl(QStringLiteral("file:////%3F/C:/privatecloud-resource.txt")) << false;
        QTest::newRow("device-unc-path") << QUrl::fromLocalFile(QStringLiteral("//?/UNC/server.invalid/share/file")) << false;
        QTest::newRow("encoded-backslash-unc") << QUrl(QStringLiteral("file:///%5C%5Cserver.invalid/share/file")) << false;
#endif
    }

    void localResourcePolicy()
    {
        QFETCH(QUrl, url);
        QFETCH(bool, allowed);
        // Pure URL checks must reject UNC paths without attempting SMB access.
        QCOMPARE(PrivateCloudTransport::allowsLocalResource(url), allowed);
    }

    void localResourcesLoadWithoutRelay()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile file(directory.filePath(QStringLiteral("resource.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        const auto contents = QByteArray("privateCloud-local-resource");
        QCOMPARE(file.write(contents), contents.size());
        file.close();
        QVERIFY(!PrivateCloudTransport::isReady());
        AccessManager manager;
        const auto fileReply = manager.get(QNetworkRequest(QUrl::fromLocalFile(file.fileName())));
        QTRY_VERIFY(fileReply->isFinished());
        QCOMPARE(fileReply->error(), QNetworkReply::NoError);
        QCOMPARE(fileReply->readAll(), contents);
        const auto dataReply = manager.get(QNetworkRequest(QUrl(QStringLiteral("data:text/plain,privateCloud-inline-resource"))));
        QTRY_VERIFY(dataReply->isFinished());
        QCOMPARE(dataReply->error(), QNetworkReply::NoError);
        QCOMPARE(dataReply->readAll(), QByteArray("privateCloud-inline-resource"));
        const auto relativeReply = manager.get(QNetworkRequest(QUrl(QStringLiteral("file:privatecloud-nonexistent-relative-resource"))));
        QTRY_VERIFY(relativeReply->isFinished());
        QCOMPARE(relativeReply->error(), QNetworkReply::ContentAccessDenied);
    }

    void credentialsAndResetKeepRoute()
    {
        PrivateCloudConnectTestServer server(QFINDTESTDATA("privatecloudtransportcert.pem"), QFINDTESTDATA("privatecloudtransportkey.pem"));
        QVERIFY(provision(server));
        const auto account = Account::create();
        QVERIFY(account->setUrl(_serverUrl));
        for (auto *credentials : QList<AbstractCredentials *>{new DummyCredentials,
                                                              new HttpCredentialsGui(QStringLiteral("alice"), QStringLiteral("password"), {}, {}),
                                                              new WebFlowCredentials(QStringLiteral("alice"), QStringLiteral("app-password"))}) {
            account->setCredentials(credentials);
            QCOMPARE(account->networkAccessManager()->proxy(), PrivateCloudTransport::proxy());
            account->setProxyHostName(QStringLiteral("ignored.example"));
            account->setProxyPort(8888);
            account->setProxyType(QNetworkProxy::Socks5Proxy);
            QCOMPARE(account->networkAccessManager()->proxy(), PrivateCloudTransport::proxy());
            account->resetNetworkAccessManager();
            QCOMPARE(account->networkAccessManager()->proxy(), PrivateCloudTransport::proxy());
            // Even a late system proxy setter cannot change the effective request route.
            account->networkAccessManager()->setProxy(QNetworkProxy::NoProxy);
            const auto reply = account->networkAccessManager()->get(requestWithTrust(_serverUrl, server.certificate));
            QSignalSpy finished(reply, &QNetworkReply::finished);
            QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
            QCOMPARE(reply->error(), QNetworkReply::NoError);
            QCOMPARE(reply->readAll(), QByteArray("OK"));
        }
        QCOMPARE(server.authenticatedConnections, 3);
        for (const auto &authority : server.authorities) {
            QCOMPARE(authority, QByteArray("CONNECT cloud.monocloud.online:443 HTTP/1.1"));
        }
        for (const auto &request : server.requests) {
            QVERIFY(!request.toLower().contains("proxy-authorization:"));
            QVERIFY(!request.contains("test-session-nonce"));
            QVERIFY(request.toLower().contains("host: cloud.monocloud.online"));
        }
    }

    void tlsVerificationRemainsEnabled()
    {
        PrivateCloudConnectTestServer server(QFINDTESTDATA("privatecloudtransportcert.pem"), QFINDTESTDATA("privatecloudtransportkey.pem"));
        QVERIFY(provision(server));
        AccessManager manager;
        const auto reply = manager.get(QNetworkRequest(_serverUrl));
        QSignalSpy sslErrors(reply, &QNetworkReply::sslErrors);
        QSignalSpy finished(reply, &QNetworkReply::finished);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QCOMPARE(server.authenticatedConnections, 1);
        QCOMPARE(reply->error(), QNetworkReply::SslHandshakeFailedError);
        QVERIFY(!sslErrors.isEmpty());
        QCOMPARE(reply->url(), _serverUrl);
        QVERIFY(server.requests.isEmpty());
    }

    void trustedCertificateWithWrongHostIsRejected()
    {
        // Reuse the upstream disposable certificate for "newstandard". Trusting
        // its issuer must not make it valid for the fixed privateCloud hostname.
        PrivateCloudConnectTestServer server(QFINDTESTDATA("e2etestsfakecert.pem"), QFINDTESTDATA("e2etestsfakecertprivatekey.pem"));
        QVERIFY(provision(server));
        AccessManager manager;
        const auto reply = manager.get(requestWithTrust(_serverUrl, server.certificate));
        QSignalSpy sslErrors(reply, &QNetworkReply::sslErrors);
        QSignalSpy finished(reply, &QNetworkReply::finished);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QCOMPARE(server.authenticatedConnections, 1);
        QCOMPARE(reply->error(), QNetworkReply::SslHandshakeFailedError);
        auto hostnameRejected = false;
        for (const auto &arguments : sslErrors) {
            for (const auto &error : arguments.first().value<QList<QSslError>>()) {
                hostnameRejected |= error.error() == QSslError::HostNameMismatch;
            }
        }
        QVERIFY(hostnameRejected);
        QVERIFY(server.requests.isEmpty());
    }

    void rejectedRelayCredentialsDoNotPromptOrExposeAccountPassword()
    {
        PrivateCloudConnectTestServer server(QFINDTESTDATA("privatecloudtransportcert.pem"), QFINDTESTDATA("privatecloudtransportkey.pem"));
        server.expectedProxyAuthorization = "Basic " + QByteArray("privatecloud-session:a-different-session").toBase64();
        QVERIFY(provision(server));
        const auto account = Account::create();
        QVERIFY(account->setUrl(_serverUrl));
        const auto user = QStringLiteral("account-user-sentinel");
        const auto password = QStringLiteral("account-password-sentinel");
        account->setCredentials(new WebFlowCredentials(user, password));
        QSignalSpy prompts(account.data(), &Account::proxyAuthenticationRequired);
        const auto reply = account->networkAccessManager()->get(QNetworkRequest(_serverUrl));
        QSignalSpy finished(reply, &QNetworkReply::finished);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QCOMPARE(reply->error(), QNetworkReply::ProxyAuthenticationRequiredError);
        QCOMPARE(prompts.size(), 0);
        QCOMPARE(server.authenticatedConnections, 0);
        QVERIFY(server.requests.isEmpty());
        QVERIFY(!server.connectHeaders.isEmpty());
        const QByteArray accountCredentials = user.toUtf8() + ':' + password.toUtf8();
        const auto accountAuthorization = accountCredentials.toBase64();
        auto relayCredentialsUsed = false;
        for (const auto &header : server.connectHeaders) {
            QVERIFY(!header.contains(user.toUtf8()));
            QVERIFY(!header.contains(password.toUtf8()));
            QVERIFY(!header.contains(accountAuthorization));
            relayCredentialsUsed |= header.contains(QByteArray("privatecloud-session:test-session-nonce").toBase64());
        }
        QVERIFY(relayCredentialsUsed);
        QCOMPARE(account->networkAccessManager()->proxy(), PrivateCloudTransport::proxy());
    }

    void endpointRemovalAbortsOutstandingRequest()
    {
        PrivateCloudConnectTestServer server(QFINDTESTDATA("privatecloudtransportcert.pem"), QFINDTESTDATA("privatecloudtransportkey.pem"));
        server.holdAfterConnect = true;
        QVERIFY(provision(server));
        AccessManager manager;
        const auto reply = manager.get(QNetworkRequest(_serverUrl));
        QSignalSpy finished(reply, &QNetworkReply::finished);
        QTRY_COMPARE_WITH_TIMEOUT(server.authenticatedConnections, 1, 10000);
        PrivateCloudTransport::clearEndpoint();
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(reply->error(), QNetworkReply::OperationCanceledError);
        const auto next = manager.get(QNetworkRequest(_serverUrl));
        QSignalSpy nextFinished(next, &QNetworkReply::finished);
        QTRY_COMPARE(nextFinished.size(), 1);
        QCOMPARE(next->error(), QNetworkReply::ProxyConnectionRefusedError);
        QCOMPARE(server.authenticatedConnections, 1);
    }

    void redirectsStayManual()
    {
        PrivateCloudConnectTestServer server(QFINDTESTDATA("privatecloudtransportcert.pem"), QFINDTESTDATA("privatecloudtransportkey.pem"));
        server.response = [](const QByteArray &) {
            return QByteArray("HTTP/1.1 302 Found\r\nLocation: https://other.example/\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        };
        QVERIFY(provision(server));
        AccessManager manager;
        auto request = requestWithTrust(_serverUrl, server.certificate);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        const auto reply = manager.get(request);
        QSignalSpy finished(reply, &QNetworkReply::finished);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 302);
        QCOMPARE(reply->url(), _serverUrl);
        QCOMPARE(server.authenticatedConnections, 1);
    }

    void endpointRotationUsesNewNonce()
    {
        PrivateCloudConnectTestServer server(QFINDTESTDATA("privatecloudtransportcert.pem"), QFINDTESTDATA("privatecloudtransportkey.pem"));
        QVERIFY(provision(server));
        AccessManager manager;
        const auto request = requestWithTrust(_serverUrl, server.certificate);
        const auto first = manager.get(request);
        QSignalSpy firstFinished(first, &QNetworkReply::finished);
        QTRY_COMPARE_WITH_TIMEOUT(firstFinished.size(), 1, 10000);
        QCOMPARE(first->error(), QNetworkReply::NoError);
        server.expectedProxyAuthorization = "Basic " + QByteArray("privatecloud-session:replacement-session-nonce").toBase64();
        QVERIFY(PrivateCloudTransport::setEndpoint(server.serverPort(), QStringLiteral("privatecloud-session"), QStringLiteral("replacement-session-nonce")));
        const auto second = manager.get(request);
        QSignalSpy secondFinished(second, &QNetworkReply::finished);
        QTRY_COMPARE_WITH_TIMEOUT(secondFinished.size(), 1, 10000);
        QCOMPARE(second->error(), QNetworkReply::NoError);
        QCOMPARE(server.authenticatedConnections, 2);
    }

    void completeAuthenticationKeepsRelayAfterTokenExchange()
    {
        PrivateCloudConnectTestServer server(QFINDTESTDATA("privatecloudtransportcert.pem"), QFINDTESTDATA("privatecloudtransportkey.pem"));
        server.response = [](const QByteArray &request) {
            auto body = QByteArray{};
            auto status = QByteArray("200 OK");
            auto type = QByteArray("application/json");
            if (request.startsWith("GET /status.php ")) {
                body = R"({"installed":true,"version":"34.0.0","maintenance":false})";
            } else if (request.startsWith("GET /ocs/v1.php/cloud/user?")) {
                body = R"({"ocs":{"meta":{"statuscode":200},"data":{"id":"alice"}}})";
            } else if (request.startsWith("GET /ocs/v2.php/core/getapppassword?")) {
                body = R"({"ocs":{"meta":{"statuscode":200},"data":{"apppassword":"generated-app-token"}}})";
            } else if (request.startsWith("PROPFIND ")) {
                status = "207 Multi-Status";
                type = "application/xml";
                body =
                    R"(<?xml version="1.0"?><d:multistatus xmlns:d="DAV:"><d:response><d:href>/</d:href><d:propstat><d:prop><d:getlastmodified>Wed, 16 Sep 2026 12:00:00 GMT</d:getlastmodified></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response></d:multistatus>)";
            } else {
                status = "404 Not Found";
            }
            return "HTTP/1.1 " + status + "\r\nContent-Type: " + type + "\r\nContent-Length: " + QByteArray::number(body.size())
                + "\r\nConnection: close\r\n\r\n" + body;
        };
        QVERIFY(provision(server));
        const auto candidate = Account::create();
        QVERIFY(candidate->setUrl(_serverUrl));
        candidate->setSslConfiguration(requestWithTrust(_serverUrl, server.certificate).sslConfiguration());
        PrivateCloudAuthentication authentication(candidate);
        QSignalSpy failed(&authentication, &PrivateCloudAuthentication::failed);
        QSignalSpy succeeded(&authentication, &PrivateCloudAuthentication::succeeded);
        authentication.start(QStringLiteral("alice"), QStringLiteral("entered-password"));
        QTRY_COMPARE_WITH_TIMEOUT(succeeded.size(), 1, 10000);
        QVERIFY(failed.isEmpty());
        QCOMPARE(candidate->credentials()->password(), QStringLiteral("generated-app-token"));
        QCOMPARE(server.requests.size(), 4);
        QCOMPARE(server.authenticatedConnections, 4);
        QCOMPARE(candidate->networkAccessManager()->proxy(), PrivateCloudTransport::proxy());
        const QByteArray authorizationHeader = "Authorization: Basic " + QByteArray("alice:generated-app-token").toBase64();
        QVERIFY(server.requests.last().contains(authorizationHeader));
        for (const auto &request : server.requests) {
            QVERIFY(!request.toLower().contains("proxy-authorization:"));
            QVERIFY(!request.contains("test-session-nonce"));
        }
    }

    void signInCandidateUsesRelay()
    {
        PrivateCloudConnectTestServer server(QFINDTESTDATA("privatecloudtransportcert.pem"), QFINDTESTDATA("privatecloudtransportkey.pem"));
        QVERIFY(provision(server));
        // A failed certificate must end authentication without contacting another route.
        const auto candidate = Account::create();
        QVERIFY(candidate->setUrl(_serverUrl));
        PrivateCloudAuthentication authentication(candidate);
        QSignalSpy failed(&authentication, &PrivateCloudAuthentication::failed);
        QSignalSpy succeeded(&authentication, &PrivateCloudAuthentication::succeeded);
        authentication.start(QStringLiteral("alice"), QStringLiteral("test-password"));
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 10000);
        QCOMPARE(server.authenticatedConnections, 1);
        QVERIFY(succeeded.isEmpty());
        QVERIFY(server.requests.isEmpty());
    }

    void websocketUsesRelayAndRejectsForeignHost()
    {
        PrivateCloudConnectTestServer server(QFINDTESTDATA("privatecloudtransportcert.pem"), QFINDTESTDATA("privatecloudtransportkey.pem"));
        QVERIFY(provision(server));
        const auto account = Account::create();
        QVERIFY(account->setUrl(_serverUrl));
        // Empty notification type avoids Account's automatic connection.
        auto endpoint = QUrl(QStringLiteral("wss://cloud.monocloud.online/push/ws"));
        account->setCapabilities(
            {{QStringLiteral("notify_push"), QVariantMap{{QStringLiteral("endpoints"), QVariantMap{{QStringLiteral("websocket"), endpoint}}}}}});
        PushNotifications notifications(account.data());
        QSignalSpy failed(&notifications, &PushNotifications::authenticationFailed);
        notifications.setup();
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 10000);
        QCOMPARE(server.authenticatedConnections, 1);
        QVERIFY(server.requests.isEmpty());
        QVERIFY(PrivateCloudTransport::allowsWebSocket(endpoint));
        endpoint.setHost(QStringLiteral("other.example"));
        QVERIFY(!PrivateCloudTransport::allowsWebSocket(endpoint));
        account->setCapabilities(
            {{QStringLiteral("notify_push"), QVariantMap{{QStringLiteral("endpoints"), QVariantMap{{QStringLiteral("websocket"), endpoint}}}}}});
        QSignalSpy lost(&notifications, &PushNotifications::connectionLost);
        notifications.setup();
        QTRY_COMPARE(lost.size(), 1);
        QCOMPARE(server.authenticatedConnections, 1);
    }
};

QTEST_GUILESS_MAIN(TestPrivateCloudTransport)
#include "testprivatecloudtransport.moc"
