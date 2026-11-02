/*
 * SPDX-FileCopyrightText: 2023 Nextcloud GmbH and Nextcloud contributors
 * SPDX-FileCopyrightText: 2018 ownCloud, Inc.
 * SPDX-License-Identifier: CC0-1.0
 *
 * This software is in the public domain, furnished "as is", without technical
 * support, and with no warranty, express or implied, as to its usefulness for
 * any purpose.
 */

#include <QtTest>
#include "syncenginetestutils.h"
#include <syncengine.h>
#include "owncloudpropagator.h"
#include "common/vfs.h"
#include <QSemaphore>
#include <QThreadPool>
#include <QScopedValueRollback>
#include <QScopeGuard>
#include <atomic>

namespace OCC
{
OCSYNC_EXPORT extern std::function<bool(const QString &, QString *)> moveToTrashOverride;
OCSYNC_EXPORT extern std::function<bool(const QString &, QString *)> removeFileOverride;
}

class RecycleTestVfs : public OCC::VfsOff
{
public:
    Mode mode() const override
    {
        return WindowsCfApi;
    }
};

using namespace OCC;

class TestSyncDelete : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        OCC::Logger::instance()->setLogFlush(true);
        OCC::Logger::instance()->setLogDebug(true);

        QStandardPaths::setTestModeEnabled(true);
    }

    void testRecycleCanServiceProviderCallbacks_data()
    {
        QTest::addColumn<bool>("succeed");
        QTest::addColumn<bool>("recycle");
        QTest::addColumn<bool>("directory");
        QTest::addColumn<bool>("alreadyMissing");
        QTest::newRow("recycle succeeds") << true << true << false << false;
        QTest::newRow("recycle fails") << false << true << false << false;
        QTest::newRow("direct file succeeds") << true << false << false << false;
        QTest::newRow("direct file fails") << false << false << false << false;
        QTest::newRow("direct directory succeeds") << true << false << true << false;
        QTest::newRow("direct directory fails") << false << false << true << false;
        QTest::newRow("recycle file already missing") << true << true << false << true;
        QTest::newRow("direct file already missing") << true << false << false << true;
        QTest::newRow("direct directory already missing") << true << false << true << true;
    }

    void testRecycleCanServiceProviderCallbacks()
    {
        QFETCH(bool, succeed);
        QFETCH(bool, recycle);
        QFETCH(bool, directory);
        QFETCH(bool, alreadyMissing);
        FakeFolder folder{FileInfo::A12_B12_C12_S12(), {}, {}, false};
        if (!Account::enforcedServerUrl().isEmpty()) {
            QVERIFY(folder.account()->setUrl(Account::enforcedServerUrl()));
        }
        QVERIFY(folder.syncJournal().open());
        SyncJournalFileRecord originalRecord;
        originalRecord._path = directory ? QByteArrayLiteral("A") : QByteArrayLiteral("A/a1");
        originalRecord._type = directory ? ItemTypeDirectory : ItemTypeFile;
        originalRecord._fileId = QByteArrayLiteral("recycle-test-id");
        originalRecord._etag = QByteArrayLiteral("recycle-test-etag");
        originalRecord._modtime = QFileInfo(folder.localPath() + QStringLiteral("A/a1")).lastModified().toSecsSinceEpoch();
        originalRecord._fileSize = QFileInfo(folder.localPath() + QStringLiteral("A/a1")).size();
        QVERIFY(folder.syncJournal().setFileRecord(originalRecord));
        QSet<QString> bulkUploadBlacklist;
        OwncloudPropagator propagator(folder.account(), folder.localPath(), QStringLiteral("/"), &folder.syncJournal(), bulkUploadBlacklist);
        auto options = folder.syncEngine().syncOptions();
        options._vfs.reset(new RecycleTestVfs);
        options._moveFilesToTrash = recycle;
        propagator.setSyncOptions(options);
        const auto item = SyncFileItemPtr(new SyncFileItem);
        item->_file = QString::fromUtf8(originalRecord._path);
        item->_originalFile = item->_file;
        item->_instruction = CSYNC_INSTRUCTION_REMOVE;
        item->_direction = SyncFileItem::Down;
        item->_type = originalRecord._type;
        if (alreadyMissing) {
            const auto path = folder.localPath() + item->_file;
            QVERIFY(directory ? QDir(path).removeRecursively() : QFile::remove(path));
        }
        auto job = std::unique_ptr<PropagateItemJob>(propagator.createJob(item));
        QSignalSpy finished(job.get(), &PropagatorJob::finished);
        auto callbackServiced = std::atomic_bool(false);
        auto usedWorker = std::atomic_bool(false);
        auto &operationOverride = recycle ? moveToTrashOverride : removeFileOverride;
        QScopedValueRollback overrideScope(operationOverride);
        operationOverride = [this, succeed, &callbackServiced, &usedWorker](const QString &path, QString *error) {
            usedWorker = QThread::currentThread() != thread();
            const auto callback = std::make_shared<QSemaphore>();
            QMetaObject::invokeMethod(
                this,
                [callback] {
                    callback->release();
                },
                Qt::QueuedConnection);
            callbackServiced = callback->tryAcquire(1, 2000);
            if (!callbackServiced || !succeed) {
                *error = QStringLiteral("Simulated removal failure");
                return false;
            }
            return QFile::remove(path);
        };
        const auto drainWorker = qScopeGuard([&] {
            job.reset();
            QThreadPool::globalInstance()->waitForDone();
        });
        QVERIFY(job->scheduleSelfOrChild());
        QCOMPARE(finished.count(), 0);
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(usedWorker.load(), !alreadyMissing);
        QCOMPARE(callbackServiced.load(), !alreadyMissing);
        QCOMPARE(item->_status, succeed ? SyncFileItem::Success : SyncFileItem::NormalError);
        SyncJournalFileRecord record;
        QVERIFY(folder.syncJournal().getFileRecord(item->_file, &record));
        QCOMPARE(record.isValid(), !succeed);
        QCOMPARE(QFileInfo::exists(folder.localPath() + item->_file), !succeed);
    }

    void testRecycleAbortWaitsForOperation_data()
    {
        QTest::addColumn<bool>("destroyWhileRunning");
        QTest::addColumn<bool>("recycle");
        QTest::addColumn<bool>("directory");
        QTest::addColumn<bool>("succeedAfterCancellation");
        QTest::newRow("recycle asynchronous abort") << false << true << false << false;
        QTest::newRow("recycle forced abort and destroy") << true << true << false << false;
        QTest::newRow("direct file asynchronous abort") << false << false << false << false;
        QTest::newRow("direct file forced abort and destroy") << true << false << false << false;
        QTest::newRow("direct directory asynchronous abort") << false << false << true << false;
        QTest::newRow("direct directory forced abort and destroy") << true << false << true << false;
        QTest::newRow("recycle completes after cancellation") << false << true << false << true;
        QTest::newRow("direct file completes after cancellation") << false << false << false << true;
        QTest::newRow("direct file completes after destruction") << true << false << false << true;
    }

    void testRecycleAbortWaitsForOperation()
    {
        QFETCH(bool, destroyWhileRunning);
        QFETCH(bool, recycle);
        QFETCH(bool, directory);
        QFETCH(bool, succeedAfterCancellation);
        QVERIFY(!OwncloudPropagator::hasPendingLocalFileOperations());
        FakeFolder folder{FileInfo::A12_B12_C12_S12(), {}, {}, false};
        if (!Account::enforcedServerUrl().isEmpty()) {
            QVERIFY(folder.account()->setUrl(Account::enforcedServerUrl()));
        }
        QVERIFY(folder.syncJournal().open());
        SyncJournalFileRecord originalRecord;
        originalRecord._path = directory ? QByteArrayLiteral("A") : QByteArrayLiteral("A/a1");
        originalRecord._type = directory ? ItemTypeDirectory : ItemTypeFile;
        originalRecord._fileId = QByteArrayLiteral("recycle-test-id");
        originalRecord._etag = QByteArrayLiteral("recycle-test-etag");
        originalRecord._modtime = QFileInfo(folder.localPath() + QStringLiteral("A/a1")).lastModified().toSecsSinceEpoch();
        originalRecord._fileSize = QFileInfo(folder.localPath() + QStringLiteral("A/a1")).size();
        QVERIFY(folder.syncJournal().setFileRecord(originalRecord));
        QSet<QString> bulkUploadBlacklist;
        OwncloudPropagator propagator(folder.account(), folder.localPath(), QStringLiteral("/"), &folder.syncJournal(), bulkUploadBlacklist);
        auto options = folder.syncEngine().syncOptions();
        options._vfs.reset(new RecycleTestVfs);
        options._moveFilesToTrash = recycle;
        propagator.setSyncOptions(options);
        const auto item = SyncFileItemPtr(new SyncFileItem);
        item->_file = QString::fromUtf8(originalRecord._path);
        item->_originalFile = item->_file;
        item->_instruction = CSYNC_INSTRUCTION_REMOVE;
        item->_direction = SyncFileItem::Down;
        item->_type = originalRecord._type;
        auto job = std::unique_ptr<PropagateItemJob>(propagator.createJob(item));
        QSignalSpy finished(job.get(), &PropagatorJob::finished);
        QSignalSpy aborted(job.get(), &PropagatorJob::abortFinished);
        QSignalSpy propagationFinished(&propagator, &OwncloudPropagator::finished);
        const auto started = std::make_shared<std::atomic_bool>(false);
        const auto release = std::make_shared<QSemaphore>();
        auto &operationOverride = recycle ? moveToTrashOverride : removeFileOverride;
        QScopedValueRollback overrideScope(operationOverride);
        operationOverride = [started, release, succeedAfterCancellation](const QString &path, QString *error) {
            started->store(true);
            release->tryAcquire(1, 2000);
            if (succeedAfterCancellation) {
                return QFile::remove(path);
            }
            *error = QStringLiteral("Simulated recycle failure");
            return false;
        };
        const auto drainWorker = qScopeGuard([&] {
            job.reset();
            release->release();
            QThreadPool::globalInstance()->waitForDone();
        });
        QVERIFY(job->scheduleSelfOrChild());
        QVERIFY(OwncloudPropagator::hasPendingLocalFileOperations());
        QTRY_VERIFY(started->load());
        job->abort(destroyWhileRunning ? PropagatorJob::Synchronous : PropagatorJob::Asynchronous);
        QCOMPARE(aborted.count(), 0);
        if (destroyWhileRunning) {
            job.reset();
        }
        QVERIFY(OwncloudPropagator::hasPendingLocalFileOperations());
        QVERIFY(QMetaObject::invokeMethod(&propagator, "abortTimeout", Qt::DirectConnection));
        QVERIFY(QMetaObject::invokeMethod(&propagator, "emitFinished", Qt::DirectConnection, Q_ARG(OCC::SyncFileItem::Status, SyncFileItem::NormalError)));
        const auto callbackServiced = std::make_shared<std::atomic_bool>(false);
        QMetaObject::invokeMethod(
            this,
            [callbackServiced] {
                callbackServiced->store(true);
            },
            Qt::QueuedConnection);
        QTRY_VERIFY(callbackServiced->load());
        QCOMPARE(propagationFinished.count(), 0);
        release->release();
        if (destroyWhileRunning) {
            QVERIFY(QThreadPool::globalInstance()->waitForDone(3000));
            QCoreApplication::processEvents();
            QCOMPARE(aborted.count(), 0);
        } else {
            QTRY_COMPARE(aborted.count(), 1);
        }
        QCOMPARE(finished.count(), 0);
        SyncJournalFileRecord record;
        QVERIFY(folder.syncJournal().getFileRecord(item->_file, &record));
        QCOMPARE(record.isValid(), destroyWhileRunning || !succeedAfterCancellation);
        QCOMPARE(QFileInfo::exists(folder.localPath() + item->_file), !succeedAfterCancellation);
        QVERIFY(propagator._activeJobList.isEmpty());
        QVERIFY(!OwncloudPropagator::hasPendingLocalFileOperations());
        QTRY_COMPARE(propagationFinished.count(), 1);
        QVERIFY(QMetaObject::invokeMethod(&propagator, "abortTimeout", Qt::DirectConnection));
        QVERIFY(QMetaObject::invokeMethod(&propagator, "emitFinished", Qt::DirectConnection, Q_ARG(OCC::SyncFileItem::Status, SyncFileItem::NormalError)));
        QCOMPARE(propagationFinished.count(), 1);
    }

    void testDirectDirectoryRemovalPreservesFailedChildren_data()
    {
        QTest::addColumn<bool>("cancelDuringSecondChild");
        QTest::newRow("second child locked") << false;
        QTest::newRow("cancel during second child") << true;
    }

    void testDirectDirectoryRemovalPreservesFailedChildren()
    {
        QFETCH(bool, cancelDuringSecondChild);
        FakeFolder folder{FileInfo::A12_B12_C12_S12(), {}, {}, false};
        if (!Account::enforcedServerUrl().isEmpty()) {
            QVERIFY(folder.account()->setUrl(Account::enforcedServerUrl()));
        }
        QVERIFY(folder.syncJournal().open());
        QVERIFY(QDir(folder.localPath()).mkpath(QStringLiteral("A/z-empty")));
        for (const auto &path : {QByteArrayLiteral("A"), QByteArrayLiteral("A/a1"), QByteArrayLiteral("A/a2"), QByteArrayLiteral("A/z-empty")}) {
            SyncJournalFileRecord record;
            record._path = path;
            record._type = path == QByteArrayLiteral("A") || path.endsWith("z-empty") ? ItemTypeDirectory : ItemTypeFile;
            record._fileId = QByteArrayLiteral("direct-removal-") + path;
            record._etag = QByteArrayLiteral("direct-removal-etag");
            const auto fileInfo = QFileInfo(folder.localPath() + QString::fromUtf8(path));
            record._modtime = fileInfo.lastModified().toSecsSinceEpoch();
            record._fileSize = fileInfo.size();
            QVERIFY(folder.syncJournal().setFileRecord(record));
        }
        QSet<QString> bulkUploadBlacklist;
        OwncloudPropagator propagator(folder.account(), folder.localPath(), QStringLiteral("/"), &folder.syncJournal(), bulkUploadBlacklist);
        auto options = folder.syncEngine().syncOptions();
        options._moveFilesToTrash = false;
        propagator.setSyncOptions(options);
        const auto item = SyncFileItemPtr(new SyncFileItem);
        item->_file = QStringLiteral("A");
        item->_originalFile = item->_file;
        item->_instruction = CSYNC_INSTRUCTION_REMOVE;
        item->_direction = SyncFileItem::Down;
        item->_type = ItemTypeDirectory;
        auto job = std::unique_ptr<PropagateItemJob>(propagator.createJob(item));
        QSignalSpy finished(job.get(), &PropagatorJob::finished);
        QSignalSpy aborted(job.get(), &PropagatorJob::abortFinished);
        auto journalUpdatedOnOwner = true;
        connect(
            job.get(),
            &PropagatorJob::finished,
            this,
            [&] {
                journalUpdatedOnOwner = QThread::currentThread() == thread();
            },
            Qt::DirectConnection);
        QScopedValueRollback overrideScope(removeFileOverride);
        removeFileOverride = [this, guardedJob = QPointer<PropagateItemJob>(job.get()), cancelDuringSecondChild](const QString &path, QString *error) {
            if (path.endsWith(QStringLiteral("/a2"))) {
                if (cancelDuringSecondChild) {
                    const auto callback = std::make_shared<QSemaphore>();
                    QMetaObject::invokeMethod(
                        this,
                        [guardedJob, callback] {
                            if (guardedJob) {
                                guardedJob->abort(PropagatorJob::Asynchronous);
                            }
                            callback->release();
                        },
                        Qt::QueuedConnection);
                    callback->tryAcquire(1, 2000);
                }
                *error = QStringLiteral("Simulated locked child");
                return false;
            }
            return QFile::remove(path);
        };
        const auto drainWorker = qScopeGuard([&] {
            job.reset();
            QThreadPool::globalInstance()->waitForDone();
        });
        QVERIFY(job->scheduleSelfOrChild());
        if (cancelDuringSecondChild) {
            QTRY_COMPARE(aborted.count(), 1);
            QCOMPARE(finished.count(), 0);
        } else {
            QTRY_COMPARE(finished.count(), 1);
            QCOMPARE(item->_status, SyncFileItem::NormalError);
        }
        QVERIFY(journalUpdatedOnOwner);
        for (const auto &path : {QStringLiteral("A"), QStringLiteral("A/a1"), QStringLiteral("A/a2"), QStringLiteral("A/z-empty")}) {
            SyncJournalFileRecord record;
            QVERIFY(folder.syncJournal().getFileRecord(path, &record));
            const auto retained = path == QStringLiteral("A/z-empty") ? cancelDuringSecondChild : path != QStringLiteral("A/a1");
            QCOMPARE(record.isValid(), retained);
            QCOMPARE(QFileInfo::exists(folder.localPath() + path), retained);
        }
        QVERIFY(propagator._activeJobList.isEmpty());
        QVERIFY(!OwncloudPropagator::hasPendingLocalFileOperations());
    }

    void testDeleteDirectoryWithNewFile()
    {
        FakeFolder fakeFolder{FileInfo::A12_B12_C12_S12()};

        // Remove a directory on the server with new files on the client
        fakeFolder.remoteModifier().remove("A");
        fakeFolder.localModifier().insert("A/hello.txt");

        // Symmetry
        fakeFolder.localModifier().remove("B");
        fakeFolder.remoteModifier().insert("B/hello.txt");

        QVERIFY(fakeFolder.syncOnce());

        // A/a1 must be gone because the directory was removed on the server, but hello.txt must be there
        QVERIFY(!fakeFolder.currentRemoteState().find("A/a1"));
        QVERIFY(!fakeFolder.currentRemoteState().find("A/hello.txt"));

        // Symmetry
        QVERIFY(!fakeFolder.currentRemoteState().find("B/b1"));
        QVERIFY(!fakeFolder.currentRemoteState().find("B/hello.txt"));

        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
    }

    void issue1329()
    {
        FakeFolder fakeFolder{FileInfo::A12_B12_C12_S12()};

        fakeFolder.localModifier().remove("B");
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        // Add a directory that was just removed in the previous sync:
        fakeFolder.localModifier().mkdir("B");
        fakeFolder.localModifier().insert("B/b1");
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(fakeFolder.currentRemoteState().find("B/b1"));
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
    }
};

QTEST_GUILESS_MAIN(TestSyncDelete)
#include "testsyncdelete.moc"
