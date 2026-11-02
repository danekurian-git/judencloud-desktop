/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "privatecloudmanagedstate.h"
#include "config.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QUrl>
#include <string>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#include <sys/stat.h>
#endif

namespace OCC
{
namespace
{
constexpr auto recordVersion = 1;
constexpr auto maximumRecordBytes = 64 * 1024;
constexpr auto maximumRoots = 64;

QString normalizedPath(const QString &path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

bool containsPath(const QString &parent, const QString &child)
{
#ifdef Q_OS_WIN
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    return child.compare(parent, sensitivity) == 0 || child.startsWith(parent.endsWith(u'/') ? parent : parent + u'/', sensitivity);
}

QString canonicalServer(const QString &server)
{
    const auto url = QUrl(server, QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != QStringLiteral("https") || url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()
        || (url.port(-1) != -1 && url.port() != 443) || (!url.path().isEmpty() && url.path() != QStringLiteral("/"))) {
        return {};
    }
    auto normalized = url;
    normalized.setPath({});
    normalized.setPort(-1);
    return normalized.toString(QUrl::FullyEncoded);
}

QString directoryIdentity(const QString &path, QString *identity, bool allowCloud)
{
#ifdef Q_OS_WIN
    const auto native = QDir::toNativeSeparators(path);
    const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()),
                                    FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                    nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return QStringLiteral("Cannot inspect the managed directory (%1).").arg(GetLastError());
    }
    const auto closeHandle = qScopeGuard([handle] {
        CloseHandle(handle);
    });
    FILE_ATTRIBUTE_TAG_INFO tag{};
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) || !GetFileInformationByHandle(handle, &info)
        || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        return QStringLiteral("The managed path is not an accessible directory.");
    }
    // Cloud Files tags do not redirect traversal. All other reparse tags are rejected.
    constexpr DWORD cloudTag = 0x9000001a;
    constexpr DWORD cloudTagMask = 0x0000f000;
    const auto isCloud = (tag.ReparseTag & ~cloudTagMask) == cloudTag;
    if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && (!allowCloud || !isCloud)) {
        return QStringLiteral("The directory contains a redirected or unsupported reparse path.");
    }
    const auto required = GetFinalPathNameByHandleW(handle, nullptr, 0, VOLUME_NAME_DOS);
    if (required == 0) {
        return QStringLiteral("Cannot resolve the managed directory path.");
    }
    auto resolved = std::wstring(required, L'\0');
    const auto length = GetFinalPathNameByHandleW(handle, resolved.data(), required, VOLUME_NAME_DOS);
    if (length == 0 || length >= required) {
        return QStringLiteral("Cannot resolve the managed directory path.");
    }
    auto finalPath = QString::fromWCharArray(resolved.data(), int(length));
    if (finalPath.startsWith(QStringLiteral("\\\\?\\"))) {
        finalPath.remove(0, 4);
    }
    if (normalizedPath(finalPath).compare(normalizedPath(path), Qt::CaseInsensitive) != 0) {
        return QStringLiteral("The managed directory resolves to a different path.");
    }
    *identity = QStringLiteral("%1:%2:%3")
                    .arg(info.dwVolumeSerialNumber, 8, 16, QLatin1Char('0'))
                    .arg(info.nFileIndexHigh, 8, 16, QLatin1Char('0'))
                    .arg(info.nFileIndexLow, 8, 16, QLatin1Char('0'));
#else
    Q_UNUSED(allowCloud)
    struct stat info{};
    if (lstat(QFile::encodeName(path).constData(), &info) != 0 || !S_ISDIR(info.st_mode)) {
        return QStringLiteral("The managed path is not a directory or is redirected.");
    }
    *identity = QStringLiteral("%1:%2").arg(qulonglong(info.st_dev)).arg(qulonglong(info.st_ino));
#endif
    return {};
}

QString inspectAncestors(const QString &path, QString *identity, bool allowCloud)
{
    auto current = path;
    while (true) {
        auto currentIdentity = QString();
        if (const auto error = directoryIdentity(current, &currentIdentity, allowCloud); !error.isEmpty()) {
            return error;
        }
        if (current == path) {
            *identity = currentIdentity;
        }
        const auto parent = QFileInfo(current).dir().absolutePath();
        if (parent == current) {
            return {};
        }
        current = parent;
    }
}

QString storagePathError(const QString &path)
{
    if (!QDir::isAbsolutePath(path)) {
        return QStringLiteral("The management directory must have an absolute path.");
    }
    auto current = normalizedPath(path);
    while (true) {
#ifdef Q_OS_WIN
        const auto native = QDir::toNativeSeparators(current);
        const auto exists = GetFileAttributesW(reinterpret_cast<LPCWSTR>(native.utf16())) != INVALID_FILE_ATTRIBUTES;
        if (!exists) {
            const auto error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
                return QStringLiteral("The management directory cannot be inspected. Contact admin.");
            }
        }
#else
        struct stat info{};
        const auto exists = lstat(QFile::encodeName(current).constData(), &info) == 0;
        if (!exists && errno != ENOENT) {
            return QStringLiteral("The management directory cannot be inspected. Contact admin.");
        }
#endif
        if (exists) {
            auto ignoredIdentity = QString();
            return inspectAncestors(current, &ignoredIdentity, false);
        }
        const auto parent = QFileInfo(current).dir().absolutePath();
        if (parent == current) {
            return QStringLiteral("The management directory has no accessible parent. Contact admin.");
        }
        current = parent;
    }
}

QString captureRoot(const QString &path, const QStringList &protectedPaths, PrivateCloudManagedState::Root *root)
{
    const auto clean = normalizedPath(path);
    if (path.isEmpty() || !QDir::isAbsolutePath(path) || clean == QStringLiteral(".") || clean.startsWith(QStringLiteral("//")) || QDir(clean).isRoot()) {
        return QStringLiteral("The managed root must be an absolute local subdirectory.");
    }
#ifdef Q_OS_WIN
    if (clean.size() < 4 || !clean.at(0).isLetter() || clean.mid(1, 2) != QStringLiteral(":/") || clean.mid(2).contains(u':') || clean.contains(u'~')
        || path.contains(QStringLiteral(".."))) {
        return QStringLiteral("The managed root must use an unambiguous local directory path.");
    }
    for (const auto &component : clean.mid(3).split(u'/')) {
        if (component.endsWith(u'.') || component.endsWith(u' ')) {
            return QStringLiteral("The managed root must not use Windows path aliases.");
        }
    }
#endif
    const auto home = normalizedPath(QDir::homePath());
    if (containsPath(clean, home)) {
        return QStringLiteral("The managed root must not contain the Windows user profile.");
    }
    auto protectedDirectories = protectedPaths;
    protectedDirectories.append(QCoreApplication::applicationDirPath());
    for (const auto &protectedDirectory : std::as_const(protectedDirectories)) {
        if (protectedDirectory.isEmpty()) {
            continue;
        }
        const auto protectedClean = normalizedPath(protectedDirectory);
        if (containsPath(clean, protectedClean) || containsPath(protectedClean, clean)) {
            return QStringLiteral("The managed root overlaps protected application or management files.");
        }
    }
    auto nativeIdentity = QString();
    if (const auto error = inspectAncestors(clean, &nativeIdentity, true); !error.isEmpty()) {
        return error;
    }
    *root = {clean, nativeIdentity};
    return {};
}

QString statusName(PrivateCloudManagedState::Status status)
{
    switch (status) {
    case PrivateCloudManagedState::Status::Pending:
        return QStringLiteral("pending");
    case PrivateCloudManagedState::Status::Completed:
        return QStringLiteral("completed");
    case PrivateCloudManagedState::Status::Failed:
        return QStringLiteral("failed");
    }
    return {};
}
}

PrivateCloudManagedState::PrivateCloudManagedState(QString storageDirectory, QString fixedServer)
    : _storageDirectory(storageDirectory.isEmpty()
                            ? QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/privateCloud/Management")
                            : normalizedPath(storageDirectory))
    , _fixedServer(std::move(fixedServer))
{
#ifdef APPLICATION_SERVER_URL
    if (_fixedServer.isEmpty()) {
        _fixedServer = QStringLiteral(APPLICATION_SERVER_URL);
    }
#endif
}

QString PrivateCloudManagedState::identity(const QString &server, const QString &canonicalUser, QString *normalizedServer, QString *recordPath) const
{
    *normalizedServer = canonicalServer(server);
    if (normalizedServer->isEmpty() || *normalizedServer != canonicalServer(_fixedServer) || canonicalUser.isEmpty() || canonicalUser.contains(QChar::Null)
        || canonicalUser.contains(u'\n') || canonicalUser.contains(u'\r')) {
        return QStringLiteral("The management command has an invalid server or canonical account identity.");
    }
    const auto key = QCryptographicHash::hash((*normalizedServer + u'\n' + canonicalUser).toUtf8(), QCryptographicHash::Sha256).toHex();
    *recordPath = _storageDirectory + u'/' + QString::fromLatin1(key) + QStringLiteral(".json");
    return {};
}

bool PrivateCloudManagedState::isBlocked(const QString &server, const QString &canonicalUser) const
{
    auto normalizedServer = QString();
    auto path = QString();
    if (!identity(server, canonicalUser, &normalizedServer, &path).isEmpty()) {
        return true;
    }
    if (!storagePathError(_storageDirectory).isEmpty()) {
        return true;
    }
#ifdef Q_OS_WIN
    const auto native = QDir::toNativeSeparators(path);
    if (GetFileAttributesW(reinterpret_cast<LPCWSTR>(native.utf16())) != INVALID_FILE_ATTRIBUTES) {
        return true;
    }
    const auto error = GetLastError();
    return error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND;
#else
    struct stat info{};
    return lstat(QFile::encodeName(path).constData(), &info) == 0 || errno != ENOENT;
#endif
}

QString PrivateCloudManagedState::read(const QString &server, const QString &canonicalUser, Record *record) const
{
    auto normalizedServer = QString();
    auto path = QString();
    if (const auto error = identity(server, canonicalUser, &normalizedServer, &path); !error.isEmpty()) {
        return error;
    }
    QFile file(path);
    auto storageIdentity = QString();
    if (!record || !inspectAncestors(_storageDirectory, &storageIdentity, false).isEmpty() || QFileInfo(path).isSymLink() || !file.open(QIODevice::ReadOnly)
        || file.size() > maximumRecordBytes) {
        return QStringLiteral("The management lock cannot be read. Contact admin.");
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    const auto object = document.object();
    const auto status = object.value(QStringLiteral("status")).toString();
    const auto roots = object.value(QStringLiteral("roots"));
    if (parseError.error != QJsonParseError::NoError || !document.isObject() || object.value(QStringLiteral("version")).toInt() != recordVersion
        || object.value(QStringLiteral("server")).toString() != normalizedServer || object.value(QStringLiteral("user")).toString() != canonicalUser
        || (status != QStringLiteral("pending") && status != QStringLiteral("completed") && status != QStringLiteral("failed")) || !roots.isArray()
        || roots.toArray().size() > maximumRoots) {
        return QStringLiteral("The management lock is invalid. Contact admin.");
    }
    auto parsed = Record{normalizedServer, canonicalUser, Status::Pending, {}, object.value(QStringLiteral("error")).toString()};
    if (status == QStringLiteral("completed")) {
        parsed.status = Status::Completed;
    } else if (status == QStringLiteral("failed")) {
        parsed.status = Status::Failed;
    }
    for (const auto &value : roots.toArray()) {
        const auto root = value.toObject();
        const auto rootPath = root.value(QStringLiteral("path")).toString();
        const auto rootIdentity = root.value(QStringLiteral("identity")).toString();
        if (rootPath.isEmpty() || rootIdentity.isEmpty() || !QDir::isAbsolutePath(rootPath) || normalizedPath(rootPath) != rootPath) {
            return QStringLiteral("The management lock has an invalid directory identity. Contact admin.");
        }
        parsed.roots.append({rootPath, rootIdentity});
    }
    *record = parsed;
    return {};
}

QString PrivateCloudManagedState::write(const QString &recordPath, const Record &record) const
{
    if (!QDir::isAbsolutePath(_storageDirectory) || !QDir().mkpath(_storageDirectory)) {
        return QStringLiteral("Cannot create the privateCloud management directory.");
    }
    auto storageIdentity = QString();
    if (const auto error = inspectAncestors(_storageDirectory, &storageIdentity, false); !error.isEmpty()) {
        return error;
    }
    auto roots = QJsonArray();
    for (const auto &root : record.roots) {
        roots.append(QJsonObject{{QStringLiteral("path"), root.path}, {QStringLiteral("identity"), root.identity}});
    }
    const auto bytes = QJsonDocument(QJsonObject{{QStringLiteral("version"), recordVersion},
                                                 {QStringLiteral("server"), record.server},
                                                 {QStringLiteral("user"), record.user},
                                                 {QStringLiteral("status"), statusName(record.status)},
                                                 {QStringLiteral("roots"), roots},
                                                 {QStringLiteral("error"), record.error}})
                           .toJson(QJsonDocument::Compact);
    if (bytes.size() > maximumRecordBytes || QFileInfo(recordPath).isSymLink()) {
        return QStringLiteral("The management lock is too large or its path is redirected.");
    }
    QSaveFile file(recordPath);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        return QStringLiteral("Cannot save the management lock. Contact admin.");
    }
    return {};
}

QString
PrivateCloudManagedState::begin(const QString &server, const QString &canonicalUser, const QStringList &trustedRoots, const QStringList &protectedPaths) const
{
    auto normalizedServer = QString();
    auto path = QString();
    if (const auto error = identity(server, canonicalUser, &normalizedServer, &path); !error.isEmpty()) {
        return error;
    }
    if (!QDir::isAbsolutePath(_storageDirectory) || !QDir().mkpath(_storageDirectory)) {
        return QStringLiteral("Cannot create the privateCloud management directory.");
    }
    auto storageIdentity = QString();
    if (const auto error = inspectAncestors(_storageDirectory, &storageIdentity, false); !error.isEmpty()) {
        return error;
    }
    QLockFile lock(_storageDirectory + QStringLiteral("/state.lock"));
    if (!lock.tryLock()) {
        return QStringLiteral("The management lock is busy. Contact admin.");
    }
    if (isBlocked(server, canonicalUser)) {
        return QStringLiteral("This account already has a management lock. Contact admin.");
    }
    auto protectedDirectories = protectedPaths;
    protectedDirectories.append(_storageDirectory);
    auto record = Record{normalizedServer, canonicalUser, Status::Pending, {}, {}};
    const auto failLocked = [&](const QString &error) {
        record.status = Status::Failed;
        record.error = error;
        record.roots.clear();
        const auto writeError = write(path, record);
        return writeError.isEmpty() ? error : writeError;
    };
    if (trustedRoots.size() > maximumRoots) {
        return failLocked(QStringLiteral("The management command has too many local roots."));
    }
    for (const auto &trustedRoot : trustedRoots) {
        auto root = Root();
        if (const auto error = captureRoot(trustedRoot, protectedDirectories, &root); !error.isEmpty()) {
            return failLocked(error);
        }
        for (const auto &other : std::as_const(record.roots)) {
            if (containsPath(root.path, other.path) || containsPath(other.path, root.path)) {
                return failLocked(QStringLiteral("The management command contains overlapping local roots."));
            }
        }
        record.roots.append(root);
    }
    return write(path, record);
}

QString PrivateCloudManagedState::finish(const QString &server, const QString &canonicalUser, bool success, const QString &error) const
{
    QLockFile lock(_storageDirectory + QStringLiteral("/state.lock"));
    if (!lock.tryLock()) {
        return QStringLiteral("The management lock is busy. Contact admin.");
    }
    auto record = Record();
    if (const auto readError = read(server, canonicalUser, &record); !readError.isEmpty()) {
        return readError;
    }
    if (record.status == Status::Completed) {
        return success ? QString() : QStringLiteral("The completed management lock cannot be reversed.");
    }
    record.status = success ? Status::Completed : Status::Failed;
    record.error = error;
    auto normalizedServer = QString();
    auto path = QString();
    if (const auto identityError = identity(server, canonicalUser, &normalizedServer, &path); !identityError.isEmpty()) {
        return identityError;
    }
    return write(path, record);
}

QString PrivateCloudManagedState::validateRoot(const Root &root, const QStringList &protectedPaths)
{
    auto current = Root();
    if (const auto error = captureRoot(root.path, protectedPaths, &current); !error.isEmpty()) {
        return error;
    }
    if (root.path != current.path || root.identity.isEmpty() || root.identity != current.identity) {
        return QStringLiteral("The managed directory identity changed; local cleanup was stopped.");
    }
    return {};
}

QString PrivateCloudManagedState::storageDirectory() const
{
    return _storageDirectory;
}
}
