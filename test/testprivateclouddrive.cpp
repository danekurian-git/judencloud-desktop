/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "account.h"
#include "configfile.h"
#include "libsync/privateclouddrive.h"
#include "syncenginetestutils.h"
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QScopeGuard>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

using namespace OCC;
using namespace Qt::StringLiterals;

class FakeDriveBackend final : public PrivateCloudDrive::Detail::Backend
{
public:
    QString target = QStringLiteral("\\??\\C:\\privateCloud\\Files\\account");
    QStringList targets;
    QVariant currentLabel;
    QVariantMap state;
    QString pathError;
    QString queryError;
    QString addError;
    QString removeError;
    QString emptyError;
    QString labelError;
    QString saveStateError;
    bool fixed = true;
    bool race = false;
    bool raceOnRemove = false;
    int replaceOnQuery = 0;
    int queries = 0;
    int validations = 0;
    int emptyChecks = 0;
    int added = 0;
    int removed = 0;
    int labelWrites = 0;
    bool requiredExistingPath = false;

    QString validatePath(const QString &, bool mustExist, QString *result) override
    {
        ++validations;
        requiredExistingPath = mustExist;
        *result = target;
        return pathError;
    }
    QString emptyDirectoryError(const QString &) override
    {
        ++emptyChecks;
        return emptyError;
    }
    QString mappings(QStringList *result) override
    {
        if (++queries == replaceOnQuery) {
            targets.prepend(QStringLiteral("\\Device\\OtherDrive"));
        }
        *result = targets;
        return queryError;
    }
    QString addMapping(const QString &value) override
    {
        ++added;
        if (addError.isEmpty()) {
            targets.prepend(value);
            if (race) {
                targets.append(QStringLiteral("\\Device\\OtherDrive"));
            }
        }
        return addError;
    }
    QString removeMapping(const QString &value) override
    {
        ++removed;
        if (removeError.isEmpty()) {
            targets.removeOne(value);
            if (raceOnRemove) {
                targets.append(QStringLiteral("\\Device\\OtherDrive"));
            }
        }
        return removeError;
    }
    bool isFixedDrive() override
    {
        return fixed;
    }
    QVariant label() override
    {
        return currentLabel;
    }
    QString setLabel(const QVariant &value) override
    {
        ++labelWrites;
        if (!labelError.isEmpty()) {
            return labelError;
        }
        currentLabel = value;
        return {};
    }
    QVariantMap savedState() override
    {
        return state;
    }
    QString saveState(const QVariantMap &value) override
    {
        if (!saveStateError.isEmpty()) {
            return saveStateError;
        }
        state = value;
        return {};
    }
};

class TestPrivateCloudDrive : public QObject
{
    Q_OBJECT
    QTemporaryDir _configDirectory;

    static QString serverUrl()
    {
        const auto enforced = Account::enforcedServerUrl();
        return enforced.isEmpty() ? QStringLiteral("https://cloud.example") : enforced.toString(QUrl::FullyEncoded);
    }

    static QString accountIdentity()
    {
        return serverUrl() + QStringLiteral("\ncanonical-user-01");
    }

    static QVariantMap adoptedRoot()
    {
        return {{QStringLiteral("ServerUrl"), serverUrl()},
                {QStringLiteral("AccountId"), QStringLiteral("canonical-user-01")},
                {QStringLiteral("Path"), QStringLiteral("D:/Cloud/Data")}};
    }

    static void writeAdoptedRoot(const QVariantMap &record)
    {
        QSettings settings(ConfigFile().configFile(), QSettings::IniFormat);
        settings.beginGroup(QStringLiteral("PrivateCloudBackingRoot"));
        settings.remove(QString());
        for (auto it = record.cbegin(); it != record.cend(); ++it) {
            settings.setValue(it.key(), it.value());
        }
        settings.sync();
    }

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(_configDirectory.isValid());
        QVERIFY(ConfigFile::setConfDir(_configDirectory.path()));
    }

    void cleanup()
    {
        writeAdoptedRoot({});
    }

    void testStablePrivateBackingPath()
    {
        const auto path = PrivateCloudDrive::backingPath(QStringLiteral("alice@example.test"));
        QCOMPARE(QFileInfo(path).fileName().size(), 64);
        QVERIFY(!path.contains(QStringLiteral("alice")));
        QCOMPARE(path, PrivateCloudDrive::backingPath(QStringLiteral("alice@example.test")));
        QVERIFY(path != PrivateCloudDrive::backingPath(QStringLiteral("bob@example.test")));
        QVERIFY(PrivateCloudDrive::backingPath({}).isEmpty());
    }

    void testSetupDefaultPreservesLegacyBackingPath()
    {
        const auto previous = PrivateCloudDrive::backingPath(accountIdentity());
        QCOMPARE(PrivateCloudDrive::defaultSetupPath(), QStringLiteral("C:/PPCsyncConfig"));
        QCOMPARE(PrivateCloudDrive::backingPath(accountIdentity()), previous);
        QVERIFY(previous.contains(QStringLiteral("/privateCloud/Files/")));
        QVERIFY(previous != PrivateCloudDrive::defaultSetupPath());
    }

    void testConfigureEmptyBackingPathPersistsCanonicalBinding()
    {
        auto record = adoptedRoot();
        record[QStringLiteral("AccountId")] = QStringLiteral("previous-user");
        record[QStringLiteral("Legacy/Extra")] = QStringLiteral("old-value");
        writeAdoptedRoot(record);
        QSettings settings(ConfigFile().configFile(), QSettings::IniFormat);
        settings.setValue(QStringLiteral("Unrelated/Keep"), QStringLiteral("unchanged"));
        FakeDriveBackend backend;
        backend.target = QStringLiteral("\\??\\D:\\Cloud\\Chosen");
        auto equivalentUrl = QUrl(serverUrl());
        equivalentUrl.setHost(equivalentUrl.host().toUpper());
        equivalentUrl.setPort(443);
        equivalentUrl.setPath(QStringLiteral("/"));
        const auto identity = equivalentUrl.toString() + QStringLiteral("\ncanonical-user-01");
        const auto error = PrivateCloudDrive::Detail::configureBackingPath(identity, QStringLiteral("D:/Cloud/Chosen/"), backend, settings);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(backend.requiredExistingPath);
        QCOMPARE(backend.emptyChecks, 1);
        QCOMPARE(backend.added, 0);
        QCOMPARE(backend.labelWrites, 0);
        QVERIFY(settings.group().isEmpty());
        QSettings reloaded(ConfigFile().configFile(), QSettings::IniFormat);
        QCOMPARE(reloaded.value(QStringLiteral("Unrelated/Keep")).toString(), QStringLiteral("unchanged"));
        reloaded.beginGroup(QStringLiteral("PrivateCloudBackingRoot"));
        QCOMPARE(reloaded.allKeys(), QStringList({QStringLiteral("AccountId"), QStringLiteral("Path"), QStringLiteral("ServerUrl")}));
        QCOMPARE(reloaded.value(QStringLiteral("AccountId")).toString(), QStringLiteral("canonical-user-01"));
        QCOMPARE(reloaded.value(QStringLiteral("Path")).toString(), QStringLiteral("D:/Cloud/Chosen"));
        auto canonicalUrl = QUrl(serverUrl()).adjusted(QUrl::StripTrailingSlash);
        canonicalUrl.setPort(-1);
        QCOMPARE(reloaded.value(QStringLiteral("ServerUrl")).toString(), canonicalUrl.toString());
        QVariantMap saved;
        for (const auto &key : reloaded.allKeys()) {
            saved.insert(key, reloaded.value(key));
        }
        QString readError;
        QCOMPARE(PrivateCloudDrive::Detail::backingPath(accountIdentity(), saved, backend, &readError), QStringLiteral("D:/Cloud/Chosen"));
        QVERIFY2(readError.isEmpty(), qPrintable(readError));
    }

    void testConfigureVerifiedFolderReusesNonEmptyLocation()
    {
        writeAdoptedRoot(adoptedRoot());
        QSettings settings(ConfigFile().configFile(), QSettings::IniFormat);
        settings.setValue(u"Unrelated/Keep"_s, u"unchanged"_s);
        FakeDriveBackend backend;
        backend.target = u"\\??\\D:\\Cloud\\Chosen"_s;
        backend.emptyError = u"Existing files and sync database"_s;

        QCOMPARE(PrivateCloudDrive::Detail::configureBackingPath(accountIdentity(), u"D:/Cloud/Chosen"_s, backend, settings), backend.emptyError);
        QCOMPARE(settings.value(u"PrivateCloudBackingRoot/Path"_s).toString(), u"D:/Cloud/Data"_s);
        QCOMPARE(backend.emptyChecks, 1);

        const auto error = PrivateCloudDrive::Detail::configureBackingPath(accountIdentity(), u"D:/Cloud/Chosen"_s, backend, settings, true);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(backend.emptyChecks, 1);
        QVERIFY(backend.requiredExistingPath);
        QCOMPARE(settings.value(u"PrivateCloudBackingRoot/Path"_s).toString(), u"D:/Cloud/Chosen"_s);
        QCOMPARE(settings.value(u"PrivateCloudBackingRoot/AccountId"_s).toString(), u"canonical-user-01"_s);
        QCOMPARE(settings.value(u"Unrelated/Keep"_s).toString(), u"unchanged"_s);
        QCOMPARE(backend.added, 0);
        QCOMPARE(backend.removed, 0);
        QCOMPARE(backend.labelWrites, 0);
    }

    void testConfigureBackingPathRejectsInvalidAdmission_data()
    {
        QTest::addColumn<QString>("identity");
        QTest::addColumn<QString>("path");
        QTest::addColumn<QString>("target");
        QTest::addColumn<QString>("storageError");
        QTest::addColumn<QString>("emptyError");
        QTest::addColumn<bool>("reuseVerifiedFolder");
        const auto add = [](const char *name,
                            const QString &identity,
                            const QString &path = QStringLiteral("D:/Cloud/Chosen"),
                            const QString &target = QStringLiteral("\\??\\D:\\Cloud\\Chosen"),
                            const QString &storageError = {},
                            const QString &emptyError = {}) {
            QTest::newRow(name) << identity << path << target << storageError << emptyError << false;
            if (emptyError.isEmpty()) {
                const QByteArray reuseName = QByteArray("verified-folder-") + name;
                QTest::newRow(reuseName.constData()) << identity << path << target << storageError << emptyError << true;
            }
        };
        add("missing identity", {});
        add("no server separator", QStringLiteral("canonical-user-01"));
        add("missing account", serverUrl() + QStringLiteral("\n"));
        add("blank account", serverUrl() + QStringLiteral("\n  "));
        add("multiple account lines", accountIdentity() + QStringLiteral("\nother-user"));
        add("carriage return account", accountIdentity() + QLatin1Char('\r'));
        add("null account", accountIdentity() + QChar::Null);
        add("insecure server", QStringLiteral("http://cloud.monocloud.online\ncanonical-user-01"));
        add("server credentials", QStringLiteral("https://user@cloud.monocloud.online\ncanonical-user-01"));
        add("server query", serverUrl() + QStringLiteral("?other=1\ncanonical-user-01"));
        if (!Account::enforcedServerUrl().isEmpty()) {
            add("other server", QStringLiteral("https://other.example\ncanonical-user-01"));
        }
        for (const auto &path : QStringList{QString(),
                                            QStringLiteral("D:/"),
                                            QStringLiteral("D:/Cloud/.."),
                                            QStringLiteral("relative/folder"),
                                            QStringLiteral("//server/share/folder"),
                                            QStringLiteral("D:/Cloud/Chosen:stream"),
                                            PrivateCloudDrive::driveRoot() + QStringLiteral("inside"),
                                            PrivateCloudDrive::driveRoot().toLower() + QStringLiteral("inside")}) {
            add(qPrintable(QStringLiteral("invalid path: ") + path), accountIdentity(), path);
        }
        add("redirected target", accountIdentity(), QStringLiteral("D:/Cloud/Chosen"), QStringLiteral("\\??\\D:\\Elsewhere"));
        add("USB storage", accountIdentity(), QStringLiteral("D:/Cloud/Chosen"), {}, QStringLiteral("USB storage is not permitted"));
        add("missing folder", accountIdentity(), QStringLiteral("D:/Cloud/Chosen"), {}, QStringLiteral("Folder must exist"));
        add("existing data",
            accountIdentity(),
            QStringLiteral("D:/Cloud/Chosen"),
            QStringLiteral("\\??\\D:\\Cloud\\Chosen"),
            {},
            QStringLiteral("Folder is occupied"));
        add("cloud placeholder root",
            accountIdentity(),
            QStringLiteral("D:/Cloud/Chosen"),
            QStringLiteral("\\??\\D:\\Cloud\\Chosen"),
            {},
            QStringLiteral("Existing cloud root"));
    }

    void testConfigureBackingPathRejectsInvalidAdmission()
    {
        QFETCH(QString, identity);
        QFETCH(QString, path);
        QFETCH(QString, target);
        QFETCH(QString, storageError);
        QFETCH(QString, emptyError);
        QFETCH(bool, reuseVerifiedFolder);
        writeAdoptedRoot(adoptedRoot());
        QSettings settings(ConfigFile().configFile(), QSettings::IniFormat);
        FakeDriveBackend backend;
        backend.target = target;
        backend.pathError = storageError;
        backend.emptyError = emptyError;
        QVERIFY(!PrivateCloudDrive::Detail::configureBackingPath(identity, path, backend, settings, reuseVerifiedFolder).isEmpty());
        QCOMPARE(settings.value(QStringLiteral("PrivateCloudBackingRoot/Path")).toString(), QStringLiteral("D:/Cloud/Data"));
        QCOMPARE(settings.value(QStringLiteral("PrivateCloudBackingRoot/AccountId")).toString(), QStringLiteral("canonical-user-01"));
        QCOMPARE(backend.added, 0);
        QCOMPARE(backend.removed, 0);
        QCOMPARE(backend.labelWrites, 0);
    }

    void testConfigureBackingPathReportsUnwritableSettings()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile blocker(directory.filePath(QStringLiteral("not-a-directory")));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        QSettings unavailable(blocker.fileName() + QStringLiteral("/settings.ini"), QSettings::IniFormat);
        FakeDriveBackend backend;
        backend.target = QStringLiteral("\\??\\D:\\Cloud\\Chosen");
        QVERIFY(!PrivateCloudDrive::Detail::configureBackingPath(accountIdentity(), QStringLiteral("D:/Cloud/Chosen"), backend, unavailable).isEmpty());
        QVERIFY(!unavailable.contains(QStringLiteral("PrivateCloudBackingRoot/Path")));
    }

    void testConfigureRealEmptyFolderAndHiddenContents()
    {
#ifdef Q_OS_WIN
        if (!PrivateCloudDrive::isEnabled()) {
            QSKIP("New folder setup is enabled in the privateCloud Windows build.");
        }
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        writeAdoptedRoot(adoptedRoot());
        QFile cached(directory.filePath(QStringLiteral("old-account-journal.db")));
        QVERIFY(cached.open(QIODevice::WriteOnly));
        QCOMPARE(cached.write("preserved"), 9);
        cached.close();
        const auto native = QDir::toNativeSeparators(cached.fileName()).toStdWString();
        QVERIFY(SetFileAttributesW(native.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM));
        QVERIFY(!PrivateCloudDrive::configureBackingPath(serverUrl() + QStringLiteral("\nother-user"), directory.path()).isEmpty());
        QSettings previous(ConfigFile().configFile(), QSettings::IniFormat);
        QCOMPARE(previous.value(QStringLiteral("PrivateCloudBackingRoot/Path")).toString(), QStringLiteral("D:/Cloud/Data"));
        QVERIFY(cached.open(QIODevice::ReadOnly));
        QCOMPARE(cached.readAll(), QByteArrayLiteral("preserved"));
        cached.close();
        QVERIFY(!PrivateCloudDrive::configureBackingPath(accountIdentity(), directory.filePath(QStringLiteral("missing"))).isEmpty());
        QVERIFY(cached.remove());
        const auto error = PrivateCloudDrive::configureBackingPath(accountIdentity(), directory.path());
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QString readError;
        QCOMPARE(PrivateCloudDrive::backingPath(accountIdentity(), &readError), directory.path());
        QVERIFY2(readError.isEmpty(), qPrintable(readError));
        QVERIFY(PrivateCloudDrive::backingPath(serverUrl() + QStringLiteral("\nother-user"), &readError).isEmpty());
        QVERIFY(!readError.isEmpty());
#else
        QSKIP("Windows folder storage validation requires Windows.");
#endif
    }

    void testMappingProbeChecksExactTargetWithoutFilesystemAccess()
    {
        FakeDriveBackend backend;
        backend.pathError = QStringLiteral("A filesystem probe must not occur");
        backend.targets = {QStringLiteral("\\??\\C:\\privateCloud\\Files\\account")};
        QVERIFY(PrivateCloudDrive::Detail::isMapped(QStringLiteral("c:/PRIVATECLOUD/Files/account/"), backend));
        QVERIFY(PrivateCloudDrive::Detail::isMapped(QStringLiteral("C:\\privateCloud\\Files\\account"), backend));
        QVERIFY(!PrivateCloudDrive::Detail::isMapped(QStringLiteral("C:/privateCloud/Files/other"), backend));
        backend.targets.prepend(QStringLiteral("\\Device\\OtherDrive"));
        QVERIFY(!PrivateCloudDrive::Detail::isMapped(QStringLiteral("C:/privateCloud/Files/account"), backend));
        backend.targets.clear();
        QVERIFY(!PrivateCloudDrive::Detail::isMapped(QStringLiteral("C:/privateCloud/Files/account"), backend));
        backend.targets = {backend.target};
        backend.queryError = QStringLiteral("Could not query drive");
        QVERIFY(!PrivateCloudDrive::Detail::isMapped(QStringLiteral("C:/privateCloud/Files/account"), backend));
        const auto previousQueries = backend.queries;
        QVERIFY(!PrivateCloudDrive::Detail::isMapped({}, backend));
        QVERIFY(!PrivateCloudDrive::Detail::isMapped(QStringLiteral("C:/"), backend));
        QVERIFY(!PrivateCloudDrive::Detail::isMapped(QStringLiteral("relative/path"), backend));
        QCOMPARE(backend.queries, previousQueries);
        QCOMPARE(backend.validations, 0);
        QCOMPARE(backend.added, 0);
        QCOMPARE(backend.removed, 0);
    }

    void testAdoptedRootMatchesCanonicalAccount()
    {
        FakeDriveBackend backend;
        backend.target = QStringLiteral("\\??\\D:\\Cloud\\Data");
        auto record = adoptedRoot();
        auto equivalentUrl = QUrl(serverUrl());
        equivalentUrl.setHost(equivalentUrl.host().toUpper());
        equivalentUrl.setPort(443);
        equivalentUrl.setPath(QStringLiteral("/"));
        record[QStringLiteral("ServerUrl")] = equivalentUrl.toString();
        record[QStringLiteral("Path")] = QStringLiteral("D:/Cloud/Data/");
        QString error;
        QCOMPARE(PrivateCloudDrive::Detail::backingPath(accountIdentity(), record, backend, &error), QStringLiteral("D:/Cloud/Data"));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(backend.requiredExistingPath);
        QCOMPARE(backend.queries, 0);
        QCOMPARE(backend.added, 0);
        QCOMPARE(backend.labelWrites, 0);
    }

    void testAdoptedRootRejectsInvalidBinding_data()
    {
        QTest::addColumn<QString>("identity");
        QTest::addColumn<QVariantMap>("record");
        QTest::addColumn<QString>("target");
        QTest::addColumn<QString>("storageError");
        const auto add = [](const char *name,
                            const QString &identity,
                            const QVariantMap &record,
                            const QString &target = QStringLiteral("\\??\\D:\\Cloud\\Data"),
                            const QString &storageError = {}) {
            QTest::newRow(name) << identity << record << target << storageError;
        };
        add("different canonical user", serverUrl() + QStringLiteral("\nother-user"), adoptedRoot());
        add("login alias is not canonical user", serverUrl() + QStringLiteral("\nlogin-alias"), adoptedRoot());
        add("different server", QStringLiteral("https://other.example\ncanonical-user-01"), adoptedRoot());
        add("missing identity", QString(), adoptedRoot());
        add("malformed identity", QStringLiteral("canonical-user-01"), adoptedRoot());
        auto record = adoptedRoot();
        record[QStringLiteral("ServerUrl")] = QStringLiteral("https://other.example");
        add("stored server mismatch", accountIdentity(), record);
        record = adoptedRoot();
        record.remove(QStringLiteral("AccountId"));
        add("missing account binding", accountIdentity(), record);
        record = adoptedRoot();
        record[QStringLiteral("SecondRoot/Path")] = QStringLiteral("D:/Second");
        add("multiple roots", accountIdentity(), record);
        record = adoptedRoot();
        record[QStringLiteral("Path")] = QStringList{QStringLiteral("D:/Cloud/Data"), QStringLiteral("D:/Second")};
        add("multiple path values", accountIdentity(), record);
        for (const auto &path :
             {QStringLiteral("relative/path"), QStringLiteral("D:/"), QStringLiteral("D:/Other/../Cloud/Data"), QStringLiteral("//server/share/cloud")}) {
            record = adoptedRoot();
            record[QStringLiteral("Path")] = path;
            add(qPrintable(path), accountIdentity(), record);
        }
        add("redirected root", accountIdentity(), adoptedRoot(), QStringLiteral("\\??\\D:\\Other"));
        add("USB or unverified storage", accountIdentity(), adoptedRoot(), QString(), QStringLiteral("External storage is not permitted"));
        add("missing root", accountIdentity(), adoptedRoot(), QString(), QStringLiteral("Backing folder is unavailable"));
    }

    void testAdoptedRootRejectsInvalidBinding()
    {
        QFETCH(QString, identity);
        QFETCH(QVariantMap, record);
        QFETCH(QString, target);
        QFETCH(QString, storageError);
        FakeDriveBackend backend;
        backend.target = target;
        backend.pathError = storageError;
        QString error;
        QVERIFY(PrivateCloudDrive::Detail::backingPath(identity, record, backend, &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QCOMPARE(backend.added, 0);
        QCOMPARE(backend.removed, 0);
        QCOMPARE(backend.labelWrites, 0);
    }

    void testPersistedAdoptedRootAndWrongFolder()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto record = adoptedRoot();
        record[QStringLiteral("Path")] = directory.path();
        writeAdoptedRoot(record);
        QString error;
        QCOMPARE(PrivateCloudDrive::backingPath(accountIdentity(), &error), directory.path());
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(PrivateCloudDrive::adoptedBackingPathError(accountIdentity(), directory.path()).isEmpty());
        QVERIFY(!PrivateCloudDrive::adoptedBackingPathError(accountIdentity(), directory.filePath(QStringLiteral("another-root"))).isEmpty());
        QVERIFY(PrivateCloudDrive::backingPath(serverUrl() + QStringLiteral("\nother-user"), &error).isEmpty());
        QVERIFY(!error.isEmpty());
        record[QStringLiteral("Path")] = directory.filePath(QStringLiteral("missing"));
        writeAdoptedRoot(record);
        QVERIFY(PrivateCloudDrive::backingPath(accountIdentity(), &error).isEmpty());
        QVERIFY(!error.isEmpty());
#else
        QSKIP("Adopted Windows backing folders require Windows.");
#endif
    }

    void testPersistedAdoptedRootRejectsJunctionAncestor()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto target = directory.filePath(QStringLiteral("target"));
        const auto link = directory.filePath(QStringLiteral("redirect"));
        QVERIFY(QDir().mkpath(target + QStringLiteral("/child")));
        QProcess createJunction;
        createJunction.start(QStringLiteral("cmd.exe"),
                             {QStringLiteral("/d"),
                              QStringLiteral("/c"),
                              QStringLiteral("mklink"),
                              QStringLiteral("/J"),
                              QDir::toNativeSeparators(link),
                              QDir::toNativeSeparators(target)});
        QVERIFY(createJunction.waitForFinished());
        QCOMPARE(createJunction.exitCode(), 0);
        const auto removeJunction = qScopeGuard([&link] {
            const auto native = QDir::toNativeSeparators(link).toStdWString();
            RemoveDirectoryW(native.c_str());
        });
        auto record = adoptedRoot();
        record[QStringLiteral("Path")] = QString(link + QStringLiteral("/child"));
        writeAdoptedRoot(record);
        QString error;
        QVERIFY(PrivateCloudDrive::backingPath(accountIdentity(), &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(QDir(target + QStringLiteral("/child")).exists());
#else
        QSKIP("Junction validation requires Windows.");
#endif
    }

    void testSyncRejectsAdoptedRootMismatchBeforeDiscovery()
    {
        if (!PrivateCloudDrive::isEnabled()) {
            QSKIP("The adopted-root sync gate is enabled in the privateCloud Windows build.");
        }
        FakeFolder folder{FileInfo::A12_B12_C12_S12(), {}, {}, false};
        QVERIFY(folder.account()->setUrl(QUrl(serverUrl())));
        folder.account()->setDavUser(QStringLiteral("another-canonical-user"));
        writeAdoptedRoot(adoptedRoot());
        constexpr qint64 previousSync = 123456;
        folder.syncJournal().keyValueStoreSet(QStringLiteral("last_sync"), previousSync);
        QCOMPARE(folder.syncJournal().keyValueStoreGetInt(QStringLiteral("last_sync"), -1), previousSync);
        const auto originalState = folder.currentLocalState();
        QSignalSpy errors(&folder.syncEngine(), &SyncEngine::syncError);
        QSignalSpy finished(&folder.syncEngine(), &SyncEngine::finished);
        folder.syncEngine().startSync();
        QCOMPARE(errors.count(), 1);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.first().first().toBool(), false);
        QCOMPARE(folder.currentLocalState(), originalState);
        QVERIFY(!folder.syncEngine().isSyncRunning());
        QCOMPARE(folder.syncJournal().keyValueStoreGetInt(QStringLiteral("last_sync"), -1), previousSync);
    }

    void testOccupiedDrive_data()
    {
        QTest::addColumn<QStringList>("targets");
        QTest::newRow("physical-drive") << QStringList{QStringLiteral("\\Device\\HarddiskVolume1")};
        QTest::newRow("another-alias") << QStringList{QStringLiteral("\\??\\C:\\OtherFolder")};
        QTest::newRow("stacked-own-alias") << QStringList{FakeDriveBackend().target, QStringLiteral("\\Device\\HarddiskVolume1")};
    }

    void testOccupiedDrive()
    {
        QFETCH(QStringList, targets);
        FakeDriveBackend backend;
        backend.targets = targets;
        QVERIFY(!PrivateCloudDrive::Detail::checkAvailability({}, backend).isEmpty());
        QVERIFY(!PrivateCloudDrive::Detail::mount({}, backend).isEmpty());
        QVERIFY(PrivateCloudDrive::Detail::unmount({}, backend).isEmpty());
        QCOMPARE(backend.targets, targets);
        QCOMPARE(backend.added, 0);
        QCOMPARE(backend.removed, 0);
        QCOMPARE(backend.labelWrites, 0);
    }

    void testMountIdempotentAndRestoreLabel_data()
    {
        QTest::addColumn<QVariant>("previousLabel");
        QTest::newRow("no-label") << QVariant();
        QTest::newRow("existing-label") << QVariant(QStringLiteral("My old label"));
    }

    void testMountIdempotentAndRestoreLabel()
    {
        QFETCH(QVariant, previousLabel);
        FakeDriveBackend backend;
        backend.currentLabel = previousLabel;
        QVERIFY(PrivateCloudDrive::Detail::checkAvailability({}, backend).isEmpty());
        QCOMPARE(backend.added, 0);
        QVERIFY(PrivateCloudDrive::Detail::mount({}, backend).isEmpty());
        QCOMPARE(backend.currentLabel.toString(), QStringLiteral("privateCloud"));
        QVERIFY(PrivateCloudDrive::Detail::mount({}, backend).isEmpty());
        QCOMPARE(backend.added, 1);
        QCOMPARE(backend.targets, QStringList{backend.target});
        QVERIFY(PrivateCloudDrive::Detail::unmount({}, backend).isEmpty());
        QVERIFY(backend.targets.isEmpty());
        QCOMPARE(backend.currentLabel, previousLabel);
        QVERIFY(backend.state.isEmpty());
        QCOMPARE(backend.removed, 1);
    }

    void testUserLabelChangePreserved()
    {
        FakeDriveBackend backend;
        QVERIFY(PrivateCloudDrive::Detail::mount({}, backend).isEmpty());
        backend.currentLabel = QStringLiteral("User label");
        QVERIFY(PrivateCloudDrive::Detail::unmount({}, backend).isEmpty());
        QCOMPARE(backend.currentLabel.toString(), QStringLiteral("User label"));
        QVERIFY(backend.state.isEmpty());
    }

    void testRaceRemovesOnlyOwnMapping()
    {
        FakeDriveBackend backend;
        backend.race = true;
        QVERIFY(!PrivateCloudDrive::Detail::mount({}, backend).isEmpty());
        QCOMPARE(backend.targets, QStringList{QStringLiteral("\\Device\\OtherDrive")});
        QCOMPARE(backend.removed, 1);
        QCOMPARE(backend.labelWrites, 0);
    }

    void testReplacementBeforeLabelDoesNotRenameForeignDrive()
    {
        FakeDriveBackend backend;
        backend.replaceOnQuery = 3;
        QVERIFY(!PrivateCloudDrive::Detail::mount({}, backend).isEmpty());
        QCOMPARE(backend.targets, QStringList{QStringLiteral("\\Device\\OtherDrive")});
        QCOMPARE(backend.labelWrites, 0);
        QVERIFY(backend.state.isEmpty());
    }

    void testReplacementAfterRemovalDoesNotRestoreLabelOnForeignDrive()
    {
        FakeDriveBackend backend;
        QVERIFY(PrivateCloudDrive::Detail::mount({}, backend).isEmpty());
        backend.raceOnRemove = true;
        QVERIFY(!PrivateCloudDrive::Detail::unmount({}, backend).isEmpty());
        QCOMPARE(backend.targets, QStringList{QStringLiteral("\\Device\\OtherDrive")});
        QCOMPARE(backend.labelWrites, 1);
        QVERIFY(!backend.state.isEmpty());
    }

    void testSignOutCleanupAllowsAnotherAccount()
    {
        FakeDriveBackend backend;
        backend.currentLabel = QStringLiteral("Previous label");
        QVERIFY(PrivateCloudDrive::Detail::mount(QStringLiteral("account-a"), backend).isEmpty());
        backend.targets.clear(); // Windows removed the per-logon alias at sign-out.
        QVERIFY(PrivateCloudDrive::Detail::unmount(QStringLiteral("account-a"), backend).isEmpty());
        QCOMPARE(backend.currentLabel.toString(), QStringLiteral("Previous label"));
        QVERIFY(backend.state.isEmpty());
        backend.target = QStringLiteral("\\??\\C:\\privateCloud\\Files\\account-b");
        QVERIFY(PrivateCloudDrive::Detail::mount(QStringLiteral("account-b"), backend).isEmpty());
        QCOMPARE(backend.targets, QStringList{backend.target});
    }

    void testUnavailableBackingStillUnmountsOwnDrive()
    {
        FakeDriveBackend backend;
        QVERIFY(PrivateCloudDrive::Detail::mount(QStringLiteral("account-a"), backend).isEmpty());
        backend.pathError = QStringLiteral("Drive disconnected");
        QVERIFY(PrivateCloudDrive::Detail::unmount(QStringLiteral("account-a"), backend).isEmpty());
        QVERIFY(backend.targets.isEmpty());
        QVERIFY(backend.state.isEmpty());
    }

    void testMountRecoversSavedStateAfterSignOut()
    {
        FakeDriveBackend backend;
        backend.currentLabel = QStringLiteral("Previous label");
        QVERIFY(PrivateCloudDrive::Detail::mount(QStringLiteral("missing-old-folder"), backend).isEmpty());
        backend.targets.clear();
        backend.target = QStringLiteral("\\??\\D:\\ExistingCloudFolder");
        backend.validations = 0;

        const auto result = PrivateCloudDrive::Detail::mount(QStringLiteral("new-folder"), backend);
        QVERIFY2(result.isEmpty(), qPrintable(result));
        QCOMPARE(backend.validations, 1); // The old backing folder need not exist.
        QCOMPARE(backend.targets, QStringList{backend.target});
        QCOMPARE(backend.state.value(QStringLiteral("target")).toString(), backend.target);
        QCOMPARE(backend.state.value(QStringLiteral("previousLabel")).toString(), QStringLiteral("Previous label"));
        QCOMPARE(backend.currentLabel.toString(), QStringLiteral("privateCloud"));
        QCOMPARE(backend.removed, 0);
    }

    void testSavedStateRecoveryPreservesOccupiedDrive_data()
    {
        QTest::addColumn<QStringList>("occupiedTargets");
        QTest::newRow("old-alias") << QStringList{FakeDriveBackend().target};
        QTest::newRow("foreign-drive") << QStringList{QStringLiteral("\\Device\\OtherDrive")};
        QTest::newRow("stacked-alias") << QStringList{FakeDriveBackend().target, QStringLiteral("\\Device\\OtherDrive")};
    }

    void testSavedStateRecoveryPreservesOccupiedDrive()
    {
        QFETCH(QStringList, occupiedTargets);
        FakeDriveBackend backend;
        QVERIFY(PrivateCloudDrive::Detail::mount(QStringLiteral("old-folder"), backend).isEmpty());
        const auto saved = backend.state;
        backend.targets = occupiedTargets;
        backend.target = QStringLiteral("\\??\\D:\\ExistingCloudFolder");
        QVERIFY(!PrivateCloudDrive::Detail::mount(QStringLiteral("new-folder"), backend).isEmpty());
        QCOMPARE(backend.targets, occupiedTargets);
        QCOMPARE(backend.state, saved);
        QCOMPARE(backend.added, 1);
        QCOMPARE(backend.removed, 0);
        QCOMPARE(backend.labelWrites, 1);
    }

    void testSavedStateCleanupFailureCanBeRetried_data()
    {
        QTest::addColumn<bool>("labelFailure");
        QTest::newRow("restore-label") << true;
        QTest::newRow("clear-state") << false;
    }

    void testSavedStateCleanupFailureCanBeRetried()
    {
        QFETCH(bool, labelFailure);
        FakeDriveBackend backend;
        backend.currentLabel = QStringLiteral("Previous label");
        QVERIFY(PrivateCloudDrive::Detail::mount(QStringLiteral("old-folder"), backend).isEmpty());
        const auto saved = backend.state;
        backend.targets.clear();
        backend.target = QStringLiteral("\\??\\D:\\ExistingCloudFolder");
        const auto error = QStringLiteral("Access denied");
        if (labelFailure) {
            backend.labelError = error;
        } else {
            backend.saveStateError = error;
        }
        QCOMPARE(PrivateCloudDrive::Detail::mount(QStringLiteral("new-folder"), backend), error);
        QCOMPARE(backend.state, saved);
        QVERIFY(backend.targets.isEmpty());
        QCOMPARE(backend.added, 1);
        backend.labelError.clear();
        backend.saveStateError.clear();
        QVERIFY(PrivateCloudDrive::Detail::mount(QStringLiteral("new-folder"), backend).isEmpty());
        QCOMPARE(backend.targets, QStringList{backend.target});
        QCOMPARE(backend.state.value(QStringLiteral("previousLabel")).toString(), QStringLiteral("Previous label"));
    }

    void testSavedStateRecoveryRechecksDriveOwnership_data()
    {
        QTest::addColumn<int>("replaceOnQuery");
        QTest::newRow("before-cleanup") << 2;
        QTest::newRow("after-cleanup") << 3;
    }

    void testSavedStateRecoveryRechecksDriveOwnership()
    {
        QFETCH(int, replaceOnQuery);
        FakeDriveBackend backend;
        QVERIFY(PrivateCloudDrive::Detail::mount(QStringLiteral("old-folder"), backend).isEmpty());
        backend.targets.clear();
        backend.target = QStringLiteral("\\??\\D:\\ExistingCloudFolder");
        backend.queries = 0;
        backend.replaceOnQuery = replaceOnQuery;
        QVERIFY(!PrivateCloudDrive::Detail::mount(QStringLiteral("new-folder"), backend).isEmpty());
        QCOMPARE(backend.targets, QStringList{QStringLiteral("\\Device\\OtherDrive")});
        QCOMPARE(backend.added, 1);
        QCOMPARE(backend.removed, 0);
        QCOMPARE(backend.labelWrites, replaceOnQuery == 2 ? 1 : 2);
    }

    void testNotFixedDriveRolledBack()
    {
        FakeDriveBackend backend;
        backend.fixed = false;
        QVERIFY(!PrivateCloudDrive::Detail::mount({}, backend).isEmpty());
        QVERIFY(backend.targets.isEmpty());
        QCOMPARE(backend.labelWrites, 0);
    }

    void testValidationAndQueryFailuresDoNotWrite()
    {
        FakeDriveBackend backend;
        backend.pathError = QStringLiteral("External USB drive");
        QCOMPARE(PrivateCloudDrive::Detail::mount({}, backend), backend.pathError);
        QCOMPARE(backend.added, 0);
        backend.pathError.clear();
        backend.queryError = QStringLiteral("Access denied");
        QCOMPARE(PrivateCloudDrive::Detail::mount({}, backend), backend.queryError);
        QCOMPARE(backend.added, 0);
        QCOMPARE(backend.labelWrites, 0);
    }

    void testRemovalFailureKeepsRecoveryState()
    {
        FakeDriveBackend backend;
        QVERIFY(PrivateCloudDrive::Detail::mount({}, backend).isEmpty());
        backend.removeError = QStringLiteral("Access denied");
        QCOMPARE(PrivateCloudDrive::Detail::unmount({}, backend), backend.removeError);
        QVERIFY(!backend.state.isEmpty());
        QCOMPARE(backend.currentLabel.toString(), QStringLiteral("privateCloud"));
        QCOMPARE(backend.targets, QStringList{backend.target});
    }

    void testExplicitlyAuthorizedRealXDrive()
    {
#ifdef Q_OS_WIN
        if (qEnvironmentVariable("PRIVATECLOUD_TEST_DRIVE_MAPPING") != QStringLiteral("1")) {
            QSKIP("Real drive mapping requires explicit opt-in for the authorized X: development drive.");
        }
        QCOMPARE(PrivateCloudDrive::driveRoot(), QStringLiteral("X:/"));
        QVERIFY(PrivateCloudDrive::isEnabled());
        wchar_t existing[32768] = {};
        const auto found = QueryDosDeviceW(L"X:", existing, 32768);
        QVERIFY2(!found && GetLastError() == ERROR_FILE_NOT_FOUND, "X: must be completely unused before this test.");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto availabilityError = PrivateCloudDrive::checkAvailability(directory.path());
        QVERIFY2(availabilityError.isEmpty(), qPrintable(availabilityError));
        QFile original(directory.filePath(QStringLiteral("mapping-test.txt")));
        QVERIFY(original.open(QIODevice::WriteOnly));
        QCOMPARE(original.write("privateCloud"), 12);
        original.close();
        const auto mountError = PrivateCloudDrive::mount(directory.path());
        QVERIFY2(mountError.isEmpty(), qPrintable(mountError));
        const auto cleanup = qScopeGuard([&directory] {
            PrivateCloudDrive::unmount(directory.path());
        });
        QCOMPARE(GetDriveTypeW(L"X:\\"), UINT(DRIVE_FIXED));
        QFile alias(QStringLiteral("X:/mapping-test.txt"));
        QVERIFY(alias.open(QIODevice::ReadOnly));
        QCOMPARE(alias.readAll(), QByteArrayLiteral("privateCloud"));
        alias.close();
        PrivateCloudDrive::unmount(directory.path());
        QVERIFY(!QueryDosDeviceW(L"X:", existing, 32768));
        QCOMPARE(GetLastError(), DWORD(ERROR_FILE_NOT_FOUND));
        QVERIFY(original.exists());
#else
        QSKIP("Windows drive mapping is Windows-only.");
#endif
    }
};

QTEST_GUILESS_MAIN(TestPrivateCloudDrive)
#include "testprivateclouddrive.moc"
