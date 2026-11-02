/*
 * SPDX-FileCopyrightText: 2020 Nextcloud GmbH and Nextcloud contributors
 * SPDX-FileCopyrightText: 2014 ownCloud GmbH
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "account.h"
#include "propagatedownloadencrypted.h"
#include "propagatorjobs.h"
#include "owncloudpropagator.h"
#include "owncloudpropagator_p.h"
#include "propagateremotemove.h"
#include "common/utility.h"
#include "common/syncjournaldb.h"
#include "common/syncjournalfilerecord.h"
#include "filesystem.h"
#include <qfile.h>
#include <qdir.h>
#include <qdiriterator.h>
#include <qtemporaryfile.h>
#include <qsavefile.h>
#include <QDateTime>
#include <qstack.h>
#include <QCoreApplication>
#include <QtConcurrentRun>
#include <QScopeGuard>

#include <filesystem>
#include <ctime>
#include <optional>


namespace OCC {

Q_LOGGING_CATEGORY(lcPropagateLocalRemove, "nextcloud.sync.propagator.localremove", QtInfoMsg)
Q_LOGGING_CATEGORY(lcPropagateLocalMkdir, "nextcloud.sync.propagator.localmkdir", QtInfoMsg)
Q_LOGGING_CATEGORY(lcPropagateLocalRename, "nextcloud.sync.propagator.localrename", QtInfoMsg)

namespace {

std::atomic_uint pendingLocalFileOperations = 0;

std::optional<QString> journalRelativePath(const QString &syncRoot, const QString &filesystemPath)
{
    const auto normalizedRoot = QDir::cleanPath(QDir::fromNativeSeparators(syncRoot));
    const auto normalizedPath = QDir::cleanPath(QDir::fromNativeSeparators(filesystemPath));
    if (normalizedRoot.isEmpty() || normalizedPath.isEmpty()) {
        return std::nullopt;
    }

    auto relativePath = QDir::fromNativeSeparators(QDir{normalizedRoot}.relativeFilePath(normalizedPath));
    relativePath = QDir::cleanPath(relativePath);
    if (relativePath == QLatin1Char('.')) {
        relativePath.clear();
    }
    if (QDir::isAbsolutePath(relativePath) || relativePath == QStringLiteral("..")
        || relativePath.startsWith(QStringLiteral("../"))) {
        return std::nullopt;
    }
    return relativePath;
}

} // namespace

QByteArray localFileIdFromFullId(const QByteArray &id)
{
    return id.left(8);
}

bool OwncloudPropagator::hasPendingLocalFileOperations()
{
    return pendingLocalFileOperations.load() != 0;
}

PropagateLocalRemove::PendingLocalFileOperation::PendingLocalFileOperation()
{
    ++pendingLocalFileOperations;
}

PropagateLocalRemove::PendingLocalFileOperation::~PendingLocalFileOperation()
{
    --pendingLocalFileOperations;
}

PropagateLocalRemove::~PropagateLocalRemove()
{
    _cancelled->store(true);
}

void PropagateLocalRemove::abort(PropagatorJob::AbortType abortType)
{
    _cancelled->store(true);
    if (_removalWatcher) {
        if (abortType == AbortType::Asynchronous) {
            _abortPending = true;
            return;
        }
        // A native recycle operation cannot be interrupted. Its worker owns only
        // copied paths; detach completion instead of blocking the hydration thread.
        disconnect(_removalWatcher, nullptr, this, nullptr);
        propagator()->_activeJobList.removeAll(this);
        _removalWatcher = nullptr;
        _pendingLocalFileOperation.reset();
        return;
    }
    PropagatorJob::abort(abortType);
}

PropagateLocalRemove::RemovalResult PropagateLocalRemove::removeRecursively(const QString &absolute,
                                                                            const QString &syncRoot,
                                                                            const QSet<QString> &deleteToClientTrashBin,
                                                                            const std::shared_ptr<std::atomic_bool> &cancelled)
{
    RemovalResult result;
    if (cancelled->load()) {
        return result;
    }
    const auto parentFolderPath = QFileInfo(absolute).dir().absolutePath();
    const auto parentPermissionsHandler = FileSystem::FilePermissionsRestore{parentFolderPath, FileSystem::FolderPermissions::ReadWrite};
    result.success = FileSystem::removeRecursively(
        absolute,
        [&result, &syncRoot](const QString &path, bool isDir) {
            const auto relativePath = journalRelativePath(syncRoot, path);
            if (!relativePath) {
                result.error = QStringLiteral("Filesystem path is outside the sync root");
                return;
            }
            result.deleted.prepend(qMakePair(*relativePath, isDir));
        },
        nullptr,
        nullptr,
        [&deleteToClientTrashBin, &cancelled](const QString &path, QString *error) {
            if (cancelled->load()) {
                return false;
            }
            if (deleteToClientTrashBin.contains(path) && FileSystem::moveToTrash(path, error)) {
                return true;
            }
            return !cancelled->load() && FileSystem::remove(path, error);
        },
        [cancelled] {
            return cancelled->load();
        });
    return result;
}

bool PropagateLocalRemove::updateJournalAfterRemoval(const RemovalResult &result, bool aborted)
{
    if (!result.success || aborted) {
        QString deletedDir;
        for (const auto &entry : result.deleted) {
            if (isPathInsideDeletedDir(entry.first, deletedDir)) {
                continue;
            }
            if (propagator()->_journal->deleteFileRecord(entry.first, entry.second)) {
                if (entry.second) {
                    deletedDir = entry.first;
                }
            } else {
                qCWarning(lcPropagateLocalRemove) << "Failed to delete file record from local DB" << entry.first;
            }
        }
    }
    if (!result.error.isEmpty()) {
        qCWarning(lcPropagateLocalRemove) << result.error;
        return false;
    }
    return result.success;
}

PropagateLocalRemove::RemovalResult PropagateLocalRemove::removeLocalItem(const QString &filename,
                                                                          const QString &relativePath,
                                                                          const QString &syncRoot,
                                                                          bool isDirectory,
                                                                          bool moveToTrash,
                                                                          bool allowTrash,
                                                                          const QSet<QString> &deleteToClientTrashBin,
                                                                          const std::shared_ptr<std::atomic_bool> &cancelled)
{
    RemovalResult result;
    if (cancelled->load()) {
        return result;
    }
    if (OwncloudPropagator::localFileNameClash(syncRoot, relativePath)) {
        result.nameClash = true;
        return result;
    }

    const auto fileInfo = QFileInfo{filename};
    if (!FileSystem::fileExists(filename, fileInfo)) {
        result.success = true;
        result.deleted.append(qMakePair(relativePath, isDirectory));
        return result;
    }

    auto moveToTrashIsFeasible = allowTrash;
    if (moveToTrash && moveToTrashIsFeasible) {
        if (fileInfo.isDir()) {
            try {
                if (FileSystem::isFolderReadOnly(fileInfo.filesystemAbsolutePath())) {
                    moveToTrashIsFeasible = false;
                }
            } catch (const std::exception &error) {
                qCWarning(lcPropagateLocalRemove) << "exception when checking parent folder read only status" << error.what();
            } catch (...) {
                qCWarning(lcPropagateLocalRemove) << "exception when checking parent folder read only status";
            }
        } else if (!FileSystem::isWritable(filename, fileInfo)) {
            moveToTrashIsFeasible = false;
        }
    }
    if (cancelled->load()) {
        return result;
    }
    if (moveToTrash && moveToTrashIsFeasible) {
        const auto parentFolderPath = fileInfo.dir().absolutePath();
        const auto parentPermissionsHandler = FileSystem::FilePermissionsRestore{parentFolderPath, FileSystem::FolderPermissions::ReadWrite};
        if (!cancelled->load()) {
            result.success = FileSystem::moveToTrash(filename, &result.error);
        }
        if (result.success) {
            result.deleted.append(qMakePair(relativePath, isDirectory));
        }
        return result;
    }
    if (isDirectory) {
        return removeRecursively(filename, syncRoot, deleteToClientTrashBin, cancelled);
    }

    const auto parentFolderPath = fileInfo.dir().absolutePath();
    const auto parentPermissionsHandler = FileSystem::FilePermissionsRestore{parentFolderPath, FileSystem::FolderPermissions::ReadWrite};
    if (!cancelled->load()) {
        result.success = FileSystem::remove(filename, &result.error);
    }
    if (result.success) {
        result.deleted.append(qMakePair(relativePath, false));
    }
    return result;
}

void PropagateLocalRemove::startAsyncRemoval(const QString &filename)
{
    // Native permission checks and deletion can trigger a virus scanner to hydrate
    // a placeholder. Keep the account thread free to service those callbacks.
    auto *watcher = new QFutureWatcher<RemovalResult>(this);
    _removalWatcher = watcher;
    propagator()->_activeJobList.append(this);
    connect(watcher, &QFutureWatcher<RemovalResult>::finished, this, [this, watcher] {
        const auto result = watcher->result();
        _removalWatcher = nullptr;
        watcher->deleteLater();
        propagator()->_activeJobList.removeAll(this);
        if (_abortPending) {
            // A native operation may finish after cancellation. Reconcile only
            // entries actually deleted while this journal owner is still alive.
            updateJournalAfterRemoval(result, true);
            propagator()->_journal->commit("Aborted local remove");
            _pendingLocalFileOperation.reset();
            _state = Finished;
            Q_EMIT abortFinished();
            return;
        }
        if (result.nameClash) {
            _pendingLocalFileOperation.reset();
            //: %1 is the local path of the file that could not be removed.
            done(SyncFileItem::FileNameClash,
                 tr("Could not remove %1 because of a local file name clash").arg(QDir::toNativeSeparators(propagator()->fullLocalPath(_item->_file))),
                 ErrorCategory::GenericError);
            return;
        }
        const auto removalSucceeded = updateJournalAfterRemoval(result);
        _pendingLocalFileOperation.reset();
        if (!removalSucceeded) {
            done(SyncFileItem::NormalError, tr("Temporary error when removing local item removed from server."), ErrorCategory::GenericError);
            return;
        }
        completeRemoval();
    });
    _pendingLocalFileOperation = std::make_shared<PendingLocalFileOperation>();
    watcher->setFuture(QtConcurrent::run([filename,
                                          relativePath = _item->_file,
                                          isDirectory = _item->isDirectory(),
                                          moveToTrash = _moveToTrash,
                                          allowTrash = propagator()->syncOptions()._vfs->mode() == Vfs::WindowsCfApi,
                                          syncRoot = propagator()->localPath(),
                                          deleteToClientTrashBin = _deleteToClientTrashBin,
                                          cancelled = _cancelled,
                                          pendingOperation = _pendingLocalFileOperation] {
        try {
            return removeLocalItem(filename, relativePath, syncRoot, isDirectory, moveToTrash, allowTrash, deleteToClientTrashBin, cancelled);
        } catch (const std::exception &error) {
            RemovalResult result;
            result.error = QString::fromUtf8(error.what());
            return result;
        } catch (...) {
            RemovalResult result;
            result.error = QStringLiteral("Unexpected exception while removing a local item");
            return result;
        }
    }));
}

void PropagateLocalRemove::start()
{
    qCInfo(lcPropagateLocalRemove) << "Start propagate local remove job";
    qCInfo(lcPermanentLog) << "delete" << _item->_file << _item->_discoveryResult;

    _moveToTrash = propagator()->syncOptions()._moveFilesToTrash || _item->_wantsSpecificActions == SyncFileItem::SynchronizationOptions::MoveToClientTrashBin;

    if (propagator()->_abortRequested) {
        return;
    }

    const QString filename = propagator()->fullLocalPath(_item->_file);
    qCInfo(lcPropagateLocalRemove) << "Going to delete:" << filename;

    if (_item->isDirectory()) {
        Q_EMIT propagator()->touchedFile(filename);
    }
    startAsyncRemoval(filename);
}

void PropagateLocalRemove::completeRemoval()
{
    propagator()->reportProgress(*_item, 0);
    if (!propagator()->_journal->deleteFileRecord(_item->_originalFile, _item->isDirectory())) {
        qCWarning(lcPropagateLocalRemove()) << "could not delete file from local DB" << _item->_originalFile;
        done(SyncFileItem::NormalError, tr("Could not delete file record %1 from local DB").arg(_item->_originalFile), ErrorCategory::GenericError);
        return;
    }
    propagator()->_journal->commit("Local remove");
    done(SyncFileItem::Success, {}, ErrorCategory::NoError);
}

void PropagateLocalMkdir::start()
{
    if (propagator()->_abortRequested) {
        return;
    }

    startLocalMkdir();
}

void PropagateLocalMkdir::setDeleteExistingFile(bool enabled)
{
    _deleteExistingFile = enabled;
}

void PropagateLocalMkdir::startLocalMkdir()
{
    QDir newDir(propagator()->fullLocalPath(_item->_file));
    QString newDirStr = QDir::toNativeSeparators(newDir.path());

    // When turning something that used to be a file into a directory
    // we need to delete the file first.
    if (FileSystem::fileExists(newDirStr) && FileSystem::isFile(newDirStr)) {
        if (_deleteExistingFile) {
            QString removeError;
            if (!FileSystem::remove(newDirStr, &removeError)) {
                done(SyncFileItem::NormalError,
                    tr("could not delete file %1, error: %2")
                        .arg(newDirStr, removeError), ErrorCategory::GenericError);
                return;
            }
        } else if (_item->_instruction == CSYNC_INSTRUCTION_CONFLICT) {
            QString error;
            if (!propagator()->createConflict(_item, _associatedComposite, &error)) {
                done(SyncFileItem::SoftError, error, ErrorCategory::GenericError);
                return;
            }
        }
    }

    if (Utility::fsCasePreserving() && propagator()->localFileNameClash(_item->_file)) {
        qCWarning(lcPropagateLocalMkdir) << "New folder to create locally already exists with different case:" << _item->_file;
        done(SyncFileItem::FileNameClash, tr("Folder %1 cannot be created because of a local file or folder name clash!").arg(newDirStr), ErrorCategory::GenericError);
        return;
    }

    auto parentFolderPath = std::filesystem::path{};
    auto parentNeedRollbackPermissions = false;
    try {
        const auto newDirPath = std::filesystem::path{newDirStr.toStdWString()};
        Q_ASSERT(newDirPath.has_parent_path());
        parentFolderPath = newDirPath.parent_path();
        if (FileSystem::isFolderReadOnly(parentFolderPath)) {
            FileSystem::setFolderPermissions(QString::fromStdWString(parentFolderPath.wstring()), FileSystem::FolderPermissions::ReadWrite);
            parentNeedRollbackPermissions = true;
            Q_EMIT propagator()->touchedFile(QString::fromStdWString(parentFolderPath.wstring()));
        }
    }
    catch (const std::filesystem::filesystem_error &e)
    {
        qCWarning(lcPropagateLocalMkdir) << "exception when checking parent folder access rights" << e.what() << e.path1().c_str() << e.path2().c_str();
    }
    catch (const std::system_error &e)
    {
        qCWarning(lcPropagateLocalMkdir) << "exception when checking parent folder access rights" << e.what();
    }
    catch (...)
    {
        qCWarning(lcPropagateLocalMkdir) << "exception when checking parent folder access rights";
    }

    Q_EMIT propagator()->touchedFile(newDirStr);
    QDir localDir(propagator()->localPath());
    if (!localDir.mkpath(_item->_file)) {
        done(SyncFileItem::NormalError, tr("Could not create folder %1").arg(newDirStr), ErrorCategory::GenericError);
        return;
    }

    if (!_item->_remotePerm.isNull() &&
        !_item->_remotePerm.hasPermissionsForReadWrite()) {
        try {
            FileSystem::setFolderPermissions(newDirStr, FileSystem::FolderPermissions::ReadOnly);
        }
        catch (const std::filesystem::filesystem_error &e)
        {
            qCWarning(lcPropagateLocalMkdir) << "exception when checking parent folder access rights" << e.what() << e.path1().c_str() << e.path2().c_str();
            done(SyncFileItem::NormalError, tr("The folder %1 cannot be made read-only: %2").arg(_item->_file, e.what()), ErrorCategory::GenericError);
            return;
        }
        catch (const std::system_error &e)
        {
            qCWarning(lcPropagateLocalMkdir) << "exception when checking parent folder access rights" << e.what();
            done(SyncFileItem::NormalError, tr("The folder %1 cannot be made read-only: %2").arg(_item->_file, e.what()), ErrorCategory::GenericError);
            return;
        }
        catch (...)
        {
            qCWarning(lcPropagateLocalMkdir) << "exception when checking parent folder access rights";
            done(SyncFileItem::NormalError, tr("The folder %1 cannot be made read-only: %2").arg(_item->_file, tr("unknown exception")), ErrorCategory::GenericError);
            return;
        }
    }

    try {
        if (parentNeedRollbackPermissions) {
            FileSystem::setFolderPermissions(QString::fromStdWString(parentFolderPath.wstring()), FileSystem::FolderPermissions::ReadOnly);
            Q_EMIT propagator()->touchedFile(QString::fromStdWString(parentFolderPath.wstring()));
        }
    }
    catch (const std::filesystem::filesystem_error &e)
    {
        qCWarning(lcPropagateLocalMkdir) << "exception when checking parent folder access rights" << e.what() << e.path1().c_str() << e.path2().c_str();
    }
    catch (const std::system_error &e)
    {
        qCWarning(lcPropagateLocalMkdir) << "exception when checking parent folder access rights" << e.what();
    }
    catch (...)
    {
        qCWarning(lcPropagateLocalMkdir) << "exception when checking parent folder access rights";
    }

    // Insert the directory into the database. The correct etag will be set later,
    // once all contents have been propagated, because should_update_metadata is true.
    // Adding an entry with a dummy etag to the database still makes sense here
    // so the database is aware that this folder exists even if the sync is aborted
    // before the correct etag is stored.
    SyncFileItem newItem(*_item);
    newItem._etag = "_invalid_";
    const auto result = propagator()->updateMetadata(newItem);
    if (!result) {
        done(SyncFileItem::FatalError, tr("Error updating metadata: %1").arg(result.error()), ErrorCategory::GenericError);
        return;
    } else if (*result == Vfs::ConvertToPlaceholderResult::Locked) {
        done(SyncFileItem::SoftError, tr("The file %1 is currently in use").arg(newItem._file), ErrorCategory::GenericError);
        return;
    }
    propagator()->_journal->commit("localMkdir");

    auto resultStatus = _item->_instruction == CSYNC_INSTRUCTION_CONFLICT
        ? SyncFileItem::Conflict
        : SyncFileItem::Success;
    done(resultStatus, {}, ErrorCategory::NoError);
}

PropagateLocalRename::PropagateLocalRename(OwncloudPropagator *propagator, const SyncFileItemPtr &item)
    : PropagateItemJob(propagator, item)
{
    qCDebug(lcPropagateLocalRename) << _item->_file << _item->_renameTarget << _item->_originalFile;
}

void PropagateLocalRename::start()
{
    if (propagator()->_abortRequested) {
        return;
    }

    auto &vfs = propagator()->syncOptions()._vfs;
    const auto previousNameInDb = propagator()->adjustRenamedPath(_item->_file);
    const auto existingFile = propagator()->fullLocalPath(previousNameInDb);
    const auto targetFile = propagator()->fullLocalPath(_item->_renameTarget);
    const auto originalFile = propagator()->fullLocalPath(_item->_originalFile);

    const auto origExists = FileSystem::fileExists(originalFile);
    const auto existingExists = FileSystem::fileExists(existingFile);
    const auto targetExists = FileSystem::fileExists(targetFile);
    const auto fileAlreadyMoved = (!origExists || !existingExists) && targetExists;
    auto pinState = OCC::PinState::Unspecified;
    if (!fileAlreadyMoved) {
        auto pinStateResult = vfs->pinState(propagator()->adjustRenamedPath(_item->_file));
        if (pinStateResult) {
            pinState = pinStateResult.get();
        }
    }

    // if the file is a file underneath a moved dir, the _item->file is equal
    // to _item->renameTarget and the file is not moved as a result.
    qCDebug(lcPropagateLocalRename) << _item->_file << _item->_renameTarget << _item->_originalFile << previousNameInDb << (fileAlreadyMoved ? "original file has already moved" : "original file is still there");
    qCDebug(lcPropagateLocalRename()) << (origExists ? "original file exists" : "original file is missing") << originalFile << _item->_originalFile;
    qCDebug(lcPropagateLocalRename()) << (existingExists ? "existing file exists" : "existing file is missing") << existingFile << previousNameInDb;
    Q_ASSERT(origExists || existingExists);
    if (_item->_file != _item->_renameTarget) {
        propagator()->reportProgress(*_item, 0);
        qCDebug(lcPropagateLocalRename) << "MOVE " << existingFile << " => " << targetFile;

        if (QString::compare(_item->_file, _item->_renameTarget, Qt::CaseInsensitive) != 0
            && propagator()->localFileNameClash(_item->_renameTarget)) {

            qCInfo(lcPropagateLocalRename) << "renaming a case clashed item" << _item->_file << _item->_renameTarget;
            if (_item->isDirectory()) {
                // #HotFix
                // fix a crash (we can not create a conflicted copy for folders)
                // right now, the conflict resolution will not even work for this scenario with folders,
                // but, let's fix it step by step, this will be a second stage
                done(SyncFileItem::FileNameClash,
                     tr("Folder %1 cannot be renamed because of a local file or folder name clash!").arg(_item->_file),
                     ErrorCategory::GenericError);
                return;
            }
            const auto caseClashConflictResult = propagator()->createCaseClashConflict(_item, existingFile);
            if (caseClashConflictResult) {
                done(SyncFileItem::SoftError, *caseClashConflictResult, ErrorCategory::GenericError);
            } else {
                done(SyncFileItem::FileNameClash, tr("File %1 downloaded but it resulted in a local file name clash!").arg(QDir::toNativeSeparators(_item->_file)), ErrorCategory::GenericError);
            }
            return;
        }

        auto targetParentFolderPath = std::filesystem::path{};
        auto targetParentFolderWasReadOnly = false;
        try {
            const auto newDirPath = std::filesystem::path{targetFile.toStdWString()};
            Q_ASSERT(newDirPath.has_parent_path());
            targetParentFolderPath = newDirPath.parent_path();
            if (FileSystem::isFolderReadOnly(targetParentFolderPath)) {
                targetParentFolderWasReadOnly = true;
                FileSystem::setFolderPermissions(QString::fromStdWString(targetParentFolderPath.wstring()), FileSystem::FolderPermissions::ReadWrite);
                Q_EMIT propagator()->touchedFile(QString::fromStdWString(targetParentFolderPath.wstring()));
            }
        }
        catch (const std::filesystem::filesystem_error &e)
        {
            qCWarning(lcPropagateLocalRename) << "exception when checking parent folder access rights" << e.what() << e.path1().c_str() << e.path2().c_str();
        }
        catch (const std::system_error &e)
        {
            qCWarning(lcPropagateLocalRename) << "exception when checking parent folder access rights" << e.what();
        }
        catch (...)
        {
            qCWarning(lcPropagateLocalRename) << "exception when checking parent folder access rights";
        }

        auto originParentFolderPath = std::filesystem::path{};
        auto originParentFolderWasReadOnly = false;
        try {
            const auto newDirPath = std::filesystem::path{existingFile.toStdWString()};
            Q_ASSERT(newDirPath.has_parent_path());
            originParentFolderPath = newDirPath.parent_path();
            if (FileSystem::isFolderReadOnly(originParentFolderPath)) {
                originParentFolderWasReadOnly = true;
                FileSystem::setFolderPermissions(QString::fromStdWString(originParentFolderPath.wstring()), FileSystem::FolderPermissions::ReadWrite);
                Q_EMIT propagator()->touchedFile(QString::fromStdWString(originParentFolderPath.wstring()));
            }
        }
        catch (const std::filesystem::filesystem_error &e)
        {
            qCWarning(lcPropagateLocalRename) << "exception when checking parent folder access rights" << e.what() << e.path1().c_str() << e.path2().c_str();
        }
        catch (const std::system_error &e)
        {
            qCWarning(lcPropagateLocalRename) << "exception when checking parent folder access rights" << e.what();
        }
        catch (...)
        {
            qCWarning(lcPropagateLocalRename) << "exception when checking parent folder access rights";
        }

        const auto restoreTargetPermissions = [this] (const auto &parentFolderPath) {
            try {
                FileSystem::setFolderPermissions(QString::fromStdWString(parentFolderPath.wstring()), FileSystem::FolderPermissions::ReadOnly);
                Q_EMIT propagator()->touchedFile(QString::fromStdWString(parentFolderPath.wstring()));
            }
            catch (const std::filesystem::filesystem_error &e)
            {
                qCWarning(lcPropagateLocalRename) << "exception when checking parent folder access rights" << e.what() << e.path1().c_str() << e.path2().c_str();
            }
            catch (const std::system_error &e)
            {
                qCWarning(lcPropagateLocalRename) << "exception when checking parent folder access rights" << e.what();
            }
            catch (...)
            {
                qCWarning(lcPropagateLocalRename) << "exception when checking parent folder access rights";
            }
        };

        const auto folderPermissionsHandler = FileSystem::FilePermissionsRestore{existingFile, FileSystem::FolderPermissions::ReadWrite};

        Q_EMIT propagator()->touchedFile(existingFile);
        Q_EMIT propagator()->touchedFile(targetFile);
        if (QString renameError; !FileSystem::rename(existingFile, targetFile, &renameError)) {
            if (targetParentFolderWasReadOnly) {
                restoreTargetPermissions(targetParentFolderPath);
            }
            if (originParentFolderWasReadOnly) {
                restoreTargetPermissions(originParentFolderPath);
            }
            done(SyncFileItem::NormalError, renameError, ErrorCategory::GenericError);
            return;
        }

        if (targetParentFolderWasReadOnly) {
            restoreTargetPermissions(targetParentFolderPath);
        }
        if (originParentFolderWasReadOnly) {
            restoreTargetPermissions(originParentFolderPath);
        }
    }

    SyncJournalFileRecord oldRecord;
    if (!propagator()->_journal->getFileRecord(fileAlreadyMoved ? previousNameInDb : _item->_originalFile, &oldRecord)) {
        qCWarning(lcPropagateLocalRename) << "Could not get file from local DB" << _item->_originalFile;
        done(SyncFileItem::NormalError, tr("Could not get file %1 from local DB").arg(_item->_originalFile), ErrorCategory::GenericError);
        return;
    }

    if (fileAlreadyMoved && !deleteOldDbRecord(previousNameInDb)) {
        return;
    } else if (!deleteOldDbRecord(previousNameInDb)) {
        qCWarning(lcPropagateLocalRename) << "Could not delete file from local DB" << _item->_originalFile;
        return;
    }

    if (!vfs->setPinState(_item->_renameTarget, pinState)) {
        qCWarning(lcPropagateLocalRename) << "Could not set pin state of" << _item->_renameTarget << "to old value" << pinState;
        done(SyncFileItem::NormalError, tr("Error setting pin state"), ErrorCategory::GenericError);
        return;
    }

    const auto oldFile = _item->_file;

    if (!_item->isDirectory()) { // Directories are saved at the end
        auto newItem(*_item);
        if (oldRecord.isValid()) {
            newItem._checksumHeader = oldRecord._checksumHeader;
        }
        const auto result = propagator()->updateMetadata(newItem);
        if (!result) {
            done(SyncFileItem::FatalError, tr("Error updating metadata: %1").arg(result.error()), ErrorCategory::GenericError);
            return;
        } else if (*result == Vfs::ConvertToPlaceholderResult::Locked) {
            done(SyncFileItem::SoftError, tr("The file %1 is currently in use").arg(newItem._file), ErrorCategory::GenericError);
            return;
        }
    } else if (!fileAlreadyMoved) {
        qCDebug(lcPropagateLocalRename) << "propagate child items after move from" << existingFile << "to" << targetFile;
        const auto dbQueryResult = propagator()->_journal->getFilesBelowPath(previousNameInDb.toUtf8(), [previousNameInDb, this] (const SyncJournalFileRecord &record) -> void {
            const auto oldFileNameString = propagator()->adjustRenamedPath(QString::fromUtf8(record._path));
            auto newFileNameString = oldFileNameString;
            newFileNameString.replace(0, previousNameInDb.length(), _item->_renameTarget);

            qCDebug(lcPropagateLocalRename) << "child rename from" << oldFileNameString << "to" << newFileNameString;

            if (oldFileNameString == newFileNameString) {
                Q_ASSERT(false);
                return;
            }

            SyncJournalFileRecord oldRecord;
            if (!propagator()->_journal->getFileRecord(oldFileNameString, &oldRecord)) {
                qCWarning(lcPropagateLocalRename) << "Could not get file from local DB" << oldFileNameString;
                return;
            }
            if (!propagator()->_journal->deleteFileRecord(oldFileNameString)) {
                qCWarning(lcPropagateLocalRename) << "could not delete file from local DB" << oldFileNameString;
                return;
            }

            const auto newItem = SyncFileItem::fromSyncJournalFileRecord(oldRecord);
            newItem->_file = newFileNameString;
            newItem->_lockToken.clear();
            newItem->_locked = SyncFileItem::LockStatus::UnlockedItem;
            const auto result = propagator()->updateMetadata(*newItem);
            if (!result) {
                return;
            }
        });
        if (!dbQueryResult) {
            done(SyncFileItem::FatalError, tr("Failed to propagate directory rename in hierarchy"), OCC::ErrorCategory::GenericError);
            return;
        }
        propagator()->_renamedDirectories.insert(oldFile, _item->_renameTarget);
        if (!PropagateRemoteMove::adjustSelectiveSync(propagator()->_journal, oldFile, _item->_renameTarget)) {
            done(SyncFileItem::FatalError, tr("Failed to rename file"), ErrorCategory::GenericError);
            return;
        }
    }
    if (pinState != PinState::Inherited && !vfs->setPinState(_item->_renameTarget, pinState)) {
        done(SyncFileItem::NormalError, tr("Error setting pin state"), ErrorCategory::GenericError);
        return;
    }

    propagator()->_journal->commit("localRename");

    done(SyncFileItem::Success, {}, ErrorCategory::NoError);
}

bool PropagateLocalRename::deleteOldDbRecord(const QString &fileName)
{
    if (SyncJournalFileRecord oldRecord; !propagator()->_journal->getFileRecord(fileName, &oldRecord)) {
        qCWarning(lcPropagateLocalRename) << "Could not get file from local DB" << fileName;
        done(SyncFileItem::NormalError, tr("Could not get file %1 from local DB").arg(fileName), OCC::ErrorCategory::GenericError);
        return false;
    }
    if (!propagator()->_journal->deleteFileRecord(fileName)) {
        qCWarning(lcPropagateLocalRename) << "could not delete file from local DB" << fileName;
        done(SyncFileItem::NormalError, tr("Could not delete file record %1 from local DB").arg(fileName), OCC::ErrorCategory::GenericError);
        return false;
    }

    return true;
}
}

#include "moc_propagatorjobs.cpp"
