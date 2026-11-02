/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

import QtQuick
import QtQuick.Layouts
import Style
import "../../tray"

Item {
    id: root

    required property var controller
    readonly property color primaryTextColor: Style.wizardPrimaryText
    readonly property color hintTextColor: Style.wizardSecondaryText

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: 24
        anchors.rightMargin: 24
        anchors.topMargin: 24
        anchors.bottomMargin: 24
        spacing: 12

        EnforcedPlainTextLabel {
            text: root.controller.privateCloudMode ? qsTr("Sign in to privateCloud")
                : root.controller.publicShareSetup ? qsTr("Connect public share") : qsTr("Enter credentials")
            color: root.primaryTextColor
            font.pixelSize: Style.pixelSize + 8
            font.bold: true
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        EnforcedPlainTextLabel {
            text: root.controller.publicShareSetup
                ? qsTr("Enter the share password if the link is password protected.")
                : root.controller.privateCloudMode
                    ? qsTr("Enter your username and password to connect your drive.")
                    : qsTr("Enter the username and password for this server.")
            color: root.hintTextColor
            font.pixelSize: Style.pixelSize + 2
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        EnforcedPlainTextLabel {
            visible: root.controller.privateCloudMode
            text: qsTr("Username")
            color: root.primaryTextColor
            Layout.fillWidth: true
        }

        WizardTextField {
            id: username
            objectName: "privateCloudUsername"
            Layout.fillWidth: true
            Layout.topMargin: root.controller.privateCloudMode ? 0 : 10
            text: root.controller.basicAuthUser
            enabled: !root.controller.busy
            placeholderText: qsTr("Username")
            selectByMouse: true
            onTextEdited: root.controller.basicAuthUser = text
            onAccepted: password.forceActiveFocus()
        }

        EnforcedPlainTextLabel {
            visible: root.controller.privateCloudMode
            text: qsTr("Password")
            color: root.primaryTextColor
            Layout.fillWidth: true
        }

        RowLayout {
            Layout.fillWidth: true

            WizardTextField {
                id: password
                objectName: "privateCloudPassword"
                Layout.fillWidth: true
                text: root.controller.basicAuthPassword
                enabled: !root.controller.busy
                placeholderText: qsTr("Password")
                echoMode: showPassword.checked ? TextInput.Normal : TextInput.Password
                selectByMouse: true
                onTextEdited: root.controller.basicAuthPassword = text
                onAccepted: root.controller.submitBasicAuth()
            }

            WizardButton {
                id: showPassword
                objectName: "privateCloudShowPassword"
                text: checked ? qsTr("Hide") : qsTr("Show")
                Accessible.name: checked ? qsTr("Hide password") : qsTr("Show password")
                checkable: true
                enabled: !root.controller.busy
                onEnabledChanged: if (!enabled) checked = false
            }
        }

        WizardButton {
            id: passwordHelp
            objectName: "privateCloudPasswordHelpToggle"
            visible: root.controller.privateCloudMode
            text: checked ? qsTr("Hide sign-in help") : qsTr("Use two-factor authentication?")
            checkable: true
        }

        EnforcedPlainTextLabel {
            objectName: "privateCloudPasswordHelp"
            visible: passwordHelp.visible && passwordHelp.checked
            text: qsTr("Use an app password from your account's Security settings instead of your usual password.")
            color: root.hintTextColor
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        EnforcedPlainTextLabel {
            visible: root.controller.errorText !== ""
            text: root.controller.errorText
            color: Style.wizardErrorText
            font.pixelSize: Style.pixelSize + 1
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        RowLayout {
            visible: root.controller.busy && root.controller.authStatusText !== ""
            Layout.fillWidth: true
            spacing: 8

            NCBusyIndicator {
                running: root.controller.busy
                visible: running
                Layout.preferredWidth: 20
                Layout.preferredHeight: 20
            }

            EnforcedPlainTextLabel {
                text: root.controller.authStatusText
                color: root.hintTextColor
                font.pixelSize: Style.pixelSize + 1
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
        }

        Item {
            Layout.fillHeight: true
        }
    }
}
