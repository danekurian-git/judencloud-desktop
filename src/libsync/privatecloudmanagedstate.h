/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "owncloudlib.h"
#include <QList>
#include <QString>
#include <QStringList>

namespace OCC
{

// The lock is separate from account settings, so removing an account cannot erase it.
class OWNCLOUDSYNC_EXPORT PrivateCloudManagedState
{
public:
    enum class Status {
        Pending,
        Completed,
        Failed,
    };
    struct Root {
        QString path;
        QString identity;
    };
    struct Record {
        QString server;
        QString user;
        Status status = Status::Pending;
        QList<Root> roots;
        QString error;
    };

    explicit PrivateCloudManagedState(QString storageDirectory = {}, QString fixedServer = {});

    [[nodiscard]] bool isBlocked(const QString &server, const QString &canonicalUser) const;
    [[nodiscard]] QString read(const QString &server, const QString &canonicalUser, Record *record) const;
    // Only call after receiving an authenticated remote-wipe authorization. Success means
    // the reconnect lock and exact directory identities have been committed to disk.
    // An unsafe root persists a failed lock and returns an error, preventing cleanup.
    [[nodiscard]] QString
    begin(const QString &server, const QString &canonicalUser, const QStringList &trustedRoots, const QStringList &protectedPaths = {}) const;
    [[nodiscard]] QString finish(const QString &server, const QString &canonicalUser, bool success, const QString &error = {}) const;

    // Call again after native operations drain and the provider is disconnected. This
    // checks the root itself; a deletion worker must independently avoid child redirects.
    [[nodiscard]] static QString validateRoot(const Root &root, const QStringList &protectedPaths = {});
    [[nodiscard]] QString storageDirectory() const;

private:
    [[nodiscard]] QString identity(const QString &server, const QString &canonicalUser, QString *canonicalServer, QString *recordPath) const;
    [[nodiscard]] QString write(const QString &recordPath, const Record &record) const;
    QString _storageDirectory;
    QString _fixedServer;
};

}
