/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "libsync/privatecloudmanagedstate.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

using namespace OCC;

class TestPrivateCloudManagedState : public QObject
{
    Q_OBJECT
    const QString _server = QStringLiteral("https://cloud.monocloud.online");
    const QString _user = QStringLiteral("canonical-dav-user-123");

    PrivateCloudManagedState store(const QTemporaryDir &directory) const
    {
        return PrivateCloudManagedState(directory.filePath(QStringLiteral("Management")), _server);
    }

    QString makeShare(const QTemporaryDir &directory, const QString &name = QStringLiteral("Share")) const
    {
        const auto path = directory.filePath(name);
        return QDir().mkpath(path) ? path : QString();
    }

    static bool createRedirect(const QString &link, const QString &target)
    {
#ifdef Q_OS_WIN
        QProcess process;
        process.start(QStringLiteral("cmd.exe"),
                      {QStringLiteral("/d"),
                       QStringLiteral("/c"),
                       QStringLiteral("mklink"),
                       QStringLiteral("/J"),
                       QDir::toNativeSeparators(link),
                       QDir::toNativeSeparators(target)});
        return process.waitForFinished() && process.exitCode() == 0;
#else
        return QFile::link(target, link);
#endif
    }

    static void removeRedirect(const QString &link)
    {
#ifdef Q_OS_WIN
        const auto native = QDir::toNativeSeparators(link);
        RemoveDirectoryW(reinterpret_cast<LPCWSTR>(native.utf16()));
#else
        QFile::remove(link);
#endif
    }

private Q_SLOTS:
    void persistsAuthorizationAndCompletionAcrossInstances()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto share = makeShare(directory);
        QVERIFY(!share.isEmpty());
        QVERIFY(!store(directory).isBlocked(_server, _user));
        QVERIFY(store(directory).begin(_server, _user, {share}).isEmpty());
        QVERIFY(store(directory).isBlocked(_server + u'/', _user));
        QVERIFY(!store(directory).isBlocked(_server, QStringLiteral("another-user")));
        auto record = PrivateCloudManagedState::Record();
        QVERIFY(store(directory).read(_server, _user, &record).isEmpty());
        QCOMPARE(record.server, _server);
        QCOMPARE(record.user, _user);
        QCOMPARE(record.status, PrivateCloudManagedState::Status::Pending);
        QCOMPARE(record.roots.size(), 1);
        QCOMPARE(record.roots.first().path, QDir::fromNativeSeparators(share));
        QVERIFY(!record.roots.first().identity.isEmpty());
        QVERIFY(PrivateCloudManagedState::validateRoot(record.roots.first()).isEmpty());
        QVERIFY(QDir().rmdir(share));
        QVERIFY(store(directory).finish(_server, _user, true).isEmpty());
        QVERIFY(store(directory).isBlocked(_server, _user));
        QVERIFY(store(directory).read(_server, _user, &record).isEmpty());
        QCOMPARE(record.status, PrivateCloudManagedState::Status::Completed);
        QVERIFY(!store(directory).begin(_server, _user, {share}).isEmpty());
        QVERIFY(!store(directory).finish(_server, _user, false).isEmpty());
    }

    void failedCleanupKeepsRootsAndLock()
    {
        QTemporaryDir directory;
        const auto share = makeShare(directory);
        QVERIFY(store(directory).begin(_server, _user, {share}).isEmpty());
        const auto expectedError = QStringLiteral("A local file is in use.");
        QVERIFY(store(directory).finish(_server, _user, false, expectedError).isEmpty());
        auto record = PrivateCloudManagedState::Record();
        QVERIFY(store(directory).read(_server, _user, &record).isEmpty());
        QCOMPARE(record.status, PrivateCloudManagedState::Status::Failed);
        QCOMPARE(record.error, expectedError);
        QCOMPARE(record.roots.size(), 1);
        QVERIFY(store(directory).isBlocked(_server, _user));
    }

    void rejectsWrongServerOrNonCanonicalIdentity_data()
    {
        QTest::addColumn<QString>("server");
        QTest::addColumn<QString>("user");
        QTest::newRow("http") << QStringLiteral("http://cloud.monocloud.online") << _user;
        QTest::newRow("other-host") << QStringLiteral("https://example.test") << _user;
        QTest::newRow("embedded-password") << QStringLiteral("https://alice:secret@cloud.monocloud.online") << _user;
        QTest::newRow("path") << _server + QStringLiteral("/other") << _user;
        QTest::newRow("query") << _server + QStringLiteral("?redirect=1") << _user;
        QTest::newRow("fragment") << _server + QStringLiteral("#fragment") << _user;
        QTest::newRow("port") << _server + QStringLiteral(":444") << _user;
        QTest::newRow("no-user") << _server << QString();
        QTest::newRow("line-break") << _server << QStringLiteral("user\nother");
    }

    void rejectsWrongServerOrNonCanonicalIdentity()
    {
        QFETCH(QString, server);
        QFETCH(QString, user);
        QTemporaryDir directory;
        QVERIFY(!store(directory).begin(server, user, {makeShare(directory)}).isEmpty());
        QVERIFY(store(directory).isBlocked(server, user));
        QVERIFY(!store(directory).isBlocked(_server, _user));
    }

    void rejectsUnsafeRootsButKeepsAccountBlocked_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::newRow("volume-root") << QStringLiteral("volume");
        QTest::newRow("home-root") << QStringLiteral("home");
        QTest::newRow("application-root") << QStringLiteral("application");
        QTest::newRow("source-root") << QStringLiteral("source");
        QTest::newRow("source-parent") << QStringLiteral("source-parent");
        QTest::newRow("source-child") << QStringLiteral("source-child");
        QTest::newRow("state-parent") << QStringLiteral("state-parent");
        QTest::newRow("relative") << QStringLiteral("relative");
        QTest::newRow("missing") << QStringLiteral("missing");
        QTest::newRow("duplicates") << QStringLiteral("duplicates");
        QTest::newRow("overlap") << QStringLiteral("overlap");
#ifdef Q_OS_WIN
        QTest::newRow("source-dot-alias") << QStringLiteral("source-dot-alias");
        QTest::newRow("source-space-alias") << QStringLiteral("source-space-alias");
#endif
    }

    void rejectsUnsafeRootsButKeepsAccountBlocked()
    {
        QFETCH(QString, kind);
        QTemporaryDir directory;
        const auto share = makeShare(directory);
        const auto child = makeShare(directory, QStringLiteral("Share/Child"));
        const auto source = makeShare(directory, QStringLiteral("Source"));
        auto roots = QStringList();
        auto protectedPaths = QStringList{source};
        if (kind == QStringLiteral("volume")) {
            roots = {QDir::rootPath()};
        } else if (kind == QStringLiteral("home")) {
            roots = {QDir::homePath()};
        } else if (kind == QStringLiteral("application")) {
            roots = {QCoreApplication::applicationDirPath()};
        } else if (kind == QStringLiteral("source")) {
            roots = {source};
        } else if (kind == QStringLiteral("source-dot-alias")) {
            roots = {source + u'.'};
        } else if (kind == QStringLiteral("source-space-alias")) {
            roots = {source + u' '};
        } else if (kind == QStringLiteral("source-child")) {
            roots = {makeShare(directory, QStringLiteral("Source/Child"))};
        } else if (kind == QStringLiteral("source-parent") || kind == QStringLiteral("state-parent")) {
            roots = {directory.path()};
            if (kind == QStringLiteral("state-parent")) {
                protectedPaths.clear();
            }
        } else if (kind == QStringLiteral("relative")) {
            roots = {QStringLiteral("relative/share")};
        } else if (kind == QStringLiteral("missing")) {
            roots = {directory.filePath(QStringLiteral("Missing"))};
        } else if (kind == QStringLiteral("duplicates")) {
            roots = {share, share};
        } else {
            roots = {share, child};
        }
        QVERIFY(!store(directory).begin(_server, _user, roots, protectedPaths).isEmpty());
        QVERIFY(store(directory).isBlocked(_server, _user));
        auto record = PrivateCloudManagedState::Record();
        QVERIFY(store(directory).read(_server, _user, &record).isEmpty());
        QCOMPARE(record.status, PrivateCloudManagedState::Status::Failed);
        QVERIFY(record.roots.isEmpty());
        QVERIFY(!record.error.isEmpty());
    }

    void rejectsRootReplacement()
    {
        QTemporaryDir directory;
        const auto share = makeShare(directory);
        QVERIFY(store(directory).begin(_server, _user, {share}).isEmpty());
        auto record = PrivateCloudManagedState::Record();
        QVERIFY(store(directory).read(_server, _user, &record).isEmpty());
        QVERIFY(QDir().rename(share, directory.filePath(QStringLiteral("Original"))));
        QVERIFY(QDir().mkpath(share));
        QVERIFY(!PrivateCloudManagedState::validateRoot(record.roots.first()).isEmpty());
        QVERIFY(QDir(share).rmdir(share));
        QVERIFY(!PrivateCloudManagedState::validateRoot(record.roots.first()).isEmpty());
    }

    void corruptStateBlocksReconnectAndCannotBeOverwritten()
    {
        QTemporaryDir directory;
        const auto share = makeShare(directory);
        QVERIFY(store(directory).begin(_server, _user, {share}).isEmpty());
        const auto files = QDir(store(directory).storageDirectory()).entryList({QStringLiteral("*.json")}, QDir::Files);
        QCOMPARE(files.size(), 1);
        QFile file(QDir(store(directory).storageDirectory()).filePath(files.first()));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write("{invalid-json"), 13);
        file.close();
        auto record = PrivateCloudManagedState::Record();
        QVERIFY(store(directory).isBlocked(_server, _user));
        QVERIFY(!store(directory).read(_server, _user, &record).isEmpty());
        QVERIFY(!store(directory).begin(_server, _user, {share}).isEmpty());
        QVERIFY(!store(directory).finish(_server, _user, true).isEmpty());
    }

    void unwritableStatePreventsAuthorizationSuccess()
    {
        QTemporaryDir directory;
        const auto blockedDirectory = directory.filePath(QStringLiteral("NotDirectory"));
        QFile file(blockedDirectory);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        const auto state = PrivateCloudManagedState(blockedDirectory + QStringLiteral("/Management"), _server);
        QVERIFY(!state.begin(_server, _user, {makeShare(directory)}).isEmpty());
    }

    void accountWithoutShareStillLocks()
    {
        QTemporaryDir directory;
        QVERIFY(store(directory).begin(_server, _user, {}).isEmpty());
        QVERIFY(store(directory).isBlocked(_server, _user));
        QVERIFY(store(directory).finish(_server, _user, true).isEmpty());
    }

    void rejectsRedirectedRootAndAncestor()
    {
        QTemporaryDir directory;
        const auto target = makeShare(directory, QStringLiteral("Target"));
        QVERIFY(QDir().mkpath(target + QStringLiteral("/Child")));
        const auto link = directory.filePath(QStringLiteral("Redirect"));
        QVERIFY(createRedirect(link, target));
        const auto removeLink = qScopeGuard([&] {
            removeRedirect(link);
        });
        QVERIFY(!store(directory).begin(_server, _user, {link}).isEmpty());
        QVERIFY(store(directory).isBlocked(_server, _user));
        QVERIFY(!store(directory).begin(_server, QStringLiteral("second-user"), {link + QStringLiteral("/Child")}).isEmpty());
        QVERIFY(store(directory).isBlocked(_server, QStringLiteral("second-user")));
        QVERIFY(QDir(target + QStringLiteral("/Child")).exists());
    }

    void redirectedManagementDirectoryCannotUnlockAccount_data()
    {
        QTest::addColumn<bool>("redirectParent");
        QTest::newRow("management-directory") << false;
        QTest::newRow("management-ancestor") << true;
    }

    void redirectedManagementDirectoryCannotUnlockAccount()
    {
        QFETCH(bool, redirectParent);
        QTemporaryDir directory;
        const auto parent = makeShare(directory, QStringLiteral("StateParent"));
        const auto storage = parent + QStringLiteral("/Management");
        const auto state = PrivateCloudManagedState(storage, _server);
        QVERIFY(state.begin(_server, _user, {makeShare(directory)}).isEmpty());
        const auto link = redirectParent ? parent : storage;
        QVERIFY(QDir().rename(link, directory.filePath(QStringLiteral("OriginalState"))));
        const auto emptyTarget = makeShare(directory, QStringLiteral("EmptyTarget"));
        QVERIFY(createRedirect(link, emptyTarget));
        const auto removeLink = qScopeGuard([&] {
            removeRedirect(link);
        });
        QVERIFY(state.isBlocked(_server, _user));
        auto record = PrivateCloudManagedState::Record();
        QVERIFY(!state.read(_server, _user, &record).isEmpty());
        QVERIFY(!state.begin(_server, _user, {}).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestPrivateCloudManagedState)
#include "testprivatecloudmanagedstate.moc"
