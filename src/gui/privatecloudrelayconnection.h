/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <QObject>
#include <QTimer>
#include <functional>

class QTcpSocket;

namespace OCC
{

class PrivateCloudRelayConnection : public QObject
{
    Q_OBJECT
public:
    using Connector = std::function<bool(QTcpSocket *)>;
    PrivateCloudRelayConnection(QTcpSocket *client, QByteArray authority, QByteArray authorization, Connector connector, QObject *parent);
    void close();

Q_SIGNALS:
    void finished();

private:
    void readRequest();
    void reject(int status);
    void pump(QTcpSocket *from, QTcpSocket *to);
    QTcpSocket *_client;
    QTcpSocket *_remote;
    QByteArray _authority;
    QByteArray _authorization;
    QByteArray _request;
    Connector _connector;
    QTimer _deadline;
    bool _connecting = false;
    bool _tunnelling = false;
    bool _closing = false;
};
}
