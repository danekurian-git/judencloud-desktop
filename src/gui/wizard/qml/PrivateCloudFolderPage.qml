/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Style
import "../../tray"

Item {
    id: root
    objectName: "privateCloudFolderPage"

    required property var controller

    ScrollView {
        id: folderScroll

        anchors.fill: parent
        anchors.margins: Style.wizardWindowMargin
        clip: true
        contentWidth: availableWidth
        contentHeight: folderContent.implicitHeight
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        ColumnLayout {
            id: folderContent

            width: folderScroll.availableWidth
            spacing: Style.wizardSectionSpacing

            EnforcedPlainTextLabel {
                text: qsTr("Choose your sync folder")
                color: Style.wizardPrimaryText
                font.pixelSize: Style.wizardHeaderTitleFontPixelSize
                font.bold: true
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }

            EnforcedPlainTextLabel {
                text: qsTr("Only files available to your account appear in J:. File contents download when opened.")
                color: Style.wizardSecondaryText
                font.pixelSize: Style.wizardBodyFontPixelSize
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }

            EnforcedPlainTextLabel {
                text: qsTr("Sync folder")
                color: Style.wizardPrimaryText
                font.bold: true
                Layout.fillWidth: true
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Style.wizardFooterSpacing

                WizardTextField {
                    objectName: "privateCloudFolderPath"
                    text: root.controller.localSyncFolderDisplay
                    readOnly: true
                    selectByMouse: true
                    Accessible.name: qsTr("Sync folder")
                    font.pixelSize: Style.pixelSize
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                }

                WizardButton {
                    objectName: "privateCloudChooseFolder"
                    text: qsTr("Browse")
                    enabled: !root.controller.busy
                    onClicked: root.controller.chooseLocalSyncFolder()
                }
            }

            EnforcedPlainTextLabel {
                objectName: "privateCloudFolderFreeSpace"
                visible: root.controller.localSyncFolderFreeSpace !== ""
                text: root.controller.localSyncFolderFreeSpace
                color: Style.wizardSecondaryText
                font.pixelSize: Style.pixelSize
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }

            EnforcedPlainTextLabel {
                objectName: "privateCloudFolderError"
                visible: root.controller.localSyncFolderError !== ""
                text: root.controller.localSyncFolderError
                color: Style.wizardErrorText
                font.pixelSize: Style.pixelSize
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }

            EnforcedPlainTextLabel {
                visible: root.controller.errorText !== ""
                    && root.controller.errorText !== root.controller.localSyncFolderError
                text: root.controller.errorText
                color: Style.wizardErrorText
                font.pixelSize: Style.pixelSize
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
        }
    }
}
