/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "account.h"
#include "privatecloudconnectrelay.h"

#include <QPointer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QTest>

using namespace OCC;

namespace OCC
{
class PrivateCloudRelayTestAccess
{
public:
    static void connector(PrivateCloudConnectRelay &relay, std::function<bool(QTcpSocket *)> connector)
    {
        relay._connector = std::move(connector);
    }
};
}

class TestPrivateCloudConnectRelay : public QObject
{
    Q_OBJECT
private:
    QByteArray authority() const
    {
        return Account::enforcedServerUrl().host().toUtf8() + ":443";
    }

    QByteArray request(const QNetworkProxy &proxy) const
    {
        const QByteArray credentials = proxy.user().toUtf8() + ':' + proxy.password().toUtf8();
        return "CONNECT " + authority() + " HTTP/1.1\r\nHost: " + authority() + "\r\nProxy-Authorization: Basic " + credentials.toBase64() + "\r\n\r\n";
    }

private Q_SLOTS:
    void qtConnectHandshakeReachesAuthorizedConnector()
    {
        PrivateCloudConnectRelay relay;
        QVERIFY(relay.start(QHostAddress("203.0.113.1"), 1, QHostAddress::LocalHost));
        auto connects = 0;
        PrivateCloudRelayTestAccess::connector(relay, [&connects](QTcpSocket *) {
            ++connects;
            return false;
        });
        QNetworkAccessManager manager;
        manager.setProxy(relay.proxy());
        const auto reply = manager.get(QNetworkRequest(Account::enforcedServerUrl()));
        QTRY_VERIFY_WITH_TIMEOUT(reply->isFinished(), 15000);
        QCOMPARE(connects, 1);
        QVERIFY(reply->error() != QNetworkReply::NoError);
        reply->deleteLater();
    }

    void explicitPhysicalInterfaceHttpsProbe()
    {
        const auto parameters = qgetenv("PRIVATECLOUD_DIRECT_HTTPS_PROBE");
        if (parameters.isEmpty()) {
            QSKIP("Explicit physical-network probe was not requested.");
        }
        const auto endpoint = QJsonDocument::fromJson(parameters).object();
        PrivateCloudConnectRelay relay;
        QVERIFY(relay.start(QHostAddress(endpoint.value("server").toString()),
                            endpoint.value("interfaceIndex").toInt(),
                            QHostAddress(endpoint.value("source").toString())));
        QNetworkAccessManager manager;
        manager.setProxy(relay.proxy());
        auto url = Account::enforcedServerUrl();
        url.setPath(QStringLiteral("/status.php"));
        auto *reply = manager.get(QNetworkRequest(url));
        QTRY_VERIFY_WITH_TIMEOUT(reply->isFinished(), 15000);
        QVERIFY2(reply->error() == QNetworkReply::NoError, qPrintable(reply->errorString()));
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
        QVERIFY(!reply->sslConfiguration().peerCertificate().isNull());
        QVERIFY(QJsonDocument::fromJson(reply->readAll()).object().contains(QStringLiteral("installed")));
        reply->deleteLater();
    }

    void rejectsInvalidRequests_data()
    {
        QTest::addColumn<QByteArray>("kind");
        QTest::addColumn<int>("expected");
        QTest::newRow("no-authentication") << QByteArray("no-auth") << 407;
        QTest::newRow("wrong-secret") << QByteArray("wrong-auth") << 407;
        QTest::newRow("foreign-host") << QByteArray("host") << 403;
        QTest::newRow("other-port") << QByteArray("port") << 403;
        QTest::newRow("http-forwarding") << QByteArray("get") << 403;
        QTest::newRow("duplicate-credentials") << QByteArray("duplicate") << 400;
        QTest::newRow("duplicate-host") << QByteArray("duplicate-host") << 400;
        QTest::newRow("foreign-host-header") << QByteArray("host-header") << 400;
        QTest::newRow("body") << QByteArray("body") << 400;
        QTest::newRow("oversized-header") << QByteArray("oversized") << 431;
    }

    void rejectsInvalidRequests()
    {
        QFETCH(QByteArray, kind);
        QFETCH(int, expected);
        PrivateCloudConnectRelay relay;
        QVERIFY(relay.start(QHostAddress("203.0.113.1"), 1, QHostAddress::LocalHost));
        int connects = 0;
        PrivateCloudRelayTestAccess::connector(relay, [&connects](QTcpSocket *) {
            ++connects;
            return false;
        });
        const auto proxy = relay.proxy();
        auto wire = request(proxy);
        if (kind == "no-auth") {
            wire = "CONNECT " + authority() + " HTTP/1.1\r\n\r\n";
        } else if (kind == "wrong-auth") {
            auto wrong = proxy;
            wrong.setPassword(QStringLiteral("not-the-secret"));
            wire = request(wrong);
        } else if (kind == "host") {
            wire.replace(authority(), "elsewhere.example:443");
        } else if (kind == "port") {
            wire.replace(":443", ":80");
        } else if (kind == "get") {
            wire.replace("CONNECT ", "GET ");
        } else if (kind == "duplicate") {
            wire.replace("\r\n\r\n", "\r\nProxy-Authorization: Basic ignored\r\n\r\n");
        } else if (kind == "duplicate-host") {
            const QByteArray extraHost = "\r\nHost: " + authority() + "\r\n\r\n";
            wire.replace("\r\n\r\n", extraHost);
        } else if (kind == "host-header") {
            const QByteArray originalHost = "Host: " + authority();
            wire.replace(originalHost, "Host: elsewhere.example");
        } else if (kind == "body") {
            wire.replace("\r\n\r\n", "\r\nContent-Length: 1\r\n\r\na");
        } else if (kind == "oversized") {
            const QByteArray padding = "\r\nPadding: " + QByteArray(9000, 'a') + "\r\n\r\n";
            wire.replace("\r\n\r\n", padding);
        }
        QTcpSocket client;
        client.setProxy(QNetworkProxy::NoProxy);
        client.connectToHost(QHostAddress::LocalHost, proxy.port());
        QTRY_COMPARE(client.state(), QAbstractSocket::ConnectedState);
        client.write(wire);
        QTRY_VERIFY(client.bytesAvailable() > 0);
        const QByteArray expectedPrefix = "HTTP/1.1 " + QByteArray::number(expected) + ' ';
        QVERIFY(client.readAll().startsWith(expectedPrefix));
        QCOMPARE(connects, 0);
        QTRY_COMPARE(relay.activeConnections(), 0);
    }

    void authenticatedTunnelPreservesBytesAndStopsOnRevocation()
    {
        QTcpServer target;
        QVERIFY(target.listen(QHostAddress::LocalHost, 0));
        connect(&target, &QTcpServer::newConnection, &target, [&target] {
            auto *peer = target.nextPendingConnection();
            connect(peer, &QTcpSocket::readyRead, peer, [peer] {
                peer->write(peer->readAll());
            });
        });
        PrivateCloudConnectRelay relay;
        QVERIFY(relay.start(QHostAddress("203.0.113.1"), 1, QHostAddress::LocalHost));
        PrivateCloudRelayTestAccess::connector(relay, [&target](QTcpSocket *socket) {
            socket->connectToHost(QHostAddress::LocalHost, target.serverPort());
            return true;
        });
        const auto proxy = relay.proxy();
        QTcpSocket client;
        client.setProxy(QNetworkProxy::NoProxy);
        client.connectToHost(QHostAddress::LocalHost, proxy.port());
        QTRY_COMPARE(client.state(), QAbstractSocket::ConnectedState);
        const auto wire = request(proxy);
        client.write(wire.first(17));
        QTest::qWait(20);
        QCOMPARE(target.hasPendingConnections(), false);
        QByteArray payload(1024 * 1024, Qt::Uninitialized);
        for (qsizetype i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<char>(i % 251);
        }
        client.write(wire.sliced(17) + payload);
        QByteArray received;
        QTRY_VERIFY_WITH_TIMEOUT((received += client.readAll()).size() >= payload.size() + 39, 10000);
        const auto headerEnd = received.indexOf("\r\n\r\n") + 4;
        QVERIFY(received.startsWith("HTTP/1.1 200 "));
        QCOMPARE(received.sliced(headerEnd), payload);
        QCOMPARE(relay.activeConnections(), 1);
        relay.stop();
        QTRY_COMPARE(client.state(), QAbstractSocket::UnconnectedState);
        QCOMPARE(relay.activeConnections(), 0);
    }

    void remoteEofDrainsBufferedData()
    {
        QTcpServer target;
        QVERIFY(target.listen(QHostAddress::LocalHost, 0));
        const QByteArray payload(768 * 1024, 'z');
        connect(&target, &QTcpServer::newConnection, &target, [&target, &payload] {
            auto *peer = target.nextPendingConnection();
            peer->write(payload);
            peer->disconnectFromHost();
        });
        PrivateCloudConnectRelay relay;
        QVERIFY(relay.start(QHostAddress("203.0.113.1"), 1, QHostAddress::LocalHost));
        PrivateCloudRelayTestAccess::connector(relay, [&target](QTcpSocket *socket) {
            socket->connectToHost(QHostAddress::LocalHost, target.serverPort());
            return true;
        });
        QTcpSocket client;
        client.setProxy(QNetworkProxy::NoProxy);
        client.setReadBufferSize(32768);
        client.connectToHost(QHostAddress::LocalHost, relay.proxy().port());
        QTRY_COMPARE(client.state(), QAbstractSocket::ConnectedState);
        client.write(request(relay.proxy()));
        QByteArray received;
        QTRY_VERIFY_WITH_TIMEOUT((received += client.readAll()).size() >= payload.size() + 39, 10000);
        const auto headerEnd = received.indexOf("\r\n\r\n") + 4;
        QCOMPARE(received.sliced(headerEnd), payload);
        QTRY_COMPARE(client.state(), QAbstractSocket::UnconnectedState);
    }

    void failedSocketSelectionCannotFallBack()
    {
        PrivateCloudConnectRelay relay;
        QVERIFY(relay.start(QHostAddress("203.0.113.1"), 1, QHostAddress::LocalHost));
        PrivateCloudRelayTestAccess::connector(relay, [](QTcpSocket *) {
            return false;
        });
        QTcpSocket client;
        client.setProxy(QNetworkProxy::NoProxy);
        client.connectToHost(QHostAddress::LocalHost, relay.proxy().port());
        QTRY_COMPARE(client.state(), QAbstractSocket::ConnectedState);
        client.write(request(relay.proxy()));
        QTRY_VERIFY(client.bytesAvailable() > 0);
        QVERIFY(client.readAll().startsWith("HTTP/1.1 502 "));
        QTRY_COMPARE(relay.activeConnections(), 0);
    }
};

QTEST_GUILESS_MAIN(TestPrivateCloudConnectRelay)
#include "testprivatecloudconnectrelay.moc"
