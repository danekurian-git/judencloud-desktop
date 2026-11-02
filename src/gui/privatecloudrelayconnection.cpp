/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "privatecloudrelayconnection.h"

#include <QNetworkProxy>
#include <QTcpSocket>

namespace OCC
{
namespace
{
constexpr qint64 headerLimit = 8192;
constexpr qint64 pendingLimit = 256 * 1024;
constexpr qint64 chunkSize = 64 * 1024;
constexpr int handshakeTimeoutMs = 10000;

bool equalSecret(const QByteArray &left, const QByteArray &right)
{
    if (left.size() != right.size()) {
        return false;
    }
    unsigned char difference = 0;
    for (qsizetype i = 0; i < left.size(); ++i) {
        difference |= static_cast<unsigned char>(left[i]) ^ static_cast<unsigned char>(right[i]);
    }
    return difference == 0;
}
}

PrivateCloudRelayConnection::PrivateCloudRelayConnection(QTcpSocket *client,
                                                         QByteArray authority,
                                                         QByteArray authorization,
                                                         Connector connector,
                                                         QObject *parent)
    : QObject(parent)
    , _client(client)
    , _remote(new QTcpSocket(this))
    , _authority(std::move(authority))
    , _authorization(std::move(authorization))
    , _connector(std::move(connector))
{
    _client->setParent(this);
    _client->setReadBufferSize(pendingLimit);
    _remote->setReadBufferSize(pendingLimit);
    _remote->setProxy(QNetworkProxy::NoProxy);
    _deadline.setSingleShot(true);
    _deadline.setInterval(handshakeTimeoutMs);
    connect(&_deadline, &QTimer::timeout, this, &PrivateCloudRelayConnection::close);
    connect(_client, &QTcpSocket::readyRead, this, [this] {
        if (_tunnelling) {
            pump(_client, _remote);
        } else if (!_connecting) {
            readRequest();
        }
    });
    connect(_remote, &QTcpSocket::readyRead, this, [this] {
        pump(_remote, _client);
    });
    connect(_remote, &QTcpSocket::bytesWritten, this, [this] {
        pump(_client, _remote);
    });
    connect(_client, &QTcpSocket::bytesWritten, this, [this] {
        pump(_remote, _client);
    });
    connect(_client, &QTcpSocket::disconnected, this, &PrivateCloudRelayConnection::close);
    connect(_client, &QTcpSocket::errorOccurred, this, &PrivateCloudRelayConnection::close);
    connect(_remote, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError error) {
        if (error == QAbstractSocket::RemoteHostClosedError) {
            return;
        }
        if (!_tunnelling) {
            reject(502);
        } else {
            close();
        }
    });
    connect(_remote, &QTcpSocket::disconnected, this, [this] {
        // Drain received bytes before sending EOF to the client.
        pump(_remote, _client);
        if (_remote->bytesAvailable() == 0) {
            _client->disconnectFromHost();
        }
    });
    connect(_remote, &QTcpSocket::connected, this, [this] {
        if (_closing || _remote->state() != QAbstractSocket::ConnectedState) {
            return;
        }
        _deadline.stop();
        _tunnelling = true;
        _request.clear();
        _authorization.clear();
        _client->write("HTTP/1.1 200 Connection Established\r\n\r\n");
        pump(_client, _remote);
    });
    _deadline.start();
    QMetaObject::invokeMethod(this, &PrivateCloudRelayConnection::readRequest, Qt::QueuedConnection);
}

void PrivateCloudRelayConnection::readRequest()
{
    if (_closing || _connecting || _tunnelling) {
        return;
    }
    // Read no bytes past the CONNECT headers: subsequent bytes belong to TLS.
    while (_client->bytesAvailable() && !_request.endsWith("\r\n\r\n")) {
        _request += _client->read(1);
        if (_request.size() > headerLimit) {
            reject(431);
            return;
        }
    }
    if (!_request.endsWith("\r\n\r\n")) {
        return;
    }
    const auto lines = _request.split('\n');
    const auto first = lines.first().trimmed().split(' ');
    if (first.size() != 3 || first[0] != "CONNECT" || first[1] != _authority || (first[2] != "HTTP/1.1" && first[2] != "HTTP/1.0")) {
        reject(403);
        return;
    }
    QByteArray authorization;
    bool foundAuthorization = false;
    bool foundHost = false;
    for (qsizetype i = 1; i + 2 < lines.size(); ++i) {
        const auto line = lines[i];
        const auto colon = line.indexOf(':');
        if (colon <= 0 || line.startsWith(' ') || line.startsWith('\t')) {
            reject(400);
            return;
        }
        const auto name = line.first(colon).toLower();
        const auto value = line.sliced(colon + 1).trimmed();
        if (name == "proxy-authorization") {
            if (foundAuthorization) {
                reject(400);
                return;
            }
            foundAuthorization = true;
            authorization = value;
        } else if (name == "host") {
            // Qt omits the default HTTPS port from Host while retaining it in
            // the CONNECT authority. Both spellings identify the same endpoint.
            const auto hostWithoutDefaultPort = _authority.chopped(4);
            if (foundHost || (value != _authority && value != hostWithoutDefaultPort)) {
                reject(400);
                return;
            }
            foundHost = true;
        } else if (name == "transfer-encoding" || (name == "content-length" && value != "0")) {
            reject(400);
            return;
        }
    }
    if (!foundAuthorization || !equalSecret(authorization, _authorization)) {
        reject(407);
        return;
    }
    _connecting = true;
    if (!_connector(_remote)) {
        reject(502);
    }
}

void PrivateCloudRelayConnection::reject(int status)
{
    if (_closing) {
        return;
    }
    _connecting = true;
    QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + " Request rejected\r\n";
    if (status == 407) {
        response += "Proxy-Authenticate: Basic realm=\"privateCloud\"\r\n";
    }
    response += "Content-Length: 0\r\nConnection: close\r\n\r\n";
    _client->write(response);
    _client->disconnectFromHost();
}

void PrivateCloudRelayConnection::pump(QTcpSocket *from, QTcpSocket *to)
{
    if (_closing || !_tunnelling || to->state() != QAbstractSocket::ConnectedState) {
        return;
    }
    while (from->bytesAvailable() && to->bytesToWrite() < pendingLimit) {
        const auto count = qMin(chunkSize, pendingLimit - to->bytesToWrite());
        const auto data = from->read(count);
        if (data.isEmpty() || to->write(data) != data.size()) {
            close();
            return;
        }
    }
    if (from == _remote && from->state() == QAbstractSocket::UnconnectedState && from->bytesAvailable() == 0) {
        _client->disconnectFromHost();
    }
}

void PrivateCloudRelayConnection::close()
{
    if (_closing) {
        return;
    }
    _closing = true;
    _deadline.stop();
    _client->abort();
    _remote->abort();
    Q_EMIT finished();
}
}
