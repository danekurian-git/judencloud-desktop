/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "advancedsettings.h"
#include "configfile.h"
#include "theme.h"
#include "wizard/accountwizardcontroller.h"

#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace OCC;

class TestMonocloudSettings : public QObject
{
    Q_OBJECT

    QTemporaryDir _configDir;

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(_configDir.isValid());
        ConfigFile::setConfDir(_configDir.path());
    }

    void ignoresSavedConfirmationSettings()
    {
        ConfigFile config;
        config.setNewBigFolderSizeLimit(true, 500);
        config.setNotifyExistingFoldersOverLimit(true);
        config.setStopSyncingExistingFoldersOverLimit(true);
        config.setConfirmExternalStorage(true);

        QVERIFY(!config.newBigFolderSizeLimit().first);
        QVERIFY(!config.useNewBigFolderSizeLimit());
        QVERIFY(!config.notifyExistingFoldersOverLimit());
        QVERIFY(!config.stopSyncingExistingFoldersOverLimit());
        QVERIFY(!config.confirmExternalStorage());

        AccountWizardController wizard;
        QVERIFY(!wizard.showLargeFolderConfirmation());
        QVERIFY(!wizard.showExternalStorageConfirmation());
        QVERIFY(!wizard.askBeforeLargeFolders());
        QVERIFY(!wizard.askBeforeExternalStorage());
        QVERIFY(!wizard.hasAdvancedOptions());
    }

    void removesConfirmationControlsFromSettings()
    {
        AdvancedSettings settings;
        for (const auto name : {"newFolderLimitLabel",
                                "newFolderLimitSpinBox",
                                "newFolderLimitCheckBox",
                                "existingFolderLimitLabel",
                                "existingFolderLimitCheckBox",
                                "stopExistingFolderNowBigSyncLabel",
                                "stopExistingFolderNowBigSyncCheckBox",
                                "newExternalStorageLabel",
                                "newExternalStorage"}) {
            const auto widget = settings.findChild<QWidget *>(QString::fromLatin1(name));
            QVERIFY2(widget, name);
            QVERIFY2(widget->isHidden(), name);
        }
        const auto unrelatedControl = settings.findChild<QWidget *>(QStringLiteral("moveFilesToTrashCheckBox"));
        QVERIFY(unrelatedControl);
        QVERIFY(!unrelatedControl->isHidden());
    }
};

QTEST_MAIN(TestMonocloudSettings)
#include "testmonocloudsettings.moc"
