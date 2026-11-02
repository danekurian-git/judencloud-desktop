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
