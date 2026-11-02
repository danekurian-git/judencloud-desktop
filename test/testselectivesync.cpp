/*
 * SPDX-FileCopyrightText: 2023 Nextcloud GmbH and Nextcloud contributors
 * SPDX-FileCopyrightText: 2018 ownCloud, Inc.
 * SPDX-License-Identifier: CC0-1.0
 *
 * This software is in the public domain, furnished "as is", without technical
 * support, and with no warranty, express or implied, as to its usefulness for
 * any purpose.
 */

#include "common/vfs.h"
#include "networkjobs.h"
#include "syncenginetestutils.h"
#include <QtTest>
#include <syncengine.h>

using namespace OCC;

class TestSelectiveSync : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        Logger::instance()->setLogFlush(true);
        Logger::instance()->setLogDebug(true);

        QStandardPaths::setTestModeEnabled(true);
    }

    void testSelectiveSyncBigFolders()
    {
        FakeFolder fakeFolder{FileInfo::A12_B12_C12_S12()};
        SyncOptions options;
        options._newBigFolderSizeLimit = 20000; // 20 K
        fakeFolder.syncEngine().setSyncOptions(options);

        QStringList sizeRequests;
        fakeFolder.setServerOverride([&](QNetworkAccessManager::Operation, const QNetworkRequest &req, QIODevice *device) -> QNetworkReply * {
            // Record what path we are querying for the size
            if (req.attribute(QNetworkRequest::CustomVerbAttribute).toString() == "PROPFIND") {
                if (device->readAll().contains("<size ")) {
                    sizeRequests << req.url().path();
                }
            }
            return nullptr;
        });

        QSignalSpy newBigFolder(&fakeFolder.syncEngine(), &SyncEngine::newBigFolder);

        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        fakeFolder.remoteModifier().createDir("A/newBigDir");
        fakeFolder.remoteModifier().createDir("A/newBigDir/subDir");
        fakeFolder.remoteModifier().insert("A/newBigDir/subDir/bigFile", options._newBigFolderSizeLimit + 10);
        fakeFolder.remoteModifier().insert("A/newBigDir/subDir/smallFile", 10);

        fakeFolder.remoteModifier().createDir("B/newSmallDir");
        fakeFolder.remoteModifier().createDir("B/newSmallDir/subDir");
        fakeFolder.remoteModifier().insert("B/newSmallDir/subDir/smallFile", 10);

        // Because the test system don't do that automatically
        fakeFolder.remoteModifier().find("A/newBigDir")->extraDavProperties = "<oc:size>20020</oc:size>";
        fakeFolder.remoteModifier().find("A/newBigDir/subDir")->extraDavProperties = "<oc:size>20020</oc:size>";
        fakeFolder.remoteModifier().find("B/newSmallDir")->extraDavProperties = "<oc:size>10</oc:size>";
        fakeFolder.remoteModifier().find("B/newSmallDir/subDir")->extraDavProperties = "<oc:size>10</oc:size>";

        QVERIFY(fakeFolder.syncOnce());

        QCOMPARE(newBigFolder.count(), 0);
        QVERIFY(sizeRequests.isEmpty());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        fakeFolder.syncJournal().schedulePathForRemoteDiscovery(QStringLiteral("A/newBigDir"));
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(newBigFolder.count(), 0);
        QVERIFY(sizeRequests.isEmpty());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
    }

    void testMountTypeClassification_data()
    {
        QTest::addColumn<QString>("mountType");
        QTest::addColumn<bool>("external");
        QTest::newRow("local") << QString{} << false;
        QTest::newRow("group") << QStringLiteral("group") << false;
        QTest::newRow("shared") << QStringLiteral("shared") << false;
        QTest::newRow("external") << QStringLiteral("external") << true;
        QTest::newRow("external-session") << QStringLiteral("external-session") << true;
    }

    void testMountTypeClassification()
    {
        QFETCH(QString, mountType);
        QFETCH(bool, external);
        const QMap<QString, QString> properties{{QStringLiteral("mount-type"), mountType}, {QStringLiteral("is-mount-root"), QStringLiteral("false")}};
        const auto permissions =
            RemotePermissions::fromServerString(QStringLiteral("GWM"), RemotePermissions::MountedPermissionAlgorithm::UseMountRootProperty, properties);
        QCOMPARE(permissions.hasPermission(RemotePermissions::IsExternalStorage), external);
        QCOMPARE(RemotePermissions::fromDbValue(permissions.toDbValue()), permissions);
    }

    void testExternalStorageNeverSelected_data()
    {
        QTest::addColumn<QString>("mountType");
        QTest::addColumn<bool>("virtualFiles");
        QTest::newRow("external") << QStringLiteral("external") << false;
        QTest::newRow("external-session") << QStringLiteral("external-session") << false;
        QTest::newRow("virtual") << QStringLiteral("external") << true;
    }

    void testExternalStorageNeverSelected()
    {
        QFETCH(QString, mountType);
        QFETCH(bool, virtualFiles);
        FakeFolder folder{FileInfo{}};
        if (virtualFiles) {
            folder.switchToVfs(QSharedPointer<Vfs>(createVfsFromPlugin(Vfs::WithSuffix).release()));
        }
        folder.remoteModifier().mkdir("external");
        folder.remoteModifier().insert("external/file");
        auto *external = folder.remoteModifier().find("external");
        external->extraDavProperties = QStringLiteral("<nc:mount-type>%1</nc:mount-type><nc:is-mount-root>true</nc:is-mount-root>").arg(mountType).toUtf8();
        external->permissions = RemotePermissions::fromServerString(QStringLiteral("GWDNVCKRM"));
        folder.syncJournal().setSelectiveSyncList(SyncJournalDb::SelectiveSyncWhiteList, {QStringLiteral("/"), QStringLiteral("external/")});
        QSignalSpy prompts(&folder.syncEngine(), &SyncEngine::newBigFolder);
        ItemCompletedSpy completed(folder);
        const auto localBefore = folder.currentLocalState();
        const auto remoteBefore = folder.currentRemoteState();
        QVERIFY(folder.syncOnce());
        QCOMPARE(folder.currentLocalState(), localBefore);
        QCOMPARE(folder.currentRemoteState(), remoteBefore);
        QCOMPARE(prompts.count(), 0);
        const auto ignored = completed.findItem(QStringLiteral("external"));
        QVERIFY(ignored);
        QCOMPARE(ignored->_instruction, CSYNC_INSTRUCTION_IGNORE);
    }

    void testPreviouslySyncedExternalStoragePreservesFiles()
    {
        FakeFolder folder{FileInfo::A12_B12_C12_S12()};
        folder.remoteModifier().find("A")->extraDavProperties = "<nc:mount-type>external</nc:mount-type>";
        folder.localModifier().insert("A/local-only", 12);
        folder.remoteModifier().insert("A/remote-only", 13);
        folder.syncJournal().setSelectiveSyncList(SyncJournalDb::SelectiveSyncWhiteList, {QStringLiteral("A/")});
        folder.syncJournal().setSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, {QStringLiteral("A/")});
        const auto localBefore = folder.currentLocalState();
        const auto remoteBefore = folder.currentRemoteState();
        QVERIFY(folder.syncOnce());
        QCOMPARE(folder.currentLocalState(), localBefore);
        QCOMPARE(folder.currentRemoteState(), remoteBefore);
        QVERIFY(folder.syncOnce());
        QCOMPARE(folder.currentLocalState(), localBefore);
        QCOMPARE(folder.currentRemoteState(), remoteBefore);
    }

    void testGroupAndSharedMountsRemainAvailable_data()
    {
        QTest::addColumn<QString>("mountType");
        QTest::newRow("group") << QStringLiteral("group");
        QTest::newRow("shared") << QStringLiteral("shared");
    }

    void testGroupAndSharedMountsRemainAvailable()
    {
        QFETCH(QString, mountType);
        FakeFolder folder{FileInfo{}};
        folder.remoteModifier().mkdir("mounted");
        folder.remoteModifier().insert("mounted/file", 30000);
        auto *mounted = folder.remoteModifier().find("mounted");
        mounted->extraDavProperties = QStringLiteral("<nc:mount-type>%1</nc:mount-type><nc:is-mount-root>true</nc:is-mount-root>").arg(mountType).toUtf8();
        mounted->permissions = RemotePermissions::fromServerString(QStringLiteral("GWDNVCKRMS"));
        QVERIFY(folder.syncOnce());
        QCOMPARE(folder.currentLocalState(), folder.currentRemoteState());
        folder.localModifier().insert("mounted/local", 5);
        QVERIFY(folder.syncOnce());
        QCOMPARE(folder.currentLocalState(), folder.currentRemoteState());
    }

    void testCachedMountClassificationIsRefreshed()
    {
        FakeFolder folder{FileInfo{}};
        folder.remoteModifier().mkdir("parent");
        folder.remoteModifier().mkdir("parent/mounted");
        folder.remoteModifier().insert("parent/mounted/file", 20);
        auto *mounted = folder.remoteModifier().find("parent/mounted");
        mounted->permissions = RemotePermissions::fromServerString(QStringLiteral("GWDNVCKRM"));
        mounted->extraDavProperties = "<nc:mount-type>group</nc:mount-type><nc:is-mount-root>true</nc:is-mount-root>";
        QVERIFY(folder.syncOnce());
        QCOMPARE(folder.currentLocalState(), folder.currentRemoteState());

        // Change only the storage classification, retaining every etag and the old journal permissions.
        mounted = folder.remoteModifier().find("parent/mounted");
        mounted->extraDavProperties = "<nc:mount-type>external</nc:mount-type><nc:is-mount-root>true</nc:is-mount-root>";
        folder.localModifier().insert("parent/mounted/local-only", 12);
        const auto localBefore = folder.currentLocalState();
        const auto remoteBefore = folder.currentRemoteState();
        QVERIFY(folder.syncOnce());
        QCOMPARE(folder.currentLocalState(), localBefore);
        QCOMPARE(folder.currentRemoteState(), remoteBefore);

        folder.syncJournal().setSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, {QStringLiteral("parent/mounted/")});
        QVERIFY(folder.syncOnce());
        QCOMPARE(folder.currentLocalState(), localBefore);
        QCOMPARE(folder.currentRemoteState(), remoteBefore);
    }

    void testExternalSyncRootStopsWithoutChanges()
    {
        FakeFolder folder{FileInfo::A12_B12_C12_S12(), FileInfo{}, QStringLiteral("/A"), false};
        folder.remoteModifier().find("A")->extraDavProperties = "<nc:mount-type>external-session</nc:mount-type><nc:is-mount-root>false</nc:is-mount-root>";
        folder.localModifier().insert("local-only", 12);
        const auto localBefore = folder.currentLocalState();
        const auto remoteBefore = folder.currentRemoteState();
        QSignalSpy errors(&folder.syncEngine(), &SyncEngine::syncError);
        QVERIFY(!folder.syncOnce());
        QVERIFY(!errors.isEmpty());
        QCOMPARE(folder.currentLocalState(), localBefore);
        QCOMPARE(folder.currentRemoteState(), remoteBefore);
    }

    void testHydrationStorageCheck_data()
    {
        QTest::addColumn<QByteArray>("properties");
        QTest::addColumn<bool>("allowed");
        QTest::newRow("local") << QByteArray("<nc:mount-type/>") << true;
        QTest::newRow("group") << QByteArray("<nc:mount-type>group</nc:mount-type>") << true;
        QTest::newRow("shared") << QByteArray("<nc:mount-type>shared</nc:mount-type>") << true;
        QTest::newRow("external") << QByteArray("<nc:mount-type>external</nc:mount-type>") << false;
        QTest::newRow("external-session") << QByteArray("<nc:mount-type>external-session</nc:mount-type>") << false;
        QTest::newRow("missing") << QByteArray{} << false;
    }

    void testHydrationStorageCheck()
    {
        QFETCH(QByteArray, properties);
        QFETCH(bool, allowed);
        FakeFolder folder{FileInfo::A12_B12_C12_S12()};
        folder.remoteModifier().find("A/a1")->extraDavProperties = properties;
        if (properties.isEmpty()) {
            // Override the server's normal empty mount-type property to simulate an unsupported response.
            folder.setServerOverride([&folder](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
                return new FakePropfindReply(folder.remoteModifier(), op, request, folder.networkAccessManager());
            });
        }
        auto completed = false;
        auto transferAllowed = !allowed;
        checkRemoteStorageAllowed(folder.account(), QStringLiteral("/A/a1"), this, [&](bool result) {
            transferAllowed = result;
            completed = true;
        });
        QTRY_VERIFY(completed);
        QCOMPARE(transferAllowed, allowed);

        folder.setServerOverride({});
        folder.serverErrorPaths().append(QStringLiteral("A/a1"), 503);
        completed = false;
        checkRemoteStorageAllowed(folder.account(), QStringLiteral("/A/a1"), this, [&](bool result) {
            transferAllowed = result;
            completed = true;
        });
        QTRY_VERIFY(completed);
        QVERIFY(!transferAllowed);
    }

    void testHydrationPropertyStatus_data()
    {
        QTest::addColumn<QString>("status");
        QTest::addColumn<bool>("allowed");
        QTest::newRow("local property available") << QStringLiteral("HTTP/1.1 200 OK") << true;
        QTest::newRow("property unsupported") << QStringLiteral("HTTP/1.1 404 Not Found") << false;
        QTest::newRow("property forbidden") << QStringLiteral("HTTP/1.1 403 Forbidden") << false;
        QTest::newRow("property status missing") << QString{} << false;
    }

    void testHydrationPropertyStatus()
    {
        QFETCH(QString, status);
        QFETCH(bool, allowed);
        FakeFolder folder{FileInfo::A12_B12_C12_S12()};
        const auto response = QStringLiteral(
                                  "<d:multistatus xmlns:d=\"DAV:\" xmlns:nc=\"http://nextcloud.org/ns\">"
                                  "<d:response><d:href>/A/a1</d:href><d:propstat><d:prop><nc:mount-type/></d:prop>"
                                  "<d:status>%1</d:status></d:propstat></d:response></d:multistatus>")
                                  .arg(status)
                                  .toUtf8();
        auto requestedProperty = false;
        folder.setServerOverride([&](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *outgoingData) -> QNetworkReply * {
            requestedProperty = request.rawHeader("Depth") == QByteArrayLiteral("0") && outgoingData
                && outgoingData->peek(outgoingData->bytesAvailable()).contains("mount-type");
            auto *reply = new FakePropfindReply(response, op, request, folder.networkAccessManager());
            reply->open(QIODevice::ReadOnly);
            return reply;
        });
        auto completed = false;
        auto transferAllowed = !allowed;
        checkRemoteStorageAllowed(folder.account(), QStringLiteral("/A/a1"), this, [&](bool result) {
            transferAllowed = result;
            completed = true;
        });
        QTRY_VERIFY(completed);
        QVERIFY(requestedProperty);
        QCOMPARE(transferAllowed, allowed);
    }

    void testHydrationStorageCheckCanceledWithContext()
    {
        FakeFolder folder{FileInfo::A12_B12_C12_S12()};
        folder.remoteModifier().find("A/a1")->extraDavProperties = "<nc:mount-type/>";
        auto context = std::make_unique<QObject>();
        auto completed = false;
        checkRemoteStorageAllowed(folder.account(), QStringLiteral("/A/a1"), context.get(), [&](bool) {
            completed = true;
        });
        context.reset();
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
        QVERIFY(!completed);
    }

    void testRestoreSubFolderForDataFingerPrint()
    {
        const auto mkcolVerb = QByteArray{"MKCOL"};
        FakeFolder fakeFolder{{}};
        fakeFolder.localModifier().mkdir("topFolder");
        fakeFolder.localModifier().mkdir("topFolder/subFolder");
        fakeFolder.localModifier().insert("topFolder/subFolder/a");
        fakeFolder.remoteModifier().extraDavProperties = "<oc:data-fingerprint>initial_finger_print</oc:data-fingerprint>";

        QVERIFY(fakeFolder.syncOnce());

        auto mkdirRequestsCounter = 0;
        fakeFolder.setServerOverride(
            [&mkdirRequestsCounter, mkcolVerb](QNetworkAccessManager::Operation, const QNetworkRequest &req, QIODevice *device) -> QNetworkReply * {
                Q_UNUSED(device)

                if (req.attribute(QNetworkRequest::CustomVerbAttribute) == mkcolVerb) {
                    ++mkdirRequestsCounter;
                }
                qDebug() << req.attribute(QNetworkRequest::CustomVerbAttribute);

                return nullptr;
            });

        fakeFolder.syncEngine().journal()->setSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, QStringList() << QLatin1String("topFolder"));
        fakeFolder.remoteModifier().extraDavProperties = "<oc:data-fingerprint>changed_finger_print</oc:data-fingerprint>";

        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(mkdirRequestsCounter, 0);
    }
};

QTEST_GUILESS_MAIN(TestSelectiveSync)
#include "testselectivesync.moc"
