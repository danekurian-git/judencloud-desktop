/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: CC0-1.0
 */

#include <QtTest>

#include <QScopeGuard>
#include <QStandardPaths>
#include <QUrl>

#include "configfile.h"
#include "managedsettingstestutils.h"
#include "theme.h"
#include "urischemehandler.h"
#include "account.h"
#include "config.h"

using namespace OCC;
using namespace Qt::StringLiterals;

Q_DECLARE_METATYPE(OCC::UriSchemeHandler::Action)

class TestUriSchemeHandler : public QObject
{
    Q_OBJECT

    static QUrl allowedServerUrl(const QString &fallback)
    {
        const auto enforced = Account::enforcedServerUrl();
        return enforced.isEmpty() ? QUrl(fallback) : enforced;
    }

    static QUrl loginUri(const QUrl &server)
    {
        return QUrl(QStringLiteral(APPLICATION_URI_HANDLER_SCHEME "://login/server:") + server.toString(QUrl::FullyEncoded));
    }

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    void parseUriHonoursCompiledServerRestriction()
    {
        const auto enforcedUrl = Account::enforcedServerUrl();
        if (enforcedUrl.isEmpty()) {
            QSKIP("This test requires a compiled server restriction.");
        }
        const auto prefix = QStringLiteral(APPLICATION_URI_HANDLER_SCHEME "://login/server:");
        const auto accepted = UriSchemeHandler::parseUri(QUrl(prefix + enforcedUrl.toString()));
        QCOMPARE(accepted.action, UriSchemeHandler::Action::Login);
        QCOMPARE(accepted.serverUrl, enforcedUrl);

        const auto rejected = UriSchemeHandler::parseUri(QUrl(prefix + QStringLiteral("https://unconfigured.example.com")));
        QCOMPARE(rejected.action, UriSchemeHandler::Action::Invalid);
        QVERIFY(!rejected.error.isEmpty());
    }

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

    void parseUri_data()
    {
        QTest::addColumn<QUrl>("url");
        QTest::addColumn<UriSchemeHandler::Action>("expectedAction");
        QTest::addColumn<QUrl>("expectedServerUrl");
        QTest::addColumn<bool>("expectError");

        const auto server = allowedServerUrl(QStringLiteral("https://cloud.example.com"));
        QTest::newRow("login") << loginUri(server) << UriSchemeHandler::Action::Login << server << false;
        QTest::newRow("login encoded server") << QUrl(QStringLiteral(APPLICATION_URI_HANDLER_SCHEME "://login/server:")
                                                      + QString::fromLatin1(QUrl::toPercentEncoding(server.toString())))
                                              << UriSchemeHandler::Action::Login << server << false;
        QTest::newRow("login uppercase host") << QUrl(QStringLiteral(APPLICATION_URI_HANDLER_SCHEME "://LOGIN/server:") + server.toString())
                                              << UriSchemeHandler::Action::Login << server << false;

        QTest::newRow("login missing server prefix") << QUrl(QStringLiteral("nc://login/https://cloud.example.com")) << UriSchemeHandler::Action::Invalid
                                                     << QUrl{} << true;

        QTest::newRow("login missing server url") << QUrl(QStringLiteral("nc://login/server:")) << UriSchemeHandler::Action::Invalid << QUrl{} << true;

        QTest::newRow("login relative server url") << QUrl(QStringLiteral("nc://login/server:cloud.example.com")) << UriSchemeHandler::Action::Invalid << QUrl{}
                                                   << true;

        QTest::newRow("login hostless server url") << QUrl(QStringLiteral("nc://login/server:https://")) << UriSchemeHandler::Action::Invalid << QUrl{} << true;

        QTest::newRow("login unsupported server url scheme")
            << QUrl(QStringLiteral("nc://login/server:ftp://cloud.example.com")) << UriSchemeHandler::Action::Invalid << QUrl{} << true;

        QTest::newRow("old add account") << QUrl(QStringLiteral("nc://addAccount/?serverUrl=https%3A%2F%2Fcloud.example.com"))
                                         << UriSchemeHandler::Action::Invalid << QUrl{} << true;

        QTest::newRow("local edit") << QUrl(QStringLiteral("nc://open/admin@cloud.example.com/Documents/file.txt?token=secret"))
                                    << UriSchemeHandler::Action::OpenLocalEdit << QUrl{} << false;

        QTest::newRow("unknown action") << QUrl(QStringLiteral("nc://unknown/")) << UriSchemeHandler::Action::Invalid << QUrl{} << true;
    }

    void parseUri()
    {
        QFETCH(QUrl, url);
        QFETCH(UriSchemeHandler::Action, expectedAction);
        QFETCH(QUrl, expectedServerUrl);
        QFETCH(bool, expectError);

        auto theme = Theme::instance();
        theme->setOverrideServerUrl(QString{});
        theme->setForceOverrideServerUrl(false);
        ConfigFile{}.setOverrideServerUrl(QString{});

        const auto result = UriSchemeHandler::parseUri(url);

        QCOMPARE(result.action, expectedAction);
        QCOMPARE(result.serverUrl, expectedServerUrl);
        QCOMPARE(result.error.isEmpty(), !expectError);
    }

    void parseUriHonoursForcedSingleServerOverride()
    {
        auto theme = Theme::instance();
        const auto previousOverrideServerUrl = theme->overrideServerUrl();
        const auto previousForceOverrideServerUrl = theme->forceOverrideServerUrl();
        const auto restoreTheme = qScopeGuard([theme, previousOverrideServerUrl, previousForceOverrideServerUrl] {
            theme->setOverrideServerUrl(previousOverrideServerUrl);
            theme->setForceOverrideServerUrl(previousForceOverrideServerUrl);
        });

        theme->setOverrideServerUrl(QStringLiteral("https://cloud.example.com"));
        theme->setForceOverrideServerUrl(true);

        auto server = allowedServerUrl(QStringLiteral("https://cloud.example.com"));
        server.setPath(server.path() + QLatin1Char('/'));
        const auto acceptedResult = UriSchemeHandler::parseUri(loginUri(server));
        QCOMPARE(acceptedResult.action, UriSchemeHandler::Action::Login);
        QCOMPARE(acceptedResult.serverUrl, server);
        QVERIFY(acceptedResult.error.isEmpty());

        const auto rejectedResult = UriSchemeHandler::parseUri(QUrl(QStringLiteral("nc://login/server:https://unconfigured.example.com")));
        QCOMPARE(rejectedResult.action, UriSchemeHandler::Action::Invalid);
        QVERIFY(!rejectedResult.error.isEmpty());
    }

    void parseUriHonoursConfiguredServerOverride()
    {
        ConfigFile cfg;
        const auto previousConfiguredOverrideServerUrl = cfg.overrideServerUrl();
        auto theme = Theme::instance();
        const auto previousOverrideServerUrl = theme->overrideServerUrl();
        const auto previousForceOverrideServerUrl = theme->forceOverrideServerUrl();
        const auto restoreState = qScopeGuard([&cfg, theme, previousConfiguredOverrideServerUrl, previousOverrideServerUrl, previousForceOverrideServerUrl] {
            cfg.setOverrideServerUrl(previousConfiguredOverrideServerUrl);
            theme->setOverrideServerUrl(previousOverrideServerUrl);
            theme->setForceOverrideServerUrl(previousForceOverrideServerUrl);
        });

        QCOMPARE(cfg.setOverrideServerUrl(QStringLiteral("https://configured.example.com")), Account::enforcedServerUrl().isEmpty());
        theme->setOverrideServerUrl(QString{});
        theme->setForceOverrideServerUrl(false);

        const auto server = allowedServerUrl(QStringLiteral("https://configured.example.com"));
        const auto acceptedResult = UriSchemeHandler::parseUri(loginUri(server));
        QCOMPARE(acceptedResult.action, UriSchemeHandler::Action::Login);
        QCOMPARE(acceptedResult.serverUrl, server);
        QVERIFY(acceptedResult.error.isEmpty());

        const auto rejectedResult = UriSchemeHandler::parseUri(QUrl(QStringLiteral("nc://login/server:https://unconfigured.example.com")));
        QCOMPARE(rejectedResult.action, UriSchemeHandler::Action::Invalid);
        QVERIFY(!rejectedResult.error.isEmpty());
    }

    void parseUriRestrictsOnlyForLockedManagedServerOverride_data()
    {
        QTest::addColumn<bool>("enforced");

        QTest::newRow("device default") << false;
        QTest::newRow("device policy") << true;
    }

    void parseUriRestrictsOnlyForLockedManagedServerOverride()
    {
        QFETCH(bool, enforced);

        ConfigFile cfg;
        auto theme = Theme::instance();
        const auto previousOverrideServerUrl = theme->overrideServerUrl();
        const auto previousForceOverrideServerUrl = theme->forceOverrideServerUrl();
        const auto restoreTheme = qScopeGuard([theme, previousOverrideServerUrl, previousForceOverrideServerUrl] {
            theme->setOverrideServerUrl(previousOverrideServerUrl);
            theme->setForceOverrideServerUrl(previousForceOverrideServerUrl);
        });

        cfg.clearOverrideServerUrl();
        theme->setOverrideServerUrl(QString{});
        theme->setForceOverrideServerUrl(false);
        ConfigFile::setDeviceSourcesFactory([enforced] {
            std::vector<std::unique_ptr<SettingSource>> sources;
            sources.push_back(std::make_unique<MapSource>(enforced ? SettingSourceType::PlatformPolicy : SettingSourceType::PlatformDefault,
                                                          enforced ? EnforcementState::Enforced : EnforcementState::NotEnforced,
                                                          enforced ? 200 : 20,
                                                          QVariantMap{{u"overrideServerUrl"_s, u"https://configured.example.com"_s}}));
            return sources;
        });

        const auto configuredResult = UriSchemeHandler::parseUri(QUrl(u"nc://login/server:https://configured.example.com"_s));
        const auto compiledRestriction = !Account::enforcedServerUrl().isEmpty();
        QCOMPARE(configuredResult.action, compiledRestriction ? UriSchemeHandler::Action::Invalid : UriSchemeHandler::Action::Login);
        QCOMPARE(configuredResult.error.isEmpty(), !compiledRestriction);
        if (compiledRestriction) {
            const auto fixedResult = UriSchemeHandler::parseUri(loginUri(Account::enforcedServerUrl()));
            QCOMPARE(fixedResult.action, UriSchemeHandler::Action::Login);
            QCOMPARE(fixedResult.serverUrl, Account::enforcedServerUrl());
        }

        const auto otherResult = UriSchemeHandler::parseUri(QUrl(u"nc://login/server:https://unconfigured.example.com"_s));
        QCOMPARE(otherResult.action, enforced || compiledRestriction ? UriSchemeHandler::Action::Invalid : UriSchemeHandler::Action::Login);
    }

    void parseUriHonoursMultipleServerOverrides()
    {
        auto theme = Theme::instance();
        const auto previousOverrideServerUrl = theme->overrideServerUrl();
        const auto previousForceOverrideServerUrl = theme->forceOverrideServerUrl();
        const auto restoreTheme = qScopeGuard([theme, previousOverrideServerUrl, previousForceOverrideServerUrl] {
            theme->setOverrideServerUrl(previousOverrideServerUrl);
            theme->setForceOverrideServerUrl(previousForceOverrideServerUrl);
        });

        theme->setOverrideServerUrl(
            QStringLiteral("["
                           R"({"name": "Primary", "url": "https://primary.example.com"},)"
                           R"({"name": "Secondary", "url": "https://secondary.example.com"})"
                           "]"));
        theme->setForceOverrideServerUrl(true);

        const auto server = allowedServerUrl(QStringLiteral("https://secondary.example.com"));
        const auto acceptedResult = UriSchemeHandler::parseUri(loginUri(server));
        QCOMPARE(acceptedResult.action, UriSchemeHandler::Action::Login);
        QCOMPARE(acceptedResult.serverUrl, server);
        QVERIFY(acceptedResult.error.isEmpty());
        if (!Account::enforcedServerUrl().isEmpty()) {
            for (const auto &alternative : {QStringLiteral("https://primary.example.com"), QStringLiteral("https://secondary.example.com")}) {
                const auto denied = UriSchemeHandler::parseUri(loginUri(QUrl(alternative)));
                QCOMPARE(denied.action, UriSchemeHandler::Action::Invalid);
                QVERIFY(!denied.error.isEmpty());
            }
        }

        const auto rejectedResult = UriSchemeHandler::parseUri(QUrl(QStringLiteral("nc://login/server:https://unconfigured.example.com")));
        QCOMPARE(rejectedResult.action, UriSchemeHandler::Action::Invalid);
        QVERIFY(!rejectedResult.error.isEmpty());
    }
};

QTEST_APPLESS_MAIN(TestUriSchemeHandler)
#include "testurischemehandler.moc"
