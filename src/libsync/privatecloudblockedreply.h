/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <QNetworkReply>

namespace OCC
{
class PrivateCloudBlockedReply final : public QNetworkReply
{
public:
    PrivateCloudBlockedReply(QNetworkAccessManager::Operation operation,
                             const QNetworkRequest &request,
                             QNetworkReply::NetworkError error,
                             const QString &message,
                             QObject *parent);
    void abort() override;

protected:
    qint64 readData(char *, qint64) override;
};
}
