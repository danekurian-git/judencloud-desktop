/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "account.h"
#include "common/ownsql.h"
#include "common/syncjournaldb.h"
#include "common/utility.h"
#include "privatecloudfolderadoption.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

using namespace Qt::StringLiterals;
using namespace OCC;

class TestPrivateCloudFolderAdoption : public QObject
{
    Q_OBJECT

    struct Fixture {
        QTemporaryDir directory;
        AccountPtr account = Account::create();
        QString root;
        QString profile;
        FolderDefinition definition;
        QByteArray journalBytes;

        Fixture()
        {
            QVERIFY(directory.isValid());
            root = directory.filePath(u"files"_s);
            profile = directory.filePath(u"nextcloud.cfg"_s);
            QVERIFY(QDir().mkpath(root));
            account->setUrl(Account::enforcedServerUrl().isEmpty() ? QUrl(u"https://cloud.example"_s) : Account::enforcedServerUrl());
            account->setDavUser(u"canonical-account"_s);
            definition.localPath = FolderDefinition::prepareLocalPath(root);
            definition.targetPath = u"/"_s;
            definition.alias = u"2"_s;
            definition.journalPath = u".sync_123abc.db"_s;
            definition.virtualFilesMode = Vfs::WindowsCfApi;
            definition.ignoreHiddenFiles = false;
            QSettings settings(profile, QSettings::IniFormat);
            settings.setValue(u"Accounts/version"_s, 13);
            settings.setValue(u"Accounts/0/version"_s, 13);
            settings.setValue(u"Accounts/0/url"_s, account->url().toString());
            settings.setValue(u"Accounts/0/dav_user"_s, account->davUser());
            settings.beginGroup(u"Accounts/0/FoldersWithPlaceholders/2"_s);
            FolderDefinition::save(settings, definition);
            settings.sync();
            {
                SyncJournalDb journal(definition.absoluteJournalPath());
                QVERIFY(journal.open());
                journal.keyValueStoreSet(u"last_sync"_s, qint64(123456));
                journal.setSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, {u"excluded/"_s});
                journal.commit(u"adoption fixture"_s, false);
            }
            journalBytes = read(definition.absoluteJournalPath());
            QVERIFY(!journalBytes.isEmpty());
            QVERIFY(write(QDir(root).filePath(u"keep.txt"_s), "existing user file"));
        }

        static bool write(const QString &path, const QByteArray &bytes)
        {
            QFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
        }

        static QByteArray read(const QString &path)
        {
            QFile file(path);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        }

        PrivateCloudFolderAdoption::Result inspect(bool running = false)
        {
            return PrivateCloudFolderAdoption::inspect(account, root, {profile}, [running] {
                return running;
            });
        }
    };

private Q_SLOTS:
    void admitsMatchingExistingFolder()
    {
        Fixture fixture;
        const auto result = fixture.inspect();
        QVERIFY(result.found);
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.definition.journalPath, fixture.definition.journalPath);
        QCOMPARE(result.definition.ignoreHiddenFiles, false);
        QCOMPARE(result.definition.virtualFilesMode, Vfs::WindowsCfApi);
        QCOMPARE(Fixture::read(fixture.definition.absoluteJournalPath()), fixture.journalBytes);
    }

    void acceptsEquivalentHttpsServer()
    {
        Fixture fixture;
        auto url = fixture.account->url();
        url.setPort(443);
        url.setPath(url.path() + u'/');
        QSettings settings(fixture.profile, QSettings::IniFormat);
        settings.setValue(u"Accounts/0/url"_s, url.toString());
        settings.sync();
        QVERIFY(fixture.inspect().error.isEmpty());
    }

    void admitsFullyDownloadedFolder()
    {
        Fixture fixture;
        QSettings settings(fixture.profile, QSettings::IniFormat);
        settings.setValue(u"Accounts/0/FoldersWithPlaceholders/2/virtualFilesMode"_s, u"off"_s);
        settings.sync();
        const auto result = fixture.inspect();
        QVERIFY(result.found);
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.definition.virtualFilesMode, Vfs::Off);
        QCOMPARE(Fixture::read(fixture.definition.absoluteJournalPath()), fixture.journalBytes);
    }

    void rejectsChangedIdentityOrDefinition_data()
    {
        QTest::addColumn<QString>("key");
        QTest::addColumn<QVariant>("value");
        QTest::newRow("other-server") << u"Accounts/0/url"_s << QVariant(u"https://elsewhere.example"_s);
        QTest::newRow("other-user") << u"Accounts/0/dav_user"_s << QVariant(u"different-account"_s);
        QTest::newRow("missing-user") << u"Accounts/0/dav_user"_s << QVariant(QString());
        QTest::newRow("http") << u"Accounts/0/url"_s << QVariant(u"http://cloud.monocloud.online"_s);
        QTest::newRow("future-config") << u"Accounts/version"_s << QVariant(99);
        QTest::newRow("future-account") << u"Accounts/0/version"_s << QVariant(99);
        QTest::newRow("future-folder") << u"Accounts/0/FoldersWithPlaceholders/2/version"_s << QVariant(99);
        QTest::newRow("subfolder") << u"Accounts/0/FoldersWithPlaceholders/2/targetPath"_s << QVariant(u"/subfolder"_s);
        QTest::newRow("suffix-vfs") << u"Accounts/0/FoldersWithPlaceholders/2/virtualFilesMode"_s << QVariant(u"suffix"_s);
        QTest::newRow("unknown-vfs") << u"Accounts/0/FoldersWithPlaceholders/2/virtualFilesMode"_s << QVariant(u"unknown"_s);
        QTest::newRow("journal-traversal") << u"Accounts/0/FoldersWithPlaceholders/2/journalPath"_s << QVariant(u"../.sync_123abc.db"_s);
        QTest::newRow("wrong-journal") << u"Accounts/0/FoldersWithPlaceholders/2/journalPath"_s << QVariant(u".sync_999999.db"_s);
    }

    void rejectsChangedIdentityOrDefinition()
    {
        QFETCH(QString, key);
        QFETCH(QVariant, value);
        Fixture fixture;
        QSettings settings(fixture.profile, QSettings::IniFormat);
        settings.setValue(key, value);
        settings.sync();
        const auto result = fixture.inspect();
        QVERIFY(result.found);
        QVERIFY(!result.error.isEmpty());
        QCOMPARE(Fixture::read(fixture.definition.absoluteJournalPath()), fixture.journalBytes);
    }

    void rejectsUnknownOrMultipleJournals()
    {
        Fixture fixture;
        const auto unknown = PrivateCloudFolderAdoption::inspect(fixture.account, fixture.root, {}, [] {
            return false;
        });
        QVERIFY(unknown.found);
        QVERIFY(!unknown.error.isEmpty());
        QVERIFY(Fixture::write(QDir(fixture.root).filePath(u".sync_999.db"_s), fixture.journalBytes));
        QVERIFY(!fixture.inspect().error.isEmpty());
    }

    void rejectsCorruptAndMissingJournal()
    {
        Fixture fixture;
        QVERIFY(Fixture::write(fixture.definition.absoluteJournalPath(), "not a database"));
        QVERIFY(!fixture.inspect().error.isEmpty());
        QVERIFY(QFile::remove(fixture.definition.absoluteJournalPath()));
        QVERIFY(!fixture.inspect().error.isEmpty());
    }

    void rejectsObsoleteJournals_data()
    {
        QTest::addColumn<QString>("legacyName");
        QTest::addColumn<bool>("modernJournal");
        for (const auto &name : {u".csync_journal.db"_s, u".csync_journal.db-wal"_s, u".csync_journal.db-shm"_s}) {
            QTest::newRow(qPrintable(name + u"-coexisting"_s)) << name << true;
            QTest::newRow(qPrintable(name + u"-alone"_s)) << name << false;
        }
    }

    void rejectsObsoleteJournals()
    {
        QFETCH(QString, legacyName);
        QFETCH(bool, modernJournal);
        Fixture fixture;
        const auto legacyPath = QDir(fixture.root).filePath(legacyName);
        const QByteArray original("obsolete sync history");
        QVERIFY(Fixture::write(legacyPath, original));
        if (!modernJournal) {
            QVERIFY(QFile::remove(fixture.definition.absoluteJournalPath()));
        }
        const auto result =
            PrivateCloudFolderAdoption::inspect(fixture.account, fixture.root, modernJournal ? QStringList{fixture.profile} : QStringList{}, [] {
                return false;
            });
        QVERIFY(result.found);
        QVERIFY(!result.error.isEmpty());
        QCOMPARE(Fixture::read(legacyPath), original);
        if (modernJournal) {
            QCOMPARE(Fixture::read(fixture.definition.absoluteJournalPath()), fixture.journalBytes);
        } else {
            QVERIFY(!QFileInfo::exists(fixture.definition.absoluteJournalPath()));
        }
        QCOMPARE(Fixture::read(QDir(fixture.root).filePath(u"keep.txt"_s)), QByteArray("existing user file"));
    }

    void rejectsActiveClient()
    {
        Fixture fixture;
        QVERIFY(!fixture.inspect(true).error.isEmpty());
        const auto admitted = fixture.inspect();
        QVERIFY(!PrivateCloudFolderAdoption::prepare(fixture.account, admitted, fixture.directory.filePath(u"backups"_s), [] {
                     return true;
                 }).isEmpty());
        QVERIFY(!QFileInfo::exists(fixture.directory.filePath(u"backups"_s)));
    }

    void freshFolderNeedsNoAdoption()
    {
        Fixture fixture;
        const auto empty = fixture.directory.filePath(u"empty"_s);
        QVERIFY(QDir().mkpath(empty));
        const auto result = PrivateCloudFolderAdoption::inspect(fixture.account, empty, {fixture.profile}, [] {
            return true;
        });
        QVERIFY(!result.found);
        QVERIFY(result.error.isEmpty());
    }

    void unrelatedCorruptProfileDoesNotBlockFreshFolder()
    {
        Fixture fixture;
        const auto empty = fixture.directory.filePath(u"empty"_s);
        QVERIFY(QDir().mkpath(empty));
        QVERIFY(Fixture::write(fixture.profile, "[malformed"));
        const auto result = PrivateCloudFolderAdoption::inspect(fixture.account, empty, {fixture.profile}, [] {
            return false;
        });
        QVERIFY(!result.found);
        QVERIFY(result.error.isEmpty());
    }

#ifdef Q_OS_WIN
    void rejectsJournalHeldByWriter()
    {
        Fixture fixture;
        const auto admitted = fixture.inspect();
        const auto journal = QDir::toNativeSeparators(fixture.definition.absoluteJournalPath()).toStdWString();
        const Utility::UniqueHandle writer(CreateFileW(journal.c_str(),
                                                       GENERIC_READ | GENERIC_WRITE,
                                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                       nullptr,
                                                       OPEN_EXISTING,
                                                       FILE_ATTRIBUTE_NORMAL,
                                                       nullptr));
        QVERIFY(writer.get() != INVALID_HANDLE_VALUE);
        const auto backups = fixture.directory.filePath(u"backups"_s);
        QVERIFY(!PrivateCloudFolderAdoption::prepare(fixture.account, admitted, backups, [] {
                     return false;
                 }).isEmpty());
        QVERIFY(!QFileInfo::exists(backups));
        QCOMPARE(Fixture::read(fixture.definition.absoluteJournalPath()), fixture.journalBytes);
    }
#endif

    void backupPreservesSourceBytesAndHistory()
    {
        Fixture fixture;
        const auto journal = fixture.definition.absoluteJournalPath();
        // Capture a real committed WAL before its separate writer closes. The
        // candidate has no live writer, like a journal left after a client exits.
        QTemporaryDir writerDirectory;
        QVERIFY(writerDirectory.isValid());
        const auto writerPath = writerDirectory.filePath(u"journal.db"_s);
        QVERIFY(QFile::copy(journal, writerPath));
        SqlDatabase writer;
        QVERIFY(writer.openOrCreateReadWrite(writerPath));
        SqlQuery update(writer);
        update.prepare("INSERT OR REPLACE INTO key_value_store (key, value) VALUES ('last_sync', '654321');");
        QVERIFY(update.exec());
        update.reset_and_clear_bindings();
        const auto databaseBytes = Fixture::read(writerPath);
        const auto walBytes = Fixture::read(writerPath + u"-wal"_s);
        const auto shmBytes = Fixture::read(writerPath + u"-shm"_s);
        QVERIFY(!databaseBytes.isEmpty());
        QVERIFY(!walBytes.isEmpty());
        QVERIFY(!shmBytes.isEmpty());
        writer.close();
        QVERIFY(Fixture::write(journal, databaseBytes));
        QVERIFY(Fixture::write(journal + u"-wal"_s, walBytes));
        QVERIFY(Fixture::write(journal + u"-shm"_s, shmBytes));
        const auto profileBytes = Fixture::read(fixture.profile);
        const auto backups = fixture.directory.filePath(u"backups"_s);
        const auto result = fixture.inspect();
        const auto error = PrivateCloudFolderAdoption::prepare(fixture.account, result, backups, [] {
            return false;
        });
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto directories = QDir(backups).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QCOMPARE(directories.size(), 1);
        const QDir backup(QDir(backups).filePath(directories.front()));
        QCOMPARE(Fixture::read(backup.filePath(fixture.definition.journalPath)), databaseBytes);
        QCOMPARE(Fixture::read(backup.filePath(fixture.definition.journalPath + u"-wal"_s)), walBytes);
        QCOMPARE(Fixture::read(backup.filePath(fixture.definition.journalPath + u"-shm"_s)), shmBytes);
        QCOMPARE(Fixture::read(backup.filePath(u"previous-profile.cfg"_s)), profileBytes);
        QVERIFY(QJsonDocument::fromJson(Fixture::read(backup.filePath(u"manifest.json"_s))).isArray());
        QCOMPARE(Fixture::read(fixture.profile), profileBytes);
        QCOMPARE(Fixture::read(journal), databaseBytes);
        QCOMPARE(Fixture::read(journal + u"-wal"_s), walBytes);
        QCOMPARE(Fixture::read(journal + u"-shm"_s), shmBytes);
        QCOMPARE(Fixture::read(QDir(fixture.root).filePath(u"keep.txt"_s)), QByteArray("existing user file"));
        QVERIFY(backup.entryList({u"validation-*"_s}, QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
    }

    void rejectsDamagedJournalWithValidHeader()
    {
        Fixture fixture;
        const QByteArray damaged = QByteArray("SQLite format 3\0", 16) + QByteArray(4096, 'x');
        QVERIFY(Fixture::write(fixture.definition.absoluteJournalPath(), damaged));
        const auto profileBytes = Fixture::read(fixture.profile);
        const auto admitted = fixture.inspect();
        QVERIFY(admitted.error.isEmpty());
        const auto backups = fixture.directory.filePath(u"backups"_s);
        const auto error = PrivateCloudFolderAdoption::prepare(fixture.account, admitted, backups, [] {
            return false;
        });
        QVERIFY(!error.isEmpty());
        QCOMPARE(Fixture::read(fixture.definition.absoluteJournalPath()), damaged);
        QCOMPARE(Fixture::read(fixture.profile), profileBytes);
        QCOMPARE(Fixture::read(QDir(fixture.root).filePath(u"keep.txt"_s)), QByteArray("existing user file"));
        const auto directories = QDir(backups).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QCOMPARE(directories.size(), 1);
        const QDir backup(QDir(backups).filePath(directories.front()));
        QCOMPARE(Fixture::read(backup.filePath(fixture.definition.journalPath)), damaged);
        QVERIFY(QJsonDocument::fromJson(Fixture::read(backup.filePath(u"manifest.json"_s))).isArray());
    }

    void rejectsUnrelatedSqliteSchema_data()
    {
        QTest::addColumn<QByteArray>("schema");
        QTest::newRow("unrelated-table") << QByteArray("CREATE TABLE unrelated (value TEXT);");
        QTest::newRow("incomplete-metadata") << QByteArray("CREATE TABLE metadata (path TEXT);");
    }

    void rejectsUnrelatedSqliteSchema()
    {
        QFETCH(QByteArray, schema);
        Fixture fixture;
        const auto journal = fixture.definition.absoluteJournalPath();
        QVERIFY(QFile::remove(journal));
        {
            SqlDatabase unrelated;
            QVERIFY(unrelated.openOrCreateReadWrite(journal));
            SqlQuery query(unrelated);
            query.prepare(schema);
            QVERIFY(query.exec());
        }
        const auto original = Fixture::read(journal);
        const auto admitted = fixture.inspect();
        QVERIFY(admitted.error.isEmpty());
        const auto backups = fixture.directory.filePath(u"backups"_s);
        QVERIFY(!PrivateCloudFolderAdoption::prepare(fixture.account, admitted, backups, [] {
                     return false;
                 }).isEmpty());
        QCOMPARE(Fixture::read(journal), original);
        const auto directories = QDir(backups).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QCOMPARE(directories.size(), 1);
        const QDir backup(QDir(backups).filePath(directories.front()));
        QCOMPARE(Fixture::read(backup.filePath(fixture.definition.journalPath)), original);
    }

    void rechecksAdmissionBeforeBackup()
    {
        Fixture fixture;
        const auto admitted = fixture.inspect();
        QSettings settings(fixture.profile, QSettings::IniFormat);
        settings.setValue(u"Accounts/0/dav_user"_s, u"changed-account"_s);
        settings.sync();
        const auto backups = fixture.directory.filePath(u"backups"_s);
        QVERIFY(!PrivateCloudFolderAdoption::prepare(fixture.account, admitted, backups, [] {
                     return false;
                 }).isEmpty());
        QVERIFY(!QFileInfo::exists(backups));
    }

    void rejectsBackupInsideSyncFolder()
    {
        Fixture fixture;
        const auto backups = QDir(fixture.root).filePath(u"backups"_s);
        QVERIFY(!PrivateCloudFolderAdoption::prepare(fixture.account, fixture.inspect(), backups, [] {
                     return false;
                 }).isEmpty());
        QVERIFY(!QFileInfo::exists(backups));
    }
};

QTEST_GUILESS_MAIN(TestPrivateCloudFolderAdoption)
#include "testprivatecloudfolderadoption.moc"
