/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifdef _WIN32
#include <qt_windows.h>
#endif

#include "privateclouddirectconnection.h"
#include "privatecloudtransport.h"

#include <QJsonDocument>
#include <QLocalServer>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <QUuid>
#include <memory>

using namespace OCC;

namespace OCC
{
class PrivateCloudDirectConnectionTestAccess
{
public:
    static void configure(PrivateCloudDirectConnection &connection, std::function<qint64()> now, std::function<void(quint64)> open)
    {
        connection._now = std::move(now);
        connection._openPipe = std::move(open);
    }

    static void adopt(PrivateCloudDirectConnection &connection, QLocalSocket *socket, quint64 generation)
    {
        connection.adoptPipe(socket, generation);
    }

    static bool responseTimeout(PrivateCloudDirectConnection &connection)
    {
        connection._responseTimer.stop();
        return QMetaObject::invokeMethod(&connection._responseTimer, "timeout", Qt::DirectConnection);
    }

    static void retry(PrivateCloudDirectConnection &connection)
    {
        connection._retryTimer.stop();
        connection.attemptConnection();
    }

    static void renew(PrivateCloudDirectConnection &connection)
    {
        connection._renewalTimer.stop();
        connection.renewLease();
    }

    static void expire(PrivateCloudDirectConnection &connection)
    {
        connection._expiryTimer.stop();
        connection.expireLease();
    }

    static void trackOpenWorker(PrivateCloudDirectConnection &connection, QObject *worker)
    {
        connection._openWatcher = worker;
    }

    static bool waitingForReply(const PrivateCloudDirectConnection &connection)
    {
        return connection._awaitingReply;
    }

    static qsizetype replySize(const PrivateCloudDirectConnection &connection)
    {
        return connection._reply.size();
    }

    static qint64 expiresAt(const PrivateCloudDirectConnection &connection)
    {
        return connection._expiresAt;
    }

    static int retryInterval(const PrivateCloudDirectConnection &connection)
    {
        return connection._retryTimer.isActive() ? connection._retryTimer.interval() : -1;
    }

    static bool allTimersStopped(const PrivateCloudDirectConnection &connection)
    {
        return !connection._retryTimer.isActive() && !connection._responseTimer.isActive() && !connection._renewalTimer.isActive()
            && !connection._expiryTimer.isActive();
    }

    static quint16 relayPort(const PrivateCloudDirectConnection &connection)
    {
        return connection._relay.proxy().port();
    }
};
}

class TestPrivateCloudDirectConnection : public QObject
{
    Q_OBJECT

    // This fake broker uses only a unique local pipe. No service opener is called,
    // and no test submits a CONNECT request to the loopback relay.
    struct Session {
        qint64 now = 1000;
        QList<quint64> opens;
        QLocalServer broker;
        PrivateCloudDirectConnection connection;
        QPointer<QLocalSocket> peer;
        QPointer<QLocalSocket> client;

        Session()
        {
            PrivateCloudDirectConnectionTestAccess::configure(
                connection,
                [this] {
                    return now;
                },
                [this](quint64 generation) {
                    opens.append(generation);
                });
        }

        bool start()
        {
            broker.setSocketOptions(QLocalServer::UserAccessOption);
            if (!broker.listen(QStringLiteral("privateCloud-direct-test-") + QUuid::createUuid().toString(QUuid::WithoutBraces))) {
                return false;
            }
            connection.start();
            return true;
        }

        bool connectPipe(quint64 generation)
        {
            auto *socket = new QLocalSocket;
            socket->connectToServer(broker.fullServerName());
            if (!socket->waitForConnected(2000) || (!broker.hasPendingConnections() && !broker.waitForNewConnection(2000))) {
                delete socket;
                return false;
            }
            peer = broker.nextPendingConnection();
            if (!peer) {
                delete socket;
                return false;
            }
            client = socket;
            PrivateCloudDirectConnectionTestAccess::adopt(connection, socket, generation);
            return true;
        }

        bool send(const QByteArray &bytes)
        {
            if (!peer || peer->write(bytes) != bytes.size()) {
                return false;
            }
            peer->flush();
            return true;
        }
    };

    static QJsonObject lease(bool renewed = false)
    {
        return {{QStringLiteral("version"), 1},
                {QStringLiteral("ok"), true},
                {QStringLiteral("lease"), QStringLiteral("12c49d2e-9a3b-49d9-a9e8-44d099da4384")},
                {QStringLiteral("family"), 4},
                {QStringLiteral("server"), QStringLiteral("8.8.8.8")},
                {QStringLiteral("port"), 443},
                {QStringLiteral("interfaceIndex"), 42},
                {QStringLiteral("source"), QStringLiteral("192.168.50.20")},
                {QStringLiteral("expiresInSeconds"), 120},
                {QStringLiteral("permitInstalled"), true},
                {QStringLiteral("renewed"), renewed}};
    }

    static QByteArray wire(const QJsonObject &object)
    {
        return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    }

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    void init()
    {
        PrivateCloudTransport::clearEndpoint();
    }

    void cleanup()
    {
        QVERIFY(!PrivateCloudTransport::isReady());
    }

    void constructionIsInactiveAndStartIsIdempotent()
    {
        Session session;
        QVERIFY(session.opens.isEmpty());
        QVERIFY(!session.connection.isReady());
        QVERIFY(PrivateCloudDirectConnectionTestAccess::allTimersStopped(session.connection));
        QVERIFY(session.start());
        session.connection.start();
        QTRY_COMPARE(session.opens.size(), 1);
        QVERIFY(PrivateCloudTransport::isRequired());
        QVERIFY(!PrivateCloudTransport::isReady());
        session.connection.stop();
        session.connection.stop();
        QVERIFY(PrivateCloudDirectConnectionTestAccess::allTimersStopped(session.connection));
        PrivateCloudDirectConnectionTestAccess::retry(session.connection);
        QCOMPARE(session.opens.size(), 1);
    }

    void fragmentedLeaseEnablesRelayAndStopRevokesIt()
    {
        Session session;
        QVERIFY(session.start());
        QTRY_COMPARE(session.opens.size(), 1);
        QVERIFY(session.connectPipe(session.opens.last()));
        QTRY_VERIFY(session.peer->canReadLine());
        QCOMPARE(session.peer->readLine(), QByteArrayLiteral("PRIVATECLOUD-DIRECT/1\n"));
        QSignalSpy ready(&session.connection, &PrivateCloudDirectConnection::readyChanged);
        const auto response = wire(lease());
        QVERIFY(session.send(response.first(response.size() / 2)));
        QTRY_COMPARE(PrivateCloudDirectConnectionTestAccess::replySize(session.connection), response.size() / 2);
        QVERIFY(!session.connection.isReady());
        QVERIFY(!PrivateCloudTransport::isReady());
        session.now += 500;
        QVERIFY(session.send(response.sliced(response.size() / 2)));
        QTRY_VERIFY(session.connection.isReady());
        QVERIFY(PrivateCloudTransport::isReady());
        QCOMPARE(PrivateCloudTransport::proxy().hostName(), QStringLiteral("127.0.0.1"));
        QVERIFY(PrivateCloudTransport::proxy().port() > 0);
        QCOMPARE(PrivateCloudDirectConnectionTestAccess::expiresAt(session.connection), 121000);
        QCOMPARE(ready.size(), 1);
        QVERIFY(ready.first().first().toBool());
        session.connection.stop();
        QVERIFY(!session.connection.isReady());
        QVERIFY(!PrivateCloudTransport::isReady());
        QCOMPARE(PrivateCloudDirectConnectionTestAccess::relayPort(session.connection), 0);
        QCOMPARE(ready.size(), 2);
        QVERIFY(!ready.last().first().toBool());
        QVERIFY(PrivateCloudDirectConnectionTestAccess::allTimersStopped(session.connection));
    }

    void invalidBrokerFrames_data()
    {
        QTest::addColumn<QByteArray>("response");
        QTest::newRow("malformed-json") << QByteArrayLiteral("{broken\n");
        QTest::newRow("array") << QByteArrayLiteral("[]\n");
        QTest::newRow("oversized") << QByteArray(4097, 'x');
        QTest::newRow("multiple-replies") << QByteArray(wire(lease()) + wire(lease()));
        auto duplicate = wire(lease());
        duplicate.insert(1, "\"version\":1,");
        QTest::newRow("duplicate-key") << duplicate;
        auto escapedDuplicate = wire(lease());
        escapedDuplicate.insert(1, "\"ver\\u0073ion\":1,");
        QTest::newRow("escaped-duplicate-key") << escapedDuplicate;
        const auto changed = [](const char *name, const QString &key, const QJsonValue &value) {
            auto response = lease();
            response.insert(key, value);
            QTest::newRow(name) << wire(response);
        };
        changed("unknown-key", QStringLiteral("extra"), 1);
        changed("wrong-version", QStringLiteral("version"), 2);
        changed("boolean-version", QStringLiteral("version"), true);
        changed("string-ok", QStringLiteral("ok"), QStringLiteral("true"));
        changed("wrong-family", QStringLiteral("family"), 6);
        changed("wrong-port", QStringLiteral("port"), 80);
        changed("wrong-duration", QStringLiteral("expiresInSeconds"), 121);
        changed("fractional-interface", QStringLiteral("interfaceIndex"), 1.5);
        changed("zero-interface", QStringLiteral("interfaceIndex"), 0);
        changed("overflow-interface", QStringLiteral("interfaceIndex"), 4294967296.0);
        changed("null-id", QStringLiteral("lease"), QStringLiteral("00000000-0000-0000-0000-000000000000"));
        changed("non-uuid-id", QStringLiteral("lease"), QStringLiteral("not-a-lease"));
        changed("string-permit", QStringLiteral("permitInstalled"), QStringLiteral("true"));
        changed("string-renewed", QStringLiteral("renewed"), QStringLiteral("false"));
        changed("initial-marked-renewed", QStringLiteral("renewed"), true);
        changed("loopback-source", QStringLiteral("source"), QStringLiteral("127.0.0.1"));
        changed("unspecified-source", QStringLiteral("source"), QStringLiteral("0.0.0.0"));
        changed("link-local-source", QStringLiteral("source"), QStringLiteral("169.254.1.2"));
        changed("multicast-source", QStringLiteral("source"), QStringLiteral("224.1.1.1"));
        changed("ipv6-source", QStringLiteral("source"), QStringLiteral("::1"));
        for (const auto &server : {QStringLiteral("0.1.2.3"),
                                   QStringLiteral("10.1.2.3"),
                                   QStringLiteral("100.64.0.1"),
                                   QStringLiteral("127.0.0.1"),
                                   QStringLiteral("169.254.1.2"),
                                   QStringLiteral("172.16.1.2"),
                                   QStringLiteral("192.168.1.2"),
                                   QStringLiteral("192.0.2.1"),
                                   QStringLiteral("198.18.0.1"),
                                   QStringLiteral("198.51.100.1"),
                                   QStringLiteral("203.0.113.1"),
                                   QStringLiteral("224.0.0.1"),
                                   QStringLiteral("255.255.255.255"),
                                   QStringLiteral("::1"),
                                   QStringLiteral("8.8.8.08")}) {
            changed(qPrintable(QStringLiteral("non-public-server-") + server), QStringLiteral("server"), server);
        }
        auto missing = lease();
        missing.remove(QStringLiteral("source"));
        QTest::newRow("missing-field") << wire(missing);
        QTest::newRow("refusal-with-extra-field") << QByteArrayLiteral("{\"version\":1,\"ok\":false,\"error\":\"unavailable\",\"extra\":1}\n");
        QTest::newRow("refusal-with-untrusted-prose") << QByteArrayLiteral("{\"version\":1,\"ok\":false,\"error\":\"<b>arbitrary text</b>\"}\n");
    }

    void invalidBrokerFrames()
    {
        QFETCH(QByteArray, response);
        Session session;
        QVERIFY(session.start());
        QTRY_COMPARE(session.opens.size(), 1);
        QVERIFY(session.connectPipe(session.opens.last()));
        QVERIFY(session.send(response));
        QTRY_VERIFY(!session.connection.lastError().isEmpty());
        QVERIFY(!session.connection.isReady());
        QVERIFY(!PrivateCloudTransport::isReady());
        QCOMPARE(PrivateCloudDirectConnectionTestAccess::retryInterval(session.connection), 1000);
    }

    void refusalRetriesAndSuccessClearsError()
    {
        Session session;
        QVERIFY(session.start());
        QTRY_COMPARE(session.opens.size(), 1);
        QVERIFY(session.connectPipe(session.opens.last()));
        QVERIFY(session.send(QByteArrayLiteral("{\"version\":1,\"ok\":false,\"error\":\"route_unavailable\"}\n")));
        QTRY_VERIFY(session.connection.lastError().contains(QStringLiteral("route_unavailable")));
        QVERIFY(!PrivateCloudTransport::isReady());
        PrivateCloudDirectConnectionTestAccess::retry(session.connection);
        QCOMPARE(session.opens.size(), 2);
        QVERIFY(session.connectPipe(session.opens.last()));
        auto response = lease();
        response[QStringLiteral("permitInstalled")] = false;
        QVERIFY(session.send(wire(response)));
        QTRY_VERIFY(session.connection.isReady());
        QVERIFY(session.connection.lastError().isEmpty());
        session.peer->abort();
        QTRY_VERIFY(!session.connection.isReady());
        QCOMPARE(PrivateCloudDirectConnectionTestAccess::retryInterval(session.connection), 1000);
    }

    void renewalPreservesEndpointAndExtendsDeadline()
    {
        Session session;
        QVERIFY(session.start());
        QTRY_COMPARE(session.opens.size(), 1);
        QVERIFY(session.connectPipe(session.opens.last()));
        QTRY_VERIFY(session.peer->canReadLine());
        session.peer->readLine();
        QVERIFY(session.send(wire(lease())));
        QTRY_VERIFY(session.connection.isReady());
        const auto proxy = PrivateCloudTransport::proxy();
        QSignalSpy ready(&session.connection, &PrivateCloudDirectConnection::readyChanged);
        session.now = 61000;
        PrivateCloudDirectConnectionTestAccess::renew(session.connection);
        QTRY_VERIFY(session.peer->canReadLine());
        QCOMPARE(session.peer->readLine(), QByteArrayLiteral("PRIVATECLOUD-RENEW/1\n"));
        QVERIFY(PrivateCloudDirectConnectionTestAccess::waitingForReply(session.connection));
        session.now = 62000;
        QVERIFY(session.send(wire(lease(true))));
        QTRY_VERIFY(!PrivateCloudDirectConnectionTestAccess::waitingForReply(session.connection));
        QVERIFY(session.connection.isReady());
        QCOMPARE(PrivateCloudTransport::proxy(), proxy);
        QCOMPARE(PrivateCloudDirectConnectionTestAccess::expiresAt(session.connection), 181000);
        QCOMPARE(ready.size(), 0);
    }

    void changedRenewalRevokesEndpoint_data()
    {
        QTest::addColumn<QString>("key");
        QTest::addColumn<QJsonValue>("value");
        QTest::newRow("lease-id") << QStringLiteral("lease") << QJsonValue(QStringLiteral("32c49d2e-9a3b-49d9-a9e8-44d099da4384"));
        QTest::newRow("server") << QStringLiteral("server") << QJsonValue(QStringLiteral("1.1.1.1"));
        QTest::newRow("interface") << QStringLiteral("interfaceIndex") << QJsonValue(43);
        QTest::newRow("source") << QStringLiteral("source") << QJsonValue(QStringLiteral("192.168.50.21"));
        QTest::newRow("permit") << QStringLiteral("permitInstalled") << QJsonValue(false);
        QTest::newRow("not-renewed") << QStringLiteral("renewed") << QJsonValue(false);
    }

    void changedRenewalRevokesEndpoint()
    {
        QFETCH(QString, key);
        QFETCH(QJsonValue, value);
        Session session;
        QVERIFY(session.start());
        QTRY_COMPARE(session.opens.size(), 1);
        QVERIFY(session.connectPipe(session.opens.last()));
        QVERIFY(session.send(wire(lease())));
        QTRY_VERIFY(session.connection.isReady());
        session.now = 61000;
        PrivateCloudDirectConnectionTestAccess::renew(session.connection);
        auto response = lease(true);
        response.insert(key, value);
        QVERIFY(session.send(wire(response)));
        QTRY_VERIFY(!session.connection.isReady());
        QVERIFY(!PrivateCloudTransport::isReady());
        QVERIFY(!session.connection.lastError().isEmpty());
    }

    void unsolicitedReplyRevokesEndpoint()
    {
        Session session;
        QVERIFY(session.start());
        QTRY_COMPARE(session.opens.size(), 1);
        QVERIFY(session.connectPipe(session.opens.last()));
        QVERIFY(session.send(wire(lease())));
        QTRY_VERIFY(session.connection.isReady());
        QVERIFY(session.send(wire(lease(true))));
        QTRY_VERIFY(!session.connection.isReady());
        QVERIFY(!PrivateCloudTransport::isReady());
    }

    void disconnectRevokesEndpoint()
    {
        Session session;
        QVERIFY(session.start());
        QTRY_COMPARE(session.opens.size(), 1);
        QVERIFY(session.connectPipe(session.opens.last()));
        QVERIFY(session.send(wire(lease())));
        QTRY_VERIFY(session.connection.isReady());
        session.peer->abort();
        QTRY_VERIFY(!session.connection.isReady());
        QVERIFY(!PrivateCloudTransport::isReady());
        QCOMPARE(PrivateCloudDirectConnectionTestAccess::relayPort(session.connection), 0);
        QVERIFY(!session.connection.lastError().isEmpty());
    }

    void responseTimeoutRevokesRenewalAndStopsRetryAfterCancel()
    {
        Session session;
        QVERIFY(session.start());
        QTRY_COMPARE(session.opens.size(), 1);
        QVERIFY(session.connectPipe(session.opens.last()));
        QVERIFY(session.send(wire(lease())));
        QTRY_VERIFY(session.connection.isReady());
        session.now = 61000;
        PrivateCloudDirectConnectionTestAccess::renew(session.connection);
        QVERIFY(PrivateCloudDirectConnectionTestAccess::responseTimeout(session.connection));
        QVERIFY(!session.connection.isReady());
        QVERIFY(!PrivateCloudTransport::isReady());
        QCOMPARE(PrivateCloudDirectConnectionTestAccess::retryInterval(session.connection), 1000);
        session.connection.stop();
        QVERIFY(PrivateCloudDirectConnectionTestAccess::allTimersStopped(session.connection));
        PrivateCloudDirectConnectionTestAccess::retry(session.connection);
        QCOMPARE(session.opens.size(), 1);
    }

    void leaseExpiryUsesClockAndDoesNotAcceptLateRenewal()
    {
        Session session;
        QVERIFY(session.start());
        QTRY_COMPARE(session.opens.size(), 1);
        QVERIFY(session.connectPipe(session.opens.last()));
        QVERIFY(session.send(wire(lease())));
        QTRY_VERIFY(session.connection.isReady());
        session.now = 120999;
        PrivateCloudDirectConnectionTestAccess::expire(session.connection);
        QVERIFY(session.connection.isReady());
        PrivateCloudDirectConnectionTestAccess::renew(session.connection);
        session.now = 121000;
        QVERIFY(session.send(wire(lease(true))));
        QTRY_VERIFY(!session.connection.isReady());
        QVERIFY(!PrivateCloudTransport::isReady());
    }

    void expiredInitialLeaseNeverBecomesReady()
    {
        Session session;
        QVERIFY(session.start());
        QTRY_COMPARE(session.opens.size(), 1);
        QVERIFY(session.connectPipe(session.opens.last()));
        QSignalSpy ready(&session.connection, &PrivateCloudDirectConnection::readyChanged);
        session.now = 121000;
        QVERIFY(session.send(wire(lease())));
        QTRY_VERIFY(!session.connection.lastError().isEmpty());
        QVERIFY(!session.connection.isReady());
        QVERIFY(!PrivateCloudTransport::isReady());
        QCOMPARE(ready.size(), 0);
    }

    void stopFromReadySignalDoesNotRestartTimers()
    {
        Session session;
        QVERIFY(session.start());
        QTRY_COMPARE(session.opens.size(), 1);
        QVERIFY(session.connectPipe(session.opens.last()));
        auto readyNotifications = 0;
        connect(&session.connection, &PrivateCloudDirectConnection::readyChanged, &session.connection, [&](bool ready) {
            if (ready) {
                ++readyNotifications;
                session.connection.stop();
            }
        });
        QVERIFY(session.send(wire(lease())));
        QTRY_COMPARE(readyNotifications, 1);
        QVERIFY(!session.connection.isReady());
        QVERIFY(!PrivateCloudTransport::isReady());
        QVERIFY(PrivateCloudDirectConnectionTestAccess::allTimersStopped(session.connection));
    }

    void stopFromEndpointPublicationDoesNotAnnounceReady()
    {
        Session session;
        QVERIFY(session.start());
        QTRY_COMPARE(session.opens.size(), 1);
        QVERIFY(session.connectPipe(session.opens.last()));
        QSignalSpy ready(&session.connection, &PrivateCloudDirectConnection::readyChanged);
        auto published = 0;
        connect(PrivateCloudTransport::instance(), &PrivateCloudTransport::configurationChanged, &session.connection, [&] {
            if (PrivateCloudTransport::isReady()) {
                ++published;
                session.connection.stop();
            }
        });
        QVERIFY(session.send(wire(lease())));
        QTRY_COMPARE(published, 1);
        QVERIFY(!session.connection.isReady());
        QVERIFY(!PrivateCloudTransport::isReady());
        QCOMPARE(ready.size(), 0);
        QVERIFY(PrivateCloudDirectConnectionTestAccess::allTimersStopped(session.connection));
    }

    void stoppedAndObsoleteOpenResultsAreDiscarded()
    {
        Session session;
        QVERIFY(session.start());
        QTRY_COMPARE(session.opens.size(), 1);
        const auto obsolete = session.opens.first();
        QVERIFY(PrivateCloudDirectConnectionTestAccess::responseTimeout(session.connection));
        PrivateCloudDirectConnectionTestAccess::retry(session.connection);
        QCOMPARE(session.opens.size(), 2);
        QVERIFY(session.connectPipe(obsolete));
        QTRY_VERIFY(session.client.isNull());
        QVERIFY(!session.connection.isReady());
        QVERIFY(!PrivateCloudTransport::isReady());
        const auto current = session.opens.last();
        session.connection.stop();
        QVERIFY(session.connectPipe(current));
        QTRY_VERIFY(session.client.isNull());
        QVERIFY(PrivateCloudDirectConnectionTestAccess::allTimersStopped(session.connection));
    }

    void retryBackoffIsCappedAndOpeningWorkIsNotDuplicated()
    {
        Session session;
        QVERIFY(session.start());
        QTRY_COMPARE(session.opens.size(), 1);
        QObject openingWorker;
        PrivateCloudDirectConnectionTestAccess::trackOpenWorker(session.connection, &openingWorker);
        QVERIFY(PrivateCloudDirectConnectionTestAccess::responseTimeout(session.connection));
        PrivateCloudDirectConnectionTestAccess::retry(session.connection);
        QCOMPARE(session.opens.size(), 1);
        PrivateCloudDirectConnectionTestAccess::trackOpenWorker(session.connection, nullptr);
        for (const auto expectedDelay : {2000, 4000, 8000, 16000, 30000, 30000}) {
            PrivateCloudDirectConnectionTestAccess::retry(session.connection);
            QVERIFY(PrivateCloudDirectConnectionTestAccess::responseTimeout(session.connection));
            QCOMPARE(PrivateCloudDirectConnectionTestAccess::retryInterval(session.connection), expectedDelay);
        }
        session.connection.stop();
        session.connection.start();
        QTRY_COMPARE(session.opens.size(), 8);
        QVERIFY(PrivateCloudDirectConnectionTestAccess::responseTimeout(session.connection));
        QCOMPARE(PrivateCloudDirectConnectionTestAccess::retryInterval(session.connection), 1000);
    }

    void windowsNativePipeRightsSupportLeaseAndReleaseOwnership()
    {
#ifdef Q_OS_WIN
        DWORD baselineHandles = 0;
        constexpr auto repetitions = 5;
        for (auto iteration = 0; iteration < repetitions; ++iteration) {
            {
                Session session;
                QVERIFY(session.start());
                QTRY_COMPARE(session.opens.size(), 1);
                const auto pipeName = session.broker.fullServerName().toStdWString();
                auto native = CreateFileW(pipeName.c_str(),
                                          FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE,
                                          0,
                                          nullptr,
                                          OPEN_EXISTING,
                                          FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                                          nullptr);
                QVERIFY2(native != INVALID_HANDLE_VALUE, qPrintable(QStringLiteral("Native test pipe open failed: %1").arg(GetLastError())));
                const auto closeUnadoptedHandle = qScopeGuard([&native] {
                    if (native != INVALID_HANDLE_VALUE) {
                        CloseHandle(native);
                    }
                });
                QVERIFY(session.broker.hasPendingConnections() || session.broker.waitForNewConnection(2000));
                session.peer = session.broker.nextPendingConnection();
                QVERIFY(session.peer);
                auto socket = std::make_unique<QLocalSocket>();
                QVERIFY2(socket->setSocketDescriptor(reinterpret_cast<qintptr>(native), QLocalSocket::ConnectedState, QIODevice::ReadWrite),
                         qPrintable(socket->errorString()));
                native = INVALID_HANDLE_VALUE; // QLocalSocket now owns the handle.
                session.client = socket.get();
                PrivateCloudDirectConnectionTestAccess::adopt(session.connection, socket.release(), session.opens.last());
                QTRY_VERIFY(session.peer->canReadLine());
                QCOMPARE(session.peer->readLine(), QByteArrayLiteral("PRIVATECLOUD-DIRECT/1\n"));
                QVERIFY(session.send(wire(lease())));
                QTRY_VERIFY(session.connection.isReady());
                QVERIFY(PrivateCloudTransport::isReady());
                session.now = 61000;
                PrivateCloudDirectConnectionTestAccess::renew(session.connection);
                QTRY_VERIFY(session.peer->canReadLine());
                QCOMPARE(session.peer->readLine(), QByteArrayLiteral("PRIVATECLOUD-RENEW/1\n"));
                QVERIFY(session.send(wire(lease(true))));
                QTRY_VERIFY(!PrivateCloudDirectConnectionTestAccess::waitingForReply(session.connection));
                session.connection.stop();
                QVERIFY(!PrivateCloudTransport::isReady());
                QVERIFY(PrivateCloudDirectConnectionTestAccess::allTimersStopped(session.connection));
                QTRY_COMPARE(session.peer->state(), QLocalSocket::UnconnectedState);
                QTRY_VERIFY(session.client.isNull());
            }
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QCoreApplication::processEvents();
            DWORD currentHandles = 0;
            QVERIFY(GetProcessHandleCount(GetCurrentProcess(), &currentHandles));
            if (iteration == 0) {
                baselineHandles = currentHandles; // Warm up Qt's Windows pipe notifier first.
            } else {
                QVERIFY2(currentHandles <= baselineHandles,
                         qPrintable(QStringLiteral("Native pipe cycle leaked handles: baseline %1, current %2").arg(baselineHandles).arg(currentHandles)));
            }
        }
#else
        QSKIP("Native broker pipe permissions apply only on Windows.");
#endif
    }

    void destructionRevokesReadyEndpoint()
    {
        {
            Session session;
            QVERIFY(session.start());
            QTRY_COMPARE(session.opens.size(), 1);
            QVERIFY(session.connectPipe(session.opens.last()));
            QVERIFY(session.send(wire(lease())));
            QTRY_VERIFY(PrivateCloudTransport::isReady());
        }
        QVERIFY(!PrivateCloudTransport::isReady());
    }
};

QTEST_GUILESS_MAIN(TestPrivateCloudDirectConnection)
#include "testprivateclouddirectconnection.moc"
