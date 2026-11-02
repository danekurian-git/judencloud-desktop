/*
 * SPDX-FileCopyrightText: 2021 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QTest>

#include "config.h"
#include "configfile.h"
#include "iconutils.h"
#include "logger.h"
#include "privateclouddrive.h"
#include "theme.h"
#include "themeutils.h"

#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>

using namespace Qt::StringLiterals;

class TestTheme : public QObject
{
    Q_OBJECT

public:
    TestTheme()
    {
        Q_INIT_RESOURCE(resources);
        Q_INIT_RESOURCE(theme);
    }

private Q_SLOTS:
    void initTestCase()
    {
        OCC::Logger::instance()->setLogFlush(true);
        OCC::Logger::instance()->setLogDebug(true);

        QStandardPaths::setTestModeEnabled(true);
    }

    void testDisplayNamePreservesPersistentIdentity()
    {
        const auto theme = OCC::Theme::instance();
        QCOMPARE(theme->appNameGUI(), QStringLiteral(APPLICATION_DISPLAY_NAME));
        QCOMPARE(theme->appName(), QStringLiteral(APPLICATION_SHORTNAME));
        QCOMPARE(theme->isBranded(), QStringLiteral(APPLICATION_NAME) != QStringLiteral("Nextcloud") && NEXTCLOUD_DEV == 0);
        QVERIFY(theme->about().startsWith(theme->appNameGUI()));
        QVERIFY(theme->versionSwitchOutput().startsWith(theme->appNameGUI() + QStringLiteral(" version ")));
        QVERIFY(theme->aboutDetails().contains(QStringLiteral(APPLICATION_DISPLAY_VENDOR)));
        QTemporaryDir configuration;
        QVERIFY(configuration.isValid());
        QVERIFY(OCC::ConfigFile::setConfDir(configuration.path()));
        const OCC::ConfigFile config;
        Q_UNUSED(config)
        QCOMPARE(QCoreApplication::applicationName(), QStringLiteral(APPLICATION_NAME));
        if (OCC::PrivateCloudDrive::isEnabled()) {
            QCOMPARE(theme->appNameGUI(), QStringLiteral("privateCloud"));
            QCOMPARE(theme->appName(), QString::fromUtf8(NEXTCLOUD_DEV ? "NextcloudDev" : "Nextcloud"));
            QCOMPARE(QStringLiteral(APPLICATION_CONFIG_NAME), QString::fromUtf8(NEXTCLOUD_DEV ? "nextclouddev" : "nextcloud"));
        }
    }

    void testWizardHideSelfHostButtonMatchesBranding()
    {
        const auto theme = OCC::Theme::instance();
        const auto hidden = static_cast<bool>(WIZARD_HIDE_SELF_HOST_BUTTON);

        QCOMPARE(theme->wizardHideSelfHostButton(), hidden);
        QCOMPARE(theme->property("wizardHideSelfHostButton").toBool(), hidden);
    }

    void testWizardSignupMatchesBranding()
    {
        const auto theme = OCC::Theme::instance();
        const auto hidden = static_cast<bool>(WIZARD_HIDE_SIGNUP_BUTTON);

        QCOMPARE(theme->wizardHideSignupButton(), hidden);
        QCOMPARE(theme->property("wizardHideSignupButton").toBool(), hidden);
#ifdef APPLICATION_SIGNUP_URL
        const auto configured = QString::fromUtf8(APPLICATION_SIGNUP_URL);
#else
        const auto configured = QString();
#endif
        QCOMPARE(theme->wizardSignupUrl(), OCC::Theme::signupUrlForBranding(configured));
    }

    void testSignupUrlForBranding_data()
    {
        QTest::addColumn<QString>("configured");
        QTest::addColumn<QUrl>("expected");

        const auto defaultUrl = QUrl(u"https://nextcloud.com/sign-up/?flow=V3"_s);
        const auto custom = u"https://signup.example.com/register?plan=business&source=desktop"_s;
        const auto http = u"http://signup.example.com:8080/register"_s;
        const auto encoded = u"https://signup.example.com/register?next=%2Ffiles&offer=$monthly#plans"_s;
        const auto encodedBraces = u"https://signup.example.com/register?offer=$%7Bcloud%7D;trial=1"_s;
        QTest::newRow("empty uses default") << QString() << defaultUrl;
        QTest::newRow("custom HTTPS") << custom << QUrl(custom);
        QTest::newRow("custom HTTP") << http << QUrl(http);
        QTest::newRow("query and fragment preserved") << encoded << QUrl(encoded);
        QTest::newRow("encoded query characters") << encodedBraces << QUrl(encodedBraces);
        QTest::newRow("relative URL") << u"/register"_s << defaultUrl;
        QTest::newRow("missing host") << u"https:///register"_s << defaultUrl;
        QTest::newRow("file URL") << u"file:///tmp/register"_s << defaultUrl;
        QTest::newRow("javascript URL") << u"javascript:alert(1)"_s << defaultUrl;
        QTest::newRow("invalid escape") << u"https://signup.example.com/%zz"_s << defaultUrl;
        QTest::newRow("unencoded braces") << u"https://signup.example.com/register?offer=${cloud}"_s << defaultUrl;
        QTest::newRow("credentials") << u"https://user:password@signup.example.com/register"_s << defaultUrl;
        QTest::newRow("empty credentials") << u"https://@signup.example.com/register"_s << defaultUrl;
        QTest::newRow("leading whitespace") << u" https://signup.example.com/register"_s << defaultUrl;
        QTest::newRow("trailing whitespace") << u"https://signup.example.com/register "_s << defaultUrl;
        QTest::newRow("line break") << u"https://signup.example.com/reg\nister"_s << defaultUrl;
    }

    void testSignupUrlForBranding()
    {
        QFETCH(QString, configured);
        QFETCH(QUrl, expected);

        QCOMPARE(OCC::Theme::signupUrlForBranding(configured), expected);
    }

    void testHidpiFileName_darkBackground_returnPathToWhiteIcon()
    {
        FakePaintDevice paintDevice;
        const QColor backgroundColor("#000000");
        const QString iconName("icon-name");

        const auto iconPath = OCC::Theme::hidpiFileName(iconName + ".png", backgroundColor, &paintDevice);

        QCOMPARE(iconPath, ":/client/theme/white/" + iconName + ".png");
    }

    void testHidpiFileName_lightBackground_returnPathToBlackIcon()
    {
        FakePaintDevice paintDevice;
        const QColor backgroundColor("#ffffff");
        const QString iconName("icon-name");

        const auto iconPath = OCC::Theme::hidpiFileName(iconName + ".png", backgroundColor, &paintDevice);

        QCOMPARE(iconPath, ":/client/theme/black/" + iconName + ".png");
    }

    void testHidpiFileName_hidpiDevice_returnHidpiIconPath()
    {
        FakePaintDevice paintDevice;
        paintDevice.setHidpi(true);
        const QColor backgroundColor("#000000");
        const QString iconName("wizard-files");

        const auto iconPath = OCC::Theme::hidpiFileName(iconName + ".png", backgroundColor, &paintDevice);

        QCOMPARE(iconPath, ":/client/theme/white/" + iconName + "@2x.png");
    }

    void testIsDarkColor_nextcloudBlue_returnTrue()
    {
        const QColor color(0, 130, 201);

        const auto result = OCC::Theme::isDarkColor(color);

        QCOMPARE(result, true);
    }

    void testIsDarkColor_lightColor_returnFalse()
    {
        const QColor color(255, 255, 255);

        const auto result = OCC::Theme::isDarkColor(color);

        QCOMPARE(result, false);
    }

    void testIsDarkColor_darkColor_returnTrue()
    {
        const QColor color(0, 0, 0);

        const auto result = OCC::Theme::isDarkColor(color);

        QCOMPARE(result, true);
    }

    void testIsHidpi_hidpi_returnTrue()
    {
        FakePaintDevice paintDevice;
        paintDevice.setHidpi(true);

        QCOMPARE(OCC::Theme::isHidpi(&paintDevice), true);
    }

    void testIsHidpi_lowdpi_returnFalse()
    {
        FakePaintDevice paintDevice;
        paintDevice.setHidpi(false);

        QCOMPARE(OCC::Theme::isHidpi(&paintDevice), false);
    }
};

QTEST_GUILESS_MAIN(TestTheme)
#include "testtheme.moc"
