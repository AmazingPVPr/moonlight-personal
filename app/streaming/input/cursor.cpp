#include "input.h"

#include <Limelight.h>
#include "SDL_compat.h"

void SdlInputHandler::setWindow(SDL_Window* window)
{
    m_Window = window;
    applyCaptureState(m_MouseCaptureRequested);
}

void SdlInputHandler::notifyFocusLost()
{
    // Preserve the existing focus-loss policy during the pause delay: windowed
    // relative capture is released, while fullscreen/absolute capture keeps its
    // intent. The paused cursor override suspends either kind of actual capture.
    if (!(SDL_GetWindowFlags(m_Window) & SDL_WINDOW_FULLSCREEN) && !m_AbsoluteMouseMode) {
        setCaptureActive(false);
    }
    raiseAllKeys();
}

void SdlInputHandler::notifyFocusGained()
{
    // A resume while unfocused leaves capture deferred until this event.
    applyCaptureState(m_MouseCaptureRequested);
}

bool SdlInputHandler::isCaptureActive()
{
    if (m_VideoPaused) {
        return false;
    }
    return SDL_GetRelativeMouseMode() || m_FakeMouseCaptureActive;
}

void SdlInputHandler::setVideoPaused(bool paused)
{
    if (m_VideoPaused == paused) {
        // These getters are cheap. Do not repeatedly change SDL modes in the
        // main loop, but correct a hide/capture changed by a recreated surface.
        if (paused && m_Window && (SDL_GetRelativeMouseMode() || m_FakeMouseCaptureActive ||
                                  SDL_ShowCursor(SDL_QUERY) != SDL_ENABLE)) {
            applyCaptureState(false);
        }
        return;
    }
    if (paused) {
        m_RelativeMouseModeBeforePause = SDL_GetRelativeMouseMode();
        m_VideoPaused = true;
        raiseAllMouseButtons();
        m_PendingMouseButtonsAllUpOnVideoRegionLeave = false;
        applyCaptureState(false);
    }
    else {
        m_VideoPaused = false;
        applyCaptureState(m_MouseCaptureRequested);
        m_RelativeMouseModeBeforePause = false;
    }
}

void SdlInputHandler::notifyWindowRecreated()
{
    if (!m_Window) {
        return;
    }
    // SDL can keep logical mode flags while replacing the native window. Force
    // a one-time rebind to the new window, preserving the requested mode.
    SDL_SetRelativeMouseMode(SDL_FALSE);
    m_FakeMouseCaptureActive = false;
    applyCaptureState(m_MouseCaptureRequested);
}

void SdlInputHandler::updateCursorVisibility()
{
    if (m_VideoPaused || !isCaptureActive()) {
        SDL_ShowCursor(SDL_ENABLE);
    }
    else if (SDL_GetRelativeMouseMode()) {
        SDL_ShowCursor(SDL_DISABLE);
    }
    else {
        bool shouldHide = m_MouseCursorCapturedVisibilityState == SDL_DISABLE &&
                          (!m_AbsoluteMouseMode || m_MouseWasInVideoRegion);
        SDL_ShowCursor(shouldHide ? SDL_DISABLE : SDL_ENABLE);
    }
}

void SdlInputHandler::toggleCursorVisibility()
{
    if (!SDL_GetRelativeMouseMode() && !(m_VideoPaused && !m_AbsoluteMouseMode && m_RelativeMouseModeBeforePause)) {
        m_MouseCursorCapturedVisibilityState = !m_MouseCursorCapturedVisibilityState;
        updateCursorVisibility();
    }
    else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Cursor can only be shown in remote desktop mouse mode");
    }
}

void SdlInputHandler::raiseAllMouseButtons()
{
    for (int button = BUTTON_LEFT; button <= BUTTON_X2; button++) {
        if (m_MouseButtonsDown & (1U << button)) {
            LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, button);
        }
    }
    m_MouseButtonsDown = 0;
}

void SdlInputHandler::setCaptureActive(bool active)
{
    m_MouseCaptureRequested = active;
    applyCaptureState(active);
}

void SdlInputHandler::applyCaptureState(bool active)
{
    if (!m_Window) {
        return;
    }
    active = active && !m_VideoPaused && (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_INPUT_FOCUS);
    if (active) {
        m_FakeMouseCaptureActive = false;
        if (m_AbsoluteMouseMode || SDL_SetRelativeMouseMode(SDL_TRUE) < 0) {
            // Relative mode is unavailable or not wanted. Keep normal absolute
            // capture intent separate from the paused native-cursor override.
            m_FakeMouseCaptureActive = true;
        }
        if (m_AbsoluteMouseMode) {
            int mouseX, mouseY;
            if (SDL_GetMouseFocus() == m_Window) {
                // Native Wayland cannot query global coordinates. Hover events
                // still update SDL's local position while video is paused.
                SDL_GetMouseState(&mouseX, &mouseY);
            }
            else {
                int windowX, windowY;
                SDL_GetGlobalMouseState(&mouseX, &mouseY);
                SDL_GetWindowPosition(m_Window, &windowX, &windowY);
                mouseX -= windowX;
                mouseY -= windowY;
            }
            m_MouseWasInVideoRegion = isMouseInVideoRegion(mouseX, mouseY);
            if (m_MouseWasInVideoRegion) {
                SDL_MouseMotionEvent motionEvent = {};
                motionEvent.type = SDL_MOUSEMOTION;
                motionEvent.timestamp = SDL_GetTicks();
                motionEvent.windowID = SDL_GetWindowID(m_Window);
                motionEvent.x = mouseX;
                motionEvent.y = mouseY;
                handleMouseMotionEvent(&motionEvent);
            }
        }
    }
    else {
        SDL_SetRelativeMouseMode(SDL_FALSE);
        SDL_CaptureMouse(SDL_FALSE);
        m_FakeMouseCaptureActive = false;
    }
    updateCursorVisibility();
    updatePointerRegionLock();
    updateKeyboardGrabState();
}

void SdlInputHandler::updateKeyboardGrabState()
{
    if (!m_Window) {
        return;
    }
    Uint32 flags = SDL_GetWindowFlags(m_Window);
    bool shouldGrab = !m_VideoPaused && m_CaptureSystemKeysMode != StreamingPreferences::CSK_OFF &&
                      isCaptureActive() && (flags & SDL_WINDOW_INPUT_FOCUS);
    if (shouldGrab && m_CaptureSystemKeysMode == StreamingPreferences::CSK_FULLSCREEN &&
            !(flags & SDL_WINDOW_FULLSCREEN)) {
        shouldGrab = false;
    }
    SDL_SetHint(SDL_HINT_WINDOWS_NO_CLOSE_ON_ALT_F4, shouldGrab ? "1" : "0");
#if SDL_VERSION_ATLEAST(2, 0, 15)
    SDL_SetWindowKeyboardGrab(m_Window, shouldGrab ? SDL_TRUE : SDL_FALSE);
#endif
    m_KeyboardCaptureActive = shouldGrab;
}

bool SdlInputHandler::isSystemKeyCaptureActive()
{
    if (m_VideoPaused || m_CaptureSystemKeysMode == StreamingPreferences::CSK_OFF || !m_Window) {
        return false;
    }
    Uint32 flags = SDL_GetWindowFlags(m_Window);
    if (!(flags & SDL_WINDOW_INPUT_FOCUS) || !m_KeyboardCaptureActive) {
        return false;
    }
    return m_CaptureSystemKeysMode != StreamingPreferences::CSK_FULLSCREEN ||
           (flags & SDL_WINDOW_FULLSCREEN);
}
