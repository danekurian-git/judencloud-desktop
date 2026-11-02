/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "owncloudlib.h"
#include <QMutex>
#include <QNetworkProxy>
#include <QObject>
#include <QUrl>

class QNetworkAccessManager;

namespace OCC
{

// Session-only routing policy. Bootstrap enables it before creating accounts and
// provisions the unelevated, fixed-destination CONNECT relay. Never persisted.
class OWNCLOUDSYNC_EXPORT PrivateCloudTransport final : public QObject
{
    Q_OBJECT
public:
    static PrivateCloudTransport *instance();
    static void enableRequired();
    [[nodiscard]] static bool setEndpoint(quint16 port, const QString &username, const QString &nonce);
    static void clearEndpoint();
    [[nodiscard]] static bool isRequired();
    [[nodiscard]] static bool isReady();
    [[nodiscard]] static bool allows(const QUrl &url);
    [[nodiscard]] static bool allowsWebSocket(const QUrl &url);
    [[nodiscard]] static bool allowsLocalResource(const QUrl &url);
    [[nodiscard]] static QNetworkProxy proxy();
    // AccessManager enforces destination/unavailable failures before sending.
    static void apply(QNetworkAccessManager *manager);

Q_SIGNALS:
    void configurationChanged();

private:
    PrivateCloudTransport() = default;
    static bool allowsScheme(const QUrl &url, const QString &scheme);
    QMutex _mutex;
    bool _required = false;
    QNetworkProxy _proxy{QNetworkProxy::NoProxy};
};
}
