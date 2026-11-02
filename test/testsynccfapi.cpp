/*
 * SPDX-FileCopyrightText: 2020 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: CC0-1.0
 * 
 * This software is in the public domain, furnished "as is", without technical
 * support, and with no warranty, express or implied, as to its usefulness for
 * any purpose.
 */

#include "syncenginetestutils.h"

#include "common/vfs.h"
#include "config.h"
#include "owncloudpropagator.h"
#include "privateclouddrive.h"
#include "syncengine.h"
#include "vfs/cfapi/cfapiwrapper.h"

#include <QProcess>
#include <QScopeGuard>
#include <QSemaphore>
#include <QtConcurrentRun>
#include <QtTest>

using namespace Qt::StringLiterals;

namespace cfapi {
using namespace OCC::CfApiWrapper;
}

#define CFVERIFY_VIRTUAL(folder, path) \
    QVERIFY(QFileInfo((folder).localPath() + (path)).exists()); \
    QVERIFY(cfapi::isSparseFile((folder).localPath() + (path))); \
    QVERIFY(dbRecord((folder), (path)).isValid()); \
    QCOMPARE(dbRecord((folder), (path))._type, ItemTypeVirtualFile);

#define CFVERIFY_NONVIRTUAL(folder, path) \
    QVERIFY(QFileInfo((folder).localPath() + (path)).exists()); \
    QVERIFY(!cfapi::isSparseFile((folder).localPath() + (path))); \
    QVERIFY(dbRecord((folder), (path)).isValid()); \
    QCOMPARE(dbRecord((folder), (path))._type, ItemTypeFile);

#define CFVERIFY_GONE(folder, path) \
    QVERIFY(!QFileInfo((folder).localPath() + (path)).exists()); \
    QVERIFY(!dbRecord((folder), (path)).isValid());

using namespace OCC;

enum ErrorKind : int {
    NoError = 0,
    // Lower code are corresponding to HTTP error code
    Timeout = 1000,
};

void setPinState(const QString &path, PinState state, cfapi::SetPinRecurseMode mode)
{
    Q_ASSERT(mode == cfapi::Recurse || mode == cfapi::NoRecurse);

    const auto p = QDir::toNativeSeparators(path);
    const auto handle = cfapi::handleForPath(p);
    Q_ASSERT(handle);

    const auto result = cfapi::setPinState(p, state, mode);
    Q_ASSERT(result);

    if (mode == cfapi::NoRecurse) {
        const auto result = cfapi::setPinState(p, PinState::Inherited, cfapi::ChildrenOnly);
        Q_ASSERT(result);
    }
}

bool itemInstruction(const ItemCompletedSpy &spy, const QString &path, const SyncInstructions instr)
{
    auto item = spy.findItem(path);
    return item->_instruction == instr;
}

SyncJournalFileRecord dbRecord(FakeFolder &folder, const QString &path)
{
    SyncJournalFileRecord record;
    [[maybe_unused]] const auto result = folder.syncJournal().getFileRecord(path, &record);
    return record;
}

void triggerDownload(FakeFolder &folder, const QByteArray &path)
{
    auto &journal = folder.syncJournal();
    SyncJournalFileRecord record;
    if (!journal.getFileRecord(path, &record) || !record.isValid()) {
        return;
    }
    record._type = ItemTypeVirtualFileDownload;
    QVERIFY(journal.setFileRecord(record));
    journal.schedulePathForRemoteDiscovery(record._path);
}

void markForDehydration(FakeFolder &folder, const QByteArray &path)
{
    auto &journal = folder.syncJournal();
    SyncJournalFileRecord record;
    if (!journal.getFileRecord(path, &record) || !record.isValid()) {
        return;
    }
    record._type = ItemTypeVirtualFileDehydration;
    QVERIFY(journal.setFileRecord(record));
    journal.schedulePathForRemoteDiscovery(record._path);
}

QSharedPointer<Vfs> setupVfs(FakeFolder &folder)
{
    auto cfapiVfs = QSharedPointer<Vfs>(createVfsFromPlugin(Vfs::WindowsCfApi).release());
    QObject::connect(&folder.syncEngine().syncFileStatusTracker(), &SyncFileStatusTracker::fileStatusChanged,
                     cfapiVfs.data(), &Vfs::fileStatusChanged);
    folder.switchToVfs(cfapiVfs);

    ::setPinState(folder.localPath(), PinState::Unspecified, cfapi::NoRecurse);

    return cfapiVfs;
}

bool enumerateCloudFolder(const QString &path)
{
    // A separate process triggers native population callbacks without leaving
    // Explorer windows pointing into a temporary folder after test teardown.
    const auto powershell = QStandardPaths::findExecutable(QStringLiteral("powershell.exe"));
    if (powershell.isEmpty()) {
        return false;
    }
    QProcess reader;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PRIVATECLOUD_ENUMERATION_TEST_PATH"), path);
    reader.setProcessEnvironment(environment);
    reader.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
        arguments->flags |= CREATE_NO_WINDOW;
    });
    reader.start(powershell,
                 {QStringLiteral("-NoProfile"),
                  QStringLiteral("-NonInteractive"),
                  QStringLiteral("-Command"),
                  QStringLiteral("$ErrorActionPreference = 'Stop'; try { "
                                 "$null = [System.IO.Directory]::GetFileSystemEntries($env:PRIVATECLOUD_ENUMERATION_TEST_PATH); "
                                 "exit 0 } catch { Write-Error $_; exit 1 }")});
    constexpr int startupTimeoutMs = 5000;
    constexpr int enumerationTimeoutMs = 30000;
    if (!reader.waitForStarted(startupTimeoutMs)) {
        return false;
    }
    // Keep servicing the provider's Qt event loop while the reader waits for it.
    if (!QTest::qWaitFor(
            [&reader] {
                return reader.state() == QProcess::NotRunning;
            },
            enumerationTimeoutMs)) {
        reader.kill();
        reader.waitForFinished(startupTimeoutMs);
        return false;
    }
    return reader.exitStatus() == QProcess::NormalExit && reader.exitCode() == 0;
}

class TestSyncCfApi : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        Logger::instance()->setLogFlush(true);
        Logger::instance()->setLogDebug(true);

        QStandardPaths::setTestModeEnabled(true);
    }

    void testTransferredPlaceholderIdentity_data()
    {
        QTest::addColumn<QByteArray>("fileId");
        QTest::addColumn<bool>("directory");
        QTest::newRow("file ascii") << QByteArrayLiteral("remote-file-id") << false;
        QTest::newRow("file embedded null") << QByteArray::fromHex("6100620063") << false;
        QTest::newRow("file non-ascii bytes") << QByteArray::fromHex("c3a9ff80") << false;
        QTest::newRow("directory ascii") << QByteArrayLiteral("remote-folder-id") << true;
        QTest::newRow("directory binary identity") << QByteArray::fromHex("c3a900ff80") << true;
    }

    void testTransferredPlaceholderIdentity()
    {
        QFETCH(QByteArray, fileId);
        QFETCH(bool, directory);
        PlaceholderCreateInfo entry;
        entry.name = QStringLiteral("subfolder/rapport-\u00e9.txt");
        entry.stdWStringName = entry.name.toStdWString();
        entry.parsedProperties.fileId = fileId;
        entry.parsedProperties.size = 12345;
        entry.parsedProperties.modtime = 1700000000;
        entry.parsedProperties.isDirectory = directory;

        const auto placeholder = cfapi::createTransferPlaceholderInfo(entry);
        QCOMPARE(placeholder.FileIdentityLength, DWORD(fileId.size()));
        QCOMPARE(QByteArray(static_cast<const char *>(placeholder.FileIdentity), placeholder.FileIdentityLength), fileId);
        QVERIFY(placeholder.FileIdentity == entry.parsedProperties.fileId.constData());
        QVERIFY(placeholder.RelativeFileName == entry.stdWStringName.c_str());
        QCOMPARE(QString::fromWCharArray(placeholder.RelativeFileName), entry.name);
        QCOMPARE(placeholder.Flags, CF_PLACEHOLDER_CREATE_FLAG_MARK_IN_SYNC);
        QCOMPARE(placeholder.FsMetadata.FileSize.QuadPart, directory ? 0LL : 12345LL);
        QCOMPARE(placeholder.FsMetadata.BasicInfo.FileAttributes, DWORD(directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL));
        constexpr auto expectedFileTime = 133444736000000000LL;
        QCOMPARE(placeholder.FsMetadata.BasicInfo.CreationTime.QuadPart, expectedFileTime);
        QCOMPARE(placeholder.FsMetadata.BasicInfo.LastWriteTime.QuadPart, expectedFileTime);
        QCOMPARE(placeholder.FsMetadata.BasicInfo.LastAccessTime.QuadPart, expectedFileTime);
        QCOMPARE(placeholder.FsMetadata.BasicInfo.ChangeTime.QuadPart, expectedFileTime);
    }

    void testProviderDisplayNamePreservesCloudIdentities()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto path = QDir::toNativeSeparators(root.path());
        const auto nativePath = path.toStdWString();
        const auto providerName = QStringLiteral(APPLICATION_NAME);
        const auto oldNativeName = providerName.toStdWString();
        const auto uniqueAccount = QStringLiteral("branding-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));

        // Reproduce the registration written by the previous client: the root
        // has no opaque identity, and the display name is its persistent name.
        CF_SYNC_REGISTRATION registration = {};
        registration.StructSize = sizeof(registration);
        registration.ProviderName = oldNativeName.c_str();
        registration.ProviderVersion = L"1.0";
        CF_SYNC_POLICIES policies = {};
        policies.StructSize = sizeof(policies);
        policies.Hydration.Primary = CF_HYDRATION_POLICY_FULL;
        policies.Population.Primary = CF_POPULATION_POLICY_PARTIAL;
        policies.InSync = CF_INSYNC_POLICY_PRESERVE_INSYNC_FOR_SYNC_ENGINE;
        policies.HardLink = CF_HARDLINK_POLICY_NONE;
        QCOMPARE(CfRegisterSyncRoot(nativePath.c_str(), &registration, &policies, CF_REGISTER_FLAG_NONE), S_OK);
        const auto cleanup = qScopeGuard([&] {
            const auto result = cfapi::unregisterSyncRoot(path, providerName, uniqueAccount);
            if (!result) {
                qWarning() << result.error();
            }
        });

        // Connect before opening placeholder metadata handles, as the client does.
        // Keep the legacy registration intact for the initial snapshot.
        const CF_CALLBACK_REGISTRATION callbacks[] = {CF_CALLBACK_REGISTRATION_END};
        CF_CONNECTION_KEY connection = {};
        auto connected = false;
        const auto connectProvider = [&] {
            const auto result = CfConnectSyncRoot(nativePath.c_str(), callbacks, nullptr, CF_CONNECT_FLAG_BLOCK_SELF_IMPLICIT_HYDRATION, &connection);
            connected = SUCCEEDED(result);
            return result;
        };
        const auto disconnect = qScopeGuard([&] {
            if (connected) {
                const auto result = CfDisconnectSyncRoot(connection);
                if (FAILED(result)) {
                    qWarning() << "Could not disconnect the branding test provider:" << Utility::formatWinError(result);
                }
            }
        });
        QCOMPARE(connectProvider(), S_OK);

        const QString filePath = path + QStringLiteral("\\branding.txt");
        const auto fileIdentity = QByteArrayLiteral("branding-file-id");
        const auto createResult = cfapi::createPlaceholderInfo(filePath, 1700000000, 64, fileIdentity);
        QVERIFY2(createResult, qPrintable(createResult ? QString{} : createResult.error()));

        CF_SYNC_ROOT_STANDARD_INFO rootBefore = {};
        DWORD returnedSize = 0;
        QCOMPARE(CfGetSyncRootInfoByPath(nativePath.c_str(), CF_SYNC_ROOT_INFO_STANDARD, &rootBefore, sizeof(rootBefore), &returnedSize), S_OK);
        QCOMPARE(QString::fromWCharArray(rootBefore.ProviderName), providerName);
        QCOMPARE(rootBefore.SyncRootIdentityLength, 0UL);
        QVERIFY(rootBefore.SyncRootFileId.QuadPart != 0);

        auto fileBeforeBuffer = std::vector<char>(sizeof(CF_PLACEHOLDER_STANDARD_INFO) + CF_PLACEHOLDER_MAX_FILE_IDENTITY_LENGTH);
        {
            const auto handle = cfapi::handleForPath(filePath);
            QVERIFY(handle);
            QCOMPARE(CfGetPlaceholderInfo(handle.get(),
                                          CF_PLACEHOLDER_INFO_STANDARD,
                                          fileBeforeBuffer.data(),
                                          static_cast<DWORD>(fileBeforeBuffer.size()),
                                          &returnedSize),
                     S_OK);
        }
        const auto nativeFilePath = filePath.toStdWString();
        const auto attributesBefore = GetFileAttributesW(nativeFilePath.c_str());
        QVERIFY(attributesBefore != INVALID_FILE_ATTRIBUTES);
        const auto fileBefore = reinterpret_cast<const CF_PLACEHOLDER_STANDARD_INFO *>(fileBeforeBuffer.data());
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(fileBefore->FileIdentity), fileBefore->FileIdentityLength), fileIdentity);

        // The client updates registration before connecting on its next startup.
        // A live connection retains the provider name from when it was opened.
        QCOMPARE(CfDisconnectSyncRoot(connection), S_OK);
        connected = false;
        const auto update = cfapi::registerSyncRoot(path,
                                                    providerName,
                                                    QStringLiteral("1.0"),
                                                    uniqueAccount,
                                                    QUuid::createUuid().toString(),
                                                    QStringLiteral(APPLICATION_DISPLAY_NAME),
                                                    uniqueAccount);
        QVERIFY(update);
        QCOMPARE(connectProvider(), S_OK);

        CF_SYNC_ROOT_STANDARD_INFO rootAfter = {};
        QCOMPARE(CfGetSyncRootInfoByPath(nativePath.c_str(), CF_SYNC_ROOT_INFO_STANDARD, &rootAfter, sizeof(rootAfter), &returnedSize), S_OK);
        QCOMPARE(QString::fromWCharArray(rootAfter.ProviderName), QStringLiteral(APPLICATION_DISPLAY_NAME));
        QCOMPARE(rootAfter.SyncRootFileId.QuadPart, rootBefore.SyncRootFileId.QuadPart);
        QCOMPARE(rootAfter.SyncRootIdentityLength, rootBefore.SyncRootIdentityLength);
        QCOMPARE(rootAfter.HydrationPolicy.Primary, rootBefore.HydrationPolicy.Primary);
        QCOMPARE(rootAfter.HydrationPolicy.Modifier, rootBefore.HydrationPolicy.Modifier);
        QCOMPARE(rootAfter.PopulationPolicy.Primary, rootBefore.PopulationPolicy.Primary);
        QCOMPARE(rootAfter.PopulationPolicy.Modifier, rootBefore.PopulationPolicy.Modifier);
        QCOMPARE(rootAfter.InSyncPolicy, rootBefore.InSyncPolicy);
        QCOMPARE(rootAfter.HardLinkPolicy, rootBefore.HardLinkPolicy);

        auto fileAfterBuffer = std::vector<char>(fileBeforeBuffer.size());
        {
            const auto handle = cfapi::handleForPath(filePath);
            QVERIFY(handle);
            QCOMPARE(CfGetPlaceholderInfo(handle.get(),
                                          CF_PLACEHOLDER_INFO_STANDARD,
                                          fileAfterBuffer.data(),
                                          static_cast<DWORD>(fileAfterBuffer.size()),
                                          &returnedSize),
                     S_OK);
        }
        const auto fileAfter = reinterpret_cast<const CF_PLACEHOLDER_STANDARD_INFO *>(fileAfterBuffer.data());
        QCOMPARE(fileAfter->FileId.QuadPart, fileBefore->FileId.QuadPart);
        QCOMPARE(fileAfter->SyncRootFileId.QuadPart, fileBefore->SyncRootFileId.QuadPart);
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(fileAfter->FileIdentity), fileAfter->FileIdentityLength), fileIdentity);
        QCOMPARE(fileAfter->PinState, fileBefore->PinState);
        QCOMPARE(fileAfter->InSyncState, fileBefore->InSyncState);
        QCOMPARE(fileAfter->OnDiskDataSize.QuadPart, fileBefore->OnDiskDataSize.QuadPart);
        // Compare all actual attributes, including sparse/reparse flags.
        const auto attributesAfter = GetFileAttributesW(nativeFilePath.c_str());
        QVERIFY(attributesAfter != INVALID_FILE_ATTRIBUTES);
        QCOMPARE(attributesAfter, attributesBefore);
    }

    void testPlaceholderUpdateReportsNativeFailure_data()
    {
        QTest::addColumn<bool>("basicMetadataOnly");
        QTest::newRow("all metadata") << false;
        QTest::newRow("basic metadata") << true;
    }

    void testPlaceholderUpdateReportsNativeFailure()
    {
        QFETCH(bool, basicMetadataOnly);
        FakeFolder folder{FileInfo{}, {}, {}, false};
        const auto vfs = setupVfs(folder);
        QVERIFY(vfs->isReady());
        const auto path = QDir::toNativeSeparators(folder.localPath() + QStringLiteral("metadata.txt"));
        const auto originalIdentity = QByteArrayLiteral("original-identity");
        QVERIFY(cfapi::createPlaceholderInfo(path, 1700000000, 1024, originalIdentity));
        QVERIFY(cfapi::setPinState(path, PinState::OnlineOnly, cfapi::NoRecurse));

        SyncFileItem item;
        item._type = ItemTypeVirtualFile;
        item._modtime = 1700000001;
        item._size = 2048;
        // Ask Windows to reject an identity larger than the Cloud Files limit.
        // The pin-state operation would still succeed, masking this failure.
        item._fileId = QByteArray(CF_PLACEHOLDER_MAX_FILE_IDENTITY_LENGTH + 1, 'x');
        auto nativeFailure = S_OK;
        {
            const auto handle = cfapi::handleForPath(path);
            QVERIFY(handle);
            nativeFailure = CfUpdatePlaceholder(handle.get(),
                                                nullptr,
                                                item._fileId.constData(),
                                                static_cast<DWORD>(item._fileId.size()),
                                                nullptr,
                                                0,
                                                CF_UPDATE_FLAG_MARK_IN_SYNC,
                                                nullptr,
                                                nullptr);
            QVERIFY(FAILED(nativeFailure));
        }
        const auto result = basicMetadataOnly ? cfapi::updatePlaceholderMarkInSync(path, item, {}) : cfapi::updatePlaceholderInfo(path, item, {});
        QVERIFY(!result);
        QVERIFY2(result.error().contains(Utility::formatWinError(nativeFailure)), qPrintable(result.error()));
        const auto info = cfapi::findPlaceholderInfo(path);
        QVERIFY(info);
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(info->FileIdentity), info->FileIdentityLength), originalIdentity);
        QCOMPARE(info->PinState, CF_PIN_STATE_UNPINNED);
        QCOMPARE(QFileInfo(path).size(), 1024);
        QCOMPARE(QFileInfo(path).lastModified().toSecsSinceEpoch(), 1700000000);

        item._fileId = QByteArrayLiteral("updated-identity");
        const auto retry = basicMetadataOnly ? cfapi::updatePlaceholderMarkInSync(path, item, {}) : cfapi::updatePlaceholderInfo(path, item, {});
        QVERIFY2(retry, retry ? "" : qPrintable(retry.error()));
        const auto updatedInfo = cfapi::findPlaceholderInfo(path);
        QVERIFY(updatedInfo);
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(updatedInfo->FileIdentity), updatedInfo->FileIdentityLength), item._fileId);
        QCOMPARE(updatedInfo->PinState, CF_PIN_STATE_UNPINNED);
        QCOMPARE(QFileInfo(path).size(), basicMetadataOnly ? 1024 : 2048);
    }

    void testPlaceholderUpdateRejectsEmptyTarget()
    {
        FakeFolder folder{FileInfo{}, {}, {}, false};
        const auto vfs = setupVfs(folder);
        QVERIFY(vfs->isReady());
        const auto originalPath = QDir::toNativeSeparators(folder.localPath() + QStringLiteral("original.txt"));
        const auto targetPath = QString{};
        const auto originalIdentity = QByteArrayLiteral("original-identity");
        QVERIFY(cfapi::createPlaceholderInfo(originalPath, 1700000000, 1024, originalIdentity));
        QVERIFY(!cfapi::handleForPath(targetPath));

        SyncFileItem item;
        item._type = ItemTypeVirtualFile;
        item._modtime = 1700000001;
        item._size = 2048;
        item._fileId = QByteArrayLiteral("updated-identity");
        // Read the preserved metadata from the original, then fail to open the
        // destination. The native update must not receive a null handle.
        const auto result = cfapi::updatePlaceholderInfo(targetPath, item, originalPath);
        QVERIFY(!result);
        QCOMPARE(result.error(), QStringLiteral("Could not open placeholder for metadata update: %1").arg(targetPath));
    }

    void testPlaceholderUpdateJobReportsNativeFailure()
    {
        FakeFolder folder{FileInfo{}, {}, {}, false};
        const auto vfs = setupVfs(folder);
        QVERIFY(vfs->isReady());
        const auto path = QDir::toNativeSeparators(folder.localPath() + QStringLiteral("metadata-job.txt"));
        QVERIFY(cfapi::createPlaceholderInfo(path, 1700000000, 1024, QByteArrayLiteral("original-identity")));
        auto item = SyncFileItemPtr::create();
        item->_file = QStringLiteral("metadata-job.txt");
        item->_originalFile = item->_file;
        item->_fileId = QByteArray(CF_PLACEHOLDER_MAX_FILE_IDENTITY_LENGTH + 1, 'x');
        item->_modtime = 1700000001;
        item->_size = 1024;
        item->_type = ItemTypeVirtualFile;
        item->_instruction = CSYNC_INSTRUCTION_UPDATE_VFS_METADATA;
        item->_direction = SyncFileItem::Down;
        const auto nativeFailure = cfapi::updatePlaceholderMarkInSync(path, *item);
        QVERIFY(!nativeFailure);

        QSet<QString> bulkUploadBlacklist;
        OwncloudPropagator propagator(folder.account(), folder.localPath(), QStringLiteral("/"), &folder.syncJournal(), bulkUploadBlacklist);
        propagator.setSyncOptions(folder.syncEngine().syncOptions());
        QSignalSpy finished(&propagator, &OwncloudPropagator::finished);
        QSignalSpy completed(&propagator, &OwncloudPropagator::itemCompleted);
        propagator.start({item});
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(completed.count(), 1);
        QCOMPARE(item->_status, SyncFileItem::NormalError);
        QCOMPARE(item->_errorString, nativeFailure.error());
    }

    void testProviderReadiness()
    {
        const auto unstartedVfs = createVfsFromPlugin(Vfs::WindowsCfApi);
        QVERIFY(unstartedVfs);
        QVERIFY(!unstartedVfs->isReady());
        FakeFolder folder{FileInfo{}};
        const auto vfs = setupVfs(folder);
        QVERIFY(vfs->isReady());
        vfs->stop();
        QVERIFY(!vfs->isReady());
    }

    void testBulkPlaceholdersKeepTheirParentDirectories()
    {
        FakeFolder folder{FileInfo{}, {}, {}, false};
        QVERIFY(folder.syncEngine().account()->setUrl(Account::enforcedServerUrl().isEmpty() ? QUrl(QStringLiteral("http://localhost/owncloud"))
                                                                                             : Account::enforcedServerUrl()));
        const auto vfs = setupVfs(folder);
        QVERIFY(vfs->isReady());
        QVERIFY(QDir(folder.localPath()).mkpath(QStringLiteral("first")));
        QVERIFY(QDir(folder.localPath()).mkpath(QStringLiteral("second/nested")));
        const auto paths =
            QStringList{QStringLiteral("first/report.txt"), QStringLiteral("second/nested/report.txt"), QStringLiteral("second/nested/other.txt")};
        auto items = QList<SyncFileItemPtr>{};
        for (const auto &path : paths) {
            auto item = SyncFileItemPtr::create();
            item->_file = path;
            item->_fileId = path.toUtf8();
            item->_type = ItemTypeVirtualFile;
            item->_modtime = 1700000000;
            item->_size = 1024;
            items.append(item);
        }
        const auto result = vfs->createPlaceholders(items);
        QVERIFY2(result, result ? "" : qPrintable(result.error()));
        for (const auto &path : paths) {
            QVERIFY(QFileInfo::exists(folder.localPath() + path));
            QVERIFY(cfapi::isSparseFile(folder.localPath() + path));
            const auto info = cfapi::findPlaceholderInfo(folder.localPath() + path);
            QVERIFY(info);
            QCOMPARE(QByteArray(reinterpret_cast<const char *>(info->FileIdentity), info->FileIdentityLength), path.toUtf8());
        }
        QVERIFY(!QFileInfo::exists(folder.localPath() + QStringLiteral("first/other.txt")));
    }

    void testBulkPlaceholdersRecoverPartialCreation()
    {
        FakeFolder folder{FileInfo{}, {}, {}, false};
        QVERIFY(folder.syncEngine().account()->setUrl(Account::enforcedServerUrl().isEmpty() ? QUrl(QStringLiteral("http://localhost/owncloud"))
                                                                                             : Account::enforcedServerUrl()));
        const auto vfs = setupVfs(folder);
        QVERIFY(vfs->isReady());
        QVERIFY(vfs->createPlaceholders({}));
        QVERIFY(cfapi::setPinState(folder.localPath(), PinState::OnlineOnly, cfapi::NoRecurse));
        auto items = QList<SyncFileItemPtr>{};
        for (const auto &name : {QStringLiteral("first.txt"), QStringLiteral("second.txt")}) {
            auto item = SyncFileItemPtr::create();
            item->_file = name;
            item->_fileId = name.toUtf8();
            item->_type = ItemTypeVirtualFile;
            item->_modtime = 1700000000;
            item->_size = 1024;
            items.append(item);
        }
        QFile localFile(folder.localPath() + QStringLiteral("second.txt"));
        QVERIFY(localFile.open(QIODevice::WriteOnly));
        QCOMPARE(localFile.write("local data"), 10);
        localFile.close();
        const auto firstAttempt = vfs->createPlaceholders(items);
        QVERIFY(!firstAttempt);
        QVERIFY(firstAttempt.error().contains(QStringLiteral("Cannot create cloud files:")));
        QVERIFY(localFile.open(QIODevice::ReadOnly));
        QCOMPARE(localFile.readAll(), QByteArrayLiteral("local data"));
        localFile.close();
        QVERIFY(cfapi::isSparseFile(folder.localPath() + QStringLiteral("first.txt")));
        QVERIFY(localFile.remove());
        QVERIFY(vfs->createPlaceholders(items));
        QVERIFY(cfapi::isSparseFile(folder.localPath() + QStringLiteral("second.txt")));
        QCOMPARE(cfapi::findPlaceholderInfo(folder.localPath() + QStringLiteral("first.txt"))->PinState, CF_PIN_STATE_UNPINNED);
        auto changedMetadata = SyncFileItemPtr::create(*items.first());
        ++changedMetadata->_modtime;
        QVERIFY(!vfs->createPlaceholders({changedMetadata}));
        auto differentIdentity = SyncFileItemPtr::create(*items.first());
        differentIdentity->_fileId = QByteArrayLiteral("another-remote-file");
        QVERIFY(!vfs->createPlaceholders({differentIdentity}));
        {
            const auto handle = cfapi::handleForPath(folder.localPath() + QStringLiteral("first.txt"));
            QCOMPARE(CfSetInSyncState(handle.get(), CF_IN_SYNC_STATE_NOT_IN_SYNC, CF_SET_IN_SYNC_FLAG_NONE, nullptr), S_OK);
        }
        QVERIFY(!vfs->createPlaceholders({items.first()}));
        items.first()->_modtime = 0;
        QVERIFY(!vfs->createPlaceholders(items));
    }

    void testBulkPlaceholderFailuresHaveAnExplanation()
    {
        FakeFolder folder{FileInfo{}, {}, {}, false};
        QVERIFY(folder.syncEngine().account()->setUrl(Account::enforcedServerUrl().isEmpty() ? QUrl(QStringLiteral("http://localhost/owncloud"))
                                                                                             : Account::enforcedServerUrl()));
        const auto vfs = setupVfs(folder);
        QVERIFY(vfs->isReady());
        QSet<QString> bulkUploadBlacklist;
        OwncloudPropagator propagator(folder.account(), folder.localPath(), QStringLiteral("/"), &folder.syncJournal(), bulkUploadBlacklist);
        propagator.setSyncOptions(folder.syncEngine().syncOptions());
        QSignalSpy finished(&propagator, &OwncloudPropagator::finished);
        QSignalSpy completed(&propagator, &OwncloudPropagator::itemCompleted);
        const auto item = SyncFileItemPtr::create();
        item->_file = QStringLiteral("bad-time.txt");
        item->_originalFile = item->_file;
        item->_fileId = QByteArrayLiteral("file-id");
        item->_modtime = 0;
        item->_type = ItemTypeVirtualFile;
        item->_instruction = CSYNC_INSTRUCTION_NEW;
        item->_direction = SyncFileItem::Down;
        propagator.start({item});
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(completed.count(), 1);
        QCOMPARE(item->_status, SyncFileItem::NormalError);
        QVERIFY(item->_errorString.contains(QStringLiteral("invalid modification time")));
        QVERIFY(!QFileInfo::exists(folder.localPath() + item->_file));
    }

    void testNativeCallbackShutdownGate()
    {
        const auto vfs = createVfsFromPlugin(Vfs::WindowsCfApi);
        QVERIFY(vfs);
        QVERIFY(!vfs->hasPendingFileOperations());
        auto entries = 0;
        const auto finishEntries = qScopeGuard([&] {
            while (entries-- > 0) {
                QMetaObject::invokeMethod(vfs.get(), "endNativeFetch", Qt::DirectConnection);
            }
        });
        const auto enter = [&] {
            auto accepted = false;
            const auto invoked = QMetaObject::invokeMethod(vfs.get(), "beginNativeFetch", Qt::DirectConnection, Q_RETURN_ARG(bool, accepted));
            if (accepted) {
                ++entries;
            }
            return invoked && accepted;
        };
        const auto leave = [&] {
            --entries;
            return QMetaObject::invokeMethod(vfs.get(), "endNativeFetch", Qt::DirectConnection);
        };
        QVERIFY(enter());
        QVERIFY(enter());
        QVERIFY(vfs->hasPendingFileOperations());
        // File and folder callbacks may both be waiting for their queued UI work.
        vfs->setShutdownRequested(true);
        QVERIFY(!enter());
        QVERIFY(leave());
        QVERIFY(vfs->hasPendingFileOperations());
        QVERIFY(leave());
        QVERIFY(!vfs->hasPendingFileOperations());
        QVERIFY(!enter());
        // A bounded shutdown timeout restores ordinary file access.
        vfs->setShutdownRequested(false);
        QVERIFY(enter());
        QVERIFY(leave());
        QVERIFY(!vfs->hasPendingFileOperations());
    }

    void testNativeCallbackShutdownGateAcrossThreads()
    {
        const auto vfs = createVfsFromPlugin(Vfs::WindowsCfApi);
        QVERIFY(vfs);
        QSemaphore entered;
        QSemaphore release;
        auto worker = QtConcurrent::run([&] {
            auto accepted = false;
            const auto invoked = QMetaObject::invokeMethod(vfs.get(), "beginNativeFetch", Qt::DirectConnection, Q_RETURN_ARG(bool, accepted));
            entered.release();
            if (!invoked || !accepted) {
                return false;
            }
            const auto finishEntry = qScopeGuard([&] {
                QMetaObject::invokeMethod(vfs.get(), "endNativeFetch", Qt::DirectConnection);
            });
            constexpr int callbackWaitDeadlineMs = 5000;
            return release.tryAcquire(1, callbackWaitDeadlineMs);
        });
        const auto finishWorker = qScopeGuard([&] {
            release.release();
            worker.waitForFinished();
        });
        QVERIFY(entered.tryAcquire(1, 5000));
        vfs->setShutdownRequested(true);
        QVERIFY(vfs->hasPendingFileOperations());
        auto accepted = true;
        QVERIFY(QMetaObject::invokeMethod(vfs.get(), "beginNativeFetch", Qt::DirectConnection, Q_RETURN_ARG(bool, accepted)));
        QVERIFY(!accepted);
        release.release();
        QTRY_VERIFY(worker.isFinished());
        QVERIFY(worker.result());
        QVERIFY(!vfs->hasPendingFileOperations());
    }

    void testExplicitlyAuthorizedDriveAliasHydration()
    {
        if (qEnvironmentVariable("PRIVATECLOUD_TEST_DRIVE_MAPPING") != QStringLiteral("1")) {
            QSKIP("Real drive hydration requires explicit opt-in for the authorized X: development drive.");
        }
        QCOMPARE(PrivateCloudDrive::driveRoot(), QStringLiteral("X:/"));
        QVERIFY(PrivateCloudDrive::isEnabled());
        wchar_t existing[32768] = {};
        const auto found = QueryDosDeviceW(L"X:", existing, 32768);
        QVERIFY2(!found && GetLastError() == ERROR_FILE_NOT_FOUND, "X: must be completely unused before this test.");
        const auto powershell = QStandardPaths::findExecutable(QStringLiteral("powershell.exe"));
        QVERIFY2(!powershell.isEmpty(), "Windows PowerShell is required for the isolated file reader.");

        FakeFolder folder{FileInfo{}, {}, {}, false};
        const auto account = folder.syncEngine().account();
        QVERIFY(account->setUrl(Account::enforcedServerUrl()));
        const auto vfs = setupVfs(folder);
        QVERIFY(vfs->isReady());

        const auto fileName = QStringLiteral("alias-hydration.bin");
        constexpr int contentSize = 8192;
        constexpr char contentByte = 'P';
        folder.remoteModifier().insert(fileName, contentSize, contentByte);
        const auto remoteFile = folder.remoteModifier().find(fileName);
        QVERIFY(remoteFile);
        SyncJournalFileRecord record;
        record._path = fileName.toUtf8();
        record._type = ItemTypeVirtualFile;
        record._fileId = remoteFile->fileId;
        record._etag = remoteFile->etag;
        record._fileSize = contentSize;
        record._modtime = remoteFile->lastModified.toSecsSinceEpoch();
        const auto item = SyncFileItem::fromSyncJournalFileRecord(record);
        const auto placeholderResult = vfs->createPlaceholder(*item);
        QVERIFY(placeholderResult);
        record = item->toSyncJournalFileRecordWithInode(folder.localPath() + fileName);
        QVERIFY(folder.syncJournal().setFileRecord(record));
        CFVERIFY_VIRTUAL(folder, fileName);

        auto propertyRequests = 0;
        auto downloads = 0;
        auto unexpectedRequests = 0;
        auto nativeCallbackTracked = false;
        folder.setServerOverride([&](QNetworkAccessManager::Operation operation, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            // The shared fake DAV replies use /owncloud paths. Keep the production
            // request on the enforced server, adapting only this fixture's response.
            auto fixtureRequest = request;
            auto fixtureUrl = request.url();
            fixtureUrl.setPath(sRootUrl2.path() + fileName);
            fixtureRequest.setUrl(fixtureUrl);
            if (request.url().path().endsWith(u'/' + fileName)) {
                if (request.attribute(QNetworkRequest::CustomVerbAttribute).toByteArray() == QByteArrayLiteral("PROPFIND")) {
                    ++propertyRequests;
                    nativeCallbackTracked = vfs->hasPendingFileOperations();
                    return new FakePropfindReply(folder.remoteModifier(), operation, fixtureRequest, folder.networkAccessManager(), true);
                }
                if (operation == QNetworkAccessManager::GetOperation) {
                    ++downloads;
                    return new FakeGetReply(folder.remoteModifier(), operation, fixtureRequest, folder.networkAccessManager());
                }
            }
            ++unexpectedRequests;
            return new FakeErrorReply(operation, request, folder.networkAccessManager(), 404);
        });
        QSignalSpy ready(vfs.data(), SIGNAL(hydrationRequestReady(QString)));
        QSignalSpy failed(vfs.data(), SIGNAL(hydrationRequestFailed(QString)));
        QVERIFY(ready.isValid());
        QVERIFY(failed.isValid());

        const auto mountError = PrivateCloudDrive::mount(folder.localPath());
        QVERIFY2(mountError.isEmpty(), qPrintable(mountError));
        const auto unmountDrive = qScopeGuard([&folder] {
            PrivateCloudDrive::unmount(folder.localPath());
        });
        QCOMPARE(GetDriveTypeW(L"X:\\"), UINT(DRIVE_FIXED));

        QProcess reader;
        const auto stopReader = qScopeGuard([&reader, &vfs] {
            if (reader.state() != QProcess::NotRunning) {
                reader.kill();
                reader.waitForFinished(5000);
            }
            vfs->stop();
        });
        reader.start(powershell,
                     {QStringLiteral("-NoProfile"),
                      QStringLiteral("-NonInteractive"),
                      QStringLiteral("-Command"),
                      QStringLiteral("[Convert]::ToBase64String([System.IO.File]::ReadAllBytes('X:\\alias-hydration.bin'))")});
        QVERIFY(reader.waitForStarted(5000));
        QTRY_VERIFY_WITH_TIMEOUT(reader.state() == QProcess::NotRunning, 30000);
        QCOMPARE(reader.exitStatus(), QProcess::NormalExit);
        const auto readerErrors = reader.readAllStandardError();
        QVERIFY2(readerErrors.isEmpty(), readerErrors.constData());
        QCOMPARE(reader.exitCode(), 0);
        QCOMPARE(QByteArray::fromBase64(reader.readAllStandardOutput().trimmed()), QByteArray(contentSize, contentByte));
        QCOMPARE(propertyRequests, 1);
        QCOMPARE(downloads, 1);
        QCOMPARE(unexpectedRequests, 0);
        QVERIFY(nativeCallbackTracked);
        QCOMPARE(ready.count(), 1);
        QCOMPARE(failed.count(), 0);
        QTRY_VERIFY(!vfs->hasPendingFileOperations());
        QVERIFY(!vfs->isDehydratedPlaceholder(folder.localPath() + fileName));
        QVERIFY(QFileInfo::exists(folder.localPath() + fileName));
    }

    void testActiveHydrationDuplicateCanBeCancelled()
    {
        FakeFolder folder{FileInfo{}};
        const auto vfs = setupVfs(folder);
        QVERIFY(vfs->isReady());
        folder.remoteModifier().insert("file", 32);
        QVERIFY(folder.syncOnce());
        CFVERIFY_VIRTUAL(folder, "file");
        auto propertyRequests = 0;
        auto downloads = 0;
        folder.setServerOverride([&](QNetworkAccessManager::Operation operation, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            if (operation == QNetworkAccessManager::GetOperation) {
                ++downloads;
            }
            if (request.attribute(QNetworkRequest::CustomVerbAttribute).toByteArray() == QByteArrayLiteral("PROPFIND")) {
                ++propertyRequests;
            }
            return nullptr;
        });
        QSignalSpy ready(vfs.data(), SIGNAL(hydrationRequestReady(QString)));
        QSignalSpy failed(vfs.data(), SIGNAL(hydrationRequestFailed(QString)));
        QSignalSpy finished(vfs.data(), SIGNAL(hydrationRequestFinished(QString)));
        QVERIFY(ready.isValid());
        QVERIFY(failed.isValid());
        QVERIFY(finished.isValid());
        const auto requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto filePath = QDir::toNativeSeparators(folder.localPath() + QStringLiteral("file"));
        QVERIFY(QMetaObject::invokeMethod(vfs.data(), "requestHydration", Qt::DirectConnection, Q_ARG(QString, requestId), Q_ARG(QString, filePath)));
        QTRY_COMPARE(ready.count(), 1);
        QVERIFY(vfs->isHydrating());
        const auto checksBeforeDuplicate = propertyRequests;
        QVERIFY(QMetaObject::invokeMethod(vfs.data(), "requestHydration", Qt::DirectConnection, Q_ARG(QString, requestId), Q_ARG(QString, filePath)));
        QCOMPARE(failed.count(), 1);
        QCOMPARE(ready.count(), 1);
        QCOMPARE(propertyRequests, checksBeforeDuplicate);
        QVERIFY(QMetaObject::invokeMethod(vfs.data(), "cancelHydration", Qt::DirectConnection, Q_ARG(QString, requestId), Q_ARG(QString, filePath)));
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(finished.at(0).at(0).toString(), requestId);
        QCOMPARE(downloads, 0);
        CFVERIFY_VIRTUAL(folder, "file");
        vfs->stop();
    }

    void testStoragePolicyHydrationDenial_data()
    {
        QTest::addColumn<QString>("scenario");
        QTest::newRow("external") << QStringLiteral("external");
        QTest::newRow("external-session") << QStringLiteral("external-session");
        QTest::newRow("missing mount type") << QStringLiteral("missing");
        QTest::newRow("canceled during check") << QStringLiteral("cancel");
        QTest::newRow("stopped during check") << QStringLiteral("stop");
        QTest::newRow("journal changed during check") << QStringLiteral("changed");
    }

    void testStoragePolicyHydrationDenial()
    {
        QFETCH(QString, scenario);
        FakeFolder folder{FileInfo{}};
        const auto vfs = setupVfs(folder);
        folder.remoteModifier().insert("file", 32);
        QVERIFY(folder.syncOnce());
        CFVERIFY_VIRTUAL(folder, "file");
        const auto originalRecord = dbRecord(folder, QStringLiteral("file"));

        if (scenario.startsWith(QStringLiteral("external"))) {
            folder.remoteModifier().find("file")->extraDavProperties = QStringLiteral("<nc:mount-type>%1</nc:mount-type>").arg(scenario).toUtf8();
        }

        auto propertyRequests = 0;
        auto completedChecks = 0;
        auto downloads = 0;
        folder.setServerOverride([&](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *outgoingData) -> QNetworkReply * {
            if (op == QNetworkAccessManager::GetOperation) {
                ++downloads;
            }
            if (request.attribute(QNetworkRequest::CustomVerbAttribute).toByteArray() != QByteArrayLiteral("PROPFIND")) {
                return nullptr;
            }
            ++propertyRequests;
            const auto requestedType = outgoingData && outgoingData->peek(outgoingData->bytesAvailable()).contains("mount-type");
            auto *reply = new FakePropfindReply(folder.remoteModifier(),
                                                op,
                                                request,
                                                folder.networkAccessManager(),
                                                requestedType && scenario != QStringLiteral("missing"));
            connect(reply, &QNetworkReply::finished, this, [&] {
                ++completedChecks;
            });
            return reply;
        });

        QSignalSpy ready(vfs.data(), SIGNAL(hydrationRequestReady(QString)));
        QSignalSpy failed(vfs.data(), SIGNAL(hydrationRequestFailed(QString)));
        QVERIFY(ready.isValid());
        QVERIFY(failed.isValid());
        const auto requestId = QStringLiteral("storage-policy-check");
        const auto filePath = QDir::toNativeSeparators(folder.localPath() + QStringLiteral("file"));
        QVERIFY(QMetaObject::invokeMethod(vfs.data(), "requestHydration", Qt::DirectConnection, Q_ARG(QString, requestId), Q_ARG(QString, filePath)));
        QCOMPARE(propertyRequests, 1);

        if (scenario == QStringLiteral("cancel")) {
            QVERIFY(QMetaObject::invokeMethod(vfs.data(), "cancelHydration", Qt::DirectConnection, Q_ARG(QString, requestId), Q_ARG(QString, filePath)));
        } else if (scenario == QStringLiteral("stop")) {
            vfs->stop();
        } else if (scenario == QStringLiteral("changed")) {
            auto changedRecord = originalRecord;
            changedRecord._etag = QByteArrayLiteral("changed-during-check");
            QVERIFY(folder.syncJournal().setFileRecord(changedRecord));
        }

        QTRY_COMPARE(completedChecks, 1);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(ready.count(), 0);
        QCOMPARE(downloads, 0);
        QVERIFY(!vfs->isHydrating());
        QCOMPARE(dbRecord(folder, QStringLiteral("file"))._type, ItemTypeVirtualFile);
        QCOMPARE(dbRecord(folder, QStringLiteral("file"))._fileId, originalRecord._fileId);
        QVERIFY(QFileInfo::exists(filePath));

        if (scenario == QStringLiteral("stop")) {
            QVERIFY(QMetaObject::invokeMethod(vfs.data(), "requestHydration", Qt::DirectConnection, Q_ARG(QString, requestId), Q_ARG(QString, filePath)));
            QCOMPARE(failed.count(), 2);
            QCOMPARE(propertyRequests, 1);
        }
    }

    void testVirtualFileLifecycle_data()
    {
        QTest::addColumn<bool>("doLocalDiscovery");

        QTest::newRow("full local discovery") << true;
        QTest::newRow("skip local discovery") << false;
    }

    void testReplaceFileByIdenticalFile()
    {
        FakeFolder fakeFolder{FileInfo{}};
        auto vfs = setupVfs(fakeFolder);
        ItemCompletedSpy completeSpy(fakeFolder);

        // Create a new local (non-placeholder) file
        fakeFolder.localModifier().insert("file0");
        fakeFolder.localModifier().insert("file1");
        CopyFile(QString(fakeFolder.localPath() + "file1").toStdWString().data(), QString(fakeFolder.localPath() + "file2").toStdWString().data(), false);
        QVERIFY(!vfs->pinState("file0").isValid());
        QVERIFY(!vfs->pinState("file1").isValid());
        QVERIFY(!vfs->pinState("file2").isValid());

        // Sync the files: files should be converted to placeholder files
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(vfs->pinState("file0").isValid());
        QVERIFY(vfs->pinState("file1").isValid());
        QVERIFY(vfs->pinState("file2").isValid());

        // Sync again to ensure items are fully synced, otherwise test may succeed due to those pending changes.
        QVERIFY(fakeFolder.syncOnce());
        completeSpy.clear();
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(completeSpy.isEmpty());

        // Replace file1 by identical file2: Windows will convert file1 to a regular (non-placeholder) file again.
        CopyFile(QString(fakeFolder.localPath() + "file2").toStdWString().data(), QString(fakeFolder.localPath() + "file1").toStdWString().data(), false);
        QVERIFY(vfs->pinState("file0").isValid());
        QVERIFY(!vfs->pinState("file1").isValid());
        QVERIFY(vfs->pinState("file2").isValid());

        // Sync again: file should be correctly converted to placeholders
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(vfs->pinState("file0").isValid());
        QVERIFY(vfs->pinState("file1").isValid());
        QVERIFY(vfs->pinState("file2").isValid());
    }

    void testReplaceOnlineOnlyFile()
    {
        FakeFolder fakeFolder{FileInfo{}};
        auto vfs = setupVfs(fakeFolder);

        // Create a new local (non-placeholder) file
        fakeFolder.localModifier().insert("file");
        QVERIFY(!vfs->pinState("file").isValid());

        CopyFile(QString(fakeFolder.localPath() + "file").toStdWString().data(), QString(fakeFolder.localPath() + "file1").toStdWString().data(), false);

        // Sync the files: files should be converted to placeholder files
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(vfs->pinState("file").isValid());

        // Convert to Online Only
        ::setPinState(fakeFolder.localPath() + "file", PinState::OnlineOnly, cfapi::Recurse);

        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(*vfs->pinState("file"), PinState::OnlineOnly);
        CFVERIFY_VIRTUAL(fakeFolder, "file");

        // Replace the file
        CopyFile(QString(fakeFolder.localPath() + "file1").toStdWString().data(), QString(fakeFolder.localPath() + "file").toStdWString().data(), false);

        // Sync again: file should be correctly dehydrated again without error.
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(vfs->pinState("file").isValid());
        QCOMPARE(*vfs->pinState("file"), PinState::OnlineOnly);
        CFVERIFY_VIRTUAL(fakeFolder, "file");
    }

    void testVirtualFileLifecycle()
    {
        QFETCH(bool, doLocalDiscovery);

        FakeFolder fakeFolder{ FileInfo() };
        setupVfs(fakeFolder);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        ItemCompletedSpy completeSpy(fakeFolder);

        auto cleanup = [&]() {
            completeSpy.clear();
            if (!doLocalDiscovery)
                fakeFolder.syncEngine().setLocalDiscoveryOptions(LocalDiscoveryStyle::DatabaseAndFilesystem);
        };
        cleanup();

        // Create a virtual file for a new remote file
        fakeFolder.remoteModifier().mkdir("A");
        fakeFolder.remoteModifier().insert("A/a1", 64);
        auto someDate = QDateTime(QDate(1984, 07, 30), QTime(1,3,2));
        fakeFolder.remoteModifier().setModTime("A/a1", someDate);
        QVERIFY(fakeFolder.syncOnce());
        QEXPECT_FAIL("", "folders on-demand breaks existing tests", Abort);
        CFVERIFY_VIRTUAL(fakeFolder, "A/a1");
        QCOMPARE(QFileInfo(fakeFolder.localPath() + "A/a1").size(), 64);
        QCOMPARE(QFileInfo(fakeFolder.localPath() + "A/a1").lastModified(), someDate);
        QVERIFY(fakeFolder.currentRemoteState().find("A/a1"));
        QVERIFY(itemInstruction(completeSpy, "A/a1", CSYNC_INSTRUCTION_NEW));
        cleanup();

        // Another sync doesn't actually lead to changes
        QVERIFY(fakeFolder.syncOnce());
        CFVERIFY_VIRTUAL(fakeFolder, "A/a1");
        QCOMPARE(QFileInfo(fakeFolder.localPath() + "A/a1").size(), 64);
        QCOMPARE(QFileInfo(fakeFolder.localPath() + "A/a1").lastModified(), someDate);
        QVERIFY(fakeFolder.currentRemoteState().find("A/a1"));
        QVERIFY(completeSpy.isEmpty());
        cleanup();

        // Not even when the remote is rediscovered
        fakeFolder.syncJournal().forceRemoteDiscoveryNextSync();
        QVERIFY(fakeFolder.syncOnce());
        CFVERIFY_VIRTUAL(fakeFolder, "A/a1");
        QCOMPARE(QFileInfo(fakeFolder.localPath() + "A/a1").size(), 64);
        QCOMPARE(QFileInfo(fakeFolder.localPath() + "A/a1").lastModified(), someDate);
        QVERIFY(fakeFolder.currentRemoteState().find("A/a1"));
        QVERIFY(completeSpy.isEmpty());
        cleanup();

        // Neither does a remote change
        fakeFolder.remoteModifier().appendByte("A/a1");
        QVERIFY(fakeFolder.syncOnce());
        CFVERIFY_VIRTUAL(fakeFolder, "A/a1");
        QCOMPARE(QFileInfo(fakeFolder.localPath() + "A/a1").size(), 65);
        QCOMPARE(QFileInfo(fakeFolder.localPath() + "A/a1").lastModified(), someDate);
        QVERIFY(fakeFolder.currentRemoteState().find("A/a1"));
        QVERIFY(itemInstruction(completeSpy, "A/a1", CSYNC_INSTRUCTION_UPDATE_METADATA));
        QCOMPARE(dbRecord(fakeFolder, "A/a1")._fileSize, 65);
        cleanup();

        // If the local virtual file is removed, this will be propagated remotely
        if (!doLocalDiscovery)
            fakeFolder.syncEngine().setLocalDiscoveryOptions(LocalDiscoveryStyle::DatabaseAndFilesystem, { "A" });
        fakeFolder.localModifier().remove("A/a1");
        QVERIFY(!fakeFolder.syncOnce());
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(!fakeFolder.currentLocalState().find("A/a1"));
        QVERIFY(!fakeFolder.currentRemoteState().find("A/a1"));
        QVERIFY(itemInstruction(completeSpy, "A/a1", CSYNC_INSTRUCTION_REMOVE));
        QVERIFY(!dbRecord(fakeFolder, "A/a1").isValid());
        cleanup();

        // Recreate a1 before carrying on with the other tests
        fakeFolder.remoteModifier().insert("A/a1", 65);
        fakeFolder.remoteModifier().setModTime("A/a1", someDate);
        QVERIFY(fakeFolder.syncOnce());
        CFVERIFY_VIRTUAL(fakeFolder, "A/a1");
        QCOMPARE(QFileInfo(fakeFolder.localPath() + "A/a1").size(), 65);
        QCOMPARE(QFileInfo(fakeFolder.localPath() + "A/a1").lastModified(), someDate);
        QVERIFY(fakeFolder.currentRemoteState().find("A/a1"));
        QVERIFY(itemInstruction(completeSpy, "A/a1", CSYNC_INSTRUCTION_NEW));
        cleanup();

        // Remote rename is propagated
        fakeFolder.remoteModifier().rename("A/a1", "A/a1m");
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(!QFileInfo(fakeFolder.localPath() + "A/a1").exists());
        CFVERIFY_VIRTUAL(fakeFolder, "A/a1m");
        QCOMPARE(QFileInfo(fakeFolder.localPath() + "A/a1m").size(), 65);
        QCOMPARE(QFileInfo(fakeFolder.localPath() + "A/a1m").lastModified(), someDate);
        QVERIFY(!fakeFolder.currentRemoteState().find("A/a1"));
        QVERIFY(fakeFolder.currentRemoteState().find("A/a1m"));
        QVERIFY(
            itemInstruction(completeSpy, "A/a1m", CSYNC_INSTRUCTION_RENAME)
            || (itemInstruction(completeSpy, "A/a1m", CSYNC_INSTRUCTION_NEW)
                && itemInstruction(completeSpy, "A/a1", CSYNC_INSTRUCTION_REMOVE)));
        QVERIFY(!dbRecord(fakeFolder, "A/a1").isValid());
        cleanup();

        // Remote remove is propagated
        fakeFolder.remoteModifier().remove("A/a1m");
        QVERIFY(!fakeFolder.syncOnce());
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(!QFileInfo(fakeFolder.localPath() + "A/a1m").exists());
        QVERIFY(!fakeFolder.currentRemoteState().find("A/a1m"));
        QVERIFY(itemInstruction(completeSpy, "A/a1m", CSYNC_INSTRUCTION_REMOVE));
        QVERIFY(!dbRecord(fakeFolder, "A/a1").isValid());
        QVERIFY(!dbRecord(fakeFolder, "A/a1m").isValid());
        cleanup();

        // Edge case: Local virtual file but no db entry for some reason
        fakeFolder.remoteModifier().insert("A/a2", 32);
        fakeFolder.remoteModifier().insert("A/a3", 33);
        QVERIFY(fakeFolder.syncOnce());
        CFVERIFY_VIRTUAL(fakeFolder, "A/a2");
        QCOMPARE(QFileInfo(fakeFolder.localPath() + "A/a2").size(), 32);
        CFVERIFY_VIRTUAL(fakeFolder, "A/a3");
        QCOMPARE(QFileInfo(fakeFolder.localPath() + "A/a3").size(), 33);
        cleanup();

        QVERIFY(fakeFolder.syncEngine().journal()->deleteFileRecord("A/a2"));
        QVERIFY(fakeFolder.syncEngine().journal()->deleteFileRecord("A/a3"));
        fakeFolder.remoteModifier().remove("A/a3");
        fakeFolder.syncEngine().setLocalDiscoveryOptions(LocalDiscoveryStyle::FilesystemOnly);
        QVERIFY(!fakeFolder.syncOnce());
        QVERIFY(fakeFolder.syncOnce());
        CFVERIFY_VIRTUAL(fakeFolder, "A/a2");
        QCOMPARE(QFileInfo(fakeFolder.localPath() + "A/a2").size(), 32);
        QVERIFY(itemInstruction(completeSpy, "A/a2", CSYNC_INSTRUCTION_UPDATE_METADATA));
        QVERIFY(!QFileInfo(fakeFolder.localPath() + "A/a3").exists());
        QVERIFY(itemInstruction(completeSpy, "A/a3", CSYNC_INSTRUCTION_REMOVE));
        QVERIFY(!dbRecord(fakeFolder, "A/a3").isValid());
        cleanup();
    }

    void testVirtualFileConflict()
    {
        FakeFolder fakeFolder{ FileInfo() };
        setupVfs(fakeFolder);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        ItemCompletedSpy completeSpy(fakeFolder);

        auto cleanup = [&]() {
            completeSpy.clear();
        };
        cleanup();

        // Create a virtual file for a new remote file
        fakeFolder.remoteModifier().mkdir("A");
        fakeFolder.remoteModifier().insert("A/a1", 11);
        fakeFolder.remoteModifier().insert("A/a2", 12);
        fakeFolder.remoteModifier().mkdir("B");
        fakeFolder.remoteModifier().insert("B/b1", 21);
        QVERIFY(fakeFolder.syncOnce());
        QEXPECT_FAIL("", "folders on-demand breaks existing tests", Abort);
        CFVERIFY_VIRTUAL(fakeFolder, "A/a1");
        CFVERIFY_VIRTUAL(fakeFolder, "A/a2");
        CFVERIFY_VIRTUAL(fakeFolder, "B/b1");
        QCOMPARE(QFileInfo(fakeFolder.localPath() + "A/a1").size(), 11);
        QCOMPARE(QFileInfo(fakeFolder.localPath() + "A/a2").size(), 12);
        QCOMPARE(QFileInfo(fakeFolder.localPath() + "B/b1").size(), 21);
        cleanup();

        // All the files are touched on the server
        fakeFolder.remoteModifier().appendByte("A/a1");
        fakeFolder.remoteModifier().appendByte("A/a2");
        fakeFolder.remoteModifier().appendByte("B/b1");

        // A: the correct file and a conflicting file are added
        // B: user adds a *directory* locally
        fakeFolder.localModifier().remove("A/a1");
        fakeFolder.localModifier().insert("A/a1", 12);
        fakeFolder.localModifier().remove("A/a2");
        fakeFolder.localModifier().insert("A/a2", 10);
        fakeFolder.localModifier().remove("B/b1");
        fakeFolder.localModifier().mkdir("B/b1");
        fakeFolder.localModifier().insert("B/b1/foo");
        QVERIFY(fakeFolder.syncOnce());

        // Everything is CONFLICT
        QVERIFY(itemInstruction(completeSpy, "A/a1", CSYNC_INSTRUCTION_CONFLICT));
        QVERIFY(itemInstruction(completeSpy, "A/a2", CSYNC_INSTRUCTION_CONFLICT));
        QVERIFY(itemInstruction(completeSpy, "B/b1", CSYNC_INSTRUCTION_CONFLICT));

        // conflict files should exist
        QCOMPARE(fakeFolder.syncJournal().conflictRecordPaths().size(), 2);

        // nothing should have the virtual file tag
        CFVERIFY_NONVIRTUAL(fakeFolder, "A/a1");
        CFVERIFY_NONVIRTUAL(fakeFolder, "A/a2");
        CFVERIFY_NONVIRTUAL(fakeFolder, "B/b1");

        cleanup();
    }

    void testStaleVirtualDirectoryWithoutDatabaseEntry()
    {
        FakeFolder fakeFolder{FileInfo{}};
        setupVfs(fakeFolder);
        ItemCompletedSpy completeSpy(fakeFolder);

        fakeFolder.remoteModifier().mkdir("stale");
        QVERIFY(fakeFolder.syncOnce());

        QCOMPARE(dbRecord(fakeFolder, "stale")._type, ItemTypeVirtualDirectory);
        QVERIFY(cfapi::findPlaceholderInfo(fakeFolder.localPath() + "stale"));

        QVERIFY(fakeFolder.syncJournal().deleteFileRecord("stale"));
        fakeFolder.remoteModifier().remove("stale");
        fakeFolder.syncEngine().setLocalDiscoveryOptions(LocalDiscoveryStyle::FilesystemOnly);
        completeSpy.clear();

        QVERIFY(fakeFolder.syncOnce());

        QCOMPARE(fakeFolder.remoteModifier().find("stale"), nullptr);
        QVERIFY(!QFileInfo(fakeFolder.localPath() + "stale").exists());
        QCOMPARE(completeSpy.findItem("stale")->_instruction, CSYNC_INSTRUCTION_REMOVE);
        QVERIFY(!dbRecord(fakeFolder, "stale").isValid());

        fakeFolder.localModifier().mkdir("new");
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(fakeFolder.remoteModifier().find("new"));
        QCOMPARE(completeSpy.findItem("new")->_instruction, CSYNC_INSTRUCTION_NEW);
    }

    void testOnlineOnlyVirtualDirectoryRename()
    {
        FakeFolder fakeFolder{FileInfo{}};
        auto vfs = setupVfs(fakeFolder);
        ItemCompletedSpy completeSpy(fakeFolder);

        fakeFolder.remoteModifier().mkdir("source");
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(dbRecord(fakeFolder, "source").isDirectory());

        ::setPinState(fakeFolder.localPath() + "source", PinState::OnlineOnly, cfapi::NoRecurse);
        fakeFolder.localModifier().rename("source", "destination");
        completeSpy.clear();

        QVERIFY(fakeFolder.syncOnce());

        QVERIFY(!fakeFolder.remoteModifier().find("source"));
        QVERIFY(fakeFolder.remoteModifier().find("destination"));
        QCOMPARE(completeSpy.findItem("destination")->_instruction, CSYNC_INSTRUCTION_RENAME);
        QCOMPARE(*vfs->pinState("destination"), PinState::OnlineOnly);
    }

    void testWithNormalSync()
    {
        FakeFolder fakeFolder{ FileInfo::A12_B12_C12_S12() };
        setupVfs(fakeFolder);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        ItemCompletedSpy completeSpy(fakeFolder);

        auto cleanup = [&]() {
            completeSpy.clear();
        };
        cleanup();

        // No effect sync
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        cleanup();

        // Existing files are propagated just fine in both directions
        fakeFolder.localModifier().appendByte("A/a1");
        fakeFolder.localModifier().insert("A/a3");
        fakeFolder.remoteModifier().appendByte("A/a2");
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        cleanup();

        // New files on the remote create virtual files
        fakeFolder.remoteModifier().insert("A/new", 42);
        QVERIFY(fakeFolder.syncOnce());
        CFVERIFY_VIRTUAL(fakeFolder, "A/new");
        QCOMPARE(QFileInfo(fakeFolder.localPath() + "A/new").size(), 42);
        QVERIFY(fakeFolder.currentRemoteState().find("A/new"));
        QVERIFY(itemInstruction(completeSpy, "A/new", CSYNC_INSTRUCTION_NEW));
        cleanup();
    }

    void testVirtualFileDownload()
    {
        FakeFolder fakeFolder{ FileInfo() };
        setupVfs(fakeFolder);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        ItemCompletedSpy completeSpy(fakeFolder);

        auto cleanup = [&]() {
            completeSpy.clear();
        };
        cleanup();

        // Create a virtual file for remote files
        fakeFolder.remoteModifier().mkdir("A");
        fakeFolder.remoteModifier().insert("A/a1");
        fakeFolder.remoteModifier().insert("A/a2");
        fakeFolder.remoteModifier().insert("A/a3");
        fakeFolder.remoteModifier().insert("A/a4");
        fakeFolder.remoteModifier().insert("A/a5");
        fakeFolder.remoteModifier().insert("A/a6");
        fakeFolder.remoteModifier().insert("A/a7");
        fakeFolder.remoteModifier().insert("A/b1");
        fakeFolder.remoteModifier().insert("A/b2");
        fakeFolder.remoteModifier().insert("A/b3");
        fakeFolder.remoteModifier().insert("A/b4");
        QVERIFY(fakeFolder.syncOnce());

        QEXPECT_FAIL("", "folders on-demand breaks existing tests", Abort);
        CFVERIFY_VIRTUAL(fakeFolder, "A/a1");
        CFVERIFY_VIRTUAL(fakeFolder, "A/a2");
        CFVERIFY_VIRTUAL(fakeFolder, "A/a3");
        CFVERIFY_VIRTUAL(fakeFolder, "A/a4");
        CFVERIFY_VIRTUAL(fakeFolder, "A/a5");
        CFVERIFY_VIRTUAL(fakeFolder, "A/a6");
        CFVERIFY_VIRTUAL(fakeFolder, "A/a7");
        CFVERIFY_VIRTUAL(fakeFolder, "A/b1");
        CFVERIFY_VIRTUAL(fakeFolder, "A/b2");
        CFVERIFY_VIRTUAL(fakeFolder, "A/b3");
        CFVERIFY_VIRTUAL(fakeFolder, "A/b4");

        cleanup();

        // Download by changing the db entry
        triggerDownload(fakeFolder, "A/a1");
        triggerDownload(fakeFolder, "A/a2");
        triggerDownload(fakeFolder, "A/a3");
        triggerDownload(fakeFolder, "A/a4");
        triggerDownload(fakeFolder, "A/a5");
        triggerDownload(fakeFolder, "A/a6");
        triggerDownload(fakeFolder, "A/a7");
        triggerDownload(fakeFolder, "A/b1");
        triggerDownload(fakeFolder, "A/b2");
        triggerDownload(fakeFolder, "A/b3");
        triggerDownload(fakeFolder, "A/b4");

        // Remote complications
        fakeFolder.remoteModifier().appendByte("A/a2");
        fakeFolder.remoteModifier().remove("A/a3");
        fakeFolder.remoteModifier().rename("A/a4", "A/a4m");
        fakeFolder.remoteModifier().appendByte("A/b2");
        fakeFolder.remoteModifier().remove("A/b3");
        fakeFolder.remoteModifier().rename("A/b4", "A/b4m");

        // Local complications
        fakeFolder.localModifier().remove("A/a5");
        fakeFolder.localModifier().insert("A/a5");
        fakeFolder.localModifier().remove("A/a6");
        fakeFolder.localModifier().insert("A/a6");

        QVERIFY(!fakeFolder.syncOnce());
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(itemInstruction(completeSpy, "A/a1", CSYNC_INSTRUCTION_SYNC));
        QCOMPARE(completeSpy.findItem("A/a1")->_type, ItemTypeVirtualFileDownload);
        QVERIFY(itemInstruction(completeSpy, "A/a2", CSYNC_INSTRUCTION_SYNC));
        QCOMPARE(completeSpy.findItem("A/a2")->_type, ItemTypeVirtualFileDownload);
        QVERIFY(itemInstruction(completeSpy, "A/a3", CSYNC_INSTRUCTION_REMOVE));
        QVERIFY(itemInstruction(completeSpy, "A/a4m", CSYNC_INSTRUCTION_NEW));
        QVERIFY(itemInstruction(completeSpy, "A/a4", CSYNC_INSTRUCTION_REMOVE));
        QVERIFY(itemInstruction(completeSpy, "A/a5", CSYNC_INSTRUCTION_CONFLICT));
        QVERIFY(itemInstruction(completeSpy, "A/a6", CSYNC_INSTRUCTION_CONFLICT));
        QVERIFY(itemInstruction(completeSpy, "A/a7", CSYNC_INSTRUCTION_SYNC));
        QVERIFY(itemInstruction(completeSpy, "A/b1", CSYNC_INSTRUCTION_SYNC));
        QVERIFY(itemInstruction(completeSpy, "A/b2", CSYNC_INSTRUCTION_SYNC));
        QVERIFY(itemInstruction(completeSpy, "A/b3", CSYNC_INSTRUCTION_REMOVE));
        QVERIFY(itemInstruction(completeSpy, "A/b4m", CSYNC_INSTRUCTION_NEW));
        QVERIFY(itemInstruction(completeSpy, "A/b4", CSYNC_INSTRUCTION_REMOVE));

        CFVERIFY_NONVIRTUAL(fakeFolder, "A/a1");
        CFVERIFY_NONVIRTUAL(fakeFolder, "A/a2");
        CFVERIFY_GONE(fakeFolder, "A/a3");
        CFVERIFY_GONE(fakeFolder, "A/a4");
        CFVERIFY_NONVIRTUAL(fakeFolder, "A/a4m");
        CFVERIFY_NONVIRTUAL(fakeFolder, "A/a5");
        CFVERIFY_NONVIRTUAL(fakeFolder, "A/a6");
        CFVERIFY_NONVIRTUAL(fakeFolder, "A/a7");
        CFVERIFY_NONVIRTUAL(fakeFolder, "A/b1");
        CFVERIFY_NONVIRTUAL(fakeFolder, "A/b2");
        CFVERIFY_GONE(fakeFolder, "A/b3");
        CFVERIFY_GONE(fakeFolder, "A/b4");
        CFVERIFY_NONVIRTUAL(fakeFolder, "A/b4m");

        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
    }

    void testVirtualFileDownloadResume()
    {
        FakeFolder fakeFolder{ FileInfo() };
        setupVfs(fakeFolder);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        ItemCompletedSpy completeSpy(fakeFolder);

        auto cleanup = [&]() {
            completeSpy.clear();
            QVERIFY(fakeFolder.syncJournal().wipeErrorBlacklist() != -1);
        };
        cleanup();

        // Create a virtual file for remote files
        fakeFolder.remoteModifier().mkdir("A");
        fakeFolder.remoteModifier().insert("A/a1");
        QVERIFY(fakeFolder.syncOnce());
        QEXPECT_FAIL("", "folders on-demand breaks existing tests", Abort);
        CFVERIFY_VIRTUAL(fakeFolder, "A/a1");
        cleanup();

        // Download by changing the db entry
        triggerDownload(fakeFolder, "A/a1");
        fakeFolder.serverErrorPaths().append("A/a1", 500);
        QVERIFY(!fakeFolder.syncOnce());
        QVERIFY(itemInstruction(completeSpy, "A/a1", CSYNC_INSTRUCTION_SYNC));
        QVERIFY(cfapi::isSparseFile(fakeFolder.localPath() + "A/a1"));
        QVERIFY(QFileInfo(fakeFolder.localPath() + "A/a1").exists());
        QCOMPARE(dbRecord(fakeFolder, "A/a1")._type, ItemTypeVirtualFileDownload);
        cleanup();

        fakeFolder.serverErrorPaths().clear();
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(itemInstruction(completeSpy, "A/a1", CSYNC_INSTRUCTION_SYNC));
        CFVERIFY_NONVIRTUAL(fakeFolder, "A/a1");
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
    }

    void testNewFilesNotVirtual()
    {
        FakeFolder fakeFolder{ FileInfo() };
        setupVfs(fakeFolder);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        fakeFolder.remoteModifier().mkdir("A");
        fakeFolder.remoteModifier().insert("A/a1");
        QVERIFY(fakeFolder.syncOnce());
        QEXPECT_FAIL("", "folders on-demand breaks existing tests", Abort);
        CFVERIFY_VIRTUAL(fakeFolder, "A/a1");

        ::setPinState(fakeFolder.localPath(), PinState::AlwaysLocal, cfapi::NoRecurse);

        // Create a new remote file, it'll not be virtual
        fakeFolder.remoteModifier().insert("A/a2");
        QVERIFY(fakeFolder.syncOnce());

        CFVERIFY_NONVIRTUAL(fakeFolder, "A/a2");
    }

    void testDownloadRecursive()
    {
        FakeFolder fakeFolder{ FileInfo() };
        setupVfs(fakeFolder);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        // Create a virtual file for remote files
        fakeFolder.remoteModifier().mkdir("A");
        fakeFolder.remoteModifier().mkdir("A/Sub");
        fakeFolder.remoteModifier().mkdir("A/Sub/SubSub");
        fakeFolder.remoteModifier().mkdir("A/Sub2");
        fakeFolder.remoteModifier().mkdir("B");
        fakeFolder.remoteModifier().mkdir("B/Sub");
        fakeFolder.remoteModifier().insert("A/a1");
        fakeFolder.remoteModifier().insert("A/a2");
        fakeFolder.remoteModifier().insert("A/Sub/a3");
        fakeFolder.remoteModifier().insert("A/Sub/a4");
        fakeFolder.remoteModifier().insert("A/Sub/SubSub/a5");
        fakeFolder.remoteModifier().insert("A/Sub2/a6");
        fakeFolder.remoteModifier().insert("B/b1");
        fakeFolder.remoteModifier().insert("B/Sub/b2");
        QVERIFY(fakeFolder.syncOnce());

        QEXPECT_FAIL("", "folders on-demand breaks existing tests", Abort);
        CFVERIFY_VIRTUAL(fakeFolder, "A/a1");
        CFVERIFY_VIRTUAL(fakeFolder, "A/a2");
        CFVERIFY_VIRTUAL(fakeFolder, "A/Sub/a3");
        CFVERIFY_VIRTUAL(fakeFolder, "A/Sub/a4");
        CFVERIFY_VIRTUAL(fakeFolder, "A/Sub/SubSub/a5");
        CFVERIFY_VIRTUAL(fakeFolder, "A/Sub2/a6");
        CFVERIFY_VIRTUAL(fakeFolder, "B/b1");
        CFVERIFY_VIRTUAL(fakeFolder, "B/Sub/b2");

        // Download All file in the directory A/Sub
        // (as in Folder::downloadVirtualFile)
        fakeFolder.syncJournal().markVirtualFileForDownloadRecursively("A/Sub");

        QVERIFY(fakeFolder.syncOnce());

        CFVERIFY_VIRTUAL(fakeFolder, "A/a1");
        CFVERIFY_VIRTUAL(fakeFolder, "A/a2");
        CFVERIFY_NONVIRTUAL(fakeFolder, "A/Sub/a3");
        CFVERIFY_NONVIRTUAL(fakeFolder, "A/Sub/a4");
        CFVERIFY_NONVIRTUAL(fakeFolder, "A/Sub/SubSub/a5");
        CFVERIFY_VIRTUAL(fakeFolder, "A/Sub2/a6");
        CFVERIFY_VIRTUAL(fakeFolder, "B/b1");
        CFVERIFY_VIRTUAL(fakeFolder, "B/Sub/b2");

        // Add a file in a subfolder that was downloaded
        // Currently, this continue to add it as a virtual file.
        fakeFolder.remoteModifier().insert("A/Sub/SubSub/a7");
        QVERIFY(fakeFolder.syncOnce());

        CFVERIFY_VIRTUAL(fakeFolder, "A/Sub/SubSub/a7");

        // Now download all files in "A"
        fakeFolder.syncJournal().markVirtualFileForDownloadRecursively("A");
        QVERIFY(fakeFolder.syncOnce());

        CFVERIFY_NONVIRTUAL(fakeFolder, "A/a1");
        CFVERIFY_NONVIRTUAL(fakeFolder, "A/a2");
        CFVERIFY_NONVIRTUAL(fakeFolder, "A/Sub/a3");
        CFVERIFY_NONVIRTUAL(fakeFolder, "A/Sub/a4");
        CFVERIFY_NONVIRTUAL(fakeFolder, "A/Sub/SubSub/a5");
        CFVERIFY_NONVIRTUAL(fakeFolder, "A/Sub/SubSub/a7");
        CFVERIFY_NONVIRTUAL(fakeFolder, "A/Sub2/a6");
        CFVERIFY_VIRTUAL(fakeFolder, "B/b1");
        CFVERIFY_VIRTUAL(fakeFolder, "B/Sub/b2");

        // Now download remaining files in "B"
        fakeFolder.syncJournal().markVirtualFileForDownloadRecursively("B");
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
    }

    void testRenameVirtual()
    {
        FakeFolder fakeFolder{ FileInfo() };
        setupVfs(fakeFolder);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        ItemCompletedSpy completeSpy(fakeFolder);

        auto cleanup = [&]() {
            completeSpy.clear();
        };
        cleanup();

        fakeFolder.remoteModifier().insert("file1", 128, 'C');
        fakeFolder.remoteModifier().insert("file2", 256, 'C');
        fakeFolder.remoteModifier().insert("file3", 256, 'C');
        QVERIFY(fakeFolder.syncOnce());

        CFVERIFY_VIRTUAL(fakeFolder, "file1");
        CFVERIFY_VIRTUAL(fakeFolder, "file2");
        CFVERIFY_VIRTUAL(fakeFolder, "file3");

        cleanup();

        fakeFolder.localModifier().rename("file1", "renamed1");
        fakeFolder.localModifier().rename("file2", "renamed2");
        triggerDownload(fakeFolder, "file2");
        triggerDownload(fakeFolder, "file3");
        QVERIFY(fakeFolder.syncOnce());

        CFVERIFY_GONE(fakeFolder, "file1");
        CFVERIFY_VIRTUAL(fakeFolder, "renamed1");

        QVERIFY(fakeFolder.currentRemoteState().find("renamed1"));
        QVERIFY(itemInstruction(completeSpy, "renamed1", CSYNC_INSTRUCTION_RENAME));

        // file2 has a conflict between the download request and the rename:
        // the rename wins, the download is ignored

        CFVERIFY_GONE(fakeFolder, "file2");
        CFVERIFY_VIRTUAL(fakeFolder, "renamed2");

        QVERIFY(fakeFolder.currentRemoteState().find("renamed2"));
        QVERIFY(itemInstruction(completeSpy, "renamed2", CSYNC_INSTRUCTION_RENAME));

        QVERIFY(itemInstruction(completeSpy, "file3", CSYNC_INSTRUCTION_SYNC));
        CFVERIFY_NONVIRTUAL(fakeFolder, "file3");
        cleanup();
    }

    void testRenameVirtual2()
    {
        FakeFolder fakeFolder{ FileInfo() };
        setupVfs(fakeFolder);
        ItemCompletedSpy completeSpy(fakeFolder);
        auto cleanup = [&]() {
            completeSpy.clear();
        };
        cleanup();

        fakeFolder.remoteModifier().insert("case3", 128, 'C');
        fakeFolder.remoteModifier().insert("case4", 256, 'C');
        QVERIFY(fakeFolder.syncOnce());

        triggerDownload(fakeFolder, "case4");
        QVERIFY(fakeFolder.syncOnce());

        CFVERIFY_VIRTUAL(fakeFolder, "case3");
        CFVERIFY_NONVIRTUAL(fakeFolder, "case4");

        cleanup();

        // Case 1: non-virtual, foo -> bar (tested elsewhere)
        // Case 2: virtual, foo -> bar (tested elsewhere)

        // Case 3: virtual, foo.oc -> bar.oc (db hydrate)
        fakeFolder.localModifier().rename("case3", "case3-rename");
        triggerDownload(fakeFolder, "case3");

        // Case 4: non-virtual foo -> bar (db dehydrate)
        fakeFolder.localModifier().rename("case4", "case4-rename");
        markForDehydration(fakeFolder, "case4");

        QVERIFY(fakeFolder.syncOnce());

        // Case 3: the rename went though, hydration is forgotten
        CFVERIFY_GONE(fakeFolder, "case3");
        CFVERIFY_VIRTUAL(fakeFolder, "case3-rename");
        QVERIFY(!fakeFolder.currentRemoteState().find("case3"));
        QVERIFY(fakeFolder.currentRemoteState().find("case3-rename"));
        QVERIFY(itemInstruction(completeSpy, "case3-rename", CSYNC_INSTRUCTION_RENAME));

        // Case 4: the rename went though, dehydration is forgotten
        CFVERIFY_GONE(fakeFolder, "case4");
        CFVERIFY_NONVIRTUAL(fakeFolder, "case4-rename");
        QVERIFY(!fakeFolder.currentRemoteState().find("case4"));
        QVERIFY(fakeFolder.currentRemoteState().find("case4-rename"));
        QVERIFY(itemInstruction(completeSpy, "case4-rename", CSYNC_INSTRUCTION_RENAME));
    }

    // Dehydration via sync works
    void testSyncDehydration()
    {
        FakeFolder fakeFolder{ FileInfo::A12_B12_C12_S12() };
        // empty files would not work, so, we're gonna remove and re-insert them with 1MB data
        fakeFolder.remoteModifier().remove("A/a1");
        fakeFolder.remoteModifier().remove("A/a2");
        fakeFolder.remoteModifier().insert("A/a1", 1024 * 1024);
        fakeFolder.remoteModifier().insert("A/a2", 1024 * 1024);
        setupVfs(fakeFolder);

        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        ItemCompletedSpy completeSpy(fakeFolder);
        auto cleanup = [&]() {
            completeSpy.clear();
        };
        cleanup();

        //
        // Mark for dehydration and check
        //

        QVERIFY(fakeFolder.currentLocalState().find("A/a1"));

        markForDehydration(fakeFolder, "A/a1");

        markForDehydration(fakeFolder, "A/a2");
        fakeFolder.remoteModifier().appendByte("A/a2");
        // expect: normal dehydration

        markForDehydration(fakeFolder, "B/b1");
        fakeFolder.remoteModifier().remove("B/b1");
        // expect: local removal

        markForDehydration(fakeFolder, "B/b2");
        fakeFolder.remoteModifier().rename("B/b2", "B/b3");
        // expect: B/b2 is gone, B/b3 is NEW placeholder

        markForDehydration(fakeFolder, "C/c1");
        fakeFolder.localModifier().appendByte("C/c1");
        // expect: no dehydration, upload of c1

        markForDehydration(fakeFolder, "C/c2");
        fakeFolder.localModifier().appendByte("C/c2");
        fakeFolder.remoteModifier().appendByte("C/c2");
        fakeFolder.remoteModifier().appendByte("C/c2");
        // expect: no dehydration, conflict

        QVERIFY(fakeFolder.syncOnce());

        auto isDehydrated = [&](const QString &path) {
            return cfapi::isSparseFile(fakeFolder.localPath() + path)
                && QFileInfo(fakeFolder.localPath() + path).exists();
        };
        auto hasDehydratedDbEntries = [&](const QString &path) {
            SyncJournalFileRecord rec;
            return fakeFolder.syncJournal().getFileRecord(path, &rec) && rec.isValid() && rec._type == ItemTypeVirtualFile;
        };

        QVERIFY(isDehydrated("A/a1"));
        QVERIFY(hasDehydratedDbEntries("A/a1"));
        QVERIFY(itemInstruction(completeSpy, "A/a1", CSYNC_INSTRUCTION_SYNC));
        QCOMPARE(completeSpy.findItem("A/a1")->_type, ItemTypeVirtualFileDehydration);
        QCOMPARE(completeSpy.findItem("A/a1")->_file, QStringLiteral("A/a1"));
        QVERIFY(isDehydrated("A/a2"));
        QVERIFY(hasDehydratedDbEntries("A/a2"));
        QVERIFY(itemInstruction(completeSpy, "A/a2", CSYNC_INSTRUCTION_SYNC));
        QCOMPARE(completeSpy.findItem("A/a2")->_type, ItemTypeVirtualFileDehydration);

        QVERIFY(!QFileInfo(fakeFolder.localPath() + "B/b1").exists());
        QVERIFY(!fakeFolder.currentRemoteState().find("B/b1"));
        QVERIFY(itemInstruction(completeSpy, "B/b1", CSYNC_INSTRUCTION_REMOVE));

        QVERIFY(!QFileInfo(fakeFolder.localPath() + "B/b2").exists());
        QVERIFY(!fakeFolder.currentRemoteState().find("B/b2"));
        QVERIFY(isDehydrated("B/b3"));
        QVERIFY(hasDehydratedDbEntries("B/b3"));
        QVERIFY(itemInstruction(completeSpy, "B/b2", CSYNC_INSTRUCTION_REMOVE));
        QVERIFY(itemInstruction(completeSpy, "B/b3", CSYNC_INSTRUCTION_NEW));

        QCOMPARE(fakeFolder.currentRemoteState().find("C/c1")->size, 25);
        QVERIFY(itemInstruction(completeSpy, "C/c1", CSYNC_INSTRUCTION_SYNC));

        QCOMPARE(fakeFolder.currentRemoteState().find("C/c2")->size, 26);
        QVERIFY(itemInstruction(completeSpy, "C/c2", CSYNC_INSTRUCTION_CONFLICT));
        cleanup();

        auto expectedRemoteState = fakeFolder.currentRemoteState();
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentRemoteState(), expectedRemoteState);

        QVERIFY(isDehydrated("A/a1"));
        QVERIFY(hasDehydratedDbEntries("A/a1"));
        QVERIFY(isDehydrated("A/a2"));
        QVERIFY(hasDehydratedDbEntries("A/a2"));

        QVERIFY(!QFileInfo(fakeFolder.localPath() + "B/b1").exists());
        QVERIFY(!QFileInfo(fakeFolder.localPath() + "B/b2").exists());
        QVERIFY(isDehydrated("B/b3"));
        QVERIFY(hasDehydratedDbEntries("B/b3"));

        QVERIFY(QFileInfo(fakeFolder.localPath() + "C/c1").exists());
        QVERIFY(dbRecord(fakeFolder, "C/c1").isValid());
        QVERIFY(!isDehydrated("C/c1"));
        QVERIFY(!hasDehydratedDbEntries("C/c1"));

        QVERIFY(QFileInfo(fakeFolder.localPath() + "C/c2").exists());
        QVERIFY(dbRecord(fakeFolder, "C/c2").isValid());
        QVERIFY(!isDehydrated("C/c2"));
        QVERIFY(!hasDehydratedDbEntries("C/c2"));
    }

    void testNewVirtuals()
    {
        FakeFolder fakeFolder{ FileInfo() };
        setupVfs(fakeFolder);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        fakeFolder.remoteModifier().mkdir("local");
        fakeFolder.remoteModifier().mkdir("online");
        fakeFolder.remoteModifier().mkdir("unspec");
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        ::setPinState(fakeFolder.localPath() + "local", PinState::AlwaysLocal, cfapi::Recurse);
        ::setPinState(fakeFolder.localPath() + "online", PinState::OnlineOnly, cfapi::Recurse);
        ::setPinState(fakeFolder.localPath() + "unspec", PinState::Unspecified, cfapi::Recurse);

        // Test 1: root is Unspecified
        fakeFolder.remoteModifier().insert("file1", 1024 * 1024);
        fakeFolder.remoteModifier().insert("online/file1", 1024 * 1024);
        fakeFolder.remoteModifier().insert("local/file1", 1024 * 1024);
        fakeFolder.remoteModifier().insert("unspec/file1", 1024 * 1024);
        QVERIFY(fakeFolder.syncOnce());

        CFVERIFY_VIRTUAL(fakeFolder, "file1");
        CFVERIFY_VIRTUAL(fakeFolder, "online/file1");
        CFVERIFY_NONVIRTUAL(fakeFolder, "local/file1");
        CFVERIFY_VIRTUAL(fakeFolder, "unspec/file1");

        // Test 2: change root to AlwaysLocal
        ::setPinState(fakeFolder.localPath(), PinState::AlwaysLocal, cfapi::Recurse);
        // Need to force pin state for the subfolders again
        ::setPinState(fakeFolder.localPath() + "local", PinState::AlwaysLocal, cfapi::Recurse);
        ::setPinState(fakeFolder.localPath() + "online", PinState::OnlineOnly, cfapi::Recurse);
        ::setPinState(fakeFolder.localPath() + "unspec", PinState::Unspecified, cfapi::Recurse);

        fakeFolder.remoteModifier().insert("file2", 1024 * 1024);
        fakeFolder.remoteModifier().insert("online/file2", 1024 * 1024);
        fakeFolder.remoteModifier().insert("local/file2", 1024 * 1024);
        fakeFolder.remoteModifier().insert("unspec/file2", 1024 * 1024);
        QVERIFY(fakeFolder.syncOnce());

        CFVERIFY_NONVIRTUAL(fakeFolder, "file2");
        CFVERIFY_VIRTUAL(fakeFolder, "online/file2");
        CFVERIFY_NONVIRTUAL(fakeFolder, "local/file2");
        CFVERIFY_VIRTUAL(fakeFolder, "unspec/file2");

        // root file1 was hydrated due to its new pin state
        CFVERIFY_NONVIRTUAL(fakeFolder, "file1");

        // file1 is unchanged in the explicitly pinned subfolders
        CFVERIFY_VIRTUAL(fakeFolder, "online/file1");
        CFVERIFY_NONVIRTUAL(fakeFolder, "local/file1");
        CFVERIFY_VIRTUAL(fakeFolder, "unspec/file1");

        // Test 3: change root to OnlineOnly
        ::setPinState(fakeFolder.localPath(), PinState::OnlineOnly, cfapi::Recurse);
        // Need to force pin state for the subfolders again
        ::setPinState(fakeFolder.localPath() + "local", PinState::AlwaysLocal, cfapi::Recurse);
        ::setPinState(fakeFolder.localPath() + "online", PinState::OnlineOnly, cfapi::Recurse);
        ::setPinState(fakeFolder.localPath() + "unspec", PinState::Unspecified, cfapi::Recurse);

        fakeFolder.remoteModifier().insert("file3", 1024 * 1024);
        fakeFolder.remoteModifier().insert("online/file3", 1024 * 1024);
        fakeFolder.remoteModifier().insert("local/file3", 1024 * 1024);
        fakeFolder.remoteModifier().insert("unspec/file3", 1024 * 1024);
        QVERIFY(fakeFolder.syncOnce());

        CFVERIFY_VIRTUAL(fakeFolder, "file3");
        CFVERIFY_VIRTUAL(fakeFolder, "online/file3");
        CFVERIFY_NONVIRTUAL(fakeFolder, "local/file3");
        CFVERIFY_VIRTUAL(fakeFolder, "unspec/file3");

        // root file1 was dehydrated due to its new pin state
        CFVERIFY_VIRTUAL(fakeFolder, "file1");

        // file1 is unchanged in the explicitly pinned subfolders
        CFVERIFY_VIRTUAL(fakeFolder, "online/file1");
        CFVERIFY_NONVIRTUAL(fakeFolder, "local/file1");
        CFVERIFY_VIRTUAL(fakeFolder, "unspec/file1");
    }

    void testAvailability()
    {
        FakeFolder fakeFolder{ FileInfo() };
        auto vfs = setupVfs(fakeFolder);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        fakeFolder.remoteModifier().mkdir("local");
        fakeFolder.remoteModifier().mkdir("local/sub");
        fakeFolder.remoteModifier().mkdir("online");
        fakeFolder.remoteModifier().mkdir("online/sub");
        fakeFolder.remoteModifier().mkdir("unspec");
        QVERIFY(fakeFolder.syncOnce());
        QEXPECT_FAIL("", "folders on-demand breaks existing tests", Abort);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        ::setPinState(fakeFolder.localPath() + "local", PinState::AlwaysLocal, cfapi::Recurse);
        ::setPinState(fakeFolder.localPath() + "online", PinState::OnlineOnly, cfapi::Recurse);
        ::setPinState(fakeFolder.localPath() + "unspec", PinState::Unspecified, cfapi::Recurse);

        fakeFolder.remoteModifier().insert("file1", 1024 * 1024);
        fakeFolder.remoteModifier().insert("online/file1", 1024 * 1024);
        fakeFolder.remoteModifier().insert("online/file2", 1024 * 1024);
        fakeFolder.remoteModifier().insert("local/file1", 1024 * 1024);
        fakeFolder.remoteModifier().insert("local/file2", 1024 * 1024);
        fakeFolder.remoteModifier().insert("unspec/file1", 1024 * 1024);
        QVERIFY(fakeFolder.syncOnce());

        // root is unspecified
        QCOMPARE(*vfs->availability("file1", Vfs::AvailabilityRecursivity::RecursiveAvailability), VfsItemAvailability::AllDehydrated);
        QCOMPARE(*vfs->availability("local", Vfs::AvailabilityRecursivity::RecursiveAvailability), VfsItemAvailability::AlwaysLocal);
        QCOMPARE(*vfs->availability("local/file1", Vfs::AvailabilityRecursivity::RecursiveAvailability), VfsItemAvailability::AlwaysLocal);
        QCOMPARE(*vfs->availability("online", Vfs::AvailabilityRecursivity::RecursiveAvailability), VfsItemAvailability::OnlineOnly);
        QCOMPARE(*vfs->availability("online/file1", Vfs::AvailabilityRecursivity::RecursiveAvailability), VfsItemAvailability::OnlineOnly);
        QCOMPARE(*vfs->availability("unspec", Vfs::AvailabilityRecursivity::RecursiveAvailability), VfsItemAvailability::AllDehydrated);
        QCOMPARE(*vfs->availability("unspec/file1", Vfs::AvailabilityRecursivity::RecursiveAvailability), VfsItemAvailability::AllDehydrated);

        // Subitem pin states can ruin "pure" availabilities
        ::setPinState(fakeFolder.localPath() + "local/sub", PinState::OnlineOnly, cfapi::NoRecurse);
        QCOMPARE(*vfs->availability("local", Vfs::AvailabilityRecursivity::RecursiveAvailability), VfsItemAvailability::AllHydrated);
        ::setPinState(fakeFolder.localPath() + "online/sub", PinState::Unspecified, cfapi::NoRecurse);
        QCOMPARE(*vfs->availability("online", Vfs::AvailabilityRecursivity::RecursiveAvailability), VfsItemAvailability::AllDehydrated);

        triggerDownload(fakeFolder, "unspec/file1");
        ::setPinState(fakeFolder.localPath() + "local/file2", PinState::OnlineOnly, cfapi::NoRecurse);
        ::setPinState(fakeFolder.localPath() + "online/file2", PinState::AlwaysLocal, cfapi::NoRecurse);
        QVERIFY(fakeFolder.syncOnce());

        QCOMPARE(*vfs->availability("unspec", Vfs::AvailabilityRecursivity::RecursiveAvailability), VfsItemAvailability::AllHydrated);
        QCOMPARE(*vfs->availability("local", Vfs::AvailabilityRecursivity::RecursiveAvailability), VfsItemAvailability::Mixed);
        QCOMPARE(*vfs->availability("online", Vfs::AvailabilityRecursivity::RecursiveAvailability), VfsItemAvailability::Mixed);

        QVERIFY(vfs->setPinState("local", PinState::AlwaysLocal));
        QVERIFY(vfs->setPinState("online", PinState::OnlineOnly));
        QVERIFY(fakeFolder.syncOnce());

        QCOMPARE(*vfs->availability("online", Vfs::AvailabilityRecursivity::RecursiveAvailability), VfsItemAvailability::OnlineOnly);
        QCOMPARE(*vfs->availability("local", Vfs::AvailabilityRecursivity::RecursiveAvailability), VfsItemAvailability::AlwaysLocal);

        auto r = vfs->availability("nonexistent", Vfs::AvailabilityRecursivity::RecursiveAvailability);
        QVERIFY(!r);
        QCOMPARE(r.error(), Vfs::AvailabilityError::NoSuchItem);
    }

    void testPinStateLocals()
    {
        FakeFolder fakeFolder{ FileInfo() };
        auto vfs = setupVfs(fakeFolder);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        fakeFolder.remoteModifier().mkdir("local");
        fakeFolder.remoteModifier().mkdir("online");
        fakeFolder.remoteModifier().mkdir("unspec");
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        ::setPinState(fakeFolder.localPath() + "local", PinState::AlwaysLocal, cfapi::NoRecurse);
        ::setPinState(fakeFolder.localPath() + "online", PinState::OnlineOnly, cfapi::NoRecurse);
        ::setPinState(fakeFolder.localPath() + "unspec", PinState::Unspecified, cfapi::NoRecurse);

        fakeFolder.localModifier().insert("file1", 1024 * 1024);
        fakeFolder.localModifier().insert("online/file1", 1024 * 1024);
        fakeFolder.localModifier().insert("online/file2", 1024 * 1024);
        fakeFolder.localModifier().insert("local/file1", 1024 * 1024);
        fakeFolder.localModifier().insert("unspec/file1", 1024 * 1024);
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        // root is unspecified
        QCOMPARE(*vfs->pinState("file1"), PinState::Unspecified);
        QCOMPARE(*vfs->pinState("local/file1"), PinState::AlwaysLocal);
        QCOMPARE(*vfs->pinState("online/file1"), PinState::Unspecified);
        QCOMPARE(*vfs->pinState("unspec/file1"), PinState::Unspecified);

        // Sync again: bad pin states of new local files usually take effect on second sync
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        // When a file in an online-only folder is renamed, it retains its pin
        fakeFolder.localModifier().rename("online/file1", "online/file1rename");
        fakeFolder.remoteModifier().rename("online/file2", "online/file2rename");
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(*vfs->pinState("online/file1rename"), PinState::Unspecified);
        QCOMPARE(*vfs->pinState("online/file2rename"), PinState::Unspecified);

        // When a folder is renamed, the pin states inside should be retained
        fakeFolder.localModifier().rename("online", "onlinerenamed1");
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(*vfs->pinState("onlinerenamed1"), PinState::OnlineOnly);
        QCOMPARE(*vfs->pinState("onlinerenamed1/file1rename"), PinState::Unspecified);

        fakeFolder.remoteModifier().rename("onlinerenamed1", "onlinerenamed2");
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(*vfs->pinState("onlinerenamed2"), PinState::OnlineOnly);
        QCOMPARE(*vfs->pinState("onlinerenamed2/file1rename"), PinState::Unspecified);

        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        // When a file is deleted and later a new file has the same name, the old pin
        // state isn't preserved.
        QCOMPARE(*vfs->pinState("onlinerenamed2/file1rename"), PinState::Unspecified);
        fakeFolder.remoteModifier().remove("onlinerenamed2/file1rename");
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(!vfs->pinState("onlinerenamed2/file1rename"));
        fakeFolder.remoteModifier().insert("onlinerenamed2/file1rename", 1024 * 1024);
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(*vfs->pinState("onlinerenamed2/file1rename"), PinState::OnlineOnly);

        // When a file is hydrated or dehydrated due to pin state it retains its pin state
        QVERIFY(vfs->setPinState("onlinerenamed2/file1rename", PinState::AlwaysLocal));
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(fakeFolder.currentLocalState().find("onlinerenamed2/file1rename"));
        QCOMPARE(*vfs->pinState("onlinerenamed2/file1rename"), PinState::AlwaysLocal);

        QVERIFY(vfs->setPinState("onlinerenamed2", PinState::Unspecified));
        QVERIFY(vfs->setPinState("onlinerenamed2/file1rename", PinState::OnlineOnly));
        QVERIFY(fakeFolder.syncOnce());

        CFVERIFY_VIRTUAL(fakeFolder, "onlinerenamed2/file1rename");

        QCOMPARE(*vfs->pinState("onlinerenamed2/file1rename"), PinState::OnlineOnly);
    }

    void testEmptyFolderInOnlineOnlyRoot()
    {
        FakeFolder fakeFolder{ FileInfo() };
        setupVfs(fakeFolder);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        ItemCompletedSpy completeSpy(fakeFolder);

        auto cleanup = [&]() {
            completeSpy.clear();
        };
        cleanup();

        // OnlineOnly forced on the root
        ::setPinState(fakeFolder.localPath(), PinState::OnlineOnly, cfapi::NoRecurse);

        // No effect sync
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        cleanup();

        // Add an empty folder which should propagate
        fakeFolder.localModifier().mkdir("A");
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        cleanup();
    }

    void testIncompatiblePins()
    {
        FakeFolder fakeFolder{ FileInfo() };
        auto vfs = setupVfs(fakeFolder);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        fakeFolder.remoteModifier().mkdir("local");
        fakeFolder.remoteModifier().mkdir("online");
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        ::setPinState(fakeFolder.localPath() + "local", PinState::AlwaysLocal, cfapi::NoRecurse);
        ::setPinState(fakeFolder.localPath() + "online", PinState::OnlineOnly, cfapi::NoRecurse);

        fakeFolder.localModifier().insert("local/file1", 1024 * 1024);
        fakeFolder.localModifier().insert("online/file1", 1024 * 1024);
        QVERIFY(fakeFolder.syncOnce());

        ::setPinState(fakeFolder.localPath() + "local/file1", PinState::Unspecified, cfapi::Recurse);
        markForDehydration(fakeFolder, "local/file1");
        triggerDownload(fakeFolder, "online/file1");

        // the sync sets the changed files pin states to unspecified
        QVERIFY(fakeFolder.syncOnce());

        CFVERIFY_NONVIRTUAL(fakeFolder, "online/file1");
        CFVERIFY_VIRTUAL(fakeFolder, "local/file1");

        QCOMPARE(*vfs->pinState("online/file1"), PinState::Unspecified);
        QCOMPARE(*vfs->pinState("local/file1"), PinState::OnlineOnly);

        // no change on another sync
        QVERIFY(fakeFolder.syncOnce());

        CFVERIFY_NONVIRTUAL(fakeFolder, "online/file1");
        CFVERIFY_VIRTUAL(fakeFolder, "local/file1");
    }

    void testOpeningOnlineFileTriggersDownload_data()
    {
        QTest::addColumn<int>("errorKind");
        QTest::newRow("no error") << static_cast<int>(NoError);
        QTest::newRow("400") << 400;
        QTest::newRow("401") << 401;
        QTest::newRow("403") << 403;
        QTest::newRow("404") << 404;
        QTest::newRow("500") << 500;
        QTest::newRow("503") << 503;
        QTest::newRow("Timeout") << static_cast<int>(Timeout);
    }

    void testOpeningOnlineFileTriggersDownload()
    {
        QFETCH(int, errorKind);

        FakeFolder fakeFolder{ FileInfo() };
        setupVfs(fakeFolder);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        fakeFolder.remoteModifier().mkdir("online");
        fakeFolder.remoteModifier().mkdir("online/sub");
        QVERIFY(fakeFolder.syncOnce());
        QEXPECT_FAIL("", "folders on-demand breaks existing tests", Abort);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        ::setPinState(fakeFolder.localPath() + "online", PinState::OnlineOnly, cfapi::Recurse);

        fakeFolder.remoteModifier().insert("online/sub/file1", 10 * 1024 * 1024);
        QVERIFY(fakeFolder.syncOnce());

        CFVERIFY_VIRTUAL(fakeFolder, "online/sub/file1");

        // Setup error case if needed
        if (errorKind == Timeout) {
            fakeFolder.setServerOverride([&](QNetworkAccessManager::Operation op, const QNetworkRequest &req, QIODevice *) -> QNetworkReply * {
                if (req.url().path().endsWith("online/sub/file1")) {
                    return new FakeHangingReply(op, req, this);
                }
                return nullptr;
            });
        } else if (errorKind != NoError) {
            fakeFolder.serverErrorPaths().append("online/sub/file1", errorKind);
        }

        // So the test that test timeout finishes fast
        QScopedValueRollback<int> setHttpTimeout(AbstractNetworkJob::httpTimeout, errorKind == Timeout ? 1 : 10000);

        // Simulate another process requesting the open
        QEventLoop loop;
        std::thread t([&] {
            QFile file(fakeFolder.localPath() + "online/sub/file1");
            if (file.open(QFile::ReadOnly)) {
                file.readAll();
                file.close();
            }
            QMetaObject::invokeMethod(&loop, &QEventLoop::quit, Qt::QueuedConnection);
        });
        loop.exec();
        t.detach();

        // if (errorKind == NoError) {
        //     CFVERIFY_NONVIRTUAL(fakeFolder, "online/sub/file1");
        // } else {
        //     CFVERIFY_VIRTUAL(fakeFolder, "online/sub/file1");
        // }

        // Nothing should change
        ItemCompletedSpy completeSpy(fakeFolder);
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(completeSpy.isEmpty());

        // if (errorKind == NoError) {
        //     CFVERIFY_NONVIRTUAL(fakeFolder, "online/sub/file1");
        // } else {
        //     CFVERIFY_VIRTUAL(fakeFolder, "online/sub/file1");
        // }
    }

    void testDataFingerPrint()
    {
        FakeFolder fakeFolder{ FileInfo{} };
        setupVfs(fakeFolder);

        fakeFolder.remoteModifier().mkdir("a");
        fakeFolder.remoteModifier().mkdir("a/b");
        fakeFolder.remoteModifier().mkdir("a/b/d");
        fakeFolder.remoteModifier().insert("a/b/otherFile.txt");

        //Server support finger print, but none is set.
        fakeFolder.remoteModifier().extraDavProperties = "<oc:data-fingerprint></oc:data-fingerprint>";

        fakeFolder.syncEngine().setLocalDiscoveryOptions(OCC::LocalDiscoveryStyle::DatabaseAndFilesystem);
        QVERIFY(fakeFolder.syncOnce());

        fakeFolder.remoteModifier().remove("a/b/otherFile.txt");
        fakeFolder.remoteModifier().remove("a/b/d");
        fakeFolder.remoteModifier().extraDavProperties = "<oc:data-fingerprint>initial_finger_print</oc:data-fingerprint>";

        fakeFolder.syncEngine().setLocalDiscoveryOptions(OCC::LocalDiscoveryStyle::DatabaseAndFilesystem);
        QVERIFY(fakeFolder.syncOnce());

        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
    }

    void testLockFile_lockedFileReadOnly_afterSync()
    {
        FakeFolder fakeFolder{ FileInfo{} };
        setupVfs(fakeFolder);

        ItemCompletedSpy completeSpy(fakeFolder);

        fakeFolder.remoteModifier().mkdir("A");
        fakeFolder.remoteModifier().insert("A/a1");

        completeSpy.clear();
        QVERIFY(fakeFolder.syncOnce());

        QCOMPARE(completeSpy.findItem(QStringLiteral("A/a1"))->_locked, OCC::SyncFileItem::LockStatus::UnlockedItem);
        OCC::SyncJournalFileRecord fileRecordBefore;
        QVERIFY(fakeFolder.syncJournal().getFileRecord(QStringLiteral("A/a1"), &fileRecordBefore));
        QEXPECT_FAIL("", "folders on-demand breaks existing tests", Abort);
        QVERIFY(fileRecordBefore.isValid());
        QVERIFY(!fileRecordBefore._lockstate._locked);

        const auto localFileNotLocked = QFileInfo{fakeFolder.localPath() + u"A/a1"};
        QVERIFY(localFileNotLocked.isWritable());

        fakeFolder.remoteModifier().modifyLockState(QStringLiteral("A/a1"), FileModifier::LockState::FileLocked, 1, QStringLiteral("Nextcloud Office"), {}, QStringLiteral("richdocuments"), QDateTime::currentDateTime().toSecsSinceEpoch(), 1226);
        fakeFolder.remoteModifier().setModTimeKeepEtag(QStringLiteral("A/a1"), QDateTime::currentDateTime());
        fakeFolder.remoteModifier().appendByte(QStringLiteral("A/a1"));

        completeSpy.clear();
        QVERIFY(fakeFolder.syncOnce());

        QCOMPARE(completeSpy.findItem(QStringLiteral("A/a1"))->_locked, OCC::SyncFileItem::LockStatus::LockedItem);
        OCC::SyncJournalFileRecord fileRecordLocked;
        QVERIFY(fakeFolder.syncJournal().getFileRecord(QStringLiteral("A/a1"), &fileRecordLocked));
        QVERIFY(fileRecordLocked.isValid());
        QVERIFY(fileRecordLocked._lockstate._locked);

        const auto localFileLocked = QFileInfo{fakeFolder.localPath() + u"A/a1"};
        QVERIFY(!localFileLocked.isWritable());
    }

    void testLinkFileDownload()
    {
        FakeFolder fakeFolder{FileInfo{}};
        auto vfs = setupVfs(fakeFolder);

        qInfo("Starting .lnk test. It might hand and will get killed after timeout...");

        // Create a Windows shortcut (.lnk) file
        fakeFolder.remoteModifier().insert("linkfile.lnk");

        QVERIFY(fakeFolder.syncOnce());
        ItemCompletedSpy completeSpy(fakeFolder);
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(vfs->pinState("linkfile.lnk").isValid());
        QVERIFY(itemInstruction(completeSpy, "linkfile.lnk", CSYNC_INSTRUCTION_NONE));
        triggerDownload(fakeFolder, "linkfile.lnk");
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(itemInstruction(completeSpy, "linkfile.lnk", CSYNC_INSTRUCTION_SYNC));

        // a real .lnk file contents stored as base64 for tests
        QFile fakeShortcutBase64(QStringLiteral("fakeshortcut.base64"));
        QVERIFY(fakeShortcutBase64.open(QFile::ReadOnly));
        const auto fakeShortcutBase64Binary = QByteArray::fromBase64(fakeShortcutBase64.readAll());
        fakeShortcutBase64.close();
        
        // fill the .lnk file with binary data from real shortcut and turn it into OnlineOnly file
        const QString shortcutFilePathOnDisk = fakeFolder.localPath() + "linkfile.lnk";
        QFile shorcutFileOnDisk(shortcutFilePathOnDisk);
        QVERIFY(shorcutFileOnDisk.open(QFile::WriteOnly));
        QVERIFY(shorcutFileOnDisk.write(fakeShortcutBase64Binary));
        shorcutFileOnDisk.close();

        // run tests on it
        ::setPinState(shortcutFilePathOnDisk, PinState::OnlineOnly, cfapi::NoRecurse);
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(vfs->pinState("linkfile.lnk").isValid());
        QVERIFY(itemInstruction(completeSpy, "linkfile.lnk", CSYNC_INSTRUCTION_SYNC));

        // trigget download of online only .lnk file
        triggerDownload(fakeFolder, "linkfile.lnk");
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(vfs->pinState("linkfile.lnk").isValid());
        QVERIFY(itemInstruction(completeSpy, "linkfile.lnk", CSYNC_INSTRUCTION_SYNC));

        qInfo("Finishing .lnk test");
    }

    void testFolderDoesNotUpdatePlaceholderMetadata()
    {
        FakeFolder fakeFolder{FileInfo{}};
        auto vfs = setupVfs(fakeFolder);

        fakeFolder.remoteModifier().mkdir("A");
        fakeFolder.remoteModifier().insert("A/file");

        QVERIFY(fakeFolder.syncOnce());
        ItemCompletedSpy completeSpy(fakeFolder);
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(itemInstruction(completeSpy, "A", CSYNC_INSTRUCTION_NONE));
    }

    void testRemoteTypeChangeExistingLocalMustGetRemoved()
    {
        FakeFolder fakeFolder{FileInfo{}};
        setupVfs(fakeFolder);

        // test file change to directory on remote
        fakeFolder.remoteModifier().mkdir("a");
        fakeFolder.remoteModifier().insert("a/TESTFILE");
        QVERIFY(fakeFolder.syncOnce());

        fakeFolder.remoteModifier().remove("a/TESTFILE");
        fakeFolder.remoteModifier().mkdir("a/TESTFILE");
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());


        // test directory change to file on remote
        fakeFolder.remoteModifier().mkdir("a/TESTDIR");
        QVERIFY(fakeFolder.syncOnce());

        fakeFolder.remoteModifier().remove("a/TESTDIR");
        fakeFolder.remoteModifier().insert("a/TESTDIR");
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
    }

    void testDetectSpuriousNotification() {
#if !defined Q_OS_WIN
        QSKIP("not applicable");
#endif
        FakeFolder fakeFolder{FileInfo{}};
        auto vfs = setupVfs(fakeFolder);

        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        const QString odpFile("odp/presentation.odp");
        const QString odtFile("odt/document.odt");
        fakeFolder.localModifier().mkdir("odp");
        fakeFolder.localModifier().insert(odpFile);
        fakeFolder.localModifier().mkdir("odt");
        fakeFolder.localModifier().insert(odtFile);

        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        ItemCompletedSpy completeSpy(fakeFolder);

        QFile odp(fakeFolder.localPath() + odpFile);
        QVERIFY(odp.open(QIODevice::ReadWrite));
        odp.write(odpFile.toLatin1(), qstrlen(odpFile.toLatin1()));
        odp.close();
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(itemInstruction(completeSpy, odpFile, CSYNC_INSTRUCTION_SYNC));
        QCOMPARE(*vfs->pinState(odpFile), PinState::Unspecified);

        QFile odt(fakeFolder.localPath() + odtFile);
        QVERIFY(odt.open(QIODevice::ReadWrite));
        odt.close();
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(itemInstruction(completeSpy, odtFile, CSYNC_INSTRUCTION_UPDATE_METADATA));
        QCOMPARE(*vfs->pinState(odtFile), PinState::Unspecified);
    }

    void renameOnBothSides()
    {
        FakeFolder fakeFolder { FileInfo::A12_B12_C12_S12() };
        auto vfs = setupVfs(fakeFolder);

        // Test that renaming a file within a directory that was renamed on the other side actually do a rename.

        // 1) move the folder alphabetically before
        fakeFolder.remoteModifier().rename("A/a1", "A/a1m");
        fakeFolder.localModifier().rename("A", "_A");
        fakeFolder.localModifier().rename("B/b1", "B/b1m");
        fakeFolder.remoteModifier().rename("B", "_B");

        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentRemoteState(), fakeFolder.currentLocalState());
        QVERIFY(fakeFolder.currentRemoteState().find("_A/a1m"));
        QVERIFY(fakeFolder.currentRemoteState().find("_B/b1m"));

        // 2) move alphabetically after
        fakeFolder.remoteModifier().rename("_A/a2", "_A/a2m");
        fakeFolder.localModifier().rename("_B/b2", "_B/b2m");
        fakeFolder.localModifier().rename("_A", "S/A");
        fakeFolder.remoteModifier().rename("_B", "S/B");
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentRemoteState(), fakeFolder.currentLocalState());
        QVERIFY(fakeFolder.currentRemoteState().find("S/A/a2m"));
        QVERIFY(fakeFolder.currentRemoteState().find("S/B/b2m"));
    }

    void createFolderAndFiles()
    {
        FakeFolder fakeFolder {FileInfo{}};
        auto vfs = setupVfs(fakeFolder);

        fakeFolder.remoteModifier().mkdir("first folder");

        QVERIFY(fakeFolder.syncOnce());

        fakeFolder.remoteModifier().insert("first folder/file1");
        fakeFolder.remoteModifier().insert("first folder/file2");
        fakeFolder.remoteModifier().insert("first folder/file3");
        fakeFolder.remoteModifier().mkdir("first folder/second folder");
        fakeFolder.remoteModifier().insert("first folder/second folder/second file1");
        fakeFolder.remoteModifier().insert("first folder/second folder/second file2");
        fakeFolder.remoteModifier().insert("first folder/second folder/second file3");

        QVERIFY(fakeFolder.syncOnce());
        QEXPECT_FAIL("", "windows VFS breaks comparison using currentLocalState()", Abort);
        QCOMPARE(fakeFolder.currentRemoteState(), fakeFolder.currentLocalState());
    }

    void testSyncFolderNewDeleteConflictExpectDeletion()
    {
        QSKIP("folders on-demand breaks existing tests");

        FakeFolder fakeFolder{FileInfo{}};
        setupVfs(fakeFolder);

        fakeFolder.remoteModifier().mkdir("directory");
        fakeFolder.remoteModifier().mkdir("directory/subdir");
        fakeFolder.remoteModifier().insert("directory/file1");
        fakeFolder.remoteModifier().insert("directory/file2");
        fakeFolder.remoteModifier().insert("directory/file3");
        fakeFolder.remoteModifier().insert("directory/subdir/fileTxt1.txt");
        fakeFolder.remoteModifier().insert("directory/subdir/fileTxt2.txt");
        fakeFolder.remoteModifier().insert("directory/subdir/fileTxt3.txt");

        // perform an initial sync to ensure local and remote have the same state
        QVERIFY(fakeFolder.syncOnce());

        fakeFolder.remoteModifier().remove("directory");
        fakeFolder.localModifier().mkdir("directory/subFolder");
        fakeFolder.localModifier().insert("directory/file4");
        fakeFolder.localModifier().insert("directory/subdir/fileTxt4.txt");

        QVERIFY(!fakeFolder.syncOnce());
        QVERIFY(fakeFolder.syncOnce());
        const auto directoryItem = fakeFolder.remoteModifier().find("directory");
        QCOMPARE(directoryItem, nullptr);
    }

    void syncFoldersOnDemand()
    {
        FakeFolder fakeFolder {FileInfo{}};
        auto vfs = setupVfs(fakeFolder);

        ItemCompletedSpy completeSpy(fakeFolder);

        const auto cleanup = [&]() {
            completeSpy.clear();
        };

        cleanup();

        fakeFolder.remoteModifier().insert("rootfile1");
        fakeFolder.remoteModifier().mkdir("first folder");
        fakeFolder.remoteModifier().insert("first folder/file1");
        fakeFolder.remoteModifier().insert("first folder/file2");
        fakeFolder.remoteModifier().insert("first folder/file3");
        fakeFolder.remoteModifier().mkdir("first folder/second folder");
        fakeFolder.remoteModifier().insert("first folder/second folder/second file1");
        fakeFolder.remoteModifier().insert("first folder/second folder/second file2");
        fakeFolder.remoteModifier().insert("first folder/second folder/second file3");

        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(completeSpy.size(), 2);
        QVERIFY(itemInstruction(completeSpy, "rootfile1", CSYNC_INSTRUCTION_NEW));
        QVERIFY(itemInstruction(completeSpy, "first folder", CSYNC_INSTRUCTION_NEW));

        cleanup();

        QVERIFY(enumerateCloudFolder(fakeFolder.localPath() + "first folder"));

        QVERIFY(fakeFolder.syncOnce());

        QVERIFY(enumerateCloudFolder(fakeFolder.localPath() + "first folder"));

        const auto file3Info = fakeFolder.localModifier().find("first folder/file3");
        QVERIFY(file3Info.exists());
        const auto secondFolderInfo = fakeFolder.localModifier().find("first folder/second folder");
        QVERIFY(secondFolderInfo.exists());
        const auto wrongFirstFolderInfo = fakeFolder.localModifier().find("first folder/first folder");
        QVERIFY(!wrongFirstFolderInfo.exists());

        QVERIFY(fakeFolder.syncOnce());

        QVERIFY(enumerateCloudFolder(fakeFolder.localPath() + "first folder/second folder"));

        const auto secondFile3Info = fakeFolder.localModifier().find("first folder/second folder/second file3");
        QVERIFY(secondFile3Info.exists());
        const auto wrongSecondFolderInfo = fakeFolder.localModifier().find("first folder/second folder/second folder");
        QVERIFY(!wrongSecondFolderInfo.exists());

        QVERIFY(fakeFolder.syncOnce());
    }

    void syncFoldersOnDemandFromSubFolder()
    {
        FileInfo localRoot{u"first folder"_s, {}};
        FileInfo remoteRoot{{}, {localRoot}};
        FakeFolder fakeFolder {remoteRoot, localRoot, u"/first folder"_s};
        auto vfs = setupVfs(fakeFolder);

        ItemCompletedSpy completeSpy(fakeFolder);

        const auto cleanup = [&]() {
            completeSpy.clear();
        };

        cleanup();

        fakeFolder.remoteModifier().insert("rootfile1");
        fakeFolder.remoteModifier().insert("first folder/file1");
        fakeFolder.remoteModifier().insert("first folder/file2");
        fakeFolder.remoteModifier().insert("first folder/file3");
        fakeFolder.remoteModifier().mkdir("first folder/second folder");
        fakeFolder.remoteModifier().insert("first folder/second folder/second file1");
        fakeFolder.remoteModifier().insert("first folder/second folder/second file2");
        fakeFolder.remoteModifier().insert("first folder/second folder/second file3");
        fakeFolder.remoteModifier().mkdir("first folder/second folder/third folder");
        fakeFolder.remoteModifier().insert("first folder/second folder/third folder/third file1");
        fakeFolder.remoteModifier().insert("first folder/second folder/third folder/third file2");
        fakeFolder.remoteModifier().insert("first folder/second folder/third folder/third file3");

        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(completeSpy.size(), 4);
        QVERIFY(itemInstruction(completeSpy, "file1", CSYNC_INSTRUCTION_NEW));
        QVERIFY(itemInstruction(completeSpy, "file2", CSYNC_INSTRUCTION_NEW));
        QVERIFY(itemInstruction(completeSpy, "file3", CSYNC_INSTRUCTION_NEW));
        QVERIFY(itemInstruction(completeSpy, "second folder", CSYNC_INSTRUCTION_NEW));

        cleanup();

        QVERIFY(enumerateCloudFolder(fakeFolder.localPath() + "second folder"));

        QVERIFY(fakeFolder.syncOnce());

        QVERIFY(enumerateCloudFolder(fakeFolder.localPath() + "second folder"));

        const auto file3Info = fakeFolder.localModifier().find("second folder/second file3");
        QVERIFY(file3Info.exists());
        const auto secondFolderInfo = fakeFolder.localModifier().find("second folder/third folder");
        QVERIFY(secondFolderInfo.exists());
        const auto wrongFirstFolderInfo = fakeFolder.localModifier().find("second folder/second folder");
        QVERIFY(!wrongFirstFolderInfo.exists());

        QVERIFY(fakeFolder.syncOnce());

        QVERIFY(enumerateCloudFolder(fakeFolder.localPath() + "second folder/third folder"));

        const auto secondFile3Info = fakeFolder.localModifier().find("second folder/third folder/third file3");
        QVERIFY(secondFile3Info.exists());
        const auto wrongSecondFolderInfo = fakeFolder.localModifier().find("second folder/third folder/third folder");
        QVERIFY(!wrongSecondFolderInfo.exists());

        QVERIFY(fakeFolder.syncOnce());
    }

    void switchVfsOffWithOnDemandFolder()
    {
        FakeFolder fakeFolder{FileInfo{}};
        setupVfs(fakeFolder);

        ItemCompletedSpy completeSpy(fakeFolder);

        fakeFolder.remoteModifier().insert("firstFile1.txt");
        fakeFolder.remoteModifier().insert("firstFile2.txt");
        fakeFolder.remoteModifier().insert("firstFile3.txt");
        fakeFolder.remoteModifier().mkdir("directory");
        fakeFolder.remoteModifier().mkdir("directory/subdir");
        fakeFolder.remoteModifier().insert("directory/file1");
        fakeFolder.remoteModifier().insert("directory/file2");
        fakeFolder.remoteModifier().insert("directory/file3");
        fakeFolder.remoteModifier().insert("directory/subdir/fileTxt1.txt");
        fakeFolder.remoteModifier().insert("directory/subdir/fileTxt2.txt");
        fakeFolder.remoteModifier().insert("directory/subdir/fileTxt3.txt");

        QVERIFY(fakeFolder.syncOnce());

        QCOMPARE(completeSpy.size(), 4);
        QVERIFY(itemInstruction(completeSpy, "directory", CSYNC_INSTRUCTION_NEW));
        QVERIFY(itemInstruction(completeSpy, "firstFile1.txt", CSYNC_INSTRUCTION_NEW));
        QVERIFY(itemInstruction(completeSpy, "firstFile2.txt", CSYNC_INSTRUCTION_NEW));
        QVERIFY(itemInstruction(completeSpy, "firstFile3.txt", CSYNC_INSTRUCTION_NEW));
        completeSpy.clear();

        SyncEngine::wipeVirtualFiles(fakeFolder.localPath(), fakeFolder.syncJournal(), *fakeFolder.syncEngine().syncOptions()._vfs);

        fakeFolder.syncEngine().syncOptions()._vfs->stop();
        fakeFolder.syncEngine().syncOptions()._vfs->unregisterFolder();

        auto noVfsPlugin = QSharedPointer<Vfs>(createVfsFromPlugin(Vfs::Off).release());
        QObject::connect(&fakeFolder.syncEngine().syncFileStatusTracker(), &SyncFileStatusTracker::fileStatusChanged,
                         noVfsPlugin.data(), &Vfs::fileStatusChanged);
        fakeFolder.switchToVfs(noVfsPlugin);

        ::setPinState(fakeFolder.localPath(), PinState::Unspecified, cfapi::NoRecurse);

        fakeFolder.syncJournal().setSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, {});

        fakeFolder.syncEngine().setLocalDiscoveryOptions(LocalDiscoveryStyle::FilesystemOnly);
        QVERIFY(fakeFolder.syncOnce());

        QCOMPARE(completeSpy.size(), 11);
        QVERIFY(itemInstruction(completeSpy, "directory", CSYNC_INSTRUCTION_NEW));
        QVERIFY(itemInstruction(completeSpy, "firstFile1.txt", CSYNC_INSTRUCTION_NEW));
        QVERIFY(itemInstruction(completeSpy, "firstFile2.txt", CSYNC_INSTRUCTION_NEW));
        QVERIFY(itemInstruction(completeSpy, "firstFile3.txt", CSYNC_INSTRUCTION_NEW));
        QVERIFY(itemInstruction(completeSpy, "directory/subdir", CSYNC_INSTRUCTION_NEW));
        QVERIFY(itemInstruction(completeSpy, "directory/file1", CSYNC_INSTRUCTION_NEW));
        QVERIFY(itemInstruction(completeSpy, "directory/file2", CSYNC_INSTRUCTION_NEW));
        QVERIFY(itemInstruction(completeSpy, "directory/file3", CSYNC_INSTRUCTION_NEW));
        QVERIFY(itemInstruction(completeSpy, "directory/subdir/fileTxt1.txt", CSYNC_INSTRUCTION_NEW));
        QVERIFY(itemInstruction(completeSpy, "directory/subdir/fileTxt2.txt", CSYNC_INSTRUCTION_NEW));
        QVERIFY(itemInstruction(completeSpy, "directory/subdir/fileTxt3.txt", CSYNC_INSTRUCTION_NEW));
        completeSpy.clear();

        fakeFolder.syncEngine().setLocalDiscoveryOptions(LocalDiscoveryStyle::FilesystemOnly);
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(completeSpy.size(), 0);
    }

    void moveItemsFromClientShouldBeMoveOnServer()
    {
        FakeFolder fakeFolder{FileInfo{}};
        setupVfs(fakeFolder);

        ItemCompletedSpy completeSpy(fakeFolder);
        const auto cleanup = [&]() {
            completeSpy.clear();
        };

        fakeFolder.remoteModifier().mkdir("zdirectory");
        fakeFolder.remoteModifier().mkdir("zdirectory/zsubdir");
        fakeFolder.remoteModifier().insert("fileFromRoot.txt");
        fakeFolder.remoteModifier().insert("zdirectory/afileFromDirectory.txt");
        fakeFolder.remoteModifier().insert("zdirectory/zsubdir/afileFromSubdir.txt");

        QVERIFY(fakeFolder.syncOnce());

        QCOMPARE(completeSpy.size(), 2);
        QVERIFY(itemInstruction(completeSpy, "zdirectory", CSYNC_INSTRUCTION_NEW));
        QVERIFY(itemInstruction(completeSpy, "fileFromRoot.txt", CSYNC_INSTRUCTION_NEW));
        cleanup();

        QVERIFY(enumerateCloudFolder(fakeFolder.localPath() + "zdirectory"));

        QVERIFY(enumerateCloudFolder(fakeFolder.localPath() + "zdirectory"));

        QVERIFY(enumerateCloudFolder(fakeFolder.localPath() + "zdirectory/zsubdir"));

        const auto afileFromDirectoryInfo = fakeFolder.localModifier().find("zdirectory/afileFromDirectory.txt");
        QVERIFY(afileFromDirectoryInfo.exists());
        const auto zsubdirInfo = fakeFolder.localModifier().find("zdirectory/zsubdir");
        QVERIFY(zsubdirInfo.exists());
        const auto afileFromSubdirInfo = fakeFolder.localModifier().find("zdirectory/zsubdir/afileFromSubdir.txt");
        QVERIFY(afileFromSubdirInfo.exists());

        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(completeSpy.size(), 0);
        cleanup();

        fakeFolder.localModifier().rename("fileFromRoot.txt", "zdirectory/zsubdir/fileFromRoot.txt");
        fakeFolder.localModifier().rename("zdirectory/afileFromDirectory.txt", "afileFromDirectory.txt");
        fakeFolder.localModifier().rename("zdirectory/zsubdir/afileFromSubdir.txt", "afileFromSubdir.txt");

        QVERIFY(fakeFolder.syncOnce());

        QCOMPARE(completeSpy.size(), 3);

        QVERIFY(itemInstruction(completeSpy, "zdirectory/zsubdir/fileFromRoot.txt", CSYNC_INSTRUCTION_RENAME));
        QVERIFY(itemInstruction(completeSpy, "afileFromDirectory.txt", CSYNC_INSTRUCTION_RENAME));
        QVERIFY(itemInstruction(completeSpy, "afileFromSubdir.txt", CSYNC_INSTRUCTION_RENAME));
    }


    void testServer_caseClash_createConflict()
    {
        FakeFolder fakeFolder{ FileInfo{} };
        setupVfs(fakeFolder);

        ItemCompletedSpy completeSpy(fakeFolder);

        const auto cleanup = [&]() {
            completeSpy.clear();
        };

        cleanup();

        fakeFolder.remoteModifier().mkdir("perso");
        fakeFolder.remoteModifier().mkdir("perso/hello");
        fakeFolder.remoteModifier().mkdir("perso/Hello");

        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(completeSpy.size(), 1);
        QVERIFY(itemInstruction(completeSpy, "perso", CSYNC_INSTRUCTION_NEW));

        cleanup();

        QVERIFY(enumerateCloudFolder(fakeFolder.localPath() + "perso"));

        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(completeSpy.size(), 0);

        QVERIFY(enumerateCloudFolder(fakeFolder.localPath() + "perso"));

        cleanup();
        fakeFolder.syncOnce();
        QCOMPARE(completeSpy.size(), 0);

        cleanup();
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(completeSpy.size(), 0);
    }
};

QTEST_GUILESS_MAIN(TestSyncCfApi)
#include "testsynccfapi.moc"
