#pragma once

#include <xcb/xcb.h>
#include <cstdlib>
#include <cstring>

namespace DesktopVisibilityX11 {

inline bool readCardinal(xcb_connection_t* connection, xcb_window_t window,
                         xcb_atom_t property, uint32_t& value)
{
    // Checked replies keep BadWindow and connection errors local to this
    // request. Xlib's process-global error handler must never be replaced or
    // invoked by a stale native ID while SDL recreates/destroys a window.
    xcb_generic_error_t* error = nullptr;
    xcb_get_property_reply_t* reply = xcb_get_property_reply(connection,
            xcb_get_property(connection, 0, window, property, XCB_ATOM_CARDINAL, 0, 1), &error);
    const bool valid = !error && reply && reply->type == XCB_ATOM_CARDINAL &&
            reply->format == 32 && xcb_get_property_value_length(reply) == sizeof(value);
    if (valid) {
        std::memcpy(&value, xcb_get_property_value(reply), sizeof(value));
    }
    std::free(error);
    std::free(reply);
    return valid;
}

inline xcb_atom_t findAtom(xcb_connection_t* connection, const char* name)
{
    xcb_generic_error_t* error = nullptr;
    xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(connection,
            xcb_intern_atom(connection, 1, static_cast<uint16_t>(std::strlen(name)), name), &error);
    const xcb_atom_t atom = !error && reply ? reply->atom : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
    std::free(error);
    std::free(reply);
    return atom;
}

}
