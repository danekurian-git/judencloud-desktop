/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include "folder.h"
#include <QUrl>
#include <functional>

namespace OCC
{
class PrivateCloudFolderAdoption
{
public:
    struct Result {
        bool found = false;
        QString error;
        FolderDefinition definition;
        QString profilePath;
        QStringList profilePaths;
    };

    static QStringList defaultProfilePaths();
    static bool stockClientRunning();
    static Result inspect(const AccountPtr &account,
                          const QString &path,
                          const QStringList &profiles = defaultProfilePaths(),
                          const std::function<bool()> &otherClientRunning = stockClientRunning);
    // Revalidates admission and preserves the journal, its sidecars and source profile before adoption.
    static QString prepare(const AccountPtr &account,
                           const Result &adoption,
                           const QString &backupRoot = {},
                           const std::function<bool()> &otherClientRunning = stockClientRunning);

private:
    static QUrl normalizedServer(QUrl url);
    static QString normalizedPath(const QString &path);
};
}
