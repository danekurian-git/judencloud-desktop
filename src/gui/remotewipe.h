/*
 * SPDX-FileCopyrightText: 2019 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef REMOTEWIPE_H
#define REMOTEWIPE_H

#include "accountmanager.h"
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>

class QJsonDocument;
class TestRemoteWipe;

namespace OCC {

class RemoteWipe : public QObject
{
    Q_OBJECT
public:
    explicit RemoteWipe(AccountPtr account, QObject *parent = nullptr);

Q_SIGNALS:
    /**
     * Notify if wipe was requested
     */
    void authorized(OCC::AccountState*);

    /**
     * Notify if user only needs to login again
     */
    void askUserCredentials();

public Q_SLOTS:
    /**
     * Once receives a 401 or 403 status response it will do a fetch to
     * <server>/index.php/core/wipe/check
     */
    void startCheckJobWithAppPassword(QString);

private Q_SLOTS:
    /**
     * If wipe is requested, delete account and data, if not continue by asking
     * the user to login again
     */
    void slotCheckJob(QNetworkReply *reply);

    /**
     * Local sync folders were wiped, this will now remove the account.
     */
    void slotWipeDone(OCC::AccountState *accountState, bool);

    /**
     * Once the client has wiped all the required data a POST to
     * <server>/index.php/core/wipe/success
     */
    void notifyServerSuccess();
    void slotNotifyServerSuccessFinished(QNetworkReply *reply);


private:
    AccountPtr _account;
    QString _appPassword;
    bool _canWipeLocalFiles = false;
    bool _wipeAuthorized = false;
    bool _wipeCompleted = false;
    bool _acknowledged = false;
    QTimer _checkDeadline;
    QTimer _successDeadline;
    QUrl _checkUrl;
    QString _checkAccountId;
    QString _checkDavUser;
    QNetworkAccessManager *_networkManager = nullptr;
    QPointer<QNetworkReply> _networkReplyCheck;
    QPointer<QNetworkReply> _networkReplySuccess;
    [[nodiscard]] AccountStatePtr currentAccountState() const;
    void cancelCheck();
    [[nodiscard]] QNetworkReply *postToken(const QString &endpoint);

    friend class ::TestRemoteWipe;
};
}
#endif // REMOTEWIPE_H
