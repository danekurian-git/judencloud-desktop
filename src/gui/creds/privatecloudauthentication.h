/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "accountfwd.h"
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <functional>

namespace OCC
{
class AbstractCredentials;
class AbstractNetworkJob;

// Validates credentials on a temporary account. Never saves them or modifies a
// registered account; the caller commits the result after succeeded().
class PrivateCloudAuthentication : public QObject
{
    Q_OBJECT
public:
    explicit PrivateCloudAuthentication(AccountPtr account, QObject *parent = nullptr);
    ~PrivateCloudAuthentication() override;
    void start(const QString &user, const QString &password, const QString &expectedDavUser = {});
    void cancel();

Q_SIGNALS:
    void succeeded();
    void failed(const QString &message);

private:
    friend class PrivateCloudAuthenticationTestAccess;
    void fetchIdentity(quint64 generation);
    void fetchAppPassword(quint64 generation);
    void verifyDav(quint64 generation);
    void prepareJob(AbstractNetworkJob *job);
    void fail(const QString &message);
    [[nodiscard]] bool isCurrent(quint64 generation) const;
    [[nodiscard]] static QString accessError();

    AccountPtr _account;
    QPointer<AbstractNetworkJob> _job;
    QTimer _deadline;
    QString _expectedDavUser;
    quint64 _generation = 0;
    bool _running = false;
    std::function<AbstractCredentials *(const QString &, const QString &)> _credentialsFactory;
};
}
