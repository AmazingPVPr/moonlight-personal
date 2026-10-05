# Personal fork validation

Run tests from separate build directories so generated files stay out of source control. Qt 6 is used below; the profiles test also passes with Qt 5.

## Profiles and persistence

```sh
mkdir -p /tmp/moonlight-profile-tests
cd /tmp/moonlight-profile-tests
qmake6 /path/to/moonlight-personal/tests/profiles/profiles.pro
make -j4
./profile-tests
```

The tests exercise a real writer/reader process restart, per-host profile selections, fallback, duplication, rename/delete references, global settings, atomic preference updates, and session snapshot independence.

## Video pause and render queues

```sh
bash tests/video-pause/run.sh
```

The test runs the production frame pacer, validates AVFrame release, checks both rendering modes, drains 1,000 paused submissions, rejects stale wakeups, and performs 100 rapid pause/resume/shutdown cycles. It also checks keyframe recovery and fast away/back transitions. These tests do not emulate a complete GameStream host.

## Desktop visibility

```sh
node tests/desktopvisibility/kwin-observer.test.js
mkdir -p /tmp/moonlight-desktop-tests
cd /tmp/moonlight-desktop-tests
qmake6 /path/to/moonlight-personal/tests/desktopvisibility/worker-test.pro
make -j4
dbus-run-session -- ./desktopvisibility-worker-test
```

The JavaScript checks execute the production observer against a mock compositor. The worker test uses a private message bus to verify callbacks with the main Qt loop blocked, sender validation, unsupported fallback, and cleanup. It does not modify a real KWin session.

The X11 helper was also checked on this client's XWayland server using an unmapped test window. Reading a destroyed window ID repeatedly returned an error safely while the separate XCB connection remained usable. The test is in `tests/desktopvisibility/x11-property-test.cpp`.

## Actual settings screen

Compile with `CONFIG+=ui-test`. Set `QT_QPA_PLATFORM=offscreen`, `QT_QUICK_BACKEND=software`, and fresh temporary `XDG_CONFIG_HOME`/`XDG_CACHE_HOME` directories for each run. Set `MOONLIGHT_UI_TEST_VIEW` to the absolute `file:///.../tests/ui/smoke.qml` URL and run the client. Optionally set `MOONLIGHT_UI_TEST_SCREENSHOT` to a PNG path. The test exercises the actual preset dialogs and settings controls, then exits. `tests/ui/dialog.qml` keeps a modal open for visual checks; this test disables its elevation effect because the software backend cannot draw that shader. The test view and screenshot environment variables are only honored in this explicit test build.

## Live Windows-host check

1. End the old client's stream when convenient, then start Moonlight Personal and pair with the host.
2. Choose a windowed preset and start the host's Desktop application. Verify normal video, audio, and input.
3. Switch virtual desktops for at least 30 seconds. Check that GPU decoder use falls and the connection stays alive. Return and verify fresh video resumes without reconnecting or a black screen.
4. Repeat with minimize/restore. Enable unfocused pausing, focus another window on the same desktop, and verify the independent option.
5. Repeat rapid switches and quit while paused. Check the temporary KWin script is removed after stream exit.
6. Disable the options and confirm the previous continuous playback behavior.

Network traffic and host encoding are expected to continue during local pause. Audio follows the separate mute setting. This checklist is still pending on the user's real host; automated test results should not be described as a measured performance reduction or a verified live connection.
