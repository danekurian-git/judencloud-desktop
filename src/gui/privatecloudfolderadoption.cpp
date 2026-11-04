/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "privatecloudfolderadoption.h"
#include "account.h"
#include "common/ownsql.h"
#include "common/utility.h"
#include "configfile.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#endif

using namespace Qt::StringLiterals;

namespace OCC
{
QUrl PrivateCloudFolderAdoption::normalizedServer(QUrl url)
{
    if (!url.isValid() || url.isRelative() || url.scheme() != u"https" || url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasQuery()
        || url.hasFragment()) {
        return {};
    }
    url = url.adjusted(QUrl::NormalizePathSegments | QUrl::StripTrailingSlash);
    if (url.path() == u"/") {
        url.setPath({});
    }
    if (url.port() == 443) {
        url.setPort(-1);
    }
    return url;
}

QString PrivateCloudFolderAdoption::normalizedPath(const QString &path)
{
    return QDir::cleanPath(QFileInfo(QDir::fromNativeSeparators(path)).canonicalFilePath());
}

QStringList PrivateCloudFolderAdoption::defaultProfilePaths()
{
    const QDir appData(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    auto result = QStringList{ConfigFile().configFile(), QDir::cleanPath(appData.filePath(u"../Nextcloud/nextcloud.cfg"_s))};
    result.removeDuplicates();
    return result;
}

bool PrivateCloudFolderAdoption::stockClientRunning()
{
#ifdef Q_OS_WIN
    const Utility::UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.get() == INVALID_HANDLE_VALUE) {
        return true;
    }
    PROCESSENTRY32W process = {};
    process.dwSize = sizeof(process);
    if (!Process32FirstW(snapshot.get(), &process)) {
        return true;
    }
    do {
        const auto name = QString::fromWCharArray(process.szExeFile);
        if (process.th32ProcessID != GetCurrentProcessId()
            && (name.compare(u"nextcloud.exe"_s, Qt::CaseInsensitive) == 0 || name.compare(u"nextclouddev.exe"_s, Qt::CaseInsensitive) == 0
                || name.compare(u"privateCloud.exe"_s, Qt::CaseInsensitive) == 0)) {
            return true;
        }
    } while (Process32NextW(snapshot.get(), &process));
    return GetLastError() != ERROR_NO_MORE_FILES;
#else
    return false;
#endif
}

PrivateCloudFolderAdoption::Result PrivateCloudFolderAdoption::inspect(const AccountPtr &account,
                                                                       const QString &path,
                                                                       const QStringList &profiles,
                                                                       const std::function<bool()> &otherClientRunning)
{
    Result result;
    result.profilePaths = profiles;
    const QFileInfo selected(path);
    if (!selected.isDir()) {
        return result;
    }
    const auto root = normalizedPath(path);
    const auto journals = QDir(root).entryList({u".sync_*.db"_s, u"._sync_*.db"_s}, QDir::Files | QDir::Hidden);
    result.found = !journals.isEmpty();
    const auto fail = [&result](const char *message) {
        result.found = true;
        result.error = QCoreApplication::translate("PrivateCloudFolderAdoption", message);
        return result;
    };
    for (const auto &legacyName : {u".csync_journal.db"_s, u".csync_journal.db-wal"_s, u".csync_journal.db-shm"_s}) {
        if (QFileInfo::exists(QDir(root).filePath(legacyName))) {
            return fail("This folder contains an obsolete sync journal. Your files were kept. Contact your administrator before reusing it.");
        }
    }
    for (const auto &profile : profiles) {
        if (!QFileInfo::exists(profile)) {
            continue;
        }
        QSettings settings(profile, QSettings::IniFormat);
        if (settings.status() != QSettings::NoError) {
            if (result.found) {
                return fail("The existing sync configuration could not be read. Choose another folder or contact your administrator.");
            }
            continue;
        }
        settings.beginGroup(u"Accounts"_s);
        const auto accountsVersion = settings.value(u"version"_s, 1).toInt();
        for (const auto &accountId : settings.childGroups()) {
            settings.beginGroup(accountId);
            const auto server = normalizedServer(QUrl(settings.value(u"url"_s).toString()));
            const auto user = settings.value(u"dav_user"_s).toString();
            const auto accountVersion = settings.value(u"version"_s, 1).toInt();
            for (const auto &group : {u"FoldersWithPlaceholders"_s, u"Folders"_s}) {
                settings.beginGroup(group);
                for (const auto &alias : settings.childGroups()) {
                    settings.beginGroup(alias);
                    FolderDefinition definition;
                    FolderDefinition::load(settings, alias, &definition);
                    const auto version = settings.value(u"version"_s, 1).toInt();
                    const auto vfsMode = settings.value(u"virtualFilesMode"_s).toString();
                    settings.endGroup();
                    if (definition.localPath.isEmpty() || normalizedPath(definition.localPath).compare(root, Qt::CaseInsensitive) != 0) {
                        continue;
                    }
                    result.found = true;
                    if (!account || account->davUser().isEmpty() || server.isEmpty() || server != normalizedServer(account->url()) || user != account->davUser()
                        || !Account::isServerUrlAllowed(server)) {
                        return fail("This sync folder belongs to a different server or account. Choose another folder.");
                    }
                    if (accountsVersion < 1 || accountsVersion > 13 || accountVersion < 1 || accountVersion > 13 || version < 1
                        || version > FolderDefinition::maxSettingsVersion() || definition.targetPath != u"/"
                        || (!vfsMode.isEmpty() && !Vfs::modeFromString(vfsMode))
                        || (definition.virtualFilesMode != Vfs::WindowsCfApi && definition.virtualFilesMode != Vfs::Off)) {
                        return fail("This existing sync setup cannot be reused by privateCloud. Choose another folder.");
                    }
                    const QRegularExpression journalName(u"^\\._?sync_[a-fA-F0-9]+\\.db$"_s);
                    const QFileInfo journal(QDir(root).filePath(definition.journalPath));
                    if (!journalName.match(definition.journalPath).hasMatch() || !definition.journalPath.startsWith(u'.') || journals.size() != 1
                        || journals.front() != definition.journalPath || journal.isSymLink() || !journal.isFile()
                        || normalizedPath(journal.absolutePath()).compare(root, Qt::CaseInsensitive) != 0) {
                        return fail("The existing sync history could not be identified safely. Choose another folder or contact your administrator.");
                    }
                    QFile database(journal.absoluteFilePath());
                    if (!database.open(QIODevice::ReadOnly) || database.read(16) != QByteArray("SQLite format 3\0", 16)) {
                        return fail("The existing sync database could not be read. Restore its backup or choose another folder.");
                    }
                    if (!result.profilePath.isEmpty() && result.definition.journalPath != definition.journalPath) {
                        return fail("Multiple sync configurations use this folder. Contact your administrator before reusing it.");
                    }
                    if (result.profilePath.isEmpty()) {
                        result.profilePath = profile;
                        definition.localPath = FolderDefinition::prepareLocalPath(root);
                        result.definition = definition;
                    }
                }
                settings.endGroup();
            }
            settings.endGroup();
        }
    }
    if (result.found && result.profilePath.isEmpty()) {
        return fail(
            "This folder contains sync history, but its server and account could not be verified. Choose another folder or restore its previous "
            "configuration.");
    }
    if (result.found && otherClientRunning()) {
        return fail("Quit the other desktop sync client before reusing this folder, then try again.");
    }
    return result;
}

QString PrivateCloudFolderAdoption::prepare(const AccountPtr &account,
                                            const Result &adoption,
                                            const QString &backupRoot,
                                            const std::function<bool()> &otherClientRunning)
{
    const auto current = inspect(account, adoption.definition.localPath, adoption.profilePaths, otherClientRunning);
    const auto failure = QCoreApplication::translate("PrivateCloudFolderAdoption",
                                                     "The existing sync history could not be backed up safely. Close other sync clients and try again.");
    if (!current.error.isEmpty()) {
        return current.error;
    }
    if (!current.found || current.profilePath != adoption.profilePath || current.definition.journalPath != adoption.definition.journalPath) {
        return failure;
    }
    const auto destinationRoot = backupRoot.isEmpty() ? QDir(ConfigFile().configPath()).filePath(u"adoption-backups"_s) : backupRoot;
    const auto destination = QDir(destinationRoot).absoluteFilePath(QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (QDir::cleanPath(destination).startsWith(QDir::cleanPath(current.definition.localPath) + u'/', Qt::CaseInsensitive)) {
        return failure;
    }
    auto sources = QStringList{current.profilePath, current.definition.absoluteJournalPath()};
    for (const auto &suffix : {u"-wal"_s, u"-shm"_s}) {
        const QString sidecar = current.definition.absoluteJournalPath() + suffix;
        if (QFileInfo::exists(sidecar)) {
            sources.append(sidecar);
        }
    }
#ifdef Q_OS_WIN
    std::vector<Utility::UniqueHandle> handles;
    for (const auto &source : sources) {
        if (QFileInfo(source).isSymLink()) {
            return failure;
        }
        const auto native = QDir::toNativeSeparators(source).toStdWString();
        Utility::UniqueHandle handle(CreateFileW(native.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        FILE_ATTRIBUTE_TAG_INFO attributes = {};
        if (handle.get() == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &attributes, sizeof(attributes))
            || (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            return failure;
        }
        handles.push_back(std::move(handle));
    }
#endif
    if (!QDir().mkpath(destination)) {
        return failure;
    }
    QJsonArray manifest;
    for (const auto &source : sources) {
        QFile input(source);
        const auto name = source == current.profilePath ? u"previous-profile.cfg"_s : QFileInfo(source).fileName();
        QSaveFile output(QDir(destination).filePath(name));
        if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
            return failure;
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        while (!input.atEnd()) {
            const auto block = input.read(1024 * 1024);
            if (input.error() != QFileDevice::NoError || output.write(block) != block.size()) {
                return failure;
            }
            hash.addData(block);
        }
        if (!output.commit()) {
            return failure;
        }
        manifest.append(QJsonObject{{u"file"_s, name}, {u"sha256"_s, QString::fromLatin1(hash.result().toHex())}});
    }
    QSaveFile record(QDir(destination).filePath(u"manifest.json"_s));
    const auto bytes = QJsonDocument(manifest).toJson();
    if (!record.open(QIODevice::WriteOnly) || record.write(bytes) != bytes.size() || !record.commit()) {
        return failure;
    }

    // The normal journal opener can replace a corrupt database. Validate a disposable
    // copy first; SQLite may rebuild its shared-memory sidecar even during read-only use.
    QTemporaryDir validation(QDir(destination).filePath(u"validation-XXXXXX"_s));
    if (!validation.isValid()) {
        return failure;
    }
    for (const auto &source : sources) {
        if (source == current.profilePath) {
            continue;
        }
        const auto name = QFileInfo(source).fileName();
        if (!QFile::copy(QDir(destination).filePath(name), validation.filePath(name))) {
            return failure;
        }
    }
    const auto invalidJournal = QCoreApplication::translate("PrivateCloudFolderAdoption",
                                                            "The existing sync database is damaged or is not a supported sync journal. Your files and original "
                                                            "sync history were kept. Restore a valid backup or contact your administrator.");
    SqlDatabase database;
    if (!database.openReadOnly(validation.filePath(current.definition.journalPath))) {
        return invalidJournal;
    }
    SqlQuery metadataTable(database);
    metadataTable.prepare("SELECT name FROM sqlite_master WHERE type='table' AND name='metadata';", true);
    if (!metadataTable.exec()) {
        return invalidJournal;
    }
    const auto table = metadataTable.next();
    if (!table.ok || !table.hasData) {
        return invalidJournal;
    }
    // These columns predate the supported configuration formats. Newer optional
    // columns remain the responsibility of the existing journal migrations.
    SqlQuery metadataColumns(database);
    metadataColumns.prepare("SELECT phash, pathlen, path, inode, modtime, type, md5 FROM metadata LIMIT 0;", true);
    if (!metadataColumns.exec() || !metadataColumns.next().ok) {
        return invalidJournal;
    }
    return {};
}
}
