// Exercises the production observer against KWin-shaped windows and signals.
// No compositor, desktop, or installed Moonlight instance is modified.
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const source = fs.readFileSync(path.join(__dirname, "../../app/streaming/desktopvisibility-kwin.js"), "utf8");

function signal() {
    const callbacks = [];
    return { connect(callback) { callbacks.push(callback); }, emit() { callbacks.slice().forEach(callback => callback()); } };
}

function harness(options = {}) {
    let now = 0;
    let acknowledge = true;
    const reports = [];
    const calls = [];
    const timers = [];
    const desktop1 = { id: "desktop-1" };
    const desktop2 = { id: "desktop-2" };
    const target = {
        pid: 1234, caption: 'Gaming PC "quoted" - Moonlight', desktops: [desktop1], activities: [],
        minimized: false, active: true,
        desktopsChanged: signal(), activitiesChanged: signal(), minimizedChanged: signal(),
        outputChanged: signal(), captionChanged: signal(), closed: signal(),
    };
    let windows = [target];
    const workspace = {
        currentDesktop: desktop1, currentActivity: "activity-1", windowList() { return windows; },
        currentDesktopChanged: signal(), currentActivityChanged: signal(), windowAdded: signal(), windowRemoved: signal(),
        ...options.workspace,
    };
    const config = {
        pid: 1234, title: target.caption, windowId: 0, service: ":1.99", path: "/Moonlight/DesktopVisibility",
        interface: "com.moonlight_stream.DesktopVisibility", token: "test-token", scriptName: "moonlight-test",
        ...options.config,
    };
    class Timer {
        constructor() { this.timeout = signal(); this.running = false; timers.push(this); }
        start() { this.running = true; }
        stop() { this.running = false; }
    }
    vm.runInNewContext(source, {
        workspace, moonlightVisibilityConfig: config, QTimer: Timer,
        Date: { now() { return now; } },
        callDBus(service, object, iface, method, ...args) {
            calls.push({ service, object, iface, method, args });
            if (method === "report") {
                const [token, known, visible, callback] = args;
                assert.equal(token, "test-token");
                reports.push({ known, visible });
                if (acknowledge) callback(true);
            }
        },
    });
    return {
        target, workspace, desktop1, desktop2, reports, calls, timers,
        get latest() { return reports[reports.length - 1]; },
        setWindows(value) { windows = value; },
        tick(ms = 2000) { now += ms; timers.filter(timer => timer.running).forEach(timer => timer.timeout.emit()); },
        stopAcknowledging() { acknowledge = false; },
    };
}

const checks = [];
function check(name, run) { run(); checks.push(name); }

check("reports same desktop as visible", () => {
    assert.deepEqual(harness().latest, { known: true, visible: true });
});
check("switches desktops and resumes on return", () => {
    const h = harness();
    h.workspace.currentDesktop = h.desktop2;
    h.workspace.currentDesktopChanged.emit();
    assert.deepEqual(h.latest, { known: true, visible: false });
    h.workspace.currentDesktop = h.desktop1;
    h.workspace.currentDesktopChanged.emit();
    assert.deepEqual(h.latest, { known: true, visible: true });
});
check("moving the stream window updates without a desktop switch", () => {
    const h = harness();
    h.target.desktops = [h.desktop2];
    h.target.desktopsChanged.emit();
    assert.equal(h.latest.visible, false);
});
check("an unfocused window on the current desktop remains visible", () => {
    const h = harness();
    h.target.active = false;
    h.tick();
    assert.equal(h.latest.visible, true);
});
check("sticky windows stay visible across desktops", () => {
    const h = harness();
    h.target.desktops = [];
    h.workspace.currentDesktop = h.desktop2;
    h.workspace.currentDesktopChanged.emit();
    assert.equal(h.latest.visible, true);
});
check("windows assigned to multiple desktops remain visible on either", () => {
    const h = harness();
    h.target.desktops = [h.desktop1, h.desktop2];
    h.workspace.currentDesktop = h.desktop2;
    h.workspace.currentDesktopChanged.emit();
    assert.equal(h.latest.visible, true);
});
check("compares desktop UUIDs rather than QObject wrapper identity", () => {
    const h = harness();
    h.workspace.currentDesktop = { id: "desktop-1" };
    h.workspace.currentDesktopChanged.emit();
    assert.equal(h.latest.visible, true);
});
check("uses the stream output's desktop with Plasma 6.7 per-screen desktops", () => {
    const h = harness();
    h.target.output = { name: "other-screen" };
    h.workspace.currentDesktop = h.desktop2;
    h.workspace.currentDesktopForScreen = output => output === h.target.output ? h.desktop1 : h.desktop2;
    h.workspace.currentDesktopChanged.emit();
    assert.equal(h.latest.visible, true);
});
check("minimized windows pause and restore", () => {
    const h = harness();
    h.target.minimized = true;
    h.target.minimizedChanged.emit();
    assert.equal(h.latest.visible, false);
    h.target.minimized = false;
    h.target.minimizedChanged.emit();
    assert.equal(h.latest.visible, true);
});
check("activity changes affect visibility", () => {
    const h = harness();
    h.target.activities = ["activity-2"];
    h.target.activitiesChanged.emit();
    assert.equal(h.latest.visible, false);
    h.workspace.currentActivity = "activity-2";
    h.workspace.currentActivityChanged.emit();
    assert.equal(h.latest.visible, true);
});
check("requires the correct owning process", () => {
    const h = harness();
    h.target.pid = 9876;
    h.tick();
    assert.deepEqual(h.latest, { known: false, visible: true });
});
check("ambiguous titles fail open", () => {
    const h = harness();
    h.setWindows([h.target, { ...h.target }]);
    h.tick();
    assert.deepEqual(h.latest, { known: false, visible: true });
});
check("missing/closed windows fail open", () => {
    const h = harness();
    h.setWindows([]);
    h.workspace.windowRemoved.emit();
    assert.deepEqual(h.latest, { known: false, visible: true });
});
check("a window appearing after startup is discovered", () => {
    const h = harness();
    h.setWindows([]);
    h.tick();
    h.setWindows([h.target]);
    h.workspace.windowAdded.emit();
    assert.deepEqual(h.latest, { known: true, visible: true });
});
check("missing desktop data fails open", () => {
    const h = harness();
    delete h.target.desktops;
    h.tick();
    assert.deepEqual(h.latest, { known: false, visible: true });
});
check("legacy KWin numeric desktops work", () => {
    const h = harness({ workspace: { currentDesktop: 2 } });
    h.target.desktop = 2;
    h.tick();
    assert.equal(h.latest.visible, true);
    h.target.desktop = 1;
    h.tick();
    assert.equal(h.latest.visible, false);
    h.target.desktop = -1;
    h.tick();
    assert.equal(h.latest.visible, true);
});
check("X11 window ID disambiguates same-title windows", () => {
    const h = harness({ config: { windowId: 42 } });
    h.target.windowId = 42;
    h.target.desktops = [h.desktop2];
    h.setWindows([h.target, { ...h.target, windowId: 43, desktops: [h.desktop1] }]);
    h.tick();
    assert.deepEqual(h.latest, { known: true, visible: false });
});
check("temporary observer unloads after client stops answering", () => {
    const h = harness();
    h.stopAcknowledging();
    h.tick(11000);
    assert.equal(h.calls[h.calls.length - 1].method, "unloadScript");
    assert.equal(h.calls[h.calls.length - 1].args[0], "moonlight-test");
    assert.equal(h.timers[0].running, false);
});

console.log(`Passed ${checks.length} desktop visibility checks.`);
