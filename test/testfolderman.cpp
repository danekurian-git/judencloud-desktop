/*
 * SPDX-FileCopyrightText: 2019 Nextcloud GmbH and Nextcloud contributors
 * SPDX-FileCopyrightText: 2016 ownCloud GmbH
 * SPDX-License-Identifier: CC0-1.0
 *
 * This software is in the public domain, furnished "as is", without technical
 * support, and with no warranty, express or implied, as to its usefulness for
 * any purpose.
 */

#include "accounttestutils.h"
#include <QDesktopServices>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>
#include <qglobal.h>

#include "QtTest/qtestcase.h"
#include "account.h"
#include "accountstate.h"
#include "common/utility.h"
#include "configfile.h"
#include "folderman.h"
#include "libsync/privateclouddrive.h"
#include "socketapi/socketapi.h"
#include "syncenginetestutils.h"
#include "testhelper.h"
#include <accountmanager.h>

#ifdef Q_OS_WIN
namespace OCC
{
OCSYNC_EXPORT extern std::function<Utility::LocalStorageType(const QString &)> localStorageTypeOverride;
}
#endif

#ifdef Q_OS_MACOS
#include "common/utility_mac_sandbox.h"
#endif

using namespace Qt::StringLiterals;
using namespace OCC;

static QByteArray fake400Response = R"(
{"ocs":{"meta":{"status":"failure","statuscode":400,"message":"Parameter is incorrect.\n"},"data":[]}}
)";

bool itemDidCompleteSuccessfully(const ItemCompletedSpy &spy, const QString &path)
{
    if (auto item = spy.findItem(path)) {
        return item->_status == SyncFileItem::Success;
    }
    return false;
}

class TestFolderMan : public QObject
{
    Q_OBJECT

    QPointer<FolderMan> _fm;
    QList<QUrl> _openedInternalLinks;

    Q_INVOKABLE void captureInternalLink(const QUrl &url)
    {
        _openedInternalLinks.append(url);
    }

Q_SIGNALS:
    void incomingShareDeleted();

private Q_SLOTS:
    void initTestCase()
    {
        OCC::Logger::instance()->setLogFlush(true);
        OCC::Logger::instance()->setLogDebug(true);

        QStandardPaths::setTestModeEnabled(true);
    }

    void testPrivateCloudSetupPendingSettingsRoundTrip()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings settings(dir.filePath(QStringLiteral("folder.ini")), QSettings::IniFormat);
        FolderDefinition definition;
        definition.localPath = FolderDefinition::prepareLocalPath(dir.path());
        definition.targetPath = QStringLiteral("/");
        definition.alias = QStringLiteral("pending-drive");
        definition.paused = true;
        for (const auto pending : {true, false}) {
            definition.privateCloudSetupPending = pending;
            FolderDefinition::save(settings, definition);
            settings.sync();
            QSettings reloadedSettings(settings.fileName(), QSettings::IniFormat);
            FolderDefinition reloaded;
            QVERIFY(FolderDefinition::load(reloadedSettings, definition.alias, &reloaded));
            QCOMPARE(reloaded.privateCloudSetupPending, pending);
            QCOMPARE(reloaded.paused, definition.paused);
            QCOMPARE(reloaded.localPath, definition.localPath);
        }
        settings.remove(QStringLiteral("privateCloudSetupPending"));
        FolderDefinition legacy;
        legacy.privateCloudSetupPending = true;
        QVERIFY(FolderDefinition::load(settings, definition.alias, &legacy));
        QVERIFY(!legacy.privateCloudSetupPending);
    }

#ifdef Q_OS_WIN
    void testNewFolderUsesCreateDirectoryPermissionWithoutCreatingIt()
    {
        const auto defaultPath = PrivateCloudDrive::defaultSetupPath();
        if (QFileInfo::exists(defaultPath)) {
            QSKIP("The read-only default-folder regression requires the default folder to be absent.");
        }
        const auto canOpenWithAccess = [](const DWORD access) {
            const Utility::UniqueHandle handle(CreateFileW(L"C:\\",
                                                           access,
                                                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                           nullptr,
                                                           OPEN_EXISTING,
                                                           FILE_FLAG_BACKUP_SEMANTICS,
                                                           nullptr));
            return handle.get() != INVALID_HANDLE_VALUE;
        };
        if (canOpenWithAccess(FILE_ADD_FILE) || !canOpenWithAccess(FILE_ADD_SUBDIRECTORY)) {
            QSKIP("This token does not have the standard Windows root distinction: subfolder creation allowed, file creation denied.");
        }
        QTemporaryDir config;
        QVERIFY(config.isValid());
        const auto previousConfig = ConfigFile().configPath();
        FolderMan::resetInstance();
        const auto restore = qScopeGuard([previousConfig] {
            FolderMan::resetInstance();
            ConfigFile::setConfDir(previousConfig);
        });
        QVERIFY(ConfigFile::setConfDir(config.path()));
        _fm = FolderMan::instance();
        const auto existingRoot = _fm->checkPathValidityForNewFolder(QStringLiteral("C:/"));
        QCOMPARE(existingRoot.first, FolderMan::PathValidityResult::ErrorRecursiveValidity);
        QVERIFY(!existingRoot.second.isEmpty());
        const auto newFolder = _fm->checkPathValidityForNewFolder(defaultPath);
        QVERIFY2(newFolder.second.isEmpty(), qPrintable(newFolder.second));
        const auto nestedFolder = _fm->checkPathValidityForNewFolder(defaultPath + QStringLiteral("/nested/child"));
        QVERIFY2(nestedFolder.second.isEmpty(), qPrintable(nestedFolder.second));
        QVERIFY(!QFileInfo::exists(defaultPath));
    }

    void testExternalStorageRejected()
    {
        FolderMan::resetInstance();
        _fm = FolderMan::instance();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QScopedValueRollback scope(localStorageTypeOverride);
        for (const auto type : {Utility::LocalStorageType::External, Utility::LocalStorageType::Unknown}) {
            localStorageTypeOverride = [type](const QString &) {
                return type;
            };
            const auto result = _fm->checkPathValidityForNewFolder(dir.path() + QStringLiteral("/new-folder"));
            QCOMPARE(result.first, FolderMan::PathValidityResult::ErrorRecursiveValidity);
            QVERIFY(!result.second.isEmpty());
            QVERIFY(!QFileInfo::exists(dir.path() + QStringLiteral("/new-folder")));
        }
        for (const auto type : {Utility::LocalStorageType::Internal, Utility::LocalStorageType::Network}) {
            localStorageTypeOverride = [type](const QString &) {
                return type;
            };
            QVERIFY(_fm->checkPathValidityForNewFolder(dir.path() + QStringLiteral("/new-folder")).second.isEmpty());
        }
    }

    void testExternalStorageStartupPreservesLegacyJournal()
    {
        FolderMan::resetInstance();
        _fm = FolderMan::instance();
        QTemporaryDir localDir;
        QTemporaryDir settingsDir;
        QVERIFY(localDir.isValid());
        QVERIFY(settingsDir.isValid());
        const auto sentinelNames = QStringList{QStringLiteral(".nextcloudpermissions.log"), QStringLiteral(".nextcloudsync.log")};
        for (const auto &name : sentinelNames) {
            QFile sentinel(localDir.filePath(name));
            QVERIFY(sentinel.open(QIODevice::WriteOnly));
            QCOMPARE(sentinel.write("do-not-change"), qint64(13));
        }
        const auto account = Account::create();
        account->setUrl(QUrl(QStringLiteral("https://example.com")));
        account->setCredentials(new FakeCredentials{new FakeQNAM({})});
        const auto state = AccountStatePtr(new FakeAccountState(account));
        FolderDefinition definition;
        definition.localPath = localDir.path();
        definition.targetPath = QStringLiteral("/");
        definition.alias = QStringLiteral("blocked-legacy-folder");
        definition.journalPath = settingsDir.filePath(QStringLiteral("legacy.db"));
        QFile journal(definition.absoluteJournalPath());
        QVERIFY(journal.open(QIODevice::WriteOnly));
        QCOMPARE(journal.write("preserved"), qint64(9));
        journal.close();
        QSettings settings(settingsDir.filePath(QStringLiteral("folders.ini")), QSettings::IniFormat);
        settings.beginGroup(definition.alias);
        FolderDefinition::save(settings, definition);
        settings.endGroup();
        QScopedValueRollback scope(localStorageTypeOverride);
        localStorageTypeOverride = [](const QString &) {
            return Utility::LocalStorageType::External;
        };
        _fm->setupFoldersHelper(settings, state, {}, true, false);
        const auto folder = _fm->folder(definition.alias);
        QVERIFY(folder);
        QCOMPARE(folder->syncResult().status(), SyncResult::SetupError);
        QCOMPARE(folder->journalDb()->databaseFilePath(), definition.journalPath);
        QVERIFY(journal.open(QIODevice::ReadOnly));
        QCOMPARE(journal.readAll(), QByteArray("preserved"));
        QCoreApplication::processEvents();
        QVERIFY(!folder->findChild<FolderWatcher *>());
        for (const auto &name : sentinelNames) {
            QFile sentinel(localDir.filePath(name));
            QVERIFY(sentinel.open(QIODevice::ReadOnly));
            QCOMPARE(sentinel.readAll(), QByteArray("do-not-change"));
        }
        QCOMPARE(QDir(localDir.path()).entryList(QDir::Files | QDir::Hidden), sentinelNames);
        FolderMan::resetInstance();
    }

    void testExternalStorageLegacyMigrationPreservesFiles()
    {
        FolderMan::resetInstance();
        _fm = FolderMan::instance();
        QTemporaryDir localDir;
        QTemporaryDir settingsDir;
        QVERIFY(localDir.isValid());
        QVERIFY(settingsDir.isValid());
        const auto previousConfigPath = ConfigFile().configPath();
        const auto restoreConfig = qScopeGuard([previousConfigPath] {
            ConfigFile::setConfDir(previousConfigPath);
        });
        QVERIFY(ConfigFile::setConfDir(settingsDir.path()));
        const auto account = Account::create();
        account->setUrl(QUrl(QStringLiteral("https://example.com")));
        account->setCredentials(new FakeCredentials{new FakeQNAM({})});
        const auto state = AccountStatePtr(new FakeAccountState(account));
        FolderDefinition definition;
        definition.localPath = localDir.path();
        definition.targetPath = QStringLiteral("/");
        definition.alias = QStringLiteral("blocked-old-config");
        definition.journalPath = QStringLiteral("legacy.db");
        QFile journal(definition.absoluteJournalPath());
        QVERIFY(journal.open(QIODevice::WriteOnly));
        QCOMPARE(journal.write("preserved"), qint64(9));
        journal.close();
        const auto legacyFilePath = settingsDir.filePath(QStringLiteral("old.cfg"));
        const auto legacyGroup = QStringLiteral("Accounts/%1/Folders/%2").arg(account->id(), definition.alias);
        const auto legacyBlacklist = QStringList{QStringLiteral("keep-excluded/")};
        {
            QSettings settings(legacyFilePath, QSettings::IniFormat);
            settings.beginGroup(legacyGroup);
            FolderDefinition::save(settings, definition);
            settings.setValue(QStringLiteral("blackList"), legacyBlacklist);
            settings.sync();
        }
        QScopedValueRollback scope(localStorageTypeOverride);
        localStorageTypeOverride = [](const QString &) {
            return Utility::LocalStorageType::External;
        };
        _fm->setupLegacyFolder(legacyFilePath, state.data());
        const auto folder = _fm->folder(definition.alias);
        QVERIFY(folder);
        QCOMPARE(folder->syncResult().status(), SyncResult::SetupError);
        QVERIFY(journal.open(QIODevice::ReadOnly));
        QCOMPARE(journal.readAll(), QByteArray("preserved"));
        QCOMPARE(QDir(localDir.path()).entryList(QDir::Files | QDir::Hidden), QStringList{QStringLiteral("legacy.db")});
        QSettings legacySettings(legacyFilePath, QSettings::IniFormat);
        QCOMPARE(legacySettings.value(legacyGroup + QStringLiteral("/blackList")).toStringList(), legacyBlacklist);
        const auto savedSettings = state->settings();
        QCOMPARE(savedSettings->value(QStringLiteral("Folders/%1/localPath").arg(definition.alias)).toString(), definition.localPath);
        FolderMan::resetInstance();
    }

    void testExternalStorageVfsTogglePreservesPlaceholder()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto account = Account::create();
        account->setUrl(QUrl(QStringLiteral("https://example.com")));
        account->setCredentials(new FakeCredentials{new FakeQNAM({})});
        const auto state = AccountStatePtr(new FakeAccountState(account));
        FolderDefinition definition;
        definition.localPath = dir.path();
        definition.targetPath = QStringLiteral("/");
        definition.alias = QStringLiteral("blocked-vfs-folder");
        definition.journalPath = QStringLiteral(".sync_test.db");
        definition.virtualFilesMode = Vfs::WithSuffix;
        auto vfs = createVfsFromPlugin(Vfs::WithSuffix);
        QVERIFY(vfs);
        const auto placeholderName = QStringLiteral("keep.txt") + vfs->fileSuffix();
        QFile placeholder(dir.filePath(placeholderName));
        QVERIFY(placeholder.open(QIODevice::WriteOnly));
        QCOMPARE(placeholder.write(" "), qint64(1));
        placeholder.close();
        QScopedValueRollback scope(localStorageTypeOverride);
        localStorageTypeOverride = [](const QString &) {
            return Utility::LocalStorageType::External;
        };
        Folder folder(definition, state.data(), std::move(vfs));
        auto journal = folder.journalDb();
        QVERIFY(journal->open());
        SyncJournalFileRecord record;
        record._path = placeholderName.toUtf8();
        record._type = ItemTypeVirtualFile;
        record._fileId = QByteArray("placeholder-id");
        record._etag = QByteArray("unchanged-etag");
        record._modtime = 1700000000;
        record._fileSize = 100;
        QVERIFY(journal->setFileRecord(record));

        folder.setVirtualFilesEnabled(false);
        folder.switchToVirtualFiles();
        folder.setRootPinState(PinState::OnlineOnly);
        QCOMPARE(folder.vfs().mode(), Vfs::WithSuffix);
        QVERIFY(placeholder.open(QIODevice::ReadOnly));
        QCOMPARE(placeholder.readAll(), QByteArray(" "));
        SyncJournalFileRecord preserved;
        QVERIFY(journal->getFileRecord(record._path, &preserved));
        QVERIFY(preserved.isValid());
        QCOMPARE(preserved._type, ItemTypeVirtualFile);
        QCOMPARE(preserved._fileId, record._fileId);
        QCOMPARE(preserved._etag, record._etag);
    }

    void testExternalStorageAddPreservesJournal()
    {
        FolderMan::resetInstance();
        _fm = FolderMan::instance();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto account = Account::create();
        account->setUrl(QUrl(QStringLiteral("https://example.com")));
        account->setCredentials(new FakeCredentials{new FakeQNAM({})});
        const auto state = AccountStatePtr(new FakeAccountState(account));
        FolderDefinition definition;
        definition.localPath = dir.path();
        definition.targetPath = QStringLiteral("/");
        definition.alias = QStringLiteral("external-storage-test");
        definition.journalPath = definition.defaultJournalPath(account);
        QFile journal(definition.absoluteJournalPath());
        QVERIFY(journal.open(QIODevice::WriteOnly));
        QCOMPARE(journal.write("preserve-existing-journal"), qint64(25));
        journal.close();
        QScopedValueRollback scope(localStorageTypeOverride);
        localStorageTypeOverride = [](const QString &) {
            return Utility::LocalStorageType::External;
        };
        QVERIFY(!_fm->addFolder(state.data(), definition));
        QVERIFY(journal.open(QIODevice::ReadOnly));
        QCOMPARE(journal.readAll(), QByteArray("preserve-existing-journal"));
    }
#endif
    // Keeps tests off the device policy of the machine running them.
    void init()
    {
        ConfigFile::setDeviceSourcesFactory([] {
            return std::vector<std::unique_ptr<SettingSource>>{};
        });
    }

    void cleanup()
    {
        ConfigFile::setDeviceSourcesFactory({});
    }

    void internalLinkSocketAction()
    {
        QTemporaryDir configDir;
        QVERIFY(configDir.isValid());
        ConfigFile::setConfDir(configDir.path());
        FolderMan::resetInstance();
        _fm = FolderMan::instance();
        FakeFolder fakeFolder{FileInfo{}};
        const auto cleanupFolders = qScopeGuard([this] {
            _fm->unloadAndDeleteAllFolders();
        });
        const auto account = fakeFolder.account();
        AccountTestAccess::setUrl(*account, QUrl{u"https://example.com/nextcloud"_s});
        const auto folder = _fm->addFolder(&fakeFolder.accountState(), folderDefinition(fakeFolder.localPath()));
        QVERIFY(folder);
        folder->setSyncPaused(true);

        SyncJournalFileRecord record;
        record._path = "file.txt"_ba;
        record._fileId = "42oc123xyz987e"_ba;
        record._inode = 1;
        record._modtime = QDateTime::currentSecsSinceEpoch();
        record._type = ItemTypeFile;
        record._etag = "etag"_ba;
        QVERIFY(folder->journalDb()->setFileRecord(record));

        _openedInternalLinks.clear();
        QDesktopServices::setUrlHandler(u"https"_s, this, "captureInternalLink");
        const auto cleanupHandler = qScopeGuard([] {
            QDesktopServices::unsetUrlHandler(u"https"_s);
        });
        const auto openLink = [this](const QString &path) {
            auto *listener = static_cast<OCC::SocketListener *>(nullptr);
            return QMetaObject::invokeMethod(_fm->socketApi(),
                                             "command_OPEN_PRIVATE_LINK",
                                             Qt::DirectConnection,
                                             QGenericArgument("QString", &path),
                                             QGenericArgument("OCC::SocketListener*", &listener));
        };
        const auto localFile = fakeFolder.localPath() + u"file.txt"_s;
        account->setCapabilities({});
        QVERIFY(openLink(localFile));
        QCOMPARE(_openedInternalLinks, QList<QUrl>{QUrl{u"https://example.com/nextcloud/index.php/f/42"_s}});

        account->setCapabilities({{u"globalscale"_s, QVariantMap{{u"token"_s, u"abc123"_s}}}});
        QVERIFY(openLink(localFile));
        QCOMPARE(_openedInternalLinks.size(), 2);
        QCOMPARE(_openedInternalLinks.last(), QUrl{u"https://example.com/nextcloud/index.php/gf/abc123/42"_s});

        QVERIFY(openLink(configDir.path() + u"/outside.txt"_s));
        QVERIFY(openLink(fakeFolder.localPath() + u"missing.txt"_s));
        record._fileId.clear();
        QVERIFY(folder->journalDb()->setFileRecord(record));
        QVERIFY(openLink(localFile));
        QCOMPARE(_openedInternalLinks.size(), 2);
    }

    void testDeleteEncryptedFiles()
    {
        FolderMan::resetInstance();
        _fm = FolderMan::instance();

        FakeFolder fakeFolder{FileInfo::A12_B12_C12_S12()};
        QCOMPARE(fakeFolder.currentLocalState().children.count(), 4);

        ItemCompletedSpy completeSpy(fakeFolder);
        fakeFolder.localModifier().mkdir("encrypted");
        fakeFolder.localModifier().setE2EE("encrypted", true);
        fakeFolder.remoteModifier().mkdir("encrypted");
        fakeFolder.remoteModifier().setE2EE("encrypted", true);

        const auto fakeFileInfo = fakeFolder.remoteModifier().find("encrypted");
        QVERIFY(fakeFileInfo);
        QVERIFY(fakeFileInfo->isEncrypted);
        QCOMPARE(fakeFolder.currentLocalState().children.count(), 5);

        const auto fakeFileId = fakeFileInfo->fileId;
        const auto fakeQnam = new FakeQNAM({});
        // Let's avoid the null filename assert in the default FakeQNAM request creation
        const auto fakeQnamOverride = [this, fakeFileId](const QNetworkAccessManager::Operation op, const QNetworkRequest &req, QIODevice *device) {
            Q_UNUSED(device)
            QNetworkReply *reply = nullptr;

            const auto reqUrl = req.url();
            const auto reqRawPath = reqUrl.path();
            const auto reqPath = reqRawPath.startsWith("/owncloud/") ? reqRawPath.mid(10) : reqRawPath;

            if (reqPath.startsWith(QStringLiteral("ocs/v2.php/apps/end_to_end_encryption/api/v1/meta-data/"))) {
                const auto splitUrlPath = reqPath.split('/');
                const auto fileId = splitUrlPath.last();

                const QUrlQuery urlQuery(req.url());
                const auto formatParam = urlQuery.queryItemValue(QStringLiteral("format"));

                if (fileId == fakeFileId && formatParam == QStringLiteral("json")) {
                    reply = new FakePayloadReply(op, req, QJsonDocument().toJson(), this);
                } else {
                    reply = new FakeErrorReply(op, req, this, 400, fake400Response);
                }
            }

            return reply;
        };
        fakeFolder.setServerOverride(fakeQnamOverride);
        fakeQnam->setOverride(fakeQnamOverride);

        const auto account = Account::create();
        const auto capabilities = QVariantMap{
            {QStringLiteral("end-to-end-encryption"),
             QVariantMap{
                 {QStringLiteral("enabled"), true},
                 {QStringLiteral("api-version"), QString::number(2.0)},
             }},
        };
        account->setCapabilities(capabilities);
        account->setCredentials(new FakeCredentials{fakeQnam});
        account->setUrl(QUrl(("owncloud://somehost/owncloud")));
        const auto accountState = new FakeAccountState(account);
        QVERIFY(accountState->isConnected());

        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        auto folderDef = folderDefinition(fakeFolder.localPath());
        folderDef.targetPath = "";
        const auto folder = FolderMan::instance()->addFolder(accountState, folderDef);
        QVERIFY(folder);

        qRegisterMetaType<OCC::SyncResult>("SyncResult");
        QSignalSpy folderSyncDone(folder, &Folder::syncFinished);

        QDir dir(folder->path() + QStringLiteral("encrypted"));
        QVERIFY(dir.exists());
        QVERIFY(fakeFolder.remoteModifier().find("encrypted"));
        QVERIFY(fakeFolder.currentLocalState().find("encrypted"));
        QCOMPARE(fakeFolder.currentLocalState().children.count(), 5);

        // Rather than go through the pain of trying to replicate the E2EE response from
        // the server, let's just manually set the encryption bool in the folder journal
        SyncJournalFileRecord rec;
        QVERIFY(folder->journalDb()->getFileRecord(QStringLiteral("encrypted"), &rec));
        rec._modtime = QDateTime::currentSecsSinceEpoch();
        rec._e2eEncryptionStatus = SyncJournalFileRecord::EncryptionStatus::EncryptedMigratedV2_0;
        rec._path = QStringLiteral("encrypted").toUtf8();
        rec._type = CSyncEnums::ItemTypeDirectory;
        QVERIFY(folder->journalDb()->setFileRecord(rec));

        SyncJournalFileRecord updatedRec;
        QVERIFY(folder->journalDb()->getFileRecord(QStringLiteral("encrypted"), &updatedRec));
        QVERIFY(updatedRec.isE2eEncrypted());
        QVERIFY(updatedRec.isDirectory());

        FolderMan::instance()->removeE2eFiles(account);

        if (folderSyncDone.isEmpty()) {
            QVERIFY(folderSyncDone.wait());
        }

        QVERIFY(fakeFolder.currentRemoteState().find("encrypted"));
        QVERIFY(!fakeFolder.currentLocalState().find("encrypted"));
        QCOMPARE(fakeFolder.currentLocalState().children.count(), 4);
    }

    void testLeaveShare()
    {
        FolderMan::resetInstance();
        _fm = FolderMan::instance();

        QTemporaryDir dir;
        ConfigFile::setConfDir(dir.path()); // we don't want to pollute the user's config file

        constexpr auto firstSharePath = "A/sharedwithme_A.txt";
        constexpr auto secondSharePath = "A/B/sharedwithme_B.data";

        QScopedPointer<FakeQNAM> fakeQnam(new FakeQNAM({}));
        OCC::AccountPtr account = OCC::Account::create();
        account->setCredentials(new FakeCredentials{fakeQnam.data()});
        account->setUrl(QUrl(("http://example.de")));
        OCC::AccountManager::instance()->addAccount(account);

        FakeFolder fakeFolder{FileInfo{}};
        fakeFolder.remoteModifier().mkdir("A");

        fakeFolder.remoteModifier().insert(firstSharePath, 100);
        const auto firstShare = fakeFolder.remoteModifier().find(firstSharePath);
        QVERIFY(firstShare);
        firstShare->permissions.setPermission(OCC::RemotePermissions::CanRead);
        firstShare->permissions.setPermission(OCC::RemotePermissions::IsShared);

        fakeFolder.remoteModifier().mkdir("A/B");

        fakeFolder.remoteModifier().insert(secondSharePath, 100);
        const auto secondShare = fakeFolder.remoteModifier().find(secondSharePath);
        QVERIFY(secondShare);
        secondShare->permissions.setPermission(OCC::RemotePermissions::CanRead);
        secondShare->permissions.setPermission(OCC::RemotePermissions::IsShared);

        FolderMan *folderman = FolderMan::instance();
        QCOMPARE(folderman, _fm.get());
        OCC::AccountState *accountState = OCC::AccountManager::instance()->accounts().first().data();
        const auto folder = folderman->addFolder(accountState, folderDefinition(fakeFolder.localPath()));
        QVERIFY(folder);

        auto realFolder = FolderMan::instance()->folderForPath(fakeFolder.localPath());
        QVERIFY(realFolder);

        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        fakeQnam->setOverride([this, accountState, &fakeFolder](QNetworkAccessManager::Operation op, const QNetworkRequest &req, QIODevice *device) {
            Q_UNUSED(device);
            QNetworkReply *reply = nullptr;

            if (op != QNetworkAccessManager::DeleteOperation) {
                reply = new FakeErrorReply(op, req, this, 405);
                return reply;
            }

            if (req.url().path().isEmpty()) {
                reply = new FakeErrorReply(op, req, this, 404);
                return reply;
            }

            const auto filePathRelative = req.url().path().remove(accountState->account()->davPath());

            const auto foundFileInRemoteFolder = fakeFolder.remoteModifier().find(filePathRelative);

            if (filePathRelative.isEmpty() || !foundFileInRemoteFolder) {
                reply = new FakeErrorReply(op, req, this, 404);
                return reply;
            }

            fakeFolder.remoteModifier().remove(filePathRelative);
            reply = new FakePayloadReply(op, req, {}, nullptr);

            Q_EMIT incomingShareDeleted();

            return reply;
        });

        QSignalSpy incomingShareDeletedSignal(this, &TestFolderMan::incomingShareDeleted);

        // verify first share gets deleted
        folderman->leaveShare(fakeFolder.localPath() + firstSharePath);
        QCOMPARE(incomingShareDeletedSignal.count(), 1);
        QVERIFY(!fakeFolder.remoteModifier().find(firstSharePath));
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        // verify no share gets deleted
        folderman->leaveShare(fakeFolder.localPath() + "A/B/notsharedwithme_B.data");
        QCOMPARE(incomingShareDeletedSignal.count(), 1);
        QVERIFY(fakeFolder.remoteModifier().find("A/B/sharedwithme_B.data"));
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        // verify second share gets deleted
        folderman->leaveShare(fakeFolder.localPath() + secondSharePath);
        QCOMPARE(incomingShareDeletedSignal.count(), 2);
        QVERIFY(!fakeFolder.remoteModifier().find(secondSharePath));
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        OCC::AccountManager::instance()->deleteAccount(accountState);
    }

    void testCheckPathValidityForNewFolder()
    {
        FolderMan::resetInstance();
        _fm = FolderMan::instance();

#ifdef Q_OS_WIN
        Utility::NtfsPermissionLookupRAII ntfs_perm;
#endif
        QTemporaryDir dir;
        ConfigFile::setConfDir(dir.path()); // we don't want to pollute the user's config file
        QVERIFY(dir.isValid());
        QDir dir2(dir.path());
        QVERIFY(dir2.mkpath("sub/ownCloud1/folder/f"));
        QVERIFY(dir2.mkpath("ownCloud2"));
        QVERIFY(dir2.mkpath("sub/free"));
        QVERIFY(dir2.mkpath("free2/sub"));
        {
            QFile f(dir.path() + "/sub/file.txt");
            const auto openResult = f.open(QFile::WriteOnly);
            if (!openResult) {
                qWarning() << "Failed to create file:" << dir.path() + "/sub/file.txt";
                return;
            }
            f.write("hello");
        }
        QString dirPath = dir2.canonicalPath();

        AccountPtr account = Account::create();
        QUrl url("http://example.de");
        auto *cred = new HttpCredentialsTest("testuser", "secret");
        account->setCredentials(cred);
        account->setUrl(url);

        AccountStatePtr newAccountState(new AccountState(account));
        FolderMan *folderman = FolderMan::instance();
        QCOMPARE(folderman, _fm.get());
        QVERIFY(folderman->addFolder(newAccountState.data(), folderDefinition(dirPath + "/sub/ownCloud1")));
        QVERIFY(folderman->addFolder(newAccountState.data(), folderDefinition(dirPath + "/ownCloud2")));

        const auto folderList = folderman->map();

        for (const auto &folder : folderList) {
            QVERIFY(!folder->isSyncRunning());
        }

        // those should be allowed
        // QString FolderMan::checkPathValidityForNewFolder(const QString& path, const QUrl &serverUrl, bool forNewDirectory).second

        QCOMPARE(folderman->checkPathValidityForNewFolder(dirPath + "/sub/free").second, QString());
        QCOMPARE(folderman->checkPathValidityForNewFolder(dirPath + "/free2/").second, QString());
        // Not an existing directory -> Ok
        QCOMPARE(folderman->checkPathValidityForNewFolder(dirPath + "/sub/bliblablu").second, QString());
        QCOMPARE(folderman->checkPathValidityForNewFolder(dirPath + "/sub/free/bliblablu").second, QString());
        // QCOMPARE(folderman->checkPathValidityForNewFolder(dirPath + "/sub/bliblablu/some/more").second, QString());

        // A file -> Error
        QVERIFY(!folderman->checkPathValidityForNewFolder(dirPath + "/sub/file.txt").second.isNull());

        // There are folders configured in those folders, url needs to be taken into account: -> ERROR
        QUrl url2(url);
        const QString user = account->credentials()->user();
        url2.setUserName(user);

        // The following both fail because they refer to the same account (user and url)
        QVERIFY(!folderman->checkPathValidityForNewFolder(dirPath + "/sub/ownCloud1", url2).second.isNull());
        QVERIFY(!folderman->checkPathValidityForNewFolder(dirPath + "/ownCloud2/", url2).second.isNull());

        // The following both fail because they are already sync folders even if for another account
        QUrl url3("http://anotherexample.org");
        url3.setUserName("dummy");
        QCOMPARE(folderman->checkPathValidityForNewFolder(dirPath + "/sub/ownCloud1", url3).second,
                 QString("Please choose a different location. %1 is already being used as a sync folder.")
                     .arg(QDir::toNativeSeparators(dirPath + "/sub/ownCloud1")));
        QCOMPARE(
            folderman->checkPathValidityForNewFolder(dirPath + "/ownCloud2", url3).second,
            QString("Please choose a different location. %1 is already being used as a sync folder.").arg(QDir::toNativeSeparators(dirPath + "/ownCloud2")));

        QVERIFY(!folderman->checkPathValidityForNewFolder(dirPath).second.isNull());
        QVERIFY(!folderman->checkPathValidityForNewFolder(dirPath + "/sub/ownCloud1/folder").second.isNull());
        QVERIFY(!folderman->checkPathValidityForNewFolder(dirPath + "/sub/ownCloud1/f").second.isNull());

#ifndef Q_OS_WIN // no links on windows, no permissions
        // make a bunch of links
        QVERIFY(QFile::link(dirPath + "/sub/free", dirPath + "/link1"));
        QVERIFY(QFile::link(dirPath + "/sub", dirPath + "/link2"));
        QVERIFY(QFile::link(dirPath + "/sub/ownCloud1", dirPath + "/link3"));
        QVERIFY(QFile::link(dirPath + "/sub/ownCloud1/folder", dirPath + "/link4"));

        // Ok
        QVERIFY(folderman->checkPathValidityForNewFolder(dirPath + "/link1").second.isNull());
        QVERIFY(folderman->checkPathValidityForNewFolder(dirPath + "/link2/free").second.isNull());

        // Not Ok
        QVERIFY(!folderman->checkPathValidityForNewFolder(dirPath + "/link2").second.isNull());

        // link 3 points to an existing sync folder. To make it fail, the account must be the same
        QVERIFY(!folderman->checkPathValidityForNewFolder(dirPath + "/link3", url2).second.isNull());
        // while with a different account, this is fine
        QCOMPARE(folderman->checkPathValidityForNewFolder(dirPath + "/link3", url3).second,
                 QString("Please choose a different location. %1 is already being used as a sync folder.").arg(dirPath + "/link3"));

        QVERIFY(!folderman->checkPathValidityForNewFolder(dirPath + "/link4").second.isNull());
        QVERIFY(!folderman->checkPathValidityForNewFolder(dirPath + "/link3/folder").second.isNull());

        // test some non existing sub path (error)
        QVERIFY(!folderman->checkPathValidityForNewFolder(dirPath + "/sub/ownCloud1/some/sub/path").second.isNull());
        QVERIFY(!folderman->checkPathValidityForNewFolder(dirPath + "/ownCloud2/blublu").second.isNull());
        QVERIFY(!folderman->checkPathValidityForNewFolder(dirPath + "/sub/ownCloud1/folder/g/h").second.isNull());
        QVERIFY(!folderman->checkPathValidityForNewFolder(dirPath + "/link3/folder/neu_folder").second.isNull());

        // Subfolder of links
        QVERIFY(folderman->checkPathValidityForNewFolder(dirPath + "/link1/subfolder").second.isNull());
        QVERIFY(folderman->checkPathValidityForNewFolder(dirPath + "/link2/free/subfolder").second.isNull());

        // Should not have the rights
        QVERIFY(!folderman->checkPathValidityForNewFolder("/").second.isNull());
        QVERIFY(!folderman->checkPathValidityForNewFolder("/usr/bin/somefolder").second.isNull());
#endif

#ifdef Q_OS_WIN // drive-letter tests
        if (!QFileInfo("v:/").exists()) {
            QVERIFY(!folderman->checkPathValidityForNewFolder("v:").second.isNull());
            QVERIFY(!folderman->checkPathValidityForNewFolder("v:/").second.isNull());
            QVERIFY(!folderman->checkPathValidityForNewFolder("v:/foo").second.isNull());
        }
        if (QFileInfo("c:/").isWritable()) {
            QVERIFY(folderman->checkPathValidityForNewFolder("c:").second.isNull());
            QVERIFY(folderman->checkPathValidityForNewFolder("c:/").second.isNull());
            QVERIFY(folderman->checkPathValidityForNewFolder("c:/foo").second.isNull());
        }
#endif

        // Invalid paths
        QVERIFY(!folderman->checkPathValidityForNewFolder("").second.isNull());

        // REMOVE ownCloud2 from the filesystem, but keep a folder sync'ed to it.
        QDir(dirPath + "/ownCloud2/").removeRecursively();
        QVERIFY(!folderman->checkPathValidityForNewFolder(dirPath + "/ownCloud2/blublu").second.isNull());
        QVERIFY(!folderman->checkPathValidityForNewFolder(dirPath + "/ownCloud2/sub/subsub/sub").second.isNull());
    }

    void testFindGoodPathForNewSyncFolder()
    {
        FolderMan::resetInstance();
        _fm = FolderMan::instance();

        // SETUP

        QTemporaryDir dir;
        ConfigFile::setConfDir(dir.path()); // we don't want to pollute the user's config file
        QVERIFY(dir.isValid());
        QDir dir2(dir.path());
        QVERIFY(dir2.mkpath("sub/ownCloud1/folder/f"));
        QVERIFY(dir2.mkpath("ownCloud"));
        QVERIFY(dir2.mkpath("ownCloud2"));
        QVERIFY(dir2.mkpath("ownCloud2/foo"));
        QVERIFY(dir2.mkpath("sub/free"));
        QVERIFY(dir2.mkpath("free2/sub"));
        QString dirPath = dir2.canonicalPath();

        AccountPtr account = Account::create();
        QUrl url("http://example.de");
        auto *cred = new HttpCredentialsTest("testuser", "secret");
        account->setCredentials(cred);
        account->setUrl(url);
        url.setUserName(cred->user());

        AccountStatePtr newAccountState(new AccountState(account));
        FolderMan *folderman = FolderMan::instance();
        QCOMPARE(folderman, _fm.get());
        QVERIFY(folderman->addFolder(newAccountState.data(), folderDefinition(dirPath + "/sub/ownCloud/")));
        QVERIFY(folderman->addFolder(newAccountState.data(), folderDefinition(dirPath + "/ownCloud2/")));

        // TEST

        QCOMPARE(folderman->findGoodPathForNewSyncFolder(dirPath + "/oc", url, FolderMan::GoodPathStrategy::AllowOnlyNewPath), QString(dirPath + "/oc"));
        QCOMPARE(folderman->findGoodPathForNewSyncFolder(dirPath + "/ownCloud", url, FolderMan::GoodPathStrategy::AllowOnlyNewPath),
                 QString(dirPath + "/ownCloud3"));
        QCOMPARE(folderman->findGoodPathForNewSyncFolder(dirPath + "/ownCloud2", url, FolderMan::GoodPathStrategy::AllowOnlyNewPath),
                 QString(dirPath + "/ownCloud22"));
        QCOMPARE(folderman->findGoodPathForNewSyncFolder(dirPath + "/ownCloud2/foo", url, FolderMan::GoodPathStrategy::AllowOnlyNewPath),
                 QString(dirPath + "/ownCloud2/foo"));
        QCOMPARE(folderman->findGoodPathForNewSyncFolder(dirPath + "/ownCloud2/bar", url, FolderMan::GoodPathStrategy::AllowOnlyNewPath),
                 QString(dirPath + "/ownCloud2/bar"));
        QCOMPARE(folderman->findGoodPathForNewSyncFolder(dirPath + "/sub", url, FolderMan::GoodPathStrategy::AllowOnlyNewPath), QString(dirPath + "/sub2"));

        // REMOVE ownCloud2 from the filesystem, but keep a folder sync'ed to it.
        // We should still not suggest this folder as a new folder.
        QDir(dirPath + "/ownCloud2/").removeRecursively();
        QCOMPARE(folderman->findGoodPathForNewSyncFolder(dirPath + "/ownCloud", url, FolderMan::GoodPathStrategy::AllowOnlyNewPath),
                 QString(dirPath + "/ownCloud3"));
        QCOMPARE(folderman->findGoodPathForNewSyncFolder(dirPath + "/ownCloud2", url, FolderMan::GoodPathStrategy::AllowOnlyNewPath),
                 QString(dirPath + "/ownCloud22"));
    }

    void testProcessingFileIdsPushNotification()
    {
        FolderMan::resetInstance();
        _fm = FolderMan::instance();

        QTemporaryDir tempDir;
        ConfigFile::setConfDir(tempDir.path()); // we don't want to pollute the user's config file
        QVERIFY(tempDir.isValid());
        QDir dir(tempDir.path());
        QVERIFY(dir.mkpath("user1_root"));
        QVERIFY(dir.mkpath("user1_subfolder"));
        QVERIFY(dir.mkpath("user2_root"));
        const auto dirPath = dir.canonicalPath();

        const auto createAccount = [](const QString &userName) -> AccountState * {
            auto account = Account::create();
            auto credentials = new FakeCredentials{new FakeQNAM({})};
            credentials->setUserName(userName);
            account->setCredentials(credentials);
            account->setUrl(QUrl{"http://nextcloud.test"});
            return new FakeAccountState(account);
        };

        auto user1 = createAccount("user1");
        auto user2 = createAccount("user2");

        const auto addFolderForTesting = [this, &dirPath](AccountState *const account,
                                                          const QString &alias,
                                                          const QString &localPath,
                                                          const QString &targetPath,
                                                          const int rootFileId,
                                                          const QList<qint64> &fileIds) -> void {
            FolderDefinition definition;
            definition.alias = alias;
            definition.localPath = dirPath + localPath;
            definition.targetPath = targetPath;

            auto folder = _fm->addFolder(account, definition);
            QVERIFY(folder);

            Q_EMIT folder->syncEngine().rootFileIdReceived(rootFileId);

            auto journal = folder->journalDb();
            for (const auto &fileId : fileIds) {
                SyncJournalFileRecord record;
                record._fileId = u"%1oc123xyz987e"_s.arg(fileId, 8, 10, '0'_L1).toLocal8Bit();
                record._modtime = QDateTime::currentSecsSinceEpoch();
                record._path = u"item%1"_s.arg(fileId).toLocal8Bit();
                record._type = ItemTypeFile;
                record._etag = "etag"_ba;
                QVERIFY(journal->setFileRecord(record));
            }
        };

        const auto verifyFolderSyncChangesOnReceivedFileIdNotification =
            [this](AccountState *const user, const QList<qint64> &fileIds, const QStringList &expectedFolderAliasesToSync) -> void {
            QStringList folderAliasesToBeSynced = {};

            _fm->_scheduledFolders.clear();
            QSignalSpy spy(_fm.get(), &FolderMan::folderSyncStateChange);

            // the account received a push notification about for specific file ids
            _fm->slotProcessFileIdsPushNotification(user->account().get(), fileIds);

            // expect the sync state for all folders of that account containing this file id to change
            QCOMPARE(spy.size(), expectedFolderAliasesToSync.size());

            for (const auto &signalParameters : std::as_const(spy)) {
                QVERIFY(signalParameters.size() == 1);
                const auto folderAlias = signalParameters.front().value<Folder *>()->alias();
                QVERIFY2(expectedFolderAliasesToSync.contains(folderAlias),
                         qPrintable("Unexpected folder alias '%1'; expected were [%2]"_L1.arg(folderAlias, expectedFolderAliasesToSync.join(", "))));
                folderAliasesToBeSynced.append(folderAlias);
            }

            // all expected folders received a sync request!
            folderAliasesToBeSynced.sort();
            QCOMPARE(folderAliasesToBeSynced, expectedFolderAliasesToSync);
        };

        addFolderForTesting(user1, "0", "/user1_root", "/", 10, {11, 12, 13, 50});
        addFolderForTesting(user1, "1", "/user1_subfolder", "/subfolder", 15, {16, 17, 18, 50});
        addFolderForTesting(user2, "2", "/user2_root", "/", 20, {21, 22, 23, 50});

        verifyFolderSyncChangesOnReceivedFileIdNotification(user1, {10}, {"0"});
        verifyFolderSyncChangesOnReceivedFileIdNotification(user1, {11}, {"0"});
        verifyFolderSyncChangesOnReceivedFileIdNotification(user1, {13, 11}, {"0"});
        verifyFolderSyncChangesOnReceivedFileIdNotification(user1, {15}, {"1"});
        verifyFolderSyncChangesOnReceivedFileIdNotification(user1, {16}, {"1"});
        verifyFolderSyncChangesOnReceivedFileIdNotification(user1, {18, 16}, {"1"});
        verifyFolderSyncChangesOnReceivedFileIdNotification(user1, {15, 11}, {"0", "1"});
        verifyFolderSyncChangesOnReceivedFileIdNotification(user1, {11, 16, 21}, {"0", "1"});
        verifyFolderSyncChangesOnReceivedFileIdNotification(user1, {50}, {"0", "1"});
        verifyFolderSyncChangesOnReceivedFileIdNotification(user1, {20, 21, 22, 23, 404}, {});

        verifyFolderSyncChangesOnReceivedFileIdNotification(user2, {20}, {"2"});
        verifyFolderSyncChangesOnReceivedFileIdNotification(user2, {21}, {"2"});
        verifyFolderSyncChangesOnReceivedFileIdNotification(user2, {23, 21}, {"2"});
        verifyFolderSyncChangesOnReceivedFileIdNotification(user2, {11, 16, 21}, {"2"});
        verifyFolderSyncChangesOnReceivedFileIdNotification(user2, {50}, {"2"});
        verifyFolderSyncChangesOnReceivedFileIdNotification(user2, {10, 11, 17, 18, 404}, {});
    }

    void testUnloadAndDeleteAllFolders()
    {
        FolderMan::resetInstance();
        _fm = FolderMan::instance();

        QTemporaryDir dir;
        ConfigFile::setConfDir(dir.path());
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("folder1")));
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("folder2")));

        auto account = Account::create();
        account->setCredentials(new FakeCredentials{new FakeQNAM({})});
        account->setUrl(QUrl(QStringLiteral("http://example.de")));
        auto accountState = new FakeAccountState(account);

        const auto folder1 = FolderMan::instance()->addFolder(accountState, folderDefinition(dir.path() + QStringLiteral("/folder1")));
        const auto folder2 = FolderMan::instance()->addFolder(accountState, folderDefinition(dir.path() + QStringLiteral("/folder2")));
        QVERIFY(folder1);
        QVERIFY(folder2);
        QCOMPARE(FolderMan::instance()->map().count(), 2);

        auto socketApi = FolderMan::instance()->socketApi();

        // verifies that calling slotUnregisterPath twice on the same alias doesn't crash
        socketApi->slotUnregisterPath(folder1->alias());
        socketApi->slotUnregisterPath(folder1->alias());

        FolderMan::instance()->unloadAndDeleteAllFolders();

        QCOMPARE(FolderMan::instance()->map().count(), 0);
    }

    void testMacFileProviderModeEnabledConfig()
    {
        QTemporaryDir dir;
        ConfigFile::setConfDir(dir.path());
        QVERIFY(dir.isValid());

        ConfigFile cfg;
        QVERIFY(!cfg.macFileProviderModeEnabledIsSet());
        QVERIFY(!cfg.macFileProviderModeEnabled());

        cfg.setMacFileProviderModeEnabled(true);
        QVERIFY(cfg.macFileProviderModeEnabledIsSet());
        QVERIFY(cfg.macFileProviderModeEnabled());

        cfg.setMacFileProviderModeEnabled(false);
        QVERIFY(cfg.macFileProviderModeEnabledIsSet());
        QVERIFY(!cfg.macFileProviderModeEnabled());
    }

#ifdef BUILD_FILE_PROVIDER_MODULE
    void testFileProviderEtagPollingRequiresConnectedAccount()
    {
        const auto accountState = std::make_unique<FakeAccountState>(Account::create());
        QVERIFY(FolderMan::canPollFileProviderEtag(*accountState));

        const auto disconnectedStates = {
            AccountState::SignedOut,
            AccountState::Disconnected,
            AccountState::ServiceUnavailable,
            AccountState::RedirectDetected,
            AccountState::MaintenanceMode,
            AccountState::NetworkError,
            AccountState::ConfigurationError,
            AccountState::AskingCredentials,
            AccountState::NeedToSignTermsOfService,
        };

        for (const auto state : disconnectedStates) {
            accountState->setStateForTesting(state);
            QVERIFY2(!FolderMan::canPollFileProviderEtag(*accountState), qPrintable(AccountState::stateString(state)));
        }
    }

    void testAddFolderRefusedWhenFileProviderModeEnabled()
    {
        FolderMan::resetInstance();
        _fm = FolderMan::instance();

        QTemporaryDir dir;
        ConfigFile::setConfDir(dir.path());
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("folder1")));

        auto account = Account::create();
        account->setCredentials(new FakeCredentials{new FakeQNAM({})});
        account->setUrl(QUrl(QStringLiteral("http://example.de")));
        auto accountState = new FakeAccountState(account);

        // Classic sync folders and File Provider mode are mutually exclusive.
        ConfigFile().setMacFileProviderModeEnabled(true);
        QVERIFY(!FolderMan::instance()->addFolder(accountState, folderDefinition(dir.path() + QStringLiteral("/folder1"))));
        QCOMPARE(FolderMan::instance()->map().count(), 0);

        ConfigFile().setMacFileProviderModeEnabled(false);
        QVERIFY(FolderMan::instance()->addFolder(accountState, folderDefinition(dir.path() + QStringLiteral("/folder1"))));
        QCOMPARE(FolderMan::instance()->map().count(), 1);

        FolderMan::instance()->unloadAndDeleteAllFolders();
        QCOMPARE(FolderMan::instance()->map().count(), 0);
    }
#endif

#ifdef Q_OS_MACOS
    void testAddFolderStartsAccessFromBookmark()
    {
        FolderMan::resetInstance();
        QTemporaryDir dir;
        ConfigFile::setConfDir(dir.path());
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkpath(u"folder1"_s));
        const auto localPath = dir.path() + u"/folder1"_s;

        auto account = Account::create();
        account->setCredentials(new FakeCredentials{new FakeQNAM({})});
        account->setUrl(QUrl(u"http://example.de"_s));
        auto accountState = new FakeAccountState(account);

        auto definition = folderDefinition(localPath);
        definition.securityScopedBookmarkData = Utility::createSecurityScopedBookmarkData(localPath);
        QVERIFY(!definition.securityScopedBookmarkData.isEmpty());

        const auto folder = FolderMan::instance()->addFolder(accountState, definition);
        QVERIFY(folder);
        QVERIFY(folder->hasSecurityScopedAccess());
        QVERIFY(!folder->needsSandboxBookmark());

        FolderMan::instance()->unloadAndDeleteAllFolders();
    }

    void testAddFolderWithUnusableBookmarkNeedsSandboxBookmark()
    {
        FolderMan::resetInstance();
        QTemporaryDir dir;
        ConfigFile::setConfDir(dir.path());
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkpath(u"folder1"_s));

        auto account = Account::create();
        account->setCredentials(new FakeCredentials{new FakeQNAM({})});
        account->setUrl(QUrl(u"http://example.de"_s));
        auto accountState = new FakeAccountState(account);

        auto definition = folderDefinition(dir.path() + u"/folder1"_s);
        definition.securityScopedBookmarkData = "not a bookmark"_ba;

        const auto folder = FolderMan::instance()->addFolder(accountState, definition);
        QVERIFY(folder);
        QVERIFY(!folder->hasSecurityScopedAccess());
        QVERIFY(folder->needsSandboxBookmark());
        QVERIFY(!folder->canSync());

        FolderMan::instance()->unloadAndDeleteAllFolders();
    }
#endif
};

QTEST_GUILESS_MAIN(TestFolderMan)
#include "testfolderman.moc"
