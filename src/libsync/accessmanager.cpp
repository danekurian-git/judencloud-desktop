/*
 * SPDX-FileCopyrightText: 2018 Nextcloud GmbH and Nextcloud contributors
 * SPDX-FileCopyrightText: 2014 ownCloud GmbH
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QAuthenticator>
#include <QLoggingCategory>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkInformation>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslConfiguration>
#include <QUuid>

#include <QPointer>

#include "accessmanager.h"
#include "common/utility.h"
#include "cookiejar.h"
#include "httplogger.h"
#include "privatecloudblockedreply.h"
#include "privatecloudtransport.h"

using namespace Qt::StringLiterals;

namespace OCC {

Q_LOGGING_CATEGORY(lcAccessManager, "nextcloud.sync.accessmanager", QtInfoMsg)

AccessManager::AccessManager(QObject *parent)
    : QNetworkAccessManager(parent)
{
    setCookieJar(new CookieJar);
    PrivateCloudTransport::apply(this);
    connect(PrivateCloudTransport::instance(), &PrivateCloudTransport::configurationChanged, this, [this] {
        if (!PrivateCloudTransport::isRequired()) {
            return;
        }
        auto replies = QList<QPointer<QNetworkReply>>{};
        for (const auto reply : findChildren<QNetworkReply *>(QString(), Qt::FindDirectChildrenOnly)) {
            replies.append(reply);
        }
        for (const auto &reply : replies) {
            if (reply && !reply->isFinished()) {
                reply->abort();
            }
        }
        clearConnectionCache();
        PrivateCloudTransport::apply(this);
    });

    // Enable HSTS (HTTP Strict Transport Security) for all connections
    enableStrictTransportSecurityStore(true);
    setStrictTransportSecurityEnabled(true);

    connect(this, &QNetworkAccessManager::authenticationRequired, this, [](QNetworkReply *reply, QAuthenticator *authenticator) {
        Q_UNUSED(reply)

        if (authenticator->user().isEmpty()) {
            qCWarning(lcAccessManager) << "Server requested authentication and we didn't provide a user";
            authenticator->setUser(QUuid::createUuid().toString());
        }
    });
}

QByteArray AccessManager::generateRequestId()
{
    return QUuid::createUuid().toByteArray(QUuid::WithoutBraces);
}

QNetworkReply *AccessManager::createRequest(QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *outgoingData)
{
    const auto opToText = [](QNetworkAccessManager::Operation op) -> QByteArray {
        auto result = QByteArray{};
        switch (op) {
        case QNetworkAccessManager::HeadOperation:
            result = "HEAD";
            break;
        case QNetworkAccessManager::GetOperation:
            result = "GET";
            break;
        case QNetworkAccessManager::PutOperation:
            result = "PUT";
            break;
        case QNetworkAccessManager::PostOperation:
            result = "POST";
            break;
        case QNetworkAccessManager::DeleteOperation:
            result = "DELETE";
            break;
        case QNetworkAccessManager::CustomOperation:
            result = "CUSTOM";
            break;
        case QNetworkAccessManager::UnknownOperation:
            result = "UNKNOWN";
            break;
        };

        return result;
    };

    QNetworkRequest newRequest(request);
    const auto localResource = PrivateCloudTransport::allowsLocalResource(request.url());
    if (PrivateCloudTransport::isRequired() && !localResource) {
        if (!PrivateCloudTransport::allows(request.url())) {
            return new PrivateCloudBlockedReply(op,
                                                request,
                                                QNetworkReply::ContentAccessDenied,
                                                tr("The privateCloud direct connection only permits its configured HTTPS server."),
                                                this);
        }
        const auto relayProxy = PrivateCloudTransport::proxy();
        if (relayProxy.type() != QNetworkProxy::HttpProxy) {
            return new PrivateCloudBlockedReply(op,
                                                request,
                                                QNetworkReply::ProxyConnectionRefusedError,
                                                tr("The privateCloud direct connection is unavailable. Retry after its local connection helper is ready."),
                                                this);
        }
        // Keep one endpoint snapshot: a concurrent clear must never turn this
        // request into NoProxy between checking availability and applying it.
        setProxy(relayProxy);
        // Redirects must return through this destination check, including jobs
        // that previously requested Qt's automatic redirect handling.
        newRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    }

    // Respect request specific user agent if any
    if (!newRequest.header(QNetworkRequest::UserAgentHeader).isValid()) {
        newRequest.setHeader(QNetworkRequest::UserAgentHeader, Utility::userAgentString());
    }

    // Some firewalls reject requests that have a "User-Agent" but no "Accept" header
    newRequest.setRawHeader(QByteArray("Accept"), "*/*");

    QByteArray verb = newRequest.attribute(QNetworkRequest::CustomVerbAttribute).toByteArray();
    // For PROPFIND (assumed to be a WebDAV op), set xml/utf8 as content type/encoding
    // This needs extension
    if (verb == "PROPFIND") {
        newRequest.setHeader(QNetworkRequest::ContentTypeHeader, QLatin1String("text/xml; charset=utf-8"));
    }

    // Generate a new request id
    QByteArray requestId = generateRequestId();
    qInfo(lcAccessManager) << opToText(op) << verb << newRequest.url().toString() << "has X-Request-ID" << requestId;
    newRequest.setRawHeader("X-Request-ID", requestId);

    if (newRequest.url().scheme() == "https") { // Not for "http": QTBUG-61397
        // http2 seems to cause issues, as with our recommended server setup we don't support http2, disable it by default for now
        static const bool http2EnabledEnv = qEnvironmentVariableIntValue("OWNCLOUD_HTTP2_ENABLED") == 1;

        newRequest.setAttribute(QNetworkRequest::Http2AllowedAttribute, http2EnabledEnv);
    }

    if (!newRequest.attribute(QNetworkRequest::RedirectPolicyAttribute).isValid()) {
        // We handle redirects ourselves in AbstractNetworkJob::slotFinished
        // Qt's automatic handling of redirects will transmit all set headers from the original
        // request again, including e.g. `Authorization`.
        newRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    }

    const auto reply = QNetworkAccessManager::createRequest(op, newRequest, outgoingData);
    HttpLogger::logRequest(reply, op, outgoingData);
    QObject::connect(reply, &QNetworkReply::requestSent, reply, [reply] {
        qCInfo(lcAccessManager()) << "Request has been sent" << reply->url().toString() << reply->request().rawHeader("X-Request-ID"_ba)
                                  << reply->header(QNetworkRequest::ContentTypeHeader).toString() << reply;
    });
    return reply;
}

} // namespace OCC
