/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "privatecloudtransport.h"
#include "account.h"
#include <QMutexLocker>
#include <QNetworkAccessManager>

namespace OCC
{
PrivateCloudTransport *PrivateCloudTransport::instance()
{
    static PrivateCloudTransport policy;
    return &policy;
}

void PrivateCloudTransport::enableRequired()
{
    const auto policy = instance();
    {
        const auto lock = QMutexLocker(&policy->_mutex);
        if (policy->_required) {
            return;
        }
        policy->_required = true;
    }
    Q_EMIT policy->configurationChanged();
}

bool PrivateCloudTransport::setEndpoint(quint16 port, const QString &username, const QString &nonce)
{
    if (port == 0 || username.isEmpty() || username.contains(QLatin1Char(':')) || nonce.isEmpty()) {
        clearEndpoint();
        return false;
    }
    const auto policy = instance();
    {
        const auto lock = QMutexLocker(&policy->_mutex);
        policy->_proxy = QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"), port, username, nonce);
    }
    Q_EMIT policy->configurationChanged();
    return true;
}

void PrivateCloudTransport::clearEndpoint()
{
    const auto policy = instance();
    {
        const auto lock = QMutexLocker(&policy->_mutex);
        policy->_proxy = QNetworkProxy(QNetworkProxy::NoProxy);
    }
    Q_EMIT policy->configurationChanged();
}

bool PrivateCloudTransport::isRequired()
{
    const auto policy = instance();
    const auto lock = QMutexLocker(&policy->_mutex);
    return policy->_required;
}

bool PrivateCloudTransport::isReady()
{
    return proxy().type() == QNetworkProxy::HttpProxy;
}

QNetworkProxy PrivateCloudTransport::proxy()
{
    const auto policy = instance();
    const auto lock = QMutexLocker(&policy->_mutex);
    return policy->_proxy;
}

bool PrivateCloudTransport::allowsScheme(const QUrl &url, const QString &scheme)
{
    if (!isRequired()) {
        return true;
    }
    const auto server = Account::enforcedServerUrl();
    return server.isValid() && server.scheme() == QStringLiteral("https") && !server.host().isEmpty() && server.port(443) == 443 && url.isValid()
        && !url.isRelative() && url.scheme() == scheme && url.host() == server.host() && url.port(443) == 443
        && !url.authority(QUrl::FullyEncoded).contains(QLatin1Char('@'));
}

bool PrivateCloudTransport::allows(const QUrl &url)
{
    return allowsScheme(url, QStringLiteral("https"));
}

bool PrivateCloudTransport::allowsWebSocket(const QUrl &url)
{
    return allowsScheme(url, QStringLiteral("wss"));
}

bool PrivateCloudTransport::allowsLocalResource(const QUrl &url)
{
    if (!url.isValid()) {
        return false;
    }
    if (url.scheme() == QStringLiteral("qrc") || url.scheme() == QStringLiteral("data")) {
        return true;
    }
    if (url.scheme() != QStringLiteral("file") || !url.authority().isEmpty()) {
        return false;
    }
    auto path = url.toLocalFile();
#ifdef Q_OS_WIN
    // QDir::fromNativeSeparators also removes Windows device/long-path prefixes.
    // Preserve them here so they cannot become an admitted drive-qualified path.
    path.replace(u'\\', u'/');
    // A file URL can name a UNC share or device namespace. Only a drive-qualified
    // absolute path belongs to the local-resource exception on Windows.
    return path.size() >= 3 && ((path.at(0) >= u'A' && path.at(0) <= u'Z') || (path.at(0) >= u'a' && path.at(0) <= u'z'))
        && path.mid(1, 2) == QStringLiteral(":/");
#else
    return path.startsWith(u'/') && !path.startsWith(QStringLiteral("//"));
#endif
}

void PrivateCloudTransport::apply(QNetworkAccessManager *manager)
{
    if (manager && isRequired()) {
        manager->setProxy(proxy());
    }
}
}
