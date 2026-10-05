// Creates only an unmapped InputOnly test window. It cannot take focus or draw
// over the user's desktop. Property helpers are the same ones used in production.
#include "../../app/streaming/desktopvisibility-x11.h"
#include <cassert>
#include <cstdio>

int main()
{
    int screenIndex = 0;
    auto connection = xcb_connect(nullptr, &screenIndex);
    if (!connection || xcb_connection_has_error(connection)) {
        std::fputs("An X11/XWayland display is required for this check.\n", stderr);
        if (connection) xcb_disconnect(connection);
        return 2;
    }
    auto screen = xcb_setup_roots_iterator(xcb_get_setup(connection));
    for (int i = 0; i < screenIndex && screen.rem; ++i) xcb_screen_next(&screen);
    assert(screen.rem);
    auto window = xcb_generate_id(connection);
    auto created = xcb_create_window_checked(connection, 0, window, screen.data->root,
                    0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, 0, nullptr);
    auto error = xcb_request_check(connection, created);
    assert(!error);
    std::free(error);

    uint32_t value = 0;
    assert(!DesktopVisibilityX11::readCardinal(connection, window, XCB_ATOM_WM_NAME, value));
    const uint32_t assigned = 5;
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NAME,
                        XCB_ATOM_CARDINAL, 32, 1, &assigned);
    assert(DesktopVisibilityX11::readCardinal(connection, window, XCB_ATOM_WM_NAME, value));
    assert(value == assigned);
    const uint32_t sticky = 0xffffffffU;
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NAME,
                        XCB_ATOM_CARDINAL, 32, 1, &sticky);
    assert(DesktopVisibilityX11::readCardinal(connection, window, XCB_ATOM_WM_NAME, value));
    assert(value == sticky);

    xcb_destroy_window(connection, window);
    // Repeated checked reads of a destroyed ID must neither terminate the
    // process nor invoke Xlib's process-global error handler.
    for (int i = 0; i < 10; ++i) {
        assert(!DesktopVisibilityX11::readCardinal(connection, window, XCB_ATOM_WM_NAME, value));
    }
    assert(!xcb_connection_has_error(connection));
    assert(DesktopVisibilityX11::findAtom(connection, "WM_NAME") == XCB_ATOM_WM_NAME);
    xcb_disconnect(connection);
    std::puts("Passed X11 property, sticky desktop value, destroyed native-window ID, and connection survival checks.");
}
