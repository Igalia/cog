/*
 * cog-platform-x11.c
 * Copyright (C) 2020-2022 Igalia S.L.
 *
 * Distributed under terms of the MIT license.
 */

#include "../../core/cog.h"

#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <wpe/webkit.h>
#include <wpe/fdo.h>
#include <wpe/fdo-egl.h>

#include <X11/Xlib-xcb.h>
#include <X11/Xlib.h>
#include <xcb/xcb.h>
#include <xcb/xcb_cursor.h>
#include <xkbcommon/xkbcommon-x11.h>

#if COG_HAVE_LIBPORTAL
#    include "../common/cog-file-chooser.h"
#endif /* COG_HAVE_LIBPORTAL */
#include "../common/cog-gl-utils.h"
#include "../common/cursors.h"
#include "../common/egl-proc-address.h"
#if COG_HAVE_LIBPORTAL
#    include "cog-xdp-parent-x11.h"
#endif /* COG_HAVE_LIBPORTAL */

#ifndef EGL_EXT_platform_base
#define EGL_EXT_platform_base 1
typedef EGLDisplay (EGLAPIENTRYP PFNEGLGETPLATFORMDISPLAYEXTPROC) (EGLenum platform, void *native_display, const EGLint *attrib_list);
#endif

#ifndef EGL_PLATFORM_X11_EXT
#    define EGL_PLATFORM_X11_EXT 0x31D5
#endif /* !EGL_PLATFORM_X11_EXT */

#define DEFAULT_WIDTH  1024
#define DEFAULT_HEIGHT  768

struct _CogX11PlatformClass {
    CogPlatformClass parent_class;
};

struct _CogX11Platform {
    CogPlatform parent;
    CogView    *web_view;
};

G_DECLARE_FINAL_TYPE(CogX11Platform, cog_x11_platform, COG, X11_PLATFORM, CogPlatform)

G_DEFINE_DYNAMIC_TYPE_EXTENDED(
    CogX11Platform,
    cog_x11_platform,
    COG_TYPE_PLATFORM,
    0,
    g_io_extension_point_implement(COG_MODULES_PLATFORM_EXTENSION_POINT, g_define_type_id, "x11", 300);)

struct CogX11Display {
    Display *display;

    struct {
        xcb_connection_t *connection;
        xcb_screen_t *screen;

        xcb_atom_t atom_wm_protocols;
        xcb_atom_t atom_wm_delete_window;
        xcb_atom_t atom_net_wm_name;
        xcb_atom_t atom_utf8_string;

        struct {
            int32_t x;
            int32_t y;
            uint32_t button;
            uint32_t state;
        } pointer;

        GSource *source;
    } xcb;

    struct {
        int32_t             device_id;
        struct xkb_context *context;
        struct xkb_keymap  *keymap;
        struct xkb_state   *state;
        xkb_mod_mask_t      shift;
        xkb_mod_mask_t      control;
        xkb_mod_mask_t      alt;
        xkb_mod_mask_t      num_lock;
        xkb_mod_mask_t      caps_lock;
    } xkb;

    struct {
        PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display;

        EGLDisplay display;
        EGLConfig config;
        EGLContext context;
    } egl;

    CogGLRenderer gl_render;
};

struct CogX11Window {
    struct {
        xcb_window_t window;

        bool needs_repaint;
        bool needs_frame_completion;

        unsigned width;
        unsigned height;
    } xcb;

    struct {
        EGLSurface surface;
    } egl;

    struct {
        struct wpe_view_backend_exportable_fdo *exportable;
        struct wpe_view_backend *backend;

        struct wpe_fdo_egl_exported_image *image;
    } wpe;
};

static struct CogX11Display *s_display = NULL;
static struct CogX11Window *s_window = NULL;

static void
xcb_schedule_notice(void)
{
    /* There is a notice message already scheduled, do not queue another. */
    if (s_window->xcb.needs_repaint || s_window->xcb.needs_frame_completion)
        return;

    xcb_client_message_event_t client_message = {
        .response_type = XCB_CLIENT_MESSAGE,
        .format = 32,
        .window = s_window->xcb.window,
        .type = XCB_ATOM_NOTICE,
    };

    xcb_send_event (s_display->xcb.connection, 0, s_window->xcb.window,
                    0, (char *) &client_message);
    xcb_flush (s_display->xcb.connection);
}

static inline void
xcb_schedule_repaint(void)
{
    xcb_schedule_notice();
    s_window->xcb.needs_repaint = true;
}

static void
xcb_paint_image (struct wpe_fdo_egl_exported_image *image)
{
    eglMakeCurrent (s_display->egl.display, s_window->egl.surface, s_window->egl.surface, s_display->egl.context);

    glViewport (0, 0, s_window->xcb.width, s_window->xcb.height);
    glClearColor (1, 1, 1, 1);
    glClear (GL_COLOR_BUFFER_BIT);
    s_window->xcb.needs_repaint = false;

    if (image != EGL_NO_IMAGE) {
        if (s_window->wpe.image != image) {
            if (s_window->wpe.image)
                wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(s_window->wpe.exportable,
                                                                                    s_window->wpe.image);

            s_window->wpe.image = image;
            xcb_schedule_notice();
            s_window->xcb.needs_frame_completion = true;
        }

        cog_gl_renderer_paint(&s_display->gl_render, wpe_fdo_egl_exported_image_get_egl_image(s_window->wpe.image),
                              COG_GL_RENDERER_ROTATION_0);
    }

    eglSwapBuffers(s_display->egl.display, s_window->egl.surface);
}

static uint32_t
xcb_update_xkb_modifiers(uint32_t event_state)
{
    xkb_mod_mask_t depressed_mods = 0;
    xkb_mod_mask_t locked_mods = 0;
    uint32_t       wpe_modifiers = 0;
    if (event_state & XCB_MOD_MASK_SHIFT) {
        depressed_mods |= s_display->xkb.shift;
        wpe_modifiers |= wpe_input_keyboard_modifier_shift;
    }
    if (event_state & XCB_MOD_MASK_CONTROL) {
        depressed_mods |= s_display->xkb.control;
        wpe_modifiers |= wpe_input_keyboard_modifier_control;
    }
    if (event_state & XCB_MOD_MASK_1) {
        depressed_mods |= s_display->xkb.alt;
        wpe_modifiers |= wpe_input_keyboard_modifier_alt;
    }

    if (event_state & XCB_MOD_MASK_LOCK)
        locked_mods |= s_display->xkb.caps_lock;
    if (event_state & XCB_MOD_MASK_2)
        locked_mods |= s_display->xkb.num_lock;
    xkb_state_update_mask(s_display->xkb.state, depressed_mods, 0, locked_mods, 0, 0, 0);
    return wpe_modifiers;
}

static void
xcb_handle_key_press(CogView *view, xcb_key_press_event_t *event)
{
    uint32_t modifiers = xcb_update_xkb_modifiers(event->state);
    uint32_t keysym = xkb_state_key_get_one_sym(s_display->xkb.state, event->detail);

    struct wpe_input_keyboard_event input_event = {
        .time = event->time,
        .key_code = keysym,
        .hardware_key_code = event->detail,
        .pressed = true,
        .modifiers = modifiers,
    };
    cog_view_handle_key_event(view, &input_event);
}

static void
xcb_handle_key_release(CogView *view, xcb_key_press_event_t *event)
{
    uint32_t modifiers = xcb_update_xkb_modifiers(event->state);
    uint32_t keysym = xkb_state_key_get_one_sym(s_display->xkb.state, event->detail);

    struct wpe_input_keyboard_event input_event = {
        .time = event->time,
        .key_code = keysym,
        .hardware_key_code = event->detail,
        .pressed = false,
        .modifiers = modifiers,
    };
    cog_view_handle_key_event(view, &input_event);
}

static void
xcb_handle_axis(xcb_button_press_event_t *event, const int16_t axis_delta[2])
{
    struct wpe_input_axis_2d_event input_event = {
        .base =
            {
                .type = wpe_input_axis_event_type_mask_2d | wpe_input_axis_event_type_motion_smooth,
                .time = event->time,
                .x = s_display->xcb.pointer.x,
                .y = s_display->xcb.pointer.y,
            },
        .x_axis = axis_delta[0],
        .y_axis = axis_delta[1],
    };

    wpe_view_backend_dispatch_axis_event (s_window->wpe.backend, &input_event.base);
}

static void
xcb_handle_button_press(xcb_button_press_event_t *event)
{
    /*
     * Match the multiplier value used e.g. by libinput when using
     * libinput_event_pointer_get_scroll_value_v120(), which in turn is
     * based on a design document by Microsoft about enhanced mouse wheel
     * support.
     */
    enum {
        SCROLL_WHEEL_STEP_SIZE = 120,
    };

    /* clang-format off */
    static const int16_t axis_delta[4][2] = {
        {  0,  SCROLL_WHEEL_STEP_SIZE },
        {  0, -SCROLL_WHEEL_STEP_SIZE },
        {  SCROLL_WHEEL_STEP_SIZE,  0 },
        { -SCROLL_WHEEL_STEP_SIZE,  0 },
    };
    /* clang-format on */

    switch (event->detail) {
    case 1:
    case 2:
    case 3:
        s_display->xcb.pointer.button = event->detail;
        s_display->xcb.pointer.state = 1;
        break;
    case 4:
    case 5:
    case 6:
    case 7:
        xcb_handle_axis (event, axis_delta[event->detail - 4]);
        return;
    default:
        return;
    }

    struct wpe_input_pointer_event input_event = {
        .type = wpe_input_pointer_event_type_button,
        .time = event->time,
        .x = s_display->xcb.pointer.x,
        .y = s_display->xcb.pointer.y,
        .button = s_display->xcb.pointer.button,
        .state = s_display->xcb.pointer.state,
    };

    wpe_view_backend_dispatch_pointer_event (s_window->wpe.backend, &input_event);
}

static void
xcb_handle_button_release (xcb_button_release_event_t *event)
{
    switch (event->detail) {
    case 1:
    case 2:
    case 3:
        s_display->xcb.pointer.button = event->detail;
        s_display->xcb.pointer.state = 0;
        break;
    default:
        return;
    }

    struct wpe_input_pointer_event input_event = {
        .type = wpe_input_pointer_event_type_button,
        .time = event->time,
        .x = s_display->xcb.pointer.x,
        .y = s_display->xcb.pointer.y,
        .button = s_display->xcb.pointer.button,
        .state = s_display->xcb.pointer.state,
    };

    wpe_view_backend_dispatch_pointer_event (s_window->wpe.backend, &input_event);
}

static void
xcb_handle_motion_event (xcb_motion_notify_event_t *event)
{
    s_display->xcb.pointer.x = event->event_x;
    s_display->xcb.pointer.y = event->event_y;

    struct wpe_input_pointer_event input_event = {
        .type = wpe_input_pointer_event_type_motion,
        .time = event->time,
        .x = s_display->xcb.pointer.x,
        .y = s_display->xcb.pointer.y,
        .button = s_display->xcb.pointer.button,
        .state = s_display->xcb.pointer.state,
    };

    wpe_view_backend_dispatch_pointer_event (s_window->wpe.backend, &input_event);
}

static void
view_backend_modify_activity_state(xcb_window_t window_id, enum wpe_view_activity_state state_flag, bool enable)
{
    struct CogX11Window *window = s_window;

    if (window->xcb.window != window_id)
        return;

    if (enable)
        wpe_view_backend_add_activity_state(window->wpe.backend, state_flag);
    else
        wpe_view_backend_remove_activity_state(window->wpe.backend, state_flag);
}

static inline void
xcb_handle_visibility_event(const xcb_visibility_notify_event_t *event)
{
    switch ((xcb_visibility_t) event->state) {
    case XCB_VISIBILITY_UNOBSCURED:
    case XCB_VISIBILITY_PARTIALLY_OBSCURED:
        view_backend_modify_activity_state(event->window, wpe_view_activity_state_visible, true);
        break;
    case XCB_VISIBILITY_FULLY_OBSCURED:
        view_backend_modify_activity_state(event->window, wpe_view_activity_state_visible, false);
        break;
    }
}

static void
on_export_fdo_egl_image(void *data, struct wpe_fdo_egl_exported_image *image)
{
    xcb_paint_image(image);
}

static void
xcb_process_events(CogView *view)
{
    bool repaint_needed = false;

    xcb_generic_event_t *event = NULL;

    while ((event = xcb_poll_for_event (s_display->xcb.connection))) {
        switch (event->response_type & 0x7f) {
        case XCB_CONFIGURE_NOTIFY:
        {
            xcb_configure_notify_event_t *configure_notify = (xcb_configure_notify_event_t *) event;
            if (configure_notify->width == s_window->xcb.width
                && configure_notify->height == s_window->xcb.height)
                break;

            s_window->xcb.width = configure_notify->width;
            s_window->xcb.height = configure_notify->height;

            wpe_view_backend_dispatch_set_size (s_window->wpe.backend,
                                                s_window->xcb.width,
                                                s_window->xcb.height);
            repaint_needed = true;
            break;
        }
        case XCB_CLIENT_MESSAGE:
        {
            xcb_client_message_event_t *client_message = (xcb_client_message_event_t *) event;
            if (client_message->window != s_window->xcb.window)
                break;

            if (client_message->type == s_display->xcb.atom_wm_protocols
                && client_message->data.data32[0] == s_display->xcb.atom_wm_delete_window) {
                g_application_quit (g_application_get_default ());
                break;
            }

            if (client_message->type == XCB_ATOM_NOTICE) {
                if (s_window->xcb.needs_frame_completion) {
                    s_window->xcb.needs_frame_completion = false;
                    wpe_view_backend_exportable_fdo_dispatch_frame_complete(s_window->wpe.exportable);
                }

                if (s_window->xcb.needs_repaint)
                    xcb_paint_image(s_window->wpe.image);
            }
            break;
        }
        case XCB_KEY_PRESS:
            xcb_handle_key_press(view, (xcb_key_press_event_t *) event);
            break;
        case XCB_KEY_RELEASE:
            xcb_handle_key_release(view, (xcb_key_release_event_t *) event);
            break;
        case XCB_BUTTON_PRESS:
            xcb_handle_button_press ((xcb_button_press_event_t *) event);
            break;
        case XCB_BUTTON_RELEASE:
            xcb_handle_button_release ((xcb_button_release_event_t *) event);
            break;
        case XCB_MOTION_NOTIFY:
            xcb_handle_motion_event ((xcb_motion_notify_event_t *) event);
            break;

        case XCB_FOCUS_IN:
            view_backend_modify_activity_state(((xcb_focus_in_event_t *) event)->event,
                                               wpe_view_activity_state_focused,
                                               true);
            break;
        case XCB_FOCUS_OUT:
            view_backend_modify_activity_state(((xcb_focus_in_event_t *) event)->event,
                                               wpe_view_activity_state_focused,
                                               false);
            break;
        case XCB_MAP_NOTIFY:
            view_backend_modify_activity_state(((xcb_focus_in_event_t *) event)->event,
                                               wpe_view_activity_state_in_window,
                                               true);
            break;
        case XCB_UNMAP_NOTIFY:
            view_backend_modify_activity_state(((xcb_focus_in_event_t *) event)->event,
                                               wpe_view_activity_state_in_window,
                                               false);
            break;
        case XCB_VISIBILITY_NOTIFY:
            xcb_handle_visibility_event((xcb_visibility_notify_event_t *) event);
            break;
        case XCB_EXPOSE: {
            const xcb_expose_event_t *ev = (const xcb_expose_event_t *) event;
            repaint_needed = (ev->window == s_window->xcb.window && !ev->count);
            break;
        }

        default:
            break;
        }
    }

    if (repaint_needed)
        xcb_schedule_repaint();
};

struct xcb_source {
    GSource source;
    GPollFD pfd;
    xcb_connection_t *connection;
    CogX11Platform   *platform;
};

static gboolean
xcb_source_check (GSource *base)
{
    struct xcb_source *source = (struct xcb_source *) base;
    return !!source->pfd.revents;
}

static gboolean
xcb_source_dispatch (GSource *base, GSourceFunc callback, gpointer user_data)
{
    struct xcb_source *source = (struct xcb_source *) base;
    if (xcb_connection_has_error (source->connection))
        return G_SOURCE_REMOVE;

    if (source->pfd.revents & (G_IO_ERR | G_IO_HUP))
        return G_SOURCE_REMOVE;

    xcb_process_events(source->platform->web_view);
    source->pfd.revents = 0;
    return G_SOURCE_CONTINUE;
}

static xcb_atom_t
get_atom (struct xcb_connection_t *connection, const char *name)
{
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom (connection, 0, strlen(name), name);
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply (connection, cookie, NULL);

    xcb_atom_t atom;
    if (reply) {
        atom = reply->atom;
        free(reply);
    } else
        atom = XCB_NONE;

    return atom;
}

static gboolean
init_xcb ()
{
    s_window->xcb.width = DEFAULT_WIDTH;
    s_window->xcb.height = DEFAULT_HEIGHT;

    s_display->display = XOpenDisplay (NULL);
    s_display->xcb.connection = XGetXCBConnection (s_display->display);
    if (xcb_connection_has_error (s_display->xcb.connection))
        return FALSE;

    s_window->xcb.window = xcb_generate_id (s_display->xcb.connection);

    const struct xcb_setup_t *setup = xcb_get_setup (s_display->xcb.connection);
    s_display->xcb.screen = xcb_setup_roots_iterator (setup).data;

    static const uint32_t window_values[] = {
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_KEY_PRESS |
        XCB_EVENT_MASK_KEY_RELEASE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
        XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_VISIBILITY_CHANGE};

    xcb_create_window (s_display->xcb.connection,
                       XCB_COPY_FROM_PARENT,
                       s_window->xcb.window,
                       s_display->xcb.screen->root,
                       0, 0, s_window->xcb.width, s_window->xcb.height,
                       0,
                       XCB_WINDOW_CLASS_INPUT_OUTPUT,
                       s_display->xcb.screen->root_visual,
                       XCB_CW_EVENT_MASK, window_values);

    s_display->xcb.atom_wm_protocols = get_atom (s_display->xcb.connection, "WM_PROTOCOLS");
    s_display->xcb.atom_wm_delete_window = get_atom (s_display->xcb.connection, "WM_DELETE_WINDOW");
    s_display->xcb.atom_net_wm_name = get_atom (s_display->xcb.connection, "_NET_WM_NAME");
    s_display->xcb.atom_utf8_string = get_atom (s_display->xcb.connection, "UTF8_STRING");
    xcb_change_property (s_display->xcb.connection,
                         XCB_PROP_MODE_REPLACE,
                         s_window->xcb.window,
                         s_display->xcb.atom_wm_protocols,
                         XCB_ATOM_ATOM,
                         32,
                         1, &s_display->xcb.atom_wm_delete_window);

    xcb_change_property (s_display->xcb.connection,
                         XCB_PROP_MODE_REPLACE,
                         s_window->xcb.window,
                         s_display->xcb.atom_net_wm_name,
                         s_display->xcb.atom_utf8_string,
                         8,
                         strlen("Cog"), "Cog");

    xcb_map_window (s_display->xcb.connection, s_window->xcb.window);
    xcb_flush (s_display->xcb.connection);

    xcb_schedule_repaint();

    return TRUE;
}

static void
clear_xcb (void)
{
    if (s_display->display)
        XCloseDisplay (s_display->display);
}

static gboolean
init_xkb (void)
{
    s_display->xkb.device_id = xkb_x11_get_core_keyboard_device_id (s_display->xcb.connection);
    if (s_display->xkb.device_id == -1)
        return FALSE;

    s_display->xkb.context = xkb_context_new (0);
    if (!s_display->xkb.context)
        return FALSE;

    s_display->xkb.keymap = xkb_x11_keymap_new_from_device (s_display->xkb.context, s_display->xcb.connection, s_display->xkb.device_id, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!s_display->xkb.keymap)
        return FALSE;

    s_display->xkb.shift = 1 << xkb_keymap_mod_get_index(s_display->xkb.keymap, "Shift");
    s_display->xkb.control = 1 << xkb_keymap_mod_get_index(s_display->xkb.keymap, "Control");
    s_display->xkb.alt = 1 << xkb_keymap_mod_get_index(s_display->xkb.keymap, "Mod1");
    s_display->xkb.caps_lock = 1 << xkb_keymap_mod_get_index(s_display->xkb.keymap, "Lock");
    s_display->xkb.num_lock = 1 << xkb_keymap_mod_get_index(s_display->xkb.keymap, "NumLock");

    s_display->xkb.state =
        xkb_x11_state_new_from_device(s_display->xkb.keymap, s_display->xcb.connection, s_display->xkb.device_id);
    if (!s_display->xkb.state)
        return FALSE;

    return TRUE;
}

static void
clear_xkb (void)
{
    if (s_display->xkb.state)
        xkb_state_unref (s_display->xkb.state);
    if (s_display->xkb.keymap)
        xkb_keymap_unref (s_display->xkb.keymap);
    if (s_display->xkb.context)
        xkb_context_unref (s_display->xkb.context);
}

static gboolean
init_egl (void)
{
    if (!(s_display->egl.get_platform_display = load_egl_proc_address("eglGetPlatformDisplayEXT"))) {
        g_warning("EGL_EXT_platform_x11 extension unavailable?");
        return FALSE;
    }

    s_display->egl.display = s_display->egl.get_platform_display(EGL_PLATFORM_X11_EXT, s_display->display, NULL);
    if (s_display->egl.display == EGL_NO_DISPLAY) {
        g_warning("Cannot open EGL display (error %#04x)", eglGetError());
        return FALSE;
    }

    if (!epoxy_has_egl_extension(s_display->egl.display, "EGL_EXT_platform_x11"))
        g_warning("eglGetPlatformDisplayEXT() returned a display, but "
                  "EGL_EXT_platform_x11 is missing. Continuing anyway, but things may break unexpectedly.");

    if (!eglInitialize (s_display->egl.display, NULL, NULL))
        return FALSE;
    if (!eglBindAPI (EGL_OPENGL_ES_API))
        return FALSE;

    static const EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE,
    };

    static const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 1,
        EGL_GREEN_SIZE, 1,
        EGL_BLUE_SIZE, 1,
        EGL_ALPHA_SIZE, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE,
    };

    {
        EGLint count = 0;
        EGLint matched = 0;
        EGLConfig *configs;

        if (!eglGetConfigs (s_display->egl.display, NULL, 0, &count) || count < 1)
            return FALSE;

        configs = g_new0 (EGLConfig, count);
        if (!eglChooseConfig (s_display->egl.display, config_attribs, configs, count, &matched) || !matched) {
            g_free (configs);
            return FALSE;
        }

        s_display->egl.config = configs[0];
        g_free (configs);
        if (!s_display->egl.config)
            return FALSE;
    }

    s_display->egl.context = eglCreateContext (s_display->egl.display,
                                               s_display->egl.config,
                                               EGL_NO_CONTEXT,
                                               context_attribs);
    if (s_display->egl.context == EGL_NO_CONTEXT)
        return FALSE;

    Window win = (Window) s_window->xcb.window;
    s_window->egl.surface =
        eglCreatePlatformWindowSurfaceEXT(s_display->egl.display, s_display->egl.config, &win, NULL);
    if (s_window->egl.surface == EGL_NO_SURFACE)
        return FALSE;

    eglMakeCurrent (s_display->egl.display, s_window->egl.surface, s_window->egl.surface, s_display->egl.context);
    return TRUE;
}

static void
clear_egl (void)
{
    if (s_display->egl.display != EGL_NO_DISPLAY) {
        if (epoxy_egl_version(s_display->egl.display) >= 12)
            eglReleaseThread();
        eglTerminate(s_display->egl.display);
        s_display->egl.display = EGL_NO_DISPLAY;
    }
}

static gboolean
init_glib(CogX11Platform *platform)
{
    static GSourceFuncs xcb_source_funcs = {
        .check = xcb_source_check,
        .dispatch = xcb_source_dispatch,
    };

    s_display->xcb.source = g_source_new (&xcb_source_funcs,
                                          sizeof (struct xcb_source));
    {
        struct xcb_source *source = (struct xcb_source *) s_display->xcb.source;
        source->connection = s_display->xcb.connection;
        source->platform = platform;

        source->pfd.fd = xcb_get_file_descriptor (s_display->xcb.connection);
        source->pfd.events = G_IO_IN | G_IO_ERR | G_IO_HUP;
        source->pfd.revents = 0;
        g_source_add_poll (s_display->xcb.source, &source->pfd);

        g_source_set_name (s_display->xcb.source, "cog-x11: xcb");
        g_source_set_can_recurse (s_display->xcb.source, TRUE);
        g_source_attach (s_display->xcb.source, g_main_context_get_thread_default ());
    }

    return TRUE;
}

static void
clear_glib (void)
{
    if (s_display->xcb.source)
        g_source_destroy (s_display->xcb.source);
    g_clear_pointer (&s_display->xcb.source, g_source_unref);
}

static struct wpe_view_backend *
gamepad_provider_get_view_backend_for_gamepad(void *provider G_GNUC_UNUSED, void *gamepad G_GNUC_UNUSED)
{
    assert(s_window && s_window->wpe.backend);
    return s_window->wpe.backend;
}

static gboolean
cog_x11_platform_setup(CogPlatform *platform, CogShell *shell G_GNUC_UNUSED, const char *params, GError **error)
{
    g_assert (platform);
    g_return_val_if_fail (COG_IS_SHELL (shell), FALSE);

    s_display = calloc (sizeof (struct CogX11Display), 1);
    s_window = calloc (sizeof (struct CogX11Window), 1);

    if (!wpe_loader_init ("libWPEBackend-fdo-1.0.so")) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to set backend library name");
        return FALSE;
    }

    if (!init_xcb ()) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to initialize XCB");
        return FALSE;
    }

    if (!init_xkb ()) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to initialize XKB");
        return FALSE;
    }

    if (!init_egl ()) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to initialize EGL");
        return FALSE;
    }

    /*
     * The call to init_egl() right above leaves the EGLContext active,
     * which means the renderer can be initialized right away.
     */
    if (!cog_gl_renderer_initialize(&s_display->gl_render, error))
        return FALSE;

    if (!init_glib(COG_X11_PLATFORM(platform))) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to initialize GLib");
        return FALSE;
    }

    /* init WPE host data */
    wpe_fdo_initialize_for_egl_display (s_display->egl.display);

    cog_gamepad_setup(gamepad_provider_get_view_backend_for_gamepad);

    return TRUE;
}

static void
cog_x11_platform_finalize(GObject *object)
{
    clear_glib ();
    cog_gl_renderer_finalize(&s_display->gl_render);
    clear_egl ();
    clear_xkb ();
    clear_xcb ();

    g_clear_pointer (&s_window, free);
    g_clear_pointer (&s_display, free);

    G_OBJECT_CLASS(cog_x11_platform_parent_class)->finalize(object);
}

static WebKitWebViewBackend *
cog_x11_platform_get_view_backend(CogPlatform *platform, WebKitWebView *related_view, GError **error)
{
    static struct wpe_view_backend_exportable_fdo_egl_client exportable_egl_client = {
        .export_fdo_egl_image = on_export_fdo_egl_image,
    };

    s_window->wpe.exportable =
        wpe_view_backend_exportable_fdo_egl_create (&exportable_egl_client,
                                                    NULL,
                                                    DEFAULT_WIDTH,
                                                    DEFAULT_HEIGHT);
    g_assert (s_window->wpe.exportable);

    /* init WPE view backend */
    s_window->wpe.backend =
        wpe_view_backend_exportable_fdo_get_view_backend (s_window->wpe.exportable);
    g_assert (s_window->wpe.backend);

    WebKitWebViewBackend *wk_view_backend =
        webkit_web_view_backend_new (s_window->wpe.backend,
                                     (GDestroyNotify) wpe_view_backend_exportable_fdo_destroy,
                                     s_window->wpe.exportable);
    g_assert (wk_view_backend);

    return wk_view_backend;
}

#if COG_HAVE_LIBPORTAL
static void
on_run_file_chooser(WebKitWebView *view, WebKitFileChooserRequest *request)
{
    g_autoptr(XdpParent) xdp_parent = xdp_parent_new_x11(&s_window->xcb.window);

    run_file_chooser(view, request, xdp_parent);
}
#endif /* COG_HAVE_LIBPORTAL */

static void
set_cursor(enum cursor_type type)
{
    xcb_cursor_context_t *ctx;
    if (xcb_cursor_context_new(s_display->xcb.connection, s_display->xcb.screen, &ctx) < 0) {
        g_warning("Could not initialize xcb-cursor");
        return;
    }

    xcb_cursor_t cursor = XCB_CURSOR_NONE;
    for (int i = 0; cursor == XCB_CURSOR_NONE && i < G_N_ELEMENTS(cursor_names[type]) && cursor_names[type][i]; i++) {
        cursor = xcb_cursor_load_cursor(ctx, cursor_names[type][i]);
    }

    if (cursor == XCB_CURSOR_NONE) {
        g_warning("Could not load %s cursor", cursor_names[type][0]);
    }

    xcb_change_window_attributes(s_display->xcb.connection, s_window->xcb.window, XCB_CW_CURSOR, &cursor);
    xcb_free_cursor(s_display->xcb.connection, cursor);
    xcb_cursor_context_free(ctx);
}

static void
on_mouse_target_changed(WebKitWebView *view, WebKitHitTestResult *hitTestResult, guint mouseModifiers)
{
    if (webkit_hit_test_result_context_is_link(hitTestResult)) {
        set_cursor(CURSOR_HAND);
    } else if (webkit_hit_test_result_context_is_editable(hitTestResult)) {
        set_cursor(CURSOR_TEXT);
    } else if (webkit_hit_test_result_context_is_selection(hitTestResult)) {
        set_cursor(CURSOR_TEXT);
    } else {
        set_cursor(CURSOR_LEFT_PTR);
    }
}

static void
cog_x11_platform_init_web_view(CogPlatform *platform, WebKitWebView *web_view)
{
#if COG_HAVE_LIBPORTAL
    g_signal_connect(web_view, "run-file-chooser", G_CALLBACK(on_run_file_chooser), NULL);
#endif /* COG_HAVE_LIBPORTAL */
    g_signal_connect(web_view, "mouse-target-changed", G_CALLBACK(on_mouse_target_changed), NULL);
    COG_X11_PLATFORM(platform)->web_view = COG_VIEW(web_view);
}

static void *
check_supported(void *data G_GNUC_UNUSED)
{
    /*
     * TODO: This could do more than only trying to connect to the X server.
     */
    Display *d = XOpenDisplay(NULL);
    if (d) {
        XCloseDisplay(d);
        return GINT_TO_POINTER(TRUE);
    } else {
        return GINT_TO_POINTER(FALSE);
    }
}

static gboolean
cog_x11_platform_is_supported(void)
{
    static GOnce once = G_ONCE_INIT;
    g_once(&once, check_supported, NULL);
    return GPOINTER_TO_INT(once.retval);
}

static void
cog_x11_platform_class_init(CogX11PlatformClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = cog_x11_platform_finalize;

    CogPlatformClass *platform_class = COG_PLATFORM_CLASS(klass);
    platform_class->is_supported = cog_x11_platform_is_supported;
    platform_class->setup = cog_x11_platform_setup;
    platform_class->get_view_backend = cog_x11_platform_get_view_backend;
    platform_class->init_web_view = cog_x11_platform_init_web_view;
}

static void
cog_x11_platform_class_finalize(CogX11PlatformClass *klass)
{
}

static void
cog_x11_platform_init(CogX11Platform *self)
{
}

G_MODULE_EXPORT void
g_io_cogplatform_x11_load(GIOModule *module)
{
    GTypeModule *type_module = G_TYPE_MODULE(module);
    cog_x11_platform_register_type(type_module);
}

G_MODULE_EXPORT void
g_io_cogplatform_x11_unload(GIOModule *module G_GNUC_UNUSED)
{
}
