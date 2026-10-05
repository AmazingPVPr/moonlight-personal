#include "streaming/input/input.h"
#include "streaming/streamutils.h"
#include <Limelight.h>

#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <cassert>
#include <cstdio>
#include <utility>
#include <vector>

// The production cursor/mouse methods use deterministic in-process SDL and host
// endpoints. No display, host connection, controller, or user settings are used.
namespace {
Uint32 windowFlags = SDL_WINDOW_INPUT_FOCUS;
SDL_bool relativeMode = SDL_FALSE;
int cursorVisibility = SDL_ENABLE;
bool relativeSupported = true;
bool keyboardGrab = false, pointerRect = false, dragCapture = false;
int mouseX = 160, mouseY = 90, modeChanges = 0, raisedKeys = 0;
int hostMotion = 0, hostScroll = 0;
std::vector<std::pair<char, int>> hostButtons;
auto* window = reinterpret_cast<SDL_Window*>(uintptr_t(1));
}

namespace WMUtils {
bool isRunningWayland() { return false; }
bool isGpuSlow() { return false; }
}

extern "C" {
Uint32 SDL_GetWindowFlags(SDL_Window*) { return windowFlags; }
SDL_Window* SDL_GetMouseFocus() { return window; }
SDL_bool SDL_GetRelativeMouseMode() { return relativeMode; }
int SDL_SetRelativeMouseMode(SDL_bool enabled) {
    modeChanges++;
    if (enabled && !relativeSupported) return -1;
    relativeMode = enabled;
    return 0;
}
int SDL_ShowCursor(int state) {
    if (state != SDL_QUERY) cursorVisibility = state;
    return cursorVisibility;
}
void SDL_SetWindowKeyboardGrab(SDL_Window*, SDL_bool grab) { keyboardGrab = grab; }
int SDL_CaptureMouse(SDL_bool enabled) { dragCapture = enabled; return 0; }
int SDL_SetWindowMouseRect(SDL_Window*, const SDL_Rect* rect) { pointerRect = rect; return 0; }
int SDL_GetNumVideoDisplays() { return 2; }
Uint32 SDL_GetGlobalMouseState(int* x, int* y) { if (x) *x=mouseX; if (y) *y=mouseY; return 0; }
Uint32 SDL_GetMouseState(int* x, int* y) { return SDL_GetGlobalMouseState(x,y); }
void SDL_GetWindowPosition(SDL_Window*, int* x, int* y) { *x=0; *y=0; }
void SDL_GetWindowSize(SDL_Window*, int* w, int* h) { *w=320; *h=180; }
Uint32 SDL_GetWindowID(SDL_Window*) { return 1; }
int SDL_PeepEvents(SDL_Event*, int, SDL_eventaction, Uint32, Uint32) { return 0; }
int LiSendMousePositionEvent(short, short, short, short) { hostMotion++; return 0; }
int LiSendMouseMoveEvent(short, short) { hostMotion++; return 0; }
int LiSendMouseButtonEvent(char action, int button) { hostButtons.emplace_back(action,button); return 0; }
int LiSendHighResScrollEvent(short) { hostScroll++; return 0; }
int LiSendHighResHScrollEvent(short) { hostScroll++; return 0; }
}

void StreamUtils::scaleSourceToDestinationSurface(SDL_Rect* src, SDL_Rect* dst)
{
    const double scale = qMin(double(dst->w)/src->w, double(dst->h)/src->h);
    const int width = int(src->w*scale), height = int(src->h*scale);
    dst->x += (dst->w-width)/2;
    dst->y += (dst->h-height)/2;
    dst->w=width; dst->h=height;
}

// Fixture initialization excludes unrelated controller setup. All exercised
// state changes below are the actual production cursor.cpp and mouse.cpp.
SdlInputHandler::SdlInputHandler(StreamingPreferences& prefs, int width, int height)
    : m_Window(nullptr), m_SwapMouseButtons(prefs.swapMouseButtons), m_ReverseScrollDirection(false),
      m_NeedsManualCaptureOnLeave(true), m_MouseWasInVideoRegion(false),
      m_PendingMouseButtonsAllUpOnVideoRegionLeave(false), m_PointerRegionLockActive(false),
      m_PointerRegionLockToggledByUser(false), m_FakeMouseCaptureActive(false), m_KeyboardCaptureActive(false),
      m_MouseCaptureRequested(false), m_VideoPaused(false), m_RelativeMouseModeBeforePause(false),
      m_MouseButtonsDown(0), m_CaptureSystemKeysMode(StreamingPreferences::CSK_ALWAYS),
      m_MouseCursorCapturedVisibilityState(SDL_DISABLE), m_StreamWidth(width), m_StreamHeight(height),
      m_AbsoluteMouseMode(prefs.absoluteMouseMode)
{}
SdlInputHandler::~SdlInputHandler() {}
void SdlInputHandler::raiseAllKeys() { raisedKeys++; }

static void reset()
{
    windowFlags=SDL_WINDOW_INPUT_FOCUS;
    relativeMode=SDL_FALSE; cursorVisibility=SDL_ENABLE; relativeSupported=true;
    keyboardGrab=pointerRect=dragCapture=false;
    mouseX=160; mouseY=90; modeChanges=raisedKeys=hostMotion=hostScroll=0;
    hostButtons.clear();
}

static void hover(SdlInputHandler& input)
{
    SDL_MouseMotionEvent event={}; event.x=mouseX; event.y=mouseY; event.xrel=2; event.yrel=3;
    input.handleMouseMotionEvent(&event);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc,argv);
    QTemporaryDir storage;
    assert(storage.isValid());
    QCoreApplication::setOrganizationName("MoonshineCursorTests");
    QCoreApplication::setApplicationName("Fixture");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,storage.path());
    auto& prefs=*StreamingPreferences::get();

    reset(); prefs.absoluteMouseMode=true; prefs.swapMouseButtons=false;
    {
        SdlInputHandler input(prefs,320,180); input.setWindow(window); input.setCaptureActive(true);
        assert(input.isCaptureActive() && cursorVisibility==SDL_DISABLE && keyboardGrab);
        hover(input); assert(hostMotion>0);
        input.setVideoPaused(true);
        assert(input.isCaptureRequested() && !input.isCaptureActive());
        assert(cursorVisibility==SDL_ENABLE && !relativeMode && !keyboardGrab && !pointerRect && !dragCapture);
        const int motion=hostMotion, changes=modeChanges;
        input.setVideoPaused(true); assert(modeChanges==changes);
        hover(input); assert(hostMotion==motion && cursorVisibility==SDL_ENABLE);
        SDL_MouseButtonEvent button={}; button.button=SDL_BUTTON_LEFT; button.state=SDL_RELEASED; button.x=160; button.y=90;
        input.handleMouseButtonEvent(&button); assert(hostButtons.empty());
        SDL_MouseWheelEvent wheel={}; wheel.preciseY=1; input.handleMouseWheelEvent(&wheel); assert(hostScroll==0);
        input.toggleCursorVisibility(); assert(cursorVisibility==SDL_ENABLE);
        input.setCaptureActive(true); assert(!input.isCaptureActive() && cursorVisibility==SDL_ENABLE);
        input.notifyWindowRecreated(); assert(!input.isCaptureActive() && cursorVisibility==SDL_ENABLE);
        input.setVideoPaused(false); assert(input.isCaptureActive() && cursorVisibility==SDL_ENABLE);
        input.toggleCursorVisibility(); assert(cursorVisibility==SDL_DISABLE);
        input.setVideoPaused(true); input.setCaptureActive(false); input.setVideoPaused(false);
        assert(!input.isCaptureRequested() && !input.isCaptureActive() && cursorVisibility==SDL_ENABLE);
    }

    reset(); prefs.absoluteMouseMode=true;
    {
        SdlInputHandler input(prefs,320,180); input.setWindow(window); input.setCaptureActive(true);
        windowFlags &= ~SDL_WINDOW_INPUT_FOCUS; input.notifyFocusLost();
        assert(input.isCaptureRequested() && input.isCaptureActive()); // ordinary delay behavior
        input.setVideoPaused(true); input.setVideoPaused(false);
        assert(input.isCaptureRequested() && !input.isCaptureActive() && cursorVisibility==SDL_ENABLE);
        windowFlags |= SDL_WINDOW_INPUT_FOCUS; input.notifyFocusGained();
        assert(input.isCaptureActive() && cursorVisibility==SDL_DISABLE);
    }

    reset(); prefs.absoluteMouseMode=false;
    {
        SdlInputHandler input(prefs,320,180); input.setWindow(window); input.setCaptureActive(true);
        assert(relativeMode && cursorVisibility==SDL_DISABLE);
        windowFlags &= ~SDL_WINDOW_INPUT_FOCUS; input.notifyFocusLost();
        assert(!input.isCaptureRequested() && !input.isCaptureActive() && raisedKeys==1);
        input.setVideoPaused(true); input.setCaptureActive(true);
        assert(input.isCaptureRequested() && !relativeMode && cursorVisibility==SDL_ENABLE);
        input.setVideoPaused(false); assert(!relativeMode && cursorVisibility==SDL_ENABLE);
        windowFlags |= SDL_WINDOW_INPUT_FOCUS; input.notifyFocusGained();
        assert(relativeMode && cursorVisibility==SDL_DISABLE);
        input.setVideoPaused(true); input.toggleCursorVisibility();
        assert(cursorVisibility==SDL_ENABLE);
        input.setVideoPaused(false); assert(relativeMode && cursorVisibility==SDL_DISABLE);
    }

    reset(); prefs.absoluteMouseMode=false;
    {
        windowFlags |= SDL_WINDOW_FULLSCREEN;
        SdlInputHandler input(prefs,320,180); input.setWindow(window); input.setCaptureActive(true);
        windowFlags &= ~SDL_WINDOW_INPUT_FOCUS; input.notifyFocusLost();
        assert(input.isCaptureRequested());
        input.setVideoPaused(true); assert(!relativeMode && cursorVisibility==SDL_ENABLE);
        input.setVideoPaused(false); assert(!relativeMode);
        windowFlags |= SDL_WINDOW_INPUT_FOCUS; input.notifyFocusGained(); assert(relativeMode);
    }

    reset(); prefs.absoluteMouseMode=true; prefs.swapMouseButtons=true;
    {
        windowFlags |= SDL_WINDOW_FULLSCREEN;
        SdlInputHandler input(prefs,320,180); input.setWindow(window); input.setCaptureActive(true);
        assert(pointerRect && keyboardGrab);
        SDL_MouseButtonEvent button={}; button.button=SDL_BUTTON_LEFT; button.state=SDL_PRESSED; button.x=160; button.y=90;
        input.handleMouseButtonEvent(&button);
        assert(hostButtons.size()==1 && hostButtons[0]==std::make_pair(char(BUTTON_ACTION_PRESS),BUTTON_RIGHT));
        dragCapture=true; input.setVideoPaused(true);
        assert(hostButtons.size()==2 && hostButtons[1]==std::make_pair(char(BUTTON_ACTION_RELEASE),BUTTON_RIGHT));
        assert(!pointerRect && !keyboardGrab && !dragCapture);
        button.state=SDL_RELEASED; input.handleMouseButtonEvent(&button); assert(hostButtons.size()==2);
        input.setVideoPaused(true); assert(hostButtons.size()==2);
        input.setVideoPaused(false); assert(pointerRect && keyboardGrab);
    }

    reset(); prefs.absoluteMouseMode=false; prefs.swapMouseButtons=false;
    {
        relativeSupported=false;
        SdlInputHandler input(prefs,320,180); input.setWindow(window); input.setCaptureActive(true);
        assert(input.isCaptureActive() && !relativeMode && cursorVisibility==SDL_DISABLE);
        input.setVideoPaused(true); input.toggleCursorVisibility(); input.setVideoPaused(false);
        assert(input.isCaptureActive() && cursorVisibility==SDL_ENABLE);
    }

    reset(); prefs.absoluteMouseMode=true;
    {
        SdlInputHandler input(prefs,320,240); input.setVideoPaused(true); input.setWindow(window);
        input.setCaptureActive(true); assert(cursorVisibility==SDL_ENABLE && !input.isCaptureActive());
        mouseX=0; input.setVideoPaused(false); assert(cursorVisibility==SDL_ENABLE); // letterbox
        mouseX=160; hover(input); assert(cursorVisibility==SDL_DISABLE);
        input.setVideoPaused(true); cursorVisibility=SDL_DISABLE; input.setVideoPaused(true);
        assert(cursorVisibility==SDL_ENABLE);
    }
    std::puts("Production cursor/mouse tests passed: pause override, focus-aware restoration, deferred capture/hotkey, native recreation, swapped-button release, and inactive hover suppression.");
}
