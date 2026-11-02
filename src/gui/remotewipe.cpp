/*
 * SPDX-FileCopyrightText: 2019 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "remotewipe.h"
#include "accessmanager.h"
#include "folderman.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace OCC
{

Q_LOGGING_CATEGORY(lcRemoteWipe, "nextcloud.gui.remotewipe", QtInfoMsg)

RemoteWipe::RemoteWipe(AccountPtr account, QObject *parent)
    : QObject(parent)
    , _account(std::move(account))
    , _networkManager(new AccessManager(this))
{
    constexpr auto requestTimeoutMs = 30000;
    _checkDeadline.setSingleShot(true);
    _checkDeadline.setInterval(requestTimeoutMs);
    _successDeadline.setSingleShot(true);
    _successDeadline.setInterval(requestTimeoutMs);
    connect(&_checkDeadline, &QTimer::timeout, this, [this] {
        cancelCheck();
        if (const auto state = currentAccountState()) {
            state->handleInvalidCredentials();
        }
    });
    connect(&_successDeadline, &QTimer::timeout, this, [this] {
        const auto reply = _networkReplySuccess;
        _networkReplySuccess.clear();
        if (reply) {
            reply->disconnect(this);
            reply->abort();
            reply->deleteLater();
        }
        qCWarning(lcRemoteWipe) << "Remote wipe completion notification timed out";
    });
    connect(AccountManager::instance(), &AccountManager::accountRemoved, this, [this](AccountState *state) {
        if (state && state->account() == _account) {
            // Shutdown and ordinary account removal are not proof of a wipe.
            cancelCheck();
        }
    });
    connect(_networkManager, &QNetworkAccessManager::sslErrors, _account.data(), &Account::slotHandleSslErrors);
    if (FolderMan::instance()) {
        _canWipeLocalFiles = true;
        connect(this, &RemoteWipe::authorized, FolderMan::instance(), &FolderMan::slotWipeFolderForAccount);
        connect(FolderMan::instance(), &FolderMan::wipeDone, this, &RemoteWipe::slotWipeDone);
    }
    connect(_account.data(), &Account::appPasswordRetrieved, this, &RemoteWipe::startCheckJobWithAppPassword);
}

AccountStatePtr RemoteWipe::currentAccountState() const
{
    for (const auto &state : AccountManager::instance()->accounts()) {
        if (state->account() == _account) {
            return state;
        }
    }
    return {};
}

QNetworkReply *RemoteWipe::postToken(const QString &endpoint)
{
    auto request = QNetworkRequest(Utility::concatUrlPath(_account->url().toString(), endpoint));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setSslConfiguration(_account->getOrCreateSslConfig());
    // Percent encoding also protects '&', '=' and '+' in a token from form parsing.
    return _networkManager->post(request, QByteArrayLiteral("token=") + QUrl::toPercentEncoding(_appPassword));
}

void RemoteWipe::cancelCheck()
{
    _checkDeadline.stop();
    const auto reply = _networkReplyCheck;
    _networkReplyCheck.clear();
    if (reply) {
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
}

void RemoteWipe::startCheckJobWithAppPassword(QString pwd)
{
    if (pwd.isEmpty() || _networkReplyCheck || _wipeAuthorized || !currentAccountState()) {
        return;
    }
    _appPassword = std::move(pwd);
    _checkUrl = _account->url();
    _checkAccountId = _account->id();
    _checkDavUser = _account->davUser();
    auto *reply = postToken(QStringLiteral("/index.php/core/wipe/check"));
    _networkReplyCheck = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        slotCheckJob(reply);
    });
    _checkDeadline.start();
}

void RemoteWipe::slotCheckJob(QNetworkReply *reply)
{
    if (_networkReplyCheck != reply) {
        return;
    }
    _checkDeadline.stop();
    _networkReplyCheck.clear();
    reply->deleteLater();
    const auto accountState = currentAccountState();
    if (!accountState || _account->url() != _checkUrl || _account->id() != _checkAccountId || _account->davUser() != _checkDavUser) {
        return;
    }
    auto parseError = QJsonParseError{};
    const auto document = QJsonDocument::fromJson(reply->readAll(), &parseError);
    const auto wipeValue = document.object().value(QStringLiteral("wipe"));
    const auto wipe = reply->error() == QNetworkReply::NoError && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200
        && parseError.error == QJsonParseError::NoError && document.isObject() && wipeValue.isBool() && wipeValue.toBool();
    if (!wipe) {
        _appPassword.clear();
        accountState->handleInvalidCredentials();
        return;
    }
    _wipeAuthorized = true;
    _account->setRemoteWipeRequested_HACK();
    Q_EMIT authorized(accountState.data());
    if (!_canWipeLocalFiles) {
        slotWipeDone(accountState.data(), true);
    }
}

void RemoteWipe::slotWipeDone(AccountState *accountState, bool dataWiped)
{
    const auto current = currentAccountState();
    if (!_wipeAuthorized || _wipeCompleted || !dataWiped || !current || current.data() != accountState) {
        return;
    }
    auto *manager = AccountManager::instance();
    manager->deleteAccount(accountState);
    manager->save();
    _wipeCompleted = true;
    notifyServerSuccess();
}

void RemoteWipe::notifyServerSuccess()
{
    if (!_wipeCompleted || _networkReplySuccess || _acknowledged) {
        return;
    }
    auto *reply = postToken(QStringLiteral("/index.php/core/wipe/success"));
    _networkReplySuccess = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        slotNotifyServerSuccessFinished(reply);
    });
    _successDeadline.start();
}

void RemoteWipe::slotNotifyServerSuccessFinished(QNetworkReply *reply)
{
    if (_networkReplySuccess != reply) {
        return;
    }
    _successDeadline.stop();
    _networkReplySuccess.clear();
    reply->deleteLater();
    _acknowledged = reply->error() == QNetworkReply::NoError && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200;
    if (_acknowledged) {
        _appPassword.clear();
    } else {
        qCWarning(lcRemoteWipe) << "Remote wipe completion notification failed";
    }
}
}
