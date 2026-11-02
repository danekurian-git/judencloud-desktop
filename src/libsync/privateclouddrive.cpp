/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "privateclouddrive.h"
#include "account.h"
#include "common/utility.h"
#include "config.h"
#include "configfile.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QScopeGuard>
#include <QSettings>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QUrl>

#ifdef Q_OS_WIN
#include <shlobj.h>
#include <windows.h>
#endif

namespace OCC::PrivateCloudDrive
{
namespace
{
QString message(const char *text)
{
    return QCoreApplication::translate("PrivateCloudDrive", text);
}

bool sameTarget(const QString &left, const QString &right)
{
    return left.compare(right, Qt::CaseInsensitive) == 0;
}

QString defaultBackingPath(const QString &accountIdentity)
{
    if (accountIdentity.isEmpty()) {
        return {};
    }
    QByteArray identity;
#ifdef APPLICATION_SERVER_URL
    identity = QByteArray(APPLICATION_SERVER_URL);
#endif
    identity += '\n' + accountIdentity.toUtf8();
    const auto hash = QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/privateCloud/Files/") + hash;
}

QString canonicalAccountIdentity(const QUrl &serverUrl, const QString &accountId)
{
    if (!serverUrl.isValid() || serverUrl.isRelative() || serverUrl.scheme() != QStringLiteral("https") || serverUrl.host().isEmpty()
        || serverUrl.authority(QUrl::FullyEncoded).contains(QLatin1Char('@')) || serverUrl.hasQuery() || serverUrl.hasFragment()
        || !Account::isServerUrlAllowed(serverUrl) || accountId.trimmed().isEmpty() || accountId.contains(QChar::Null) || accountId.contains(QLatin1Char('\n'))
        || accountId.contains(QLatin1Char('\r'))) {
        return {};
    }
    auto normalized = serverUrl.adjusted(QUrl::NormalizePathSegments | QUrl::StripTrailingSlash);
    if (normalized.path() == QStringLiteral("/")) {
        normalized.setPath({});
    }
    if (normalized.port() == 443) {
        normalized.setPort(-1);
    }
    normalized.setHost(normalized.host().toCaseFolded());
    return normalized.toString(QUrl::FullyEncoded) + QLatin1Char('\n') + accountId;
}

QVariantMap adoptedRootSettings()
{
    QSettings settings(ConfigFile().configFile(), QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("PrivateCloudBackingRoot"));
    QVariantMap result;
    for (const auto &key : settings.allKeys()) {
        result.insert(key, settings.value(key));
    }
    if (settings.status() != QSettings::NoError) {
        result.insert(QStringLiteral("InvalidConfiguration"), true);
    }
    return result;
}

bool ownsMapping(const QStringList &mappings, const QString &target)
{
    return mappings.size() == 1 && sameTarget(mappings.front(), target);
}

QString conflictError()
{
    //: %1 is the reserved Windows drive letter, for example J:/.
    return message("Drive %1 is already in use. Free this drive letter before setting up privateCloud.").arg(driveRoot());
}

#ifdef Q_OS_WIN
QString rejectRedirectingAncestors(const QString &path)
{
    auto current = QDir::cleanPath(path);
    while (true) {
        const auto native = QDir::toNativeSeparators(current).toStdWString();
        const Utility::Handle handle(CreateFileW(native.c_str(),
                                                 0,
                                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                 nullptr,
                                                 OPEN_EXISTING,
                                                 FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                                 nullptr));
        FILE_ATTRIBUTE_TAG_INFO attributes = {};
        if (!handle || !GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes, sizeof(attributes))) {
            return message("The privateCloud files folder could not be verified.");
        }
        // Cloud Files tags describe placeholders; name-surrogate tags redirect paths.
        if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(attributes.ReparseTag)) {
            return message("The privateCloud files folder cannot use a symbolic link or redirected folder.");
        }
        if (QDir(current).isRoot()) {
            return {};
        }
        const auto parent = QFileInfo(current).dir().absolutePath();
        if (sameTarget(parent, current)) {
            return message("The privateCloud files folder could not be verified.");
        }
        current = parent;
    }
}

QString windowsError()
{
    const auto error = GetLastError();
    //: %1 is a numeric Windows system error code.
    return message("Windows could not update the privateCloud drive (error %1).").arg(error);
}

class WindowsBackend final : public Detail::Backend
{
public:
    QString validatePath(const QString &path, bool mustExist, QString *target) override
    {
        const QFileInfo info(path);
        if (path.isEmpty() || !info.isAbsolute() || (mustExist && !info.isDir())) {
            return message("The privateCloud files folder must be an existing local folder.");
        }
        QFileInfo ancestor(QDir::cleanPath(info.absoluteFilePath()));
        while (!ancestor.exists()) {
            const auto parent = ancestor.dir().absolutePath();
            if (parent == ancestor.absoluteFilePath()) {
                return message("The privateCloud files folder is unavailable.");
            }
            ancestor.setFile(parent);
        }
        if (!ancestor.isDir()) {
            return message("The privateCloud files location is not a folder.");
        }
        const auto redirectError = rejectRedirectingAncestors(ancestor.absoluteFilePath());
        if (!redirectError.isEmpty()) {
            return redirectError;
        }
        const auto canonicalAncestor = ancestor.canonicalFilePath();
        const auto storageError = Utility::syncRootStorageError(canonicalAncestor);
        if (!storageError.isEmpty()) {
            return storageError;
        }
        if (Utility::localStorageType(canonicalAncestor) != Utility::LocalStorageType::Internal) {
            return message("The privateCloud files folder requires an internal NTFS drive.");
        }
        const QStorageInfo storage(canonicalAncestor);
        if (!storage.isValid() || !storage.isReady() || storage.fileSystemType().toUpper() != QByteArrayLiteral("NTFS")) {
            return message("The privateCloud files folder requires an internal NTFS drive.");
        }
        const auto relativePath = QDir(ancestor.absoluteFilePath()).relativeFilePath(info.absoluteFilePath());
        const auto canonicalPath = QDir::cleanPath(QDir(canonicalAncestor).filePath(relativePath));
        if (canonicalPath.size() < 4 || canonicalPath.at(1) != u':' || canonicalPath.at(2) != u'/' || QDir(canonicalPath).isRoot()) {
            return message("The privateCloud files folder must be below an internal drive root.");
        }
        *target = QStringLiteral("\\??\\") + QDir::toNativeSeparators(canonicalPath);
        return {};
    }

    QString emptyDirectoryError(const QString &path) override
    {
        auto current = path;
        while (true) {
            const auto native = QDir::toNativeSeparators(current).toStdWString();
            const auto attributes = GetFileAttributesW(native.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                return message("The privateCloud files folder could not be inspected.");
            }
            if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                return message("Choose a folder outside existing cloud folders and redirected locations.");
            }
            if (QDir(current).isRoot()) {
                break;
            }
            current = QFileInfo(current).dir().absolutePath();
        }
        const auto wildcard = QDir::toNativeSeparators(QDir(path).filePath(QStringLiteral("*"))).toStdWString();
        WIN32_FIND_DATAW data = {};
        const auto handle = FindFirstFileW(wildcard.c_str(), &data);
        if (handle == INVALID_HANDLE_VALUE) {
            return GetLastError() == ERROR_FILE_NOT_FOUND ? QString() : message("The privateCloud files folder could not be inspected.");
        }
        const auto close = qScopeGuard([handle] {
            FindClose(handle);
        });
        do {
            const auto name = QString::fromWCharArray(data.cFileName);
            if (name != QStringLiteral(".") && name != QStringLiteral("..")) {
                return message("Choose an empty folder for privateCloud. Existing files and folders will not be adopted during setup.");
            }
        } while (FindNextFileW(handle, &data));
        return GetLastError() == ERROR_NO_MORE_FILES ? QString() : message("The privateCloud files folder could not be inspected.");
    }

    QString mappings(QStringList *targets) override
    {
        targets->clear();
        constexpr DWORD maximumMappingCharacters = 32768;
        wchar_t buffer[maximumMappingCharacters] = {};
        const auto device = driveRoot().first(2).toStdWString();
        if (!QueryDosDeviceW(device.c_str(), buffer, maximumMappingCharacters)) {
            return GetLastError() == ERROR_FILE_NOT_FOUND ? QString() : windowsError();
        }
        for (const auto *item = buffer; *item; item += wcslen(item) + 1) {
            targets->append(QString::fromWCharArray(item));
        }
        return {};
    }

    QString addMapping(const QString &target) override
    {
        return changeMapping(target, DDD_RAW_TARGET_PATH);
    }

    QString removeMapping(const QString &target) override
    {
        return changeMapping(target, DDD_RAW_TARGET_PATH | DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE);
    }

    bool isFixedDrive() override
    {
        const auto root = QDir::toNativeSeparators(driveRoot()).toStdWString();
        return GetDriveTypeW(root.c_str()) == DRIVE_FIXED;
    }

    QVariant label() override
    {
        QSettings settings(labelKey(), QSettings::NativeFormat);
        return settings.value(QStringLiteral("."));
    }

    QString setLabel(const QVariant &value) override
    {
        QSettings settings(labelKey(), QSettings::NativeFormat);
        if (value.isValid()) {
            settings.setValue(QStringLiteral("."), value);
        } else {
            settings.remove(QStringLiteral("."));
        }
        settings.sync();
        const auto root = QDir::toNativeSeparators(driveRoot()).toStdWString();
        SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, root.c_str(), nullptr);
        return settings.status() == QSettings::NoError ? QString() : message("The privateCloud drive label could not be saved.");
    }

    QVariantMap savedState() override
    {
        QSettings settings(stateKey(), QSettings::NativeFormat);
        return settings.value(QStringLiteral("State")).toMap();
    }

    QString saveState(const QVariantMap &state) override
    {
        QSettings settings(stateKey(), QSettings::NativeFormat);
        if (state.isEmpty()) {
            settings.remove(QStringLiteral("State"));
        } else {
            settings.setValue(QStringLiteral("State"), state);
        }
        settings.sync();
        return settings.status() == QSettings::NoError ? QString() : message("The privateCloud drive settings could not be saved.");
    }

private:
    QString changeMapping(const QString &target, DWORD flags)
    {
        const auto device = driveRoot().first(2).toStdWString();
        const auto nativeTarget = target.toStdWString();
        return DefineDosDeviceW(flags, device.c_str(), nativeTarget.c_str()) ? QString() : windowsError();
    }

    static QString labelKey()
    {
        return QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\DriveIcons\\") + driveRoot().first(1)
            + QStringLiteral("\\DefaultLabel");
    }

    static QString stateKey()
    {
        return QStringLiteral("HKEY_CURRENT_USER\\Software\\privateCloud\\DriveMappings\\") + driveRoot().first(1);
    }
};

QString lockPath()
{
    const auto directory = QString(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/privateCloud"));
    QDir().mkpath(directory);
    return directory + QStringLiteral("/drive-") + driveRoot().first(1) + QStringLiteral(".lock");
}
#endif
}

bool isEnabled()
{
#if defined(Q_OS_WIN) && defined(APPLICATION_SERVER_URL_ENFORCE) && defined(APPLICATION_SERVER_URL)
    return !QStringLiteral(APPLICATION_SERVER_URL).isEmpty();
#else
    return false;
#endif
}

QString driveRoot()
{
    return QStringLiteral(PRIVATECLOUD_DRIVE_LETTER ":/");
}

QString defaultSetupPath()
{
    return QStringLiteral("C:/PPCsyncConfig");
}

QString Detail::configureBackingPath(const QString &accountIdentity, const QString &path, Backend &backend, QSettings &settings)
{
    const auto separator = accountIdentity.indexOf(QLatin1Char('\n'));
    const auto identity = separator > 0 ? canonicalAccountIdentity(QUrl(accountIdentity.first(separator)), accountIdentity.mid(separator + 1)) : QString();
    if (identity.isEmpty()) {
        return message("The privateCloud account could not be verified. Sign in again before choosing a files folder.");
    }
    const auto normalizedPath = QDir::fromNativeSeparators(path);
    if (normalizedPath.startsWith(driveRoot(), Qt::CaseInsensitive)) {
        return message("Choose a local folder outside the privateCloud drive.");
    }
    const auto canonicalSeparator = identity.indexOf(QLatin1Char('\n'));
    auto record = QVariantMap{{QStringLiteral("ServerUrl"), identity.first(canonicalSeparator)},
                              {QStringLiteral("AccountId"), identity.mid(canonicalSeparator + 1)},
                              {QStringLiteral("Path"), normalizedPath}};
    QString error;
    const auto admittedPath = Detail::backingPath(identity, record, backend, &error);
    if (admittedPath.isEmpty()) {
        return error;
    }
    error = backend.emptyDirectoryError(admittedPath);
    if (!error.isEmpty()) {
        return error;
    }
    record[QStringLiteral("Path")] = admittedPath;
    settings.sync();
    if (!settings.group().isEmpty() || settings.status() != QSettings::NoError || !settings.isWritable()) {
        return message("The privateCloud files folder settings could not be saved.");
    }
    settings.setAtomicSyncRequired(true);
    settings.beginGroup(QStringLiteral("PrivateCloudBackingRoot"));
    const auto endGroup = qScopeGuard([&settings] {
        settings.endGroup();
    });
    QVariantMap previous;
    for (const auto &key : settings.allKeys()) {
        previous.insert(key, settings.value(key));
    }
    const auto writeRecord = [&settings](const QVariantMap &values) {
        settings.remove(QString());
        for (auto it = values.cbegin(); it != values.cend(); ++it) {
            settings.setValue(it.key(), it.value());
        }
    };
    writeRecord(record);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        writeRecord(previous);
        settings.sync();
        return message("The privateCloud files folder settings could not be saved.");
    }
    return {};
}

QString configureBackingPath(const QString &accountIdentity, const QString &path)
{
#ifdef Q_OS_WIN
    if (isEnabled()) {
        WindowsBackend backend;
        QSettings settings(ConfigFile().configFile(), QSettings::IniFormat);
        return Detail::configureBackingPath(accountIdentity, path, backend, settings);
    }
#else
    Q_UNUSED(accountIdentity)
    Q_UNUSED(path)
#endif
    return message("The privateCloud files folder requires Windows.");
}

QString Detail::backingPath(const QString &accountIdentity, const QVariantMap &adoptedRoot, Backend &backend, QString *error)
{
    if (error) {
        error->clear();
    }
    if (adoptedRoot.isEmpty()) {
        return defaultBackingPath(accountIdentity);
    }
    const auto fail = [error](const QString &reason) {
        if (error) {
            *error = reason;
        }
        return QString();
    };
    const auto requiredKeys = QStringList{QStringLiteral("AccountId"), QStringLiteral("Path"), QStringLiteral("ServerUrl")};
    if (adoptedRoot.keys() != requiredKeys) {
        return fail(message("The adopted privateCloud folder must specify one server, account and folder."));
    }
    for (const auto &value : adoptedRoot) {
        if (value.metaType() != QMetaType::fromType<QString>() || value.toString().isEmpty()) {
            return fail(message("The adopted privateCloud folder settings are invalid."));
        }
    }
    const auto separator = accountIdentity.indexOf(QLatin1Char('\n'));
    const auto requestedIdentity =
        separator > 0 ? canonicalAccountIdentity(QUrl(accountIdentity.first(separator)), accountIdentity.mid(separator + 1)) : QString();
    const auto storedIdentity =
        canonicalAccountIdentity(QUrl(adoptedRoot.value(QStringLiteral("ServerUrl")).toString()), adoptedRoot.value(QStringLiteral("AccountId")).toString());
    if (requestedIdentity.isEmpty() || storedIdentity.isEmpty() || requestedIdentity != storedIdentity) {
        return fail(message("The adopted privateCloud folder belongs to a different server or account."));
    }
    const auto configuredPath = QDir::fromNativeSeparators(adoptedRoot.value(QStringLiteral("Path")).toString());
    if (configuredPath.size() < 4 || !configuredPath.at(0).isLetter() || configuredPath.at(1) != QLatin1Char(':') || configuredPath.at(2) != QLatin1Char('/')
        || configuredPath.mid(2).contains(QLatin1Char(':')) || configuredPath.split(QLatin1Char('/')).contains(QStringLiteral(".."))) {
        return fail(message("The adopted privateCloud folder must be an absolute folder below an internal drive root."));
    }
    const auto path = QDir::cleanPath(configuredPath);
    if (path.size() < 4) {
        return fail(message("The adopted privateCloud folder must be below an internal drive root."));
    }
    QString target;
    const auto validationError = backend.validatePath(path, true, &target);
    if (!validationError.isEmpty()) {
        return fail(validationError);
    }
    const auto expectedTarget = QString(QStringLiteral("\\??\\") + QString(path).replace(QLatin1Char('/'), QLatin1Char('\\')));
    if (!sameTarget(target, expectedTarget)) {
        return fail(message("The adopted privateCloud folder resolves to a different location."));
    }
    return path;
}

QString backingPath(const QString &accountIdentity, QString *error)
{
    const auto adoptedRoot = adoptedRootSettings();
    if (adoptedRoot.isEmpty()) {
        if (error) {
            error->clear();
        }
        return defaultBackingPath(accountIdentity);
    }
#ifdef Q_OS_WIN
    WindowsBackend backend;
    return Detail::backingPath(accountIdentity, adoptedRoot, backend, error);
#else
    if (error) {
        *error = message("The adopted privateCloud folder requires Windows.");
    }
    return {};
#endif
}

QString adoptedBackingPathError(const QString &accountIdentity, const QString &path)
{
    if (adoptedRootSettings().isEmpty()) {
        return {};
    }
    QString error;
    const auto expectedPath = backingPath(accountIdentity, &error);
    if (!error.isEmpty()) {
        return error;
    }
    if (expectedPath.isEmpty() || !sameTarget(QDir::cleanPath(QDir::fromNativeSeparators(path)), expectedPath)) {
        return message("The configured sync folder does not match the adopted privateCloud folder.");
    }
    return {};
}

QString Detail::checkAvailability(const QString &path, Backend &backend)
{
    QString target;
    auto error = backend.validatePath(path, false, &target);
    if (!error.isEmpty()) {
        return error;
    }
    QStringList mappings;
    error = backend.mappings(&mappings);
    if (!error.isEmpty()) {
        return error;
    }
    return mappings.isEmpty() || ownsMapping(mappings, target) ? QString() : conflictError();
}

bool Detail::isMapped(const QString &path, Backend &backend)
{
    const auto normalized = QDir::cleanPath(QString(path).replace(QLatin1Char('\\'), QLatin1Char('/')));
    if (normalized.size() < 4 || !normalized.at(0).isLetter() || normalized.at(1) != QLatin1Char(':') || normalized.at(2) != QLatin1Char('/')) {
        return false;
    }
    const auto target = QString(QStringLiteral("\\??\\") + QString(normalized).replace(QLatin1Char('/'), QLatin1Char('\\')));
    QStringList mappings;
    return backend.mappings(&mappings).isEmpty() && ownsMapping(mappings, target);
}

QString Detail::mount(const QString &path, Backend &backend)
{
    QString target;
    auto error = backend.validatePath(path, true, &target);
    if (!error.isEmpty()) {
        return error;
    }
    QStringList mappings;
    error = backend.mappings(&mappings);
    if (!error.isEmpty()) {
        return error;
    }
    if (!mappings.isEmpty() && !ownsMapping(mappings, target)) {
        return conflictError();
    }
    const auto created = mappings.isEmpty();
    if (created) {
        error = backend.addMapping(target);
        if (!error.isEmpty()) {
            return error;
        }
        error = backend.mappings(&mappings);
        if (!error.isEmpty() || !ownsMapping(mappings, target)) {
            backend.removeMapping(target);
            return error.isEmpty() ? conflictError() : error;
        }
    }
    if (!backend.isFixedDrive()) {
        if (created) {
            backend.removeMapping(target);
        }
        return message("Windows could not present privateCloud as a fixed drive.");
    }
    auto state = backend.savedState();
    if (!state.isEmpty() && !sameTarget(state.value(QStringLiteral("target")).toString(), target)) {
        if (created) {
            backend.removeMapping(target);
        }
        return message("A previous privateCloud drive must be disconnected before connecting this account.");
    }
    const auto createdState = state.isEmpty();
    if (createdState) {
        state.insert(QStringLiteral("target"), target);
        state.insert(QStringLiteral("path"), QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
        const auto previousLabel = backend.label();
        if (previousLabel.isValid()) {
            state.insert(QStringLiteral("previousLabel"), previousLabel);
        }
        error = backend.saveState(state);
    }
    if (error.isEmpty()) {
        error = backend.mappings(&mappings);
        if (!error.isEmpty() || !ownsMapping(mappings, target)) {
            if (created) {
                backend.removeMapping(target);
            }
            if (createdState) {
                backend.saveState({});
            }
            return error.isEmpty() ? conflictError() : error;
        }
        error = backend.setLabel(QStringLiteral("privateCloud"));
    }
    if (!error.isEmpty() && created) {
        Detail::unmount(path, backend);
    }
    return error;
}

QString Detail::unmount(const QString &path, Backend &backend)
{
    const auto state = backend.savedState();
    const auto savedTarget = state.value(QStringLiteral("target")).toString();
    const auto normalizedPath = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    QString target;
    QString error;
    // A disconnected disk must not prevent removing this application's own alias.
    if (!savedTarget.isEmpty() && sameTarget(state.value(QStringLiteral("path")).toString(), normalizedPath)) {
        target = savedTarget;
    } else {
        error = backend.validatePath(path, false, &target);
        if (!error.isEmpty()) {
            return error;
        }
    }
    QStringList mappings;
    error = backend.mappings(&mappings);
    if (!error.isEmpty() || (!mappings.isEmpty() && !ownsMapping(mappings, target))) {
        return error;
    }
    if (!mappings.isEmpty()) {
        error = backend.removeMapping(mappings.front());
        if (!error.isEmpty()) {
            return error;
        }
        error = backend.mappings(&mappings);
        if (!error.isEmpty() || !mappings.isEmpty()) {
            return error.isEmpty() ? conflictError() : error;
        }
    }
    // Windows also drops per-logon aliases at sign-out. Clean up their saved state.
    if (sameTarget(savedTarget, target)) {
        if (backend.label().toString() == QStringLiteral("privateCloud")) {
            error = backend.setLabel(state.value(QStringLiteral("previousLabel")));
        }
        if (error.isEmpty()) {
            error = backend.saveState({});
        }
    }
    return error;
}

QString checkAvailability(const QString &path)
{
#ifdef Q_OS_WIN
    if (isEnabled()) {
        WindowsBackend backend;
        return Detail::checkAvailability(path, backend);
    }
#else
    Q_UNUSED(path)
#endif
    return message("The privateCloud drive is unavailable in this build.");
}

QString mount(const QString &path)
{
#ifdef Q_OS_WIN
    if (isEnabled()) {
        QLockFile lock(lockPath());
        if (!lock.tryLock()) {
            return message("Another privateCloud process is updating the drive. Try again.");
        }
        WindowsBackend backend;
        return Detail::mount(path, backend);
    }
#else
    Q_UNUSED(path)
#endif
    return message("The privateCloud drive is unavailable in this build.");
}

bool isMapped(const QString &path)
{
#ifdef Q_OS_WIN
    if (isEnabled()) {
        WindowsBackend backend;
        return Detail::isMapped(path, backend);
    }
#else
    Q_UNUSED(path)
#endif
    return false;
}

QString unmountChecked(const QString &path)
{
#ifdef Q_OS_WIN
    if (isEnabled() && !path.isEmpty()) {
        QLockFile lock(lockPath());
        if (!lock.tryLock()) {
            return message("Another privateCloud process is updating the drive. Try again.");
        }
        WindowsBackend backend;
        return Detail::unmount(path, backend);
    }
#else
    Q_UNUSED(path)
#endif
    return {};
}

void unmount(const QString &path)
{
    const auto error = unmountChecked(path);
    if (!error.isEmpty()) {
        qWarning() << error;
    }
}
}
