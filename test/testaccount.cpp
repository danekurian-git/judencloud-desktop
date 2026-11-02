/*
 * SPDX-FileCopyrightText: 2021 Nextcloud GmbH and Nextcloud contributors
 * SPDX-FileCopyrightText: 2017 ownCloud GmbH
 * SPDX-License-Identifier: CC0-1.0
 *
 * This software is in the public domain, furnished "as is", without technical
 * support, and with no warranty, express or implied, as to its usefulness for
 * any purpose.
 */

#include <qglobal.h>
#include <QTemporaryDir>
#include <QtTest>

#include "account.h"
#include "accountstate.h"
#include "logger.h"
#include "syncenginetestutils.h"
#include <QScopeGuard>
#include <QSettings>
#include "configfile.h"
#include "theme.h"

using namespace OCC;
using namespace Qt::StringLiterals;

class TestAccount : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        OCC::Logger::instance()->setLogFlush(true);
        OCC::Logger::instance()->setLogDebug(true);

        QStandardPaths::setTestModeEnabled(true);
    }

    void internalLinkUrl_data()
    {
        QTest::addColumn<QString>("baseUrl");
        QTest::addColumn<QVariantMap>("capabilities");
        QTest::addColumn<QByteArray>("fileId");
        QTest::addColumn<QString>("expectedUrl");

        const auto globalScale = QVariantMap{{u"globalscale"_s, QVariantMap{{u"token"_s, u"abc123"_s}}}};
        QTest::newRow("ordinary") << u"https://example.com"_s << QVariantMap{} << "42"_ba << u"https://example.com/index.php/f/42"_s;
        QTest::newRow("older-gss") << u"https://example.com"_s << QVariantMap{{u"globalscale"_s, QVariantMap{{u"enabled"_s, true}}}} << "42"_ba
                                   << u"https://example.com/index.php/f/42"_s;
        QTest::newRow("gss-token") << u"https://example.com"_s << globalScale << "42"_ba << u"https://example.com/index.php/gf/abc123/42"_s;
        QTest::newRow("subdirectory") << u"https://example.com/nextcloud/"_s << globalScale << "42"_ba
                                      << u"https://example.com/nextcloud/index.php/gf/abc123/42"_s;
        QTest::newRow("ordinary-subdirectory") << u"https://example.com/nextcloud"_s << QVariantMap{} << "42"_ba
                                               << u"https://example.com/nextcloud/index.php/f/42"_s;
        QTest::newRow("empty-token") << u"https://example.com"_s << QVariantMap{{u"globalscale"_s, QVariantMap{{u"token"_s, QString{}}}}} << "42"_ba
                                     << u"https://example.com/index.php/f/42"_s;
        QTest::newRow("malformed-token") << u"https://example.com"_s << QVariantMap{{u"globalscale"_s, QVariantMap{{u"token"_s, 123}}}} << "42"_ba
                                         << u"https://example.com/index.php/f/42"_s;
        QTest::newRow("encoded-token") << u"https://example.com"_s << QVariantMap{{u"globalscale"_s, QVariantMap{{u"token"_s, u"a/b?#%"_s}}}} << "42"_ba
                                       << u"https://example.com/index.php/gf/a%2Fb%3F%23%25/42"_s;
        QTest::newRow("encoded-base-path") << u"https://example.com/cloud%20files/"_s << globalScale << "42"_ba
                                           << u"https://example.com/cloud%20files/index.php/gf/abc123/42"_s;
        QTest::newRow("encoded-id") << u"https://example.com"_s << globalScale << "42/7?#%"_ba << u"https://example.com/index.php/gf/abc123/42%2F7%3F%23%25"_s;
        QTest::newRow("empty-id") << u"https://example.com"_s << globalScale << QByteArray{} << QString{};
        QTest::newRow("empty-id-without-gss") << u"https://example.com"_s << QVariantMap{} << QByteArray{} << QString{};
    }

    void internalLinkUrl()
    {
        QFETCH(QString, baseUrl);
        QFETCH(QVariantMap, capabilities);
        QFETCH(QByteArray, fileId);
        QFETCH(QString, expectedUrl);
        const auto account = Account::create();
        // This fixture exercises URL formatting independently of server admission.
        account->_url = QUrl{baseUrl};
        account->setCapabilities(capabilities);
        QCOMPARE(account->internalLinkUrl(fileId).toString(QUrl::FullyEncoded), expectedUrl);
    }

    void testAccount_urlPropertyCannotBypassSetter()
    {
        const auto account = Account::create();
        const auto otherUrl = QUrl(QStringLiteral("https://unconfigured.example.com"));
        QVERIFY(!account->setProperty("url", otherUrl));
        QVERIFY(account->url().isEmpty());

        const auto enforcedUrl = Account::enforcedServerUrl();
        const auto initialUrl = enforcedUrl.isEmpty() ? QUrl(QStringLiteral("https://initial.example.com")) : enforcedUrl;
        QVERIFY(account->setUrl(initialUrl));
        QCOMPARE(account->property("url").toUrl(), initialUrl);
        QVERIFY(!account->setProperty("url", otherUrl));
        QCOMPARE(account->url(), initialUrl);
        QCOMPARE(account->property("url").toUrl(), initialUrl);
    }

    void testAccountDavPath_unitialized_noCrash()
    {
        AccountPtr account = Account::create();
        [[maybe_unused]] const auto davPath = account->davPath();
    }

    void testAppPasswordUsesCanonicalIdentity()
    {
        const auto account = Account::create();
        account->_id = QStringLiteral("remote-wipe-unit-test");
        QVERIFY(account->setUrl(Account::enforcedServerUrl().isEmpty() ? QUrl(QStringLiteral("https://cloud.example")) : Account::enforcedServerUrl()));
        auto *credentials = new FakeCredentials(new FakeQNAM({}));
        credentials->setUserName(QStringLiteral("login-alias"));
        account->setCredentials(credentials);
        account->setDavUser(QStringLiteral("canonical-user-id"));
        const auto canonicalKey = AbstractCredentials::keychainKey(account->url().toString(), QStringLiteral("canonical-user-id_app-password"), account->id());
        const auto legacyKey = AbstractCredentials::keychainKey(account->url().toString(), QStringLiteral("login-alias_app-password"), account->id());
        auto store = QHash<QString, QByteArray>{};
        auto operations = QStringList{};
        account->_appPasswordStoreOverride =
            [&store, &operations](Account::AppPasswordOperation op, const QString &key, const QByteArray &data, Account::AppPasswordCallback callback) {
                operations.append(key);
                if (op == Account::AppPasswordOperation::Write) {
                    store.insert(key, data);
                    callback(Account::AppPasswordResult::Success, {});
                } else if (op == Account::AppPasswordOperation::Delete) {
                    store.remove(key);
                    callback(Account::AppPasswordResult::Success, {});
                } else {
                    callback(store.contains(key) ? Account::AppPasswordResult::Success : Account::AppPasswordResult::Missing, store.value(key));
                }
            };
        auto retrieved = QSignalSpy(account.data(), &Account::appPasswordRetrieved);
        account->writeAppPasswordOnce(QStringLiteral("synthetic-token"));
        account->writeAppPasswordOnce(QStringLiteral("must-not-overwrite"));
        QCOMPARE(store.value(canonicalKey), QByteArrayLiteral("synthetic-token"));
        QVERIFY(!store.contains(legacyKey));
        account->retrieveAppPassword();
        QCOMPARE(retrieved.takeFirst().at(0).toString(), QStringLiteral("synthetic-token"));
        QCOMPARE(operations, QStringList({canonicalKey, canonicalKey}));

        // Older alias-keyed entries are tried only when the canonical entry is absent.
        store.remove(canonicalKey);
        store.insert(legacyKey, QByteArrayLiteral("legacy-token"));
        operations.clear();
        account->retrieveAppPassword();
        QCOMPARE(retrieved.takeFirst().at(0).toString(), QStringLiteral("legacy-token"));
        QCOMPARE(operations, QStringList({canonicalKey, legacyKey}));
        store.insert(canonicalKey, QByteArrayLiteral("current-token"));
        account->deleteAppPassword();
        QVERIFY(store.isEmpty());
        account->writeAppPasswordOnce(QStringLiteral("replacement-token"));
        QCOMPARE(store.value(canonicalKey), QByteArrayLiteral("replacement-token"));
    }

    void testAppPasswordReadFailures_data()
    {
        QTest::addColumn<bool>("missing");
        QTest::newRow("unavailable-keychain") << false;
        QTest::newRow("missing-token") << true;
    }

    void testAppPasswordReadFailures()
    {
        QFETCH(bool, missing);
        const auto account = Account::create();
        account->_id = QStringLiteral("remote-wipe-unit-test");
        QVERIFY(account->setUrl(Account::enforcedServerUrl().isEmpty() ? QUrl(QStringLiteral("https://cloud.example")) : Account::enforcedServerUrl()));
        account->setCredentials(new FakeCredentials(new FakeQNAM({})));
        account->setDavUser(QStringLiteral("canonical-user-id"));
        auto reads = 0;
        account->_appPasswordStoreOverride =
            [&reads, missing](Account::AppPasswordOperation op, const QString &, const QByteArray &, Account::AppPasswordCallback callback) {
                QCOMPARE(op, Account::AppPasswordOperation::Read);
                ++reads;
                callback(missing ? Account::AppPasswordResult::Missing : Account::AppPasswordResult::Error, {});
            };
        auto retrieved = QSignalSpy(account.data(), &Account::appPasswordRetrieved);
        account->retrieveAppPassword();
        QCOMPARE(retrieved.count(), 1);
        QVERIFY(retrieved.at(0).at(0).toString().isEmpty());
        QCOMPARE(reads, missing ? 2 : 1);
    }

    void testAppPasswordReadCannotCrossAccountChange()
    {
        const auto account = Account::create();
        account->_id = QStringLiteral("remote-wipe-unit-test");
        QVERIFY(account->setUrl(Account::enforcedServerUrl().isEmpty() ? QUrl(QStringLiteral("https://cloud.example")) : Account::enforcedServerUrl()));
        account->setCredentials(new FakeCredentials(new FakeQNAM({})));
        account->setDavUser(QStringLiteral("first-user"));
        auto pending = Account::AppPasswordCallback{};
        account->_appPasswordStoreOverride =
            [&pending](Account::AppPasswordOperation, const QString &, const QByteArray &, Account::AppPasswordCallback callback) {
                pending = std::move(callback);
            };
        auto retrieved = QSignalSpy(account.data(), &Account::appPasswordRetrieved);
        account->retrieveAppPassword();
        QVERIFY(pending);
        account->setDavUser(QStringLiteral("second-user"));
        pending(Account::AppPasswordResult::Success, QByteArrayLiteral("first-users-token"));
        QCOMPARE(retrieved.count(), 1);
        QVERIFY(retrieved.at(0).at(0).toString().isEmpty());
    }

    void testExplicitSignOutStaysSignedOutWhileDisconnected()
    {
        const auto account = Account::create();
        QVERIFY(account->setUrl(Account::enforcedServerUrl().isEmpty() ? QUrl(QStringLiteral("https://cloud.example")) : Account::enforcedServerUrl()));
        account->setCredentials(new FakeCredentials(new FakeQNAM({})));
        AccountState state(account);
        QCOMPARE(state.state(), AccountState::Disconnected);
        state.signOutByUi();
        QCOMPARE(state.state(), AccountState::SignedOut);
        QCoreApplication::processEvents();
        QCOMPARE(state.state(), AccountState::SignedOut);
    }

    void testAccount_serverRestriction_data()
    {
        QTest::addColumn<QUrl>("url");
        QTest::addColumn<bool>("allowedWhenEnforced");

        auto base = Account::enforcedServerUrl();
        if (base.isEmpty()) {
            base = QUrl(QStringLiteral("https://cloud.monocloud.online"));
        }
        QTest::newRow("compiled-server") << base << true;
        auto candidate = base;
        candidate.setPath(base.path() + QLatin1Char('/'));
        QTest::newRow("trailing-slash") << candidate << true;
        candidate = base;
        candidate.setPort(443);
        QTest::newRow("default-port") << candidate << true;
        candidate = base;
        candidate.setHost(base.host().toUpper());
        QTest::newRow("host-case") << candidate << true;
        candidate = base;
        candidate.setHost(QStringLiteral("unconfigured.example.com"));
        QTest::newRow("other-host") << candidate << false;
        candidate = base;
        candidate.setHost(base.host() + QStringLiteral(".example.com"));
        QTest::newRow("host-suffix") << candidate << false;
        candidate = base;
        candidate.setScheme(QStringLiteral("http"));
        QTest::newRow("http") << candidate << false;
        candidate = base;
        candidate.setPort(8443);
        QTest::newRow("other-port") << candidate << false;
        candidate = base;
        candidate.setPath(QStringLiteral("/other-server"));
        QTest::newRow("other-base-path") << candidate << false;
        candidate = base;
        candidate.setUserName(QStringLiteral("user"));
        QTest::newRow("userinfo") << candidate << false;
        candidate = base;
        candidate.setQuery(QStringLiteral("server=other"));
        QTest::newRow("query") << candidate << false;
        candidate = base;
        candidate.setFragment(QStringLiteral("other"));
        QTest::newRow("fragment") << candidate << false;
        QTest::newRow("relative") << QUrl(QStringLiteral("/other-server")) << false;
        QTest::newRow("empty") << QUrl{} << false;
    }

    void testAccount_serverRestriction()
    {
        QFETCH(QUrl, url);
        QFETCH(bool, allowedWhenEnforced);

        const auto allowed = Account::enforcedServerUrl().isEmpty() || allowedWhenEnforced;
        const auto account = Account::create();
        QCOMPARE(Account::isServerUrlAllowed(url), allowed);
        QCOMPARE(account->setUrl(url), allowed);
        QCOMPARE(account->url(), allowed ? url : QUrl{});
    }

    void testAccount_serverRestrictionPreservesExistingUrl()
    {
        const auto account = Account::create();
        const auto enforcedUrl = Account::enforcedServerUrl();
        const auto initialUrl = enforcedUrl.isEmpty() ? QUrl(QStringLiteral("https://initial.example.com")) : enforcedUrl;
        const auto otherUrl = QUrl(QStringLiteral("https://unconfigured.example.com"));
        QVERIFY(account->setUrl(initialUrl));

        QCOMPARE(account->setUrl(otherUrl), enforcedUrl.isEmpty());
        QCOMPARE(account->url(), enforcedUrl.isEmpty() ? otherUrl : initialUrl);
    }

    void testAccount_compiledServerRestrictionIgnoresRuntimeOverrides()
    {
        const auto enforcedUrl = Account::enforcedServerUrl();
        if (enforcedUrl.isEmpty()) {
            QSKIP("This test requires a compiled server restriction.");
        }

        auto config = ConfigFile{};
        auto settings = QSettings(config.configFile(), QSettings::IniFormat);
        const auto previousSetting = settings.value(QStringLiteral("overrideServerUrl"));
        const auto theme = Theme::instance();
        const auto previousThemeUrl = theme->overrideServerUrl();
        const auto restore = qScopeGuard([&settings, previousSetting, theme, previousThemeUrl] {
            if (previousSetting.isValid()) {
                settings.setValue(QStringLiteral("overrideServerUrl"), previousSetting);
            } else {
                settings.remove(QStringLiteral("overrideServerUrl"));
            }
            settings.sync();
            theme->setOverrideServerUrl(previousThemeUrl);
        });
        const auto otherUrl = QStringLiteral("https://unconfigured.example.com");
        settings.setValue(QStringLiteral("overrideServerUrl"), otherUrl);
        settings.sync();
        theme->setOverrideServerUrl(otherUrl);
        config.setOverrideServerUrl(otherUrl);

        QCOMPARE(config.overrideServerUrl(), enforcedUrl.toString());
        QVERIFY(!Account::isServerUrlAllowed(QUrl(otherUrl)));
        QVERIFY(Account::isServerUrlAllowed(enforcedUrl));
    }

    void testAccount_isPublicShareLink_data()
    {
        QTest::addColumn<QString>("url");
        QTest::addColumn<bool>("expectedResult");
        QTest::addColumn<QString>("expectedDavUser");

        QTest::newRow("plain URL") << "https://example.com" << false << "";
        QTest::newRow("plain URL, trailing slash") << "https://example.com/" << false << "";
        QTest::newRow("share link") << "https://example.com/s/rPZLaTKfWct37Nd" << true << "rPZLaTKfWct37Nd";
        QTest::newRow("share link, trailing slash") << "https://example.com/s/rPZLaTKfWct37Nd/" << false << "";
        QTest::newRow("subpath containing /s/ (looks like share link)") << "https://example.com/s/nextcloud" << true << "nextcloud";
        QTest::newRow("subpath containing /s/, trailing slash") << "https://example.com/s/nextcloud/" << false << "";
        QTest::newRow("subpath containing /s/, share link") << "https://example.com/s/nextcloud/s/rPZLaTKfWct37Nd" << true << "rPZLaTKfWct37Nd";
        QTest::newRow("subpath containing /s/, share link, trailing slash") << "https://example.com/s/nextcloud/s/rPZLaTKfWct37Nd/" << false << "";
    }

    void testAccount_isPublicShareLink()
    {
        AccountPtr account = Account::create();

        QFETCH(QString, url);
        QFETCH(bool, expectedResult);
        QFETCH(QString, expectedDavUser);

        account->setUrl(QUrl{url});
        QCOMPARE(account->isPublicShareLink(), expectedResult);
        QCOMPARE(account->davUser(), expectedDavUser);
    }

    void testAccount_setLimitSettings_globalNetworkLimitFallback()
    {
        using LimitSetting = Account::AccountNetworkTransferLimitSetting;
        AccountPtr account = Account::create();

        const auto setLimitSettings = [account](const LimitSetting setting) -> void {
            account->setDownloadLimitSetting(setting);
            account->setUploadLimitSetting(setting);
        };

        const auto verifyLimitSettings = [account](const LimitSetting expectedSetting) -> void {
            QCOMPARE_EQ(expectedSetting, account->downloadLimitSetting());
            QCOMPARE_EQ(expectedSetting, account->uploadLimitSetting());
        };

        // the default setting should be NoLimit
        verifyLimitSettings(LimitSetting::NoLimit);

        // changing it to ManualLimit should succeed
        setLimitSettings(LimitSetting::ManualLimit);
        verifyLimitSettings(LimitSetting::ManualLimit);

        // changing it to AutoLimit should fall back to NoLimit
        setLimitSettings(LimitSetting::AutoLimit);
        verifyLimitSettings(LimitSetting::NoLimit);

        // changing it to LegacyGlobalLimit (-2) should fall back to NoLimit
        setLimitSettings(LimitSetting::LegacyGlobalLimit);
        verifyLimitSettings(LimitSetting::NoLimit);
    }

    void testExternalStoragePlaceholders_data()
    {
        QTest::addColumn<QString>("mountType");
        QTest::addColumn<QString>("subPath");
        QTest::addColumn<QStringList>("expectedPaths");
        QTest::newRow("external entry") << QStringLiteral("external") << QString{} << QStringList{"B", "C", "S"};
        QTest::newRow("external root") << QStringLiteral("external") << QStringLiteral("A/") << QStringList{};
        QTest::newRow("external-session root") << QStringLiteral("external-session") << QStringLiteral("A/") << QStringList{};
        QTest::newRow("group root") << QStringLiteral("group") << QStringLiteral("A/") << QStringList{"A/a1", "A/a2"};
        QTest::newRow("shared root") << QStringLiteral("shared") << QStringLiteral("A/") << QStringList{"A/a1", "A/a2"};
    }

    void testExternalStoragePlaceholders()
    {
        QFETCH(QString, mountType);
        QFETCH(QString, subPath);
        QFETCH(QStringList, expectedPaths);
        FakeFolder folder{FileInfo::A12_B12_C12_S12()};
        folder.remoteModifier().find("A")->extraDavProperties = QStringLiteral("<nc:mount-type>%1</nc:mount-type>").arg(mountType).toUtf8();
        folder.syncJournal().clearFileTable();
        auto promise = QPromise<OCC::PlaceholderCreateInfo>{};
        const auto future = promise.future();
        folder.account()->listRemoteFolder(&promise, {}, subPath, &folder.syncJournal());
        QTRY_VERIFY(future.isFinished());
        QStringList paths;
        for (const auto &placeholder : future.results()) {
            paths.append(placeholder.fullPath);
        }
        paths.sort();
        expectedPaths.sort();
        QCOMPARE(paths, expectedPaths);
    }

    void testAccount_listRemoteFolder_data()
    {
        QTest::addColumn<QString>("remotePath");
        QTest::addColumn<QString>("subPath");
        QTest::addColumn<QStringList>("expectedPaths");

        QTest::newRow("root = /; requesting ''") << "" << "" << QStringList{"A", "B", "C", "S"};
        QTest::newRow("root = /; requesting A/") << "" << "A/" << QStringList{"A/a1", "A/a2", "A/sub1"};
        QTest::newRow("root = /; requesting A/sub1/") << "" << "A/sub1/" << QStringList{"A/sub1/sub2"};
        QTest::newRow("root = /; requesting A/sub1/sub2") << "" << "A/sub1/sub2/" << QStringList{};
        QTest::newRow("root = /; requesting A/sub_nodownload/") << "" << "A/sub_nodownload/" << QStringList{};
        QTest::newRow("root = /A; requesting A/") << "/A" << "A/" << QStringList{"a1", "a2", "sub1"};
        QTest::newRow("root = /A; requesting A/sub1") << "/A" << "A/sub1/" << QStringList{"sub1/sub2"};
        QTest::newRow("root = /A; requesting A/sub1/sub2") << "/A" << "A/sub1/sub2/" << QStringList{};
        QTest::newRow("root = /A; requesting A/sub_nodownload/") << "/A" << "A/sub_nodownload/" << QStringList{};
        QTest::newRow("root = /shared_nodownload; requesting shared_nodownload/") << "/shared_nodownload" << "shared_nodownload/" << QStringList{};
    }

    void testAccount_listRemoteFolder()
    {
        QFETCH(QString, remotePath);
        QFETCH(QString, subPath);
        QFETCH(QStringList, expectedPaths);

        FakeFolder fakeFolder{FileInfo::A12_B12_C12_S12()};
        fakeFolder.remoteModifier().mkdir("A/sub1");
        fakeFolder.remoteModifier().mkdir("A/sub1/sub2");
        fakeFolder.remoteModifier().mkdir("A/sub1/sub2");

        // prepare shares that are not allowed to be downloaded
        std::function<void(FileInfo &)> makeSharedNoDownload;
        makeSharedNoDownload = [&makeSharedNoDownload](FileInfo &fileInfo) -> void {
            fileInfo.isShared = true;
            fileInfo.downloadForbidden = true;
            for (auto &subItem : fileInfo.children) {
                makeSharedNoDownload(subItem);
            }
        };
        FileInfo sharedNoDownload{"shared_nodownload"_L1, {{"file1"_L1, 32}, {"file2"_L1, 32}}};
        FileInfo sharedNoDownloadSub{"subdir"_L1, {{"file3"_L1, 32}, {"file4"_L1, 32}}};
        FileInfo aSubSharedNoDownload{"sub_nodownload"_L1, {{"file4"_L1, 32}, {"file5"_L1, 32}}};
        sharedNoDownload.children.insert(sharedNoDownloadSub.name, std::move(sharedNoDownloadSub));
        makeSharedNoDownload(sharedNoDownload);
        makeSharedNoDownload(aSubSharedNoDownload);
        fakeFolder.remoteModifier().children.insert(sharedNoDownload.name, std::move(sharedNoDownload));
        fakeFolder.remoteModifier().find("A")->children.insert(aSubSharedNoDownload.name, std::move(aSubSharedNoDownload));

        const auto account = fakeFolder.account();

        QEventLoop localEventLoop;
        auto lsPropPromise = QPromise<OCC::PlaceholderCreateInfo>{};
        auto lsPropFuture = lsPropPromise.future();
        auto lsPropFutureWatcher = QFutureWatcher<OCC::PlaceholderCreateInfo>{};
        lsPropFutureWatcher.setFuture(lsPropFuture);

        QList<QString> receivedPaths;

        QObject::connect(&lsPropFutureWatcher, &QFutureWatcher<QStringList>::finished, &localEventLoop, [&localEventLoop]() {
            qInfo() << "ls prop finished";
            localEventLoop.quit();
        });

        QObject::connect(&lsPropFutureWatcher,
                         &QFutureWatcher<QStringList>::resultReadyAt,
                         &localEventLoop,
                         [&receivedPaths, &lsPropFutureWatcher](int resultIndex) {
                             qInfo() << "ls prop result at index" << resultIndex;
                             const auto &newResultEntries = lsPropFutureWatcher.resultAt(resultIndex);
                             receivedPaths.append(newResultEntries.fullPath);
                         });

        fakeFolder.syncJournal().clearFileTable(); // pretend we only need to fetch placeholders

        account->listRemoteFolder(&lsPropPromise, remotePath, subPath, &fakeFolder.syncJournal());
        localEventLoop.exec();

        receivedPaths.sort();
        expectedPaths.sort();
        QCOMPARE_EQ(receivedPaths, expectedPaths);
    }
};

QTEST_GUILESS_MAIN(TestAccount)
#include "testaccount.moc"
