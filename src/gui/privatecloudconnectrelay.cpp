/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "privatecloudconnectrelay.h"
#include "privatecloudrelayconnection.h"
#include "account.h"

#include <QTcpSocket>
#include <QUuid>

namespace OCC
{
namespace
{
constexpr qsizetype maxConnections = 32;
const auto proxyUser = QStringLiteral("privateCloud");
}

PrivateCloudConnectRelay::PrivateCloudConnectRelay(QObject *parent)
    : QObject(parent)
{
    _listener.setProxy(QNetworkProxy::NoProxy);
    _listener.setMaxPendingConnections(maxConnections);
    connect(&_listener, &QTcpServer::newConnection, this, &PrivateCloudConnectRelay::acceptConnections);
}

PrivateCloudConnectRelay::~PrivateCloudConnectRelay()
{
    stop();
}

bool PrivateCloudConnectRelay::start(const QHostAddress &server, quint32 interfaceIndex, const QHostAddress &source)
{
    stop();
    if (server.protocol() != QAbstractSocket::IPv4Protocol || source.protocol() != QAbstractSocket::IPv4Protocol || interfaceIndex == 0) {
        return false;
    }
    _connector = [server, interfaceIndex, source](QTcpSocket *socket) {
#ifdef Q_OS_WIN
        if (!socket->bind(source, 0, QAbstractSocket::DontShareAddress)) {
            socket->abort();
            return false;
        }
        const auto native = static_cast<SOCKET>(socket->socketDescriptor());
        const DWORD selectedInterface = htonl(interfaceIndex);
        if (setsockopt(native, IPPROTO_IP, IP_UNICAST_IF, reinterpret_cast<const char *>(&selectedInterface), sizeof(selectedInterface)) != 0) {
            socket->abort();
            return false;
        }
        QObject::connect(socket, &QTcpSocket::stateChanged, socket, [socket, interfaceIndex, source](QAbstractSocket::SocketState state) {
            if (state != QAbstractSocket::ConnectedState) {
                return;
            }
            DWORD actualInterface = 0;
            int size = sizeof(actualInterface);
            const auto descriptor = static_cast<SOCKET>(socket->socketDescriptor());
            if (socket->localAddress() != source || getsockopt(descriptor, IPPROTO_IP, IP_UNICAST_IF, reinterpret_cast<char *>(&actualInterface), &size) != 0
                || actualInterface != interfaceIndex) {
                socket->abort();
            }
        });
        socket->connectToHost(server, 443);
        return true;
#else
        Q_UNUSED(socket)
        Q_UNUSED(server)
        Q_UNUSED(interfaceIndex)
        Q_UNUSED(source)
        return false;
#endif
    };
    _password = QUuid::createUuid().toString(QUuid::WithoutBraces) + QUuid::createUuid().toString(QUuid::WithoutBraces);
    return _listener.listen(QHostAddress::LocalHost, 0);
}

void PrivateCloudConnectRelay::stop()
{
    _listener.close();
    const auto active = _connections;
    for (auto *connection : active) {
        connection->close();
    }
    _password.clear();
    _connector = {};
}

QNetworkProxy PrivateCloudConnectRelay::proxy() const
{
    if (!_listener.isListening()) {
        return QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"), 0);
    }
    return QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"), _listener.serverPort(), proxyUser, _password);
}

qsizetype PrivateCloudConnectRelay::activeConnections() const
{
    return _connections.size();
}

void PrivateCloudConnectRelay::acceptConnections()
{
    const QByteArray authority = Account::enforcedServerUrl().host().toUtf8() + ":443";
    const QByteArray credentials = proxyUser.toUtf8() + ':' + _password.toUtf8();
    const QByteArray authorization = "Basic " + credentials.toBase64();
    while (_listener.hasPendingConnections()) {
        auto *client = _listener.nextPendingConnection();
        if (!_connector || _connections.size() >= maxConnections || !client->peerAddress().isLoopback()) {
            client->abort();
            client->deleteLater();
            continue;
        }
        auto *connection = new PrivateCloudRelayConnection(client, authority, authorization, _connector, this);
        _connections.insert(connection);
        connect(connection, &PrivateCloudRelayConnection::finished, this, [this, connection] {
            _connections.remove(connection);
            connection->deleteLater();
        });
    }
}
}
