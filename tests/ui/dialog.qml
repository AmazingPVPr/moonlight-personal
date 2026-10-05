import QtQuick 2.9

Loader {
    objectName: qsTr("Settings")
    source: "smoke.qml"
    onLoaded: item.showDialogForScreenshot = true
}
