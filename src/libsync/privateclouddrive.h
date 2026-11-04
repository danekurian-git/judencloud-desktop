/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "owncloudlib.h"
#include <QString>
#include <QStringList>
#include <QVariantMap>

class QSettings;

namespace OCC::PrivateCloudDrive
{

OWNCLOUDSYNC_EXPORT bool isEnabled();
OWNCLOUDSYNC_EXPORT QString driveRoot();
// New setup only. Existing installations retain their adopted or account-specific path.
OWNCLOUDSYNC_EXPORT QString defaultSetupPath();
// accountIdentity is the server URL, a newline, then the server's canonical DAV user ID.
OWNCLOUDSYNC_EXPORT QString backingPath(const QString &accountIdentity, QString *error = nullptr);
// Existing folders require prior profile/account verification and a journal backup by the setup wizard.
OWNCLOUDSYNC_EXPORT QString configureBackingPath(const QString &accountIdentity, const QString &path, bool reuseVerifiedFolder = false);
OWNCLOUDSYNC_EXPORT QString adoptedBackingPathError(const QString &accountIdentity, const QString &path);
OWNCLOUDSYNC_EXPORT QString checkAvailability(const QString &path);
// Only checks the current alias against a previously validated backing path.
OWNCLOUDSYNC_EXPORT bool isMapped(const QString &path);
OWNCLOUDSYNC_EXPORT QString mount(const QString &path);
OWNCLOUDSYNC_EXPORT QString unmountChecked(const QString &path);
OWNCLOUDSYNC_EXPORT void unmount(const QString &path);

namespace Detail
{
// Isolates Windows device and registry operations for deterministic lifecycle tests.
class Backend
{
public:
    virtual ~Backend() = default;
    virtual QString validatePath(const QString &path, bool mustExist, QString *target) = 0;
    virtual QString emptyDirectoryError(const QString &path) = 0;
    virtual QString mappings(QStringList *targets) = 0;
    virtual QString addMapping(const QString &target) = 0;
    virtual QString removeMapping(const QString &target) = 0;
    virtual bool isFixedDrive() = 0;
    virtual QVariant label() = 0;
    virtual QString setLabel(const QVariant &value) = 0;
    virtual QVariantMap savedState() = 0;
    virtual QString saveState(const QVariantMap &state) = 0;
};

OWNCLOUDSYNC_EXPORT QString checkAvailability(const QString &path, Backend &backend);
OWNCLOUDSYNC_EXPORT bool isMapped(const QString &path, Backend &backend);
OWNCLOUDSYNC_EXPORT QString mount(const QString &path, Backend &backend);
OWNCLOUDSYNC_EXPORT QString unmount(const QString &path, Backend &backend);
OWNCLOUDSYNC_EXPORT QString backingPath(const QString &accountIdentity, const QVariantMap &adoptedRoot, Backend &backend, QString *error = nullptr);
OWNCLOUDSYNC_EXPORT QString
configureBackingPath(const QString &accountIdentity, const QString &path, Backend &backend, QSettings &settings, bool reuseVerifiedFolder = false);
}
}
