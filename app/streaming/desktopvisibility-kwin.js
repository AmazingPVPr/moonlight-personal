/* Temporary, read-only KWin observer. The C++ worker prepends JSON configuration.
 * No window focus, desktop assignment, compositor settings, or persistent KWin
 * script configuration is changed. Supports KWin 5.27 and Plasma 6 APIs.
 */
(function (config) {
    var heartbeat = null;
    var watched = null;
    var lastAcknowledged = Date.now();
    var stopped = false;

    function connectSignal(object, name, callback) {
        if (object && object[name] && typeof object[name].connect === "function") {
            object[name].connect(callback);
        }
    }

    function windows() {
        if (typeof workspace.windowList === "function") {
            return workspace.windowList();
        }
        if (typeof workspace.clientList === "function") {
            return workspace.clientList();
        }
        return [];
    }

    function matches(window) {
        if (!window || window.deleted || Number(window.pid) !== config.pid) {
            return false;
        }
        if (config.windowId && window.windowId !== undefined) {
            return Number(window.windowId) === config.windowId;
        }
        return window.caption === config.title || window.captionNormal === config.title;
    }

    function desktopId(desktop) {
        // KWin 6 uses VirtualDesktop objects; KWin 5's current desktop is a number.
        return desktop && desktop.id !== undefined ? String(desktop.id) : String(desktop);
    }

    function isOnDesktop(window) {
        if (window.onAllDesktops === true) {
            return true;
        }
        var current = workspace.currentDesktop;
        if (window.output && typeof workspace.currentDesktopForScreen === "function") {
            current = workspace.currentDesktopForScreen(window.output);
        }
        if (current === undefined || current === null) {
            return null;
        }
        if (typeof window.desktop === "number" && typeof current === "number") {
            return window.desktop === -1 || window.desktop === current;
        }
        if (window.desktops !== undefined && window.desktops !== null) {
            var desktops = window.desktops;
            // An empty desktop list is KWin's representation of all desktops.
            if (desktops.length === 0) {
                return true;
            }
            for (var i = 0; i < desktops.length; ++i) {
                if (desktopId(desktops[i]) === desktopId(current)) {
                    return true;
                }
            }
            return false;
        }
        return null;
    }

    function isOnActivity(window) {
        var activities = window.activities;
        var current = workspace.currentActivity;
        if (!activities || activities.length === 0 || !current) {
            return true;
        }
        return activities.indexOf(current) !== -1;
    }

    function report(known, visible) {
        callDBus(config.service, config.path, config.interface, "report",
                 config.token, known, visible, function () {
            lastAcknowledged = Date.now();
        });
    }

    function update() {
        if (stopped) {
            return;
        }
        try {
            var list = windows();
            var target = null;
            for (var i = 0; i < list.length; ++i) {
                if (matches(list[i])) {
                    // An ambiguous PID/title match must never pause the wrong window.
                    if (target !== null) {
                        report(false, true);
                        return;
                    }
                    target = list[i];
                }
            }
            if (target === null) {
                report(false, true);
                return;
            }
            if (target !== watched) {
                watched = target;
                connectSignal(target, "desktopsChanged", update);
                connectSignal(target, "desktopChanged", update);
                connectSignal(target, "activitiesChanged", update);
                connectSignal(target, "minimizedChanged", update);
                connectSignal(target, "outputChanged", update);
                connectSignal(target, "screenChanged", update);
                connectSignal(target, "captionChanged", update);
                connectSignal(target, "closed", update);
            }
            var onDesktop = isOnDesktop(target);
            if (onDesktop === null) {
                report(false, true);
                return;
            }
            report(true, onDesktop && isOnActivity(target) && !target.minimized);
        } catch (error) {
            report(false, true);
        }
    }

    connectSignal(workspace, "currentDesktopChanged", update);
    connectSignal(workspace, "currentActivityChanged", update);
    connectSignal(workspace, "desktopsChanged", update);
    connectSignal(workspace, "screensChanged", update);
    connectSignal(workspace, "windowAdded", update);
    connectSignal(workspace, "windowRemoved", update);
    connectSignal(workspace, "clientAdded", update);
    connectSignal(workspace, "clientRemoved", update);

    // This also detects a stream window created just after script startup and
    // lets the client fail open if compositor callbacks stop arriving.
    heartbeat = new QTimer();
    heartbeat.interval = 2000;
    heartbeat.timeout.connect(function () {
        if (Date.now() - lastAcknowledged > 10000) {
            // If the client crashes, remove this temporary observer ourselves.
            stopped = true;
            heartbeat.stop();
            callDBus("org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting",
                     "unloadScript", config.scriptName);
            return;
        }
        update();
    });
    heartbeat.start();
    update();
})(moonlightVisibilityConfig);
