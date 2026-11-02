/*
 * SPDX-FileCopyrightText: 2025 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: CC0-1.0
 *
 * This software is in the public domain, furnished "as is", without technical
 * support, and with no warranty, express or implied, as to its usefulness for
 * any purpose.
 */

#include <QtTest>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QSettings>

#include "accountstate.h"
#include "common/utility.h"
#include "common/vfs.h"
#include "configfile.h"
#include "privateclouddrive.h"
#include "syncengine.h"

#ifdef Q_OS_WIN
namespace OCC
{
OCSYNC_EXPORT extern std::function<Utility::LocalStorageType(const QString &)> localStorageTypeOverride;
}
#endif

#include "folder.h"

#include "account.h"
#include "accountmanager.h"
#include "logger.h"
#include "testhelper.h"
#include "theme.h"

using namespace Qt::StringLiterals;

using namespace OCC;

class TestFolder : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        OCC::Logger::instance()->setLogFlush(true);
        OCC::Logger::instance()->setLogDebug(true);

        QStandardPaths::setTestModeEnabled(true);
    }

#ifdef Q_OS_WIN
    void testAdoptedRootBlocksVfsBeforeRegistration_data()
    {
        QTest::addColumn<bool>("wrongAccount");
        QTest::newRow("wrong-account") << true;
        QTest::newRow("wrong-root") << false;
    }

    void testAdoptedRootBlocksVfsBeforeRegistration()
    {
        if (!PrivateCloudDrive::isEnabled()) {
            QSKIP("The adopted-root provider gate is enabled in the privateCloud Windows build.");
        }
        QFETCH(bool, wrongAccount);
        class RecoverableVfs final : public VfsOff
        {
        public:
            bool isReady() const override
            {
                return ready;
            }
            bool ready = false;
            int starts = 0;

        protected:
            void startImpl(const VfsSetupParams &) override
            {
                ready = true;
                ++starts;
            }
        };
        QTemporaryDir configDirectory;
        QTemporaryDir directory;
        QTemporaryDir otherDirectory;
        QVERIFY(configDirectory.isValid());
        QVERIFY(directory.isValid());
        QVERIFY(otherDirectory.isValid());
        const auto originalConfig = ConfigFile().configPath();
        QVERIFY(ConfigFile::setConfDir(configDirectory.path()));
        const auto restoreConfig = qScopeGuard([originalConfig] {
            ConfigFile::setConfDir(originalConfig);
        });
        QScopedValueRollback storageScope(localStorageTypeOverride);
        localStorageTypeOverride = [](const QString &) {
            return Utility::LocalStorageType::Internal;
        };
        const auto account = Account::create();
        const auto serverUrl = Account::enforcedServerUrl().isEmpty() ? QUrl(QStringLiteral("https://cloud.example")) : Account::enforcedServerUrl();
        QVERIFY(account->setUrl(serverUrl));
        account->setDavUser(QStringLiteral("canonical-user-01"));
        const auto writeBinding = [&](bool valid) {
            QSettings settings(ConfigFile().configFile(), QSettings::IniFormat);
            settings.beginGroup(QStringLiteral("PrivateCloudBackingRoot"));
            settings.setValue(QStringLiteral("ServerUrl"), serverUrl.toString(QUrl::FullyEncoded));
            settings.setValue(QStringLiteral("AccountId"), !valid && wrongAccount ? QStringLiteral("other-user") : account->davUser());
            settings.setValue(QStringLiteral("Path"), !valid && !wrongAccount ? otherDirectory.path() : directory.path());
            settings.sync();
        };
        writeBinding(false);
        const auto state = AccountStatePtr(new FakeAccountState(account));
        FolderDefinition definition;
        definition.localPath = directory.path();
        definition.targetPath = QStringLiteral("/");
        definition.alias = QStringLiteral("adopted-root-vfs");
        definition.journalPath = QStringLiteral(".adopted-root-test.db");
        auto vfs = std::make_unique<RecoverableVfs>();
        const auto backend = vfs.get();
        Folder folder(definition, state.data(), std::move(vfs));
        QCOMPARE(folder.syncResult().status(), SyncResult::SetupError);
        QVERIFY(!folder.syncResult().errorStrings().isEmpty());
        QCOMPARE(backend->starts, 0);
        QVERIFY(backend->params().filesystemPath.isEmpty());
        QVERIFY(!QFileInfo::exists(definition.absoluteJournalPath()));
        QVERIFY(!folder.ensureVfsReady());
        QCOMPARE(backend->starts, 0);
        QVERIFY(!QFileInfo::exists(definition.absoluteJournalPath()));

        writeBinding(true);
        QVERIFY(folder.ensureVfsReady());
        QCOMPARE(backend->starts, 1);
        QVERIFY(!backend->params().filesystemPath.isEmpty());
        QVERIFY(QFileInfo::exists(definition.absoluteJournalPath()));
        writeBinding(false);
        QVERIFY(!folder.ensureVfsReady());
        QCOMPARE(backend->starts, 1);
    }

    void testVfsInitializationRecoversAfterStorageBecomesAvailable()
    {
        class RecoverableVfs final : public VfsOff
        {
        public:
            bool isReady() const override
            {
                return ready;
            }
            bool ready = false;
            int starts = 0;

        protected:
            void startImpl(const VfsSetupParams &) override
            {
                ready = true;
                ++starts;
            }
        };
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto account = Account::create();
        account->setUrl(QUrl(QStringLiteral("https://example.com")));
        const auto state = AccountStatePtr(new FakeAccountState(account));
        FolderDefinition definition;
        definition.localPath = directory.path();
        definition.targetPath = QStringLiteral("/");
        definition.alias = QStringLiteral("retry-vfs");
        QScopedValueRollback scope(localStorageTypeOverride);
        localStorageTypeOverride = [](const QString &) {
            return Utility::LocalStorageType::External;
        };
        auto vfs = std::make_unique<RecoverableVfs>();
        const auto backend = vfs.get();
        Folder folder(definition, state.data(), std::move(vfs));
        QVERIFY(backend->params().filesystemPath.isEmpty());
        QVERIFY(!folder.ensureVfsReady());
        QCOMPARE(backend->starts, 0);
        localStorageTypeOverride = [](const QString &) {
            return Utility::LocalStorageType::Internal;
        };
        QVERIFY(folder.ensureVfsReady());
        QVERIFY(!backend->params().filesystemPath.isEmpty());
        QCOMPARE(backend->starts, 1);
        QVERIFY(folder.ensureVfsReady());
        QCOMPARE(backend->starts, 1);
    }

    void testExternalStorageStopsExistingFolder()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile localFile(dir.filePath(QStringLiteral("keep.txt")));
        QVERIFY(localFile.open(QIODevice::WriteOnly));
        QCOMPARE(localFile.write("keep"), qint64(4));
        localFile.close();
        const auto account = Account::create();
        account->setUrl(QUrl(QStringLiteral("https://example.com")));
        const auto state = AccountStatePtr(new FakeAccountState(account));
        FolderDefinition definition;
        definition.localPath = dir.path();
        definition.targetPath = QStringLiteral("/");
        definition.alias = QStringLiteral("external-storage-test");
        QScopedValueRollback scope(localStorageTypeOverride);
        localStorageTypeOverride = [](const QString &) {
            return Utility::LocalStorageType::Internal;
        };
        Folder folder(definition, state.data(), createVfsFromPlugin(Vfs::Off));
        QSignalSpy started(&folder, &Folder::syncStarted);
        QSignalSpy finished(&folder, &Folder::syncFinished);
        localStorageTypeOverride = [](const QString &) {
            return Utility::LocalStorageType::External;
        };
        folder.startSync();
        QCOMPARE(folder.syncResult().status(), SyncResult::SetupError);
        QVERIFY(!folder.syncResult().errorStrings().isEmpty());
        QVERIFY(!folder.syncEngine().isSyncRunning());
        QCOMPARE(started.count(), 0);
        QTRY_COMPARE(finished.count(), 1);
        QVERIFY(localFile.open(QIODevice::ReadOnly));
        QCOMPARE(localFile.readAll(), QByteArray("keep"));

        Folder blockedAtStartup(definition, state.data(), createVfsFromPlugin(Vfs::Off));
        QCOMPARE(blockedAtStartup.syncResult().status(), SyncResult::SetupError);
    }
#endif

    void test_sidebarDisplayName()
    {
        const auto createAccount = [](const QString &url, const QString &displayName) -> AccountState * {
            auto account = Account::create();
            account->setUrl(QUrl(url));
            account->setDavDisplayName(displayName);
            return AccountManager::instance()->addAccount(account);
        };

        const auto createFolder = [](AccountState *const accountState, const QString &targetPath, const QString &alias) -> Folder {
            FolderDefinition definition;
            definition.localPath = QTemporaryDir().path(); // for these tests the local folder path doesn't matter
            definition.targetPath = targetPath; // also known as the remote path
            definition.alias = alias;

            return Folder(definition, accountState, createVfsFromPlugin(Vfs::Off));
        };

        const auto appName = Theme::instance()->appNameGUI();

        // at this stage only one account is created
        const auto account1 = createAccount("http://admin:admin@nextcloud.local"_L1, "Admin"_L1);
        QVERIFY(account1);

        // when syncing the root folder it should return the (branded) app name
        auto folder1 = createFolder(account1, "/"_L1, "1"_L1);
        QCOMPARE(folder1.sidebarDisplayName(), appName);

        // when syncing subfolders it should return the remote folder path without the leading slash
        auto folder2 = createFolder(account1, "/Documents"_L1, "2"_L1);
        QCOMPARE(folder2.sidebarDisplayName(), "Documents"_L1);
        auto folder3 = createFolder(account1, "/Photos/to_sort"_L1, "3"_L1);
        QCOMPARE(folder3.sidebarDisplayName(), "Photos/to_sort"_L1);

        // so far we only had one account, let's add another one!
        const auto account2 = createAccount("http://testuser:testuser@stable31.local"_L1, "Test user"_L1);
        const auto folder4 = createFolder(account2, "/"_L1, "1"_L1);

        const auto expectedSuffixAccount1 = " - nextcloud.local - Admin"_L1;
        const auto expectedSuffixAccount2 = " - stable31.local - Test user"_L1;

        // now the account-specific suffixes should be added
        QCOMPARE(folder1.sidebarDisplayName(), appName + expectedSuffixAccount1);
        QCOMPARE(folder2.sidebarDisplayName(), "Documents"_L1 + expectedSuffixAccount1);
        QCOMPARE(folder3.sidebarDisplayName(), "Photos/to_sort"_L1 + expectedSuffixAccount1);
        QCOMPARE(folder4.sidebarDisplayName(), appName + expectedSuffixAccount2);
    }
};

QTEST_GUILESS_MAIN(TestFolder)
#include "testfolder.moc"
