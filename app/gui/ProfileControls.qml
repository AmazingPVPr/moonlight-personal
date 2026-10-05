import QtQuick 2.9
import QtQuick.Controls 2.5
import QtQuick.Layouts 1.3

import StreamingPreferences 1.0

Item {
    id: profileControls
    property bool compact: false
    property string contextName: ""
    implicitHeight: profileLayout.implicitHeight
    implicitWidth: profileLayout.implicitWidth

    ColumnLayout {
        id: profileLayout
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                text: qsTr("Preset")
                visible: !profileControls.compact || profileControls.width >= 220
                font.pointSize: profileControls.compact ? 10 : 12
            }

            AutoResizingComboBox {
                id: profileSelector
                objectName: "profileSelector"
                restoreUiNavMode: !profileControls.compact
                Layout.fillWidth: true
                Layout.minimumWidth: 110
                maximumWidth: profileControls.width
                model: StreamingPreferences.profileNames
                function refreshSelection() {
                    currentIndex = Math.max(0, StreamingPreferences.profileNames.indexOf(StreamingPreferences.currentProfile))
                    recalculateWidth()
                }
                onActivated: StreamingPreferences.currentProfile = model[currentIndex]
                hoverEnabled: true
                ToolTip.delay: 700
                ToolTip.visible: hovered
                ToolTip.text: profileControls.contextName ?
                                  qsTr("Stream preset for %1").arg(profileControls.contextName) :
                                  qsTr("Choose a saved stream preset")
                Component.onCompleted: refreshSelection()
                onModelChanged: refreshSelection()
                Connections {
                    target: StreamingPreferences
                    function onCurrentProfileChanged() { profileSelector.refreshSelection() }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: !profileControls.compact
            wrapMode: Text.Wrap
            font.pointSize: 10
            text: profileControls.contextName ?
                      qsTr("Your choice is remembered for %1. Changes apply to the next stream.").arg(profileControls.contextName) :
                      qsTr("Save quality, audio and input settings as presets. Choose a preset inside each computer to remember it there.")
        }

        GridLayout {
            Layout.fillWidth: true
            visible: !profileControls.compact
            columns: profileControls.width >= 440 ? 4 : 2
            columnSpacing: 8
            rowSpacing: 4

            Button {
                objectName: "newProfileButton"
                text: qsTr("New preset")
                Layout.fillWidth: true
                onClicked: profileNameDialog.start("create")
            }
            Button {
                objectName: "duplicateProfileButton"
                text: qsTr("Duplicate")
                Layout.fillWidth: true
                onClicked: profileNameDialog.start("duplicate")
            }
            Button {
                objectName: "renameProfileButton"
                text: qsTr("Rename")
                Layout.fillWidth: true
                enabled: StreamingPreferences.currentProfile !== "Default"
                onClicked: profileNameDialog.start("rename")
            }
            Button {
                objectName: "deleteProfileButton"
                text: qsTr("Delete")
                Layout.fillWidth: true
                enabled: StreamingPreferences.currentProfile !== "Default"
                onClicked: {
                    deleteProfileDialog.profileName = StreamingPreferences.currentProfile
                    deleteProfileDialog.open()
                }
            }
        }

    }

    NavigableDialog {
        id: profileNameDialog
        objectName: "profileNameDialog"
        property string operation: "create"
        property string sourceName: ""
        property string errorText: ""
        title: operation === "rename" ? qsTr("Rename preset") :
               operation === "duplicate" ? qsTr("Duplicate preset") : qsTr("New preset")
        width: Math.min(420, Overlay.overlay ? Overlay.overlay.width - 32 : 420)

        function start(action) {
            operation = action
            sourceName = StreamingPreferences.currentProfile
            errorText = ""
            profileNameField.text = action === "rename" ? sourceName :
                                    action === "duplicate" && sourceName !== "Default" ? qsTr("%1 copy").arg(sourceName) : ""
            open()
            profileNameField.forceActiveFocus(Qt.TabFocus)
            profileNameField.selectAll()
        }

        function savePreset() {
            var name = profileNameField.text.trim()
            var success = operation === "rename" ? StreamingPreferences.renameProfile(sourceName, name) :
                          StreamingPreferences.duplicateProfile(operation === "create" ? "Default" : sourceName, name)
            if (success) {
                close()
            }
            else {
                errorText = qsTr("Use a unique name. Default is reserved.")
                profileNameField.forceActiveFocus()
            }
        }

        contentItem: ColumnLayout {
            spacing: 10
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: profileNameDialog.operation === "create" ?
                          qsTr("Start with Default settings, then adjust the new preset below.") :
                          profileNameDialog.operation === "duplicate" ?
                          qsTr("Make a separate copy of the current preset.") : qsTr("Give this preset a new name.")
            }
            TextField {
                id: profileNameField
                objectName: "profileNameField"
                Layout.fillWidth: true
                placeholderText: qsTr("Preset name")
                maximumLength: 80
                selectByMouse: true
                onTextChanged: profileNameDialog.errorText = ""
                Keys.onReturnPressed: profileNameDialog.savePreset()
                Keys.onEnterPressed: profileNameDialog.savePreset()
            }
            Label {
                Layout.fillWidth: true
                visible: text.length > 0
                color: "#ef9a9a"
                wrapMode: Text.Wrap
                text: profileNameDialog.errorText
            }
        }

        footer: RowLayout {
            implicitHeight: 56
            spacing: 8
            Item { Layout.fillWidth: true }
            Button {
                text: qsTr("Cancel")
                onClicked: profileNameDialog.close()
            }
            Button {
                objectName: "saveProfileButton"
                text: qsTr("Save")
                enabled: profileNameField.text.trim().length > 0
                onClicked: profileNameDialog.savePreset()
            }
        }
    }

    NavigableMessageDialog {
        id: deleteProfileDialog
        objectName: "deleteProfileDialog"
        property string profileName: ""
        title: qsTr("Delete preset")
        standardButtons: Dialog.Yes | Dialog.No
        text: qsTr("Delete \"%1\"? Computers using this preset will return to Default.").arg(profileName)
        onAccepted: StreamingPreferences.deleteProfile(profileName)
    }
}
