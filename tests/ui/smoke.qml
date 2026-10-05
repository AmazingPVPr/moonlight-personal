import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Controls.Material 2.2
import StreamingPreferences 1.0
import "../../app/gui" as Views

Views.SettingsView {
    id: testedSettings
    property bool showDialogForScreenshot: false
    property bool checksFinished: false
    property int exitStatus: 0

    Views.ProfileControls {
        id: compactProfileTest
        compact: true
        visible: false
    }

    function findObject(root, name, seen) {
        if (!root || seen.indexOf(root) >= 0) return null
        seen.push(root)
        if (root.objectName === name) return root
        var lists = [root.children, root.data]
        for (var listIndex = 0; listIndex < lists.length; ++listIndex) {
            var list = lists[listIndex]
            if (!list) continue
            for (var i = 0; i < list.length; ++i) {
                var found = findObject(list[i], name, seen)
                if (found) return found
            }
        }
        var parts = [root.contentItem, root.footer, root.header, root.background]
        for (var partIndex = 0; partIndex < parts.length; ++partIndex) {
            var part = findObject(parts[partIndex], name, seen)
            if (part) return part
        }
        return null
    }

    function control(name) {
        var object = findObject(testedSettings, name, [])
        if (!object) throw new Error("Missing control: " + name)
        return object
    }

    function check(condition, message) {
        if (!condition) throw new Error(message)
    }

    function chooseProfile(name) {
        var selector = control("profileSelector")
        var index = StreamingPreferences.profileNames.indexOf(name)
        selector.currentIndex = index
        selector.activated(index)
    }

    function useNameDialog(action, name) {
        var dialog = control("profileNameDialog")
        dialog.start(action)
        control("profileNameField").text = name
        control("saveProfileButton").clicked()
    }

    function runChecks() {
        selectModelValue(control("windowModeComboBox"), null, StreamingPreferences.WM_FULLSCREEN)
        check(control("profileSelector").restoreUiNavMode, "Settings chooser must restore tab navigation")
        var compactSelector = findObject(compactProfileTest, "profileSelector", [])
        check(compactSelector && !compactSelector.restoreUiNavMode, "Toolbar chooser must restore grid navigation")
        check(control("pauseHiddenCheck").checked, "Hidden pause should default on")
        check(!control("pauseUnfocusedCheck").checked, "Focus pause should default off")
        control("pauseUnfocusedCheck").toggle()
        control("pauseUnfocusedCheck").toggled()
        check(StreamingPreferences.pauseVideoWhenUnfocused, "Focus pause checkbox must update preferences")

        useNameDialog("create", "Gaming")
        check(StreamingPreferences.currentProfile === "Gaming", "New preset should select itself")
        StreamingPreferences.width = 1600
        StreamingPreferences.height = 900
        StreamingPreferences.fps = 90
        StreamingPreferences.bitrateKbps = 42000
        StreamingPreferences.audioConfig = StreamingPreferences.AC_51_SURROUND
        StreamingPreferences.videoCodecConfig = StreamingPreferences.VCC_FORCE_HEVC
        StreamingPreferences.videoDecoderSelection = StreamingPreferences.VDS_FORCE_SOFTWARE
        StreamingPreferences.captureSysKeysMode = StreamingPreferences.CSK_ALWAYS
        StreamingPreferences.enableHdr = true
        StreamingPreferences.autoAdjustBitrate = false
        useNameDialog("duplicate", "Gaming copy")
        check(StreamingPreferences.currentProfile === "Gaming copy", "Duplicate should select itself")
        useNameDialog("rename", "Desktop")
        check(StreamingPreferences.currentProfile === "Desktop", "Rename should preserve current selection")
        chooseProfile("Default")
        check(StreamingPreferences.width === 1920 && StreamingPreferences.fps === 60, "Default should restore global settings")
        chooseProfile("Gaming")
        check(StreamingPreferences.width === 1600 && StreamingPreferences.height === 900 && StreamingPreferences.fps === 90, "Preset mode should survive UI refresh")
        check(StreamingPreferences.bitrateKbps === 42000 && !StreamingPreferences.autoAdjustBitrate, "Preset bitrate must not be reset by controls")
        check(StreamingPreferences.enableHdr, "Unsupported HDR UI must not overwrite saved preset")
        check(StreamingPreferences.captureSysKeysMode === StreamingPreferences.CSK_ALWAYS, "Capture controls must not overwrite preset")
        check(control("slider").value === 42000, "Bitrate slider must refresh")
        var resolution = control("resolutionComboBox")
        check(resolution.model.get(resolution.currentIndex).video_width === "1600", "Custom resolution selection must refresh")
        var fps = control("fpsComboBox")
        check(fps.model.get(fps.currentIndex).video_fps === "90", "Custom FPS selection must refresh")
        for (var i = 0; i < 4; ++i) {
            chooseProfile("Default")
            chooseProfile("Gaming")
        }
        check(fps.currentIndex >= 0 && fps.model.get(fps.currentIndex).video_fps === "90", "Repeated preset switches must keep custom FPS")
        check(control("audioComboBox").model.get(control("audioComboBox").currentIndex).val === StreamingPreferences.AC_51_SURROUND, "Audio selection must refresh")
        check(control("codecComboBox").model.get(control("codecComboBox").currentIndex).val === StreamingPreferences.VCC_FORCE_HEVC, "Codec selection must refresh")
        control("deleteProfileButton").clicked()
        control("deleteProfileDialog").accept()
        check(StreamingPreferences.currentProfile === "Default", "Delete should fall back to Default")
        check(StreamingPreferences.profileNames.indexOf("Gaming") < 0, "Deleted preset must disappear")
        chooseProfile("Desktop")
        check(StreamingPreferences.currentProfile === "Desktop" && StreamingPreferences.bitrateKbps === 42000, "Duplicate must remain independent")
        if (showDialogForScreenshot) {
            control("profileNameDialog").start("create")
            control("profileNameField").text = "Living room"
            var screenshotDialog = control("profileNameDialog")
            check(screenshotDialog.background.height >= screenshotDialog.header.height +
                  screenshotDialog.contentItem.height + screenshotDialog.footer.height,
                  "Modal background must enclose header, content and buttons")
            // Material's elevation shader is unsupported by Qt's software
            // renderer. Preserve modal geometry without that effect for QA.
            screenshotDialog.Material.elevation = 0
        }
        console.log("UI TEST PASS: preset dialogs, switching, custom modes, bitrate preservation, and pause controls")
        checksFinished = true
    }

    Timer {
        interval: 350
        running: true
        onTriggered: {
            try {
                runChecks()
            }
            catch (error) {
                console.error("UI TEST FAIL: " + error.message)
                exitStatus = 1
                checksFinished = true
            }
        }
    }

    Timer {
        interval: 3000
        running: true
        onTriggered: Qt.exit(checksFinished ? exitStatus : 2)
    }
}
