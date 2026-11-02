/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <QHostAddress>
#include <QNetworkProxy>
#include <QObject>
#include <QSet>
#include <QTcpServer>
#include <functional>

namespace OCC
{
class PrivateCloudRelayConnection;

class PrivateCloudConnectRelay : public QObject
{
    Q_OBJECT
public:
    explicit PrivateCloudConnectRelay(QObject *parent = nullptr);
    ~PrivateCloudConnectRelay() override;
    bool start(const QHostAddress &server, quint32 interfaceIndex, const QHostAddress &source);
    void stop();
    [[nodiscard]] QNetworkProxy proxy() const;
    [[nodiscard]] qsizetype activeConnections() const;

private:
    friend class PrivateCloudRelayTestAccess;
    void acceptConnections();
    QTcpServer _listener;
    QSet<PrivateCloudRelayConnection *> _connections;
    QString _password;
    std::function<bool(QTcpSocket *)> _connector;
};
}
