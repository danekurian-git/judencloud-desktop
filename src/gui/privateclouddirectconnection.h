/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include "privatecloudconnectrelay.h"
#include <QElapsedTimer>
#include <QJsonObject>
#include <QLocalSocket>
#include <QPointer>
#include <QTimer>
#include <functional>

namespace OCC
{

// Inactive until start() is explicitly called by application bootstrap.
class PrivateCloudDirectConnection final : public QObject
{
    Q_OBJECT
public:
    explicit PrivateCloudDirectConnection(QObject *parent = nullptr);
    ~PrivateCloudDirectConnection() override;
    void start();
    void stop();
    [[nodiscard]] bool isReady() const;
    [[nodiscard]] QString lastError() const;

Q_SIGNALS:
    void readyChanged(bool ready);
    void errorChanged(const QString &error);

private:
    friend class PrivateCloudDirectConnectionTestAccess;
    void attemptConnection();
    void openBrokerPipe(quint64 generation);
    void adoptPipe(QLocalSocket *socket, quint64 generation);
    void sendRequest(bool renewal);
    void readReply();
    void acceptReply(const QJsonObject &reply);
    void renewLease();
    void expireLease();
    void fail(const QString &error);
    void closeSession();
    static bool validLease(const QJsonObject &reply);

    PrivateCloudConnectRelay _relay;
    QPointer<QLocalSocket> _pipe;
    QPointer<QObject> _openWatcher;
    QTimer _retryTimer;
    QTimer _responseTimer;
    QTimer _renewalTimer;
    QTimer _expiryTimer;
    QElapsedTimer _clock;
    std::function<qint64()> _now;
    std::function<void(quint64)> _openPipe;
    QByteArray _reply;
    QJsonObject _lease;
    QString _lastError;
    qint64 _expiresAt = 0;
    qint64 _requestAt = 0;
    quint64 _generation = 0;
    int _retryDelayMs = 1000;
    bool _active = false;
    bool _ready = false;
    bool _awaitingReply = false;
};
}
