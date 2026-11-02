/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "privatecloudblockedreply.h"
#include <QTimer>

namespace OCC
{
PrivateCloudBlockedReply::PrivateCloudBlockedReply(QNetworkAccessManager::Operation operation,
                                                   const QNetworkRequest &request,
                                                   QNetworkReply::NetworkError error,
                                                   const QString &message,
                                                   QObject *parent)
    : QNetworkReply(parent)
{
    setOperation(operation);
    setRequest(request);
    setUrl(request.url());
    open(QIODevice::ReadOnly);
    setError(error, message);
    QTimer::singleShot(0, this, [this] {
        if (!isFinished()) {
            setFinished(true);
            Q_EMIT errorOccurred(this->error());
            Q_EMIT finished();
        }
    });
}

void PrivateCloudBlockedReply::abort()
{
    if (!isFinished()) {
        setError(QNetworkReply::OperationCanceledError, tr("Request canceled."));
    }
}

qint64 PrivateCloudBlockedReply::readData(char *, qint64)
{
    return -1;
}
}
