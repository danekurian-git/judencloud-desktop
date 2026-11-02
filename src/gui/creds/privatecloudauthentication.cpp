/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "privatecloudauthentication.h"
#include "account.h"
#include "creds/credentialsfactory.h"
#include "creds/webflowcredentials.h"
#include "networkjobs.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>

namespace OCC
{

PrivateCloudAuthentication::PrivateCloudAuthentication(AccountPtr account, QObject *parent)
    : QObject(parent)
    , _account(std::move(account))
    , _credentialsFactory([](const QString &user, const QString &password) {
        return new WebFlowCredentials(user, password);
    })
{
    constexpr auto authenticationTimeoutMs = 30000;
    _deadline.setInterval(authenticationTimeoutMs);
    _deadline.setSingleShot(true);
    connect(&_deadline, &QTimer::timeout, this, [this] {
        fail(tr("Sign-in timed out. Check your connection and try again."));
    });
}

PrivateCloudAuthentication::~PrivateCloudAuthentication()
{
    cancel();
}

void PrivateCloudAuthentication::start(const QString &user, const QString &password, const QString &expectedDavUser)
{
    cancel();
    _running = true;
    const auto generation = ++_generation;
    _expectedDavUser = expectedDavUser;
    if (!_account || _account->url().scheme() != QStringLiteral("https") || !Account::isServerUrlAllowed(_account->url())) {
        fail(tr("Sign-in requires the configured HTTPS server."));
        return;
    }
    if (user.trimmed().isEmpty() || password.isEmpty()) {
        fail(tr("Enter both your username and password."));
        return;
    }
    _account->setCredentials(_credentialsFactory(user.trimmed(), password));
    _account->clearCookieJar();
    _deadline.start();
    auto *job = new CheckServerJob(_account, this);
    prepareJob(job);
    connect(job, &CheckServerJob::instanceFound, this, [this, generation](const QUrl &url, const QJsonObject &info) {
        if (!isCurrent(generation)) {
            return;
        }
        if (!Account::isServerUrlAllowed(url)) {
            fail(tr("The sign-in response came from a different server."));
            return;
        }
        _account->setServerVersion(CheckServerJob::version(info));
        QTimer::singleShot(0, this, [this, generation] {
            if (isCurrent(generation)) {
                fetchIdentity(generation);
            }
        });
    });
    connect(job, &CheckServerJob::instanceNotFound, this, [this, generation] {
        if (isCurrent(generation)) {
            fail(tr("Could not connect to privateCloud. Check your connection and try again."));
        }
    });
    connect(job, &CheckServerJob::timeout, this, [this, generation] {
        if (isCurrent(generation)) {
            fail(tr("Sign-in timed out. Check your connection and try again."));
        }
    });
    job->start();
}

void PrivateCloudAuthentication::prepareJob(AbstractNetworkJob *job)
{
    _job = job;
    job->setIgnoreCredentialFailure(true);
    job->setFollowRedirects(false);
    job->setTimeout(_deadline.interval());
}

bool PrivateCloudAuthentication::isCurrent(quint64 generation) const
{
    return _running && generation == _generation;
}

QString PrivateCloudAuthentication::accessError()
{
    return tr(
        "Sign-in failed. Check your username and password. If your account uses two-factor authentication or single sign-on, enter an app password from your "
        "account's Security settings.");
}

void PrivateCloudAuthentication::fetchIdentity(quint64 generation)
{
    auto *job = new SimpleApiJob(_account, QStringLiteral("/ocs/v1.php/cloud/user"), this);
    prepareJob(job);
    job->addQueryParams(QUrlQuery(QStringLiteral("format=json")));
    connect(job, &SimpleApiJob::resultReceived, this, [this, job, generation](int status) {
        if (!isCurrent(generation)) {
            return;
        }
        const auto ocs = QJsonDocument::fromJson(job->reply()->readAll()).object().value(QStringLiteral("ocs")).toObject();
        const auto meta = ocs.value(QStringLiteral("meta")).toObject().value(QStringLiteral("statuscode")).toInt();
        const auto data = ocs.value(QStringLiteral("data")).toObject();
        const auto userId = data.value(QStringLiteral("id")).toString();
        if (status != 200 || (meta != 100 && meta != 200) || userId.isEmpty()) {
            fail(accessError());
            return;
        }
        if (!_expectedDavUser.isEmpty() && userId != _expectedDavUser) {
            fail(tr("These credentials belong to a different account. Sign out before changing accounts."));
            return;
        }
        _account->setDavUser(userId);
        _account->setDavDisplayName(data.value(QStringLiteral("display-name")).toString());
        QTimer::singleShot(0, this, [this, generation] {
            if (isCurrent(generation)) {
                fetchAppPassword(generation);
            }
        });
    });
    job->start();
}

void PrivateCloudAuthentication::fetchAppPassword(quint64 generation)
{
    auto *job = new SimpleApiJob(_account, QStringLiteral("/ocs/v2.php/core/getapppassword"), this);
    prepareJob(job);
    job->addQueryParams(QUrlQuery(QStringLiteral("format=json")));
    connect(job, &SimpleApiJob::resultReceived, this, [this, job, generation](int status) {
        if (!isCurrent(generation)) {
            return;
        }
        // The documented endpoint returns 403 when the supplied credential is
        // already an app password. Identity was authenticated before this call.
        QString appPassword;
        if (status != 403) {
            const auto ocs = QJsonDocument::fromJson(job->reply()->readAll()).object().value(QStringLiteral("ocs")).toObject();
            const auto meta = ocs.value(QStringLiteral("meta")).toObject().value(QStringLiteral("statuscode")).toInt();
            const auto token = ocs.value(QStringLiteral("data")).toObject().value(QStringLiteral("apppassword")).toString();
            if (status != 200 || meta != 200 || token.isEmpty()) {
                fail(tr("Could not create an app password. Create one in your account's Security settings and use it to sign in."));
                return;
            }
            appPassword = token;
        }
        // Replacing credentials also replaces QNAM. Let this reply finish first.
        QTimer::singleShot(0, this, [this, generation, appPassword] {
            if (!isCurrent(generation)) {
                return;
            }
            if (!appPassword.isEmpty()) {
                const auto user = _account->credentials()->user();
                _account->setCredentials(_credentialsFactory(user, appPassword));
            }
            _account->clearCookieJar();
            verifyDav(generation);
        });
    });
    job->start();
}

void PrivateCloudAuthentication::verifyDav(quint64 generation)
{
    auto *job = new PropfindJob(_account, QStringLiteral("/"), this);
    prepareJob(job);
    job->setProperties({QByteArrayLiteral("getlastmodified")});
    connect(job, &PropfindJob::result, this, [this, generation] {
        if (!isCurrent(generation)) {
            return;
        }
        QTimer::singleShot(0, this, [this, generation] {
            if (!isCurrent(generation)) {
                return;
            }
            _running = false;
            _deadline.stop();
            _job.clear();
            _account->setCredentialSetting(QStringLiteral("user"), _account->credentials()->user());
            Q_EMIT succeeded();
        });
    });
    connect(job, &PropfindJob::finishedWithError, this, [this, generation] {
        if (isCurrent(generation)) {
            fail(accessError());
        }
    });
    job->start();
}

void PrivateCloudAuthentication::cancel()
{
    if (!_running) {
        return;
    }
    _running = false;
    ++_generation;
    _deadline.stop();
    if (_job) {
        _job->disconnect(this);
        if (_job->reply()) {
            _job->reply()->abort();
        }
        _job->deleteLater();
        _job.clear();
    }
    if (_account) {
        _account->setCredentials(CredentialsFactory::create(QStringLiteral("dummy")));
        _account->clearCookieJar();
    }
}

void PrivateCloudAuthentication::fail(const QString &message)
{
    if (!_running) {
        return;
    }
    // Invalidate callbacks now; clean up after the active job's signal returns.
    const auto generation = ++_generation;
    _deadline.stop();
    if (_job) {
        _job->disconnect(this);
    }
    QTimer::singleShot(0, this, [this, generation, message] {
        if (!isCurrent(generation)) {
            return;
        }
        cancel();
        Q_EMIT failed(message);
    });
}
}
