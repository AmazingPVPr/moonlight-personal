# Moonshine Client validation

Run tests from separate build directories so generated files stay out of source control. Qt 6 is used below; the profiles test also passes with Qt 5.

## Profiles and persistence

```sh
mkdir -p /tmp/moonlight-profile-tests
cd /tmp/moonlight-profile-tests
qmake6 /path/to/moonlight-personal/tests/profiles/profiles.pro
make -j4
./profile-tests
```

The tests exercise a real writer/reader process restart, per-host profile selections, fallback, duplication, rename/delete references, global settings, atomic preference updates, and session snapshot independence. They also cover independent video/audio delays, bounds, and typed legacy settings migration without overwriting existing settings. There are 28 passing cases on both Qt 5 and Qt 6.

## Video pause and render queues

```sh
bash tests/video-pause/run.sh
```

The test runs the production frame pacer, validates AVFrame release, checks both rendering modes, drains 1,000 paused submissions, rejects stale wakeups, and performs 100 rapid pause/resume/shutdown cycles. An injected monotonic clock checks the one-second startup warmup, reference-dependent frame admission, exact deadline boundaries, expiry without new frames, clock wrap, a two-second failure bound before the first presentation, and stale generations. It also checks keyframe recovery, failed presentation, uninterrupted normal playback, and independent focus/visibility timers. These tests do not emulate a complete GameStream host. See [video-pause/README.md](video-pause/README.md) for the policy and clock details.

```sh
bash tests/video-pause/run-startup-vulkan.sh
```

This Linux smoke test needs access to an X11/XWayland display and a Vulkan driver. It uses the production Vulkan renderer and pacer: the initial black buffer does not start the preview timer, several synthetic video images render during the injected one-second warmup, and queued candidates plus 100 further frames are discarded at its deadline. The last successfully presented image remains retained. It never connects to a host or takes window focus. Exit code 77 means the graphical environment is unavailable, not a passing test.

```sh
bash tests/video-pause/run-renderer-presentation.sh
bash tests/video-pause/run-sdl-presentation.sh
```

These tests exercise actual Vulkan and SDL presentation success/failure. Linux renderer changes were compiled on this client. Windows and macOS presentation hooks were reviewed but have not been compiled or tested on those platforms.

## Paused native cursor

```sh
bash tests/input-cursor/run-tests.sh
```

This headless check compiles the production cursor and mouse handlers. It checks relative and absolute capture, paused hover and mouse input, cursor shortcuts, deferred capture intent, held-button release, focus restoration, and window recreation. It does not verify how a real desktop compositor draws the cursor.

## Desktop visibility

```sh
node tests/desktopvisibility/kwin-observer.test.js
mkdir -p /tmp/moonlight-desktop-tests
cd /tmp/moonlight-desktop-tests
qmake6 /path/to/moonlight-personal/tests/desktopvisibility/worker-test.pro
make -j4
dbus-run-session -- env MOONLIGHT_PRIVATE_TEST_BUS=1 SDL_VIDEODRIVER=dummy ./desktopvisibility-worker-test
```

The JavaScript checks execute the production observer against a mock compositor. The worker test uses a private message bus to verify callbacks with the main Qt loop blocked, sender validation, unsupported fallback, and cleanup. It does not modify a real KWin session.

The X11 helper was also checked on this client's XWayland server using an unmapped test window. Reading a destroyed window ID repeatedly returned an error safely while the separate XCB connection remained usable. The test is in `tests/desktopvisibility/x11-property-test.cpp`.

## Actual settings screen

Compile with `CONFIG+=ui-test`. Set `QT_QPA_PLATFORM=offscreen`, `QT_QUICK_BACKEND=software`, and fresh temporary `XDG_CONFIG_HOME`/`XDG_CACHE_HOME` directories for each run. Set `MOONLIGHT_UI_TEST_VIEW` to the absolute `file:///.../tests/ui/smoke.qml` URL and run the client. Optionally set `MOONLIGHT_UI_TEST_SCREENSHOT` to a PNG path. The test exercises the actual preset dialogs and settings controls, including mixed video/audio delays and retained disabled values, then exits. `tests/ui/dialog.qml` keeps a modal open for visual checks; this test disables its elevation effect because the software backend cannot draw that shader. The test view and screenshot environment variables are only honored in this explicit test build. Ship with `CONFIG-=ui-test` and rebuild `main.o` after changing this flag.

## Live Windows-host check

1. End the old client's stream when convenient, then start Moonshine Client. Existing pairing and presets should be retained after settings migration.
2. Choose a windowed preset and start the host's Desktop application. Verify normal video, audio, and input.
3. Switch virtual desktops for at least 30 seconds. Check that GPU decoder use falls and the connection stays alive. Return and verify fresh video resumes without reconnecting or a black screen.
4. Repeat with minimize/restore. Enable unfocused pausing, focus another window on the same desktop, and verify the independent option.
5. Repeat rapid switches and quit while paused. Check the temporary KWin script is removed after stream exit.
6. Disable the options and confirm the previous continuous playback behavior.
7. With unfocused pausing enabled before launch, verify the stream window appears immediately, then focus it and confirm video resumes. Repeat while the stream starts in the background, including with the Vulkan renderer.
8. Start unfocused and verify actual video renders for about one second before freezing. Check the retained preview is readable. Set the video delay to 60 seconds and check focus restores/cancels the countdown.
9. Enable audio mute independently of video. Test different audio focus/hidden delays, both together, and audio-only background controls.
10. Hover over a paused stream and verify the local cursor is visible. Focus the stream and check normal cursor/capture behavior returns. Repeat with remote-desktop mouse mode, letterboxing, cursor visibility toggled, and a held mouse button while losing focus.

The initial build was checked against the user's Windows host on 2026-10-05. The user confirmed video resumes after returning to its virtual desktop. While away, the client logged local video pause, retained three streaming UDP sockets, used about 0.5% CPU over a two-second sample, and reported 0% GPU decoder activity in three samples. These are spot checks, not a full performance comparison. The initial Vulkan startup issue was fixed and the user confirmed unfocused pause works with the visible window. A later same-desktop spot check again logged video pause, retained three streaming sockets, and used about 0.5% CPU; GPU counters were unavailable in that check. The startup warmup and independent timers pass automated tests but still need a complete live host check.

Network traffic and host encoding are expected to continue during local pause. Audio follows the separate mute setting. The rest of the live checklist remains pending; automated tests alone should not be described as measured performance savings.
