/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <QFile>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <functional>
#include <memory>

// Entirely local CONNECT/TLS peer: it never resolves or contacts a destination.
class PrivateCloudConnectTestServer final : public QTcpServer
{
public:
    explicit PrivateCloudConnectTestServer(const QString &certificateFile, const QString &keyFile)
    {
        auto certificateInput = QFile(certificateFile);
        if (certificateInput.open(QIODevice::ReadOnly)) {
            certificate = QSslCertificate(certificateInput.readAll());
        }
        auto keyInput = QFile(keyFile);
        if (keyInput.open(QIODevice::ReadOnly)) {
            key = QSslKey(keyInput.readAll(), QSsl::Rsa);
        }
    }

    QSslCertificate certificate;
    QSslKey key;
    QList<QByteArray> authorities;
    QList<QByteArray> connectHeaders;
    QList<QByteArray> requests;
    int authenticatedConnections = 0;
    QByteArray expectedProxyAuthorization = "Basic " + QByteArray("privatecloud-session:test-session-nonce").toBase64();
    bool holdAfterConnect = false;
    std::function<QByteArray(const QByteArray &)> response = [](const QByteArray &) {
        return QByteArray("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK");
    };

protected:
    void incomingConnection(qintptr descriptor) override
    {
        auto *socket = new QSslSocket(this);
        if (!socket->setSocketDescriptor(descriptor)) {
            socket->deleteLater();
            return;
        }
        socket->setLocalCertificate(certificate);
        socket->setPrivateKey(key);
        socket->setPeerVerifyMode(QSslSocket::VerifyNone);
        auto buffer = std::make_shared<QByteArray>();
        auto tunneled = std::make_shared<bool>(false);
        connect(socket, &QSslSocket::readyRead, socket, [this, socket, buffer, tunneled] {
            buffer->append(socket->readAll());
            const auto end = buffer->indexOf("\r\n\r\n");
            if (end < 0) {
                return;
            }
            if (!*tunneled) {
                const auto header = buffer->left(end + 4);
                connectHeaders.append(header);
                buffer->remove(0, end + 4);
                authorities.append(header.left(header.indexOf("\r\n")));
                const QByteArray authorizationHeader = "Proxy-Authorization: " + expectedProxyAuthorization + "\r\n";
                if (!header.contains(authorizationHeader)) {
                    socket->write(
                        "HTTP/1.1 407 Proxy Authentication Required\r\nProxy-Authenticate: Basic realm=\"privateCloud\"\r\nContent-Length: 0\r\n\r\n");
                    return;
                }
                ++authenticatedConnections;
                *tunneled = true;
                socket->write("HTTP/1.1 200 Connection Established\r\n\r\n");
                socket->flush();
                if (!holdAfterConnect) {
                    socket->startServerEncryption();
                }
                return;
            }
            if (!socket->isEncrypted()) {
                return;
            }
            const auto request = *buffer;
            buffer->clear();
            requests.append(request);
            socket->write(response(request));
            socket->disconnectFromHost();
        });
    }
};
