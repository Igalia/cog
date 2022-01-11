/*
 * cog-platform-x11.c
 * Copyright (C) 2020-2021 Igalia S.L.
 *
 * Distributed under terms of the MIT license.
 */

#include "../../core/cog.h"

#include <epoxy/egl.h>
#include <epoxy/gl.h>

#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <wpe/webkit.h>
#include <wpe/fdo.h>
#include <wpe/fdo-egl.h>

#include <xcb/xcb.h>
#include <xkbcommon/xkbcommon-x11.h>
#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>

#include "../common/egl-proc-address.h"


#ifndef EGL_EXT_platform_base
#define EGL_EXT_platform_base 1
typedef EGLDisplay (EGLAPIENTRYP PFNEGLGETPLATFORMDISPLAYEXTPROC) (EGLenum platform, void *native_display, const EGLint *attrib_list);
typedef EGLSurface (EGLAPIENTRYP PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC) (EGLDisplay dpy, EGLConfig config, void *native_window, const EGLint *attrib_list);
typedef EGLSurface (EGLAPIENTRYP PFNEGLCREATEPLATFORMPIXMAPSURFACEEXTPROC) (EGLDisplay dpy, EGLConfig config, void *native_pixmap, const EGLint *attrib_list);
#endif

#ifndef EGL_EXT_platform_x11
#define EGL_EXT_platform_x11 1
#define EGL_PLATFORM_X11_EXT              0x31D5
#define EGL_PLATFORM_X11_SCREEN_EXT       0x31D6
#endif /* EGL_EXT_platform_x11 */

#ifndef GL_OES_EGL_image
#define GL_OES_EGL_image 1
typedef void *GLeglImageOES;
typedef void (GL_APIENTRYP PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) (GLenum target, GLeglImageOES image);
typedef void (GL_APIENTRYP PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC) (GLenum target, GLeglImageOES image);
#endif

#define DEFAULT_WIDTH  1024
#define DEFAULT_HEIGHT  768

struct _CogX11PlatformClass {
    CogPlatformClass parent_class;
};

struct _CogX11Platform {
    CogPlatform parent;
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
        int32_t device_id;
        struct xkb_context *context;
        struct xkb_keymap *keymap;
        struct xkb_state *state;

        uint8_t modifiers;
    } xkb;

    struct {
        PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display;
        PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC create_platform_window_surface;
        PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture;

        EGLDisplay display;
        EGLConfig config;
        EGLContext context;
    } egl;
};

struct CogX11Window {
    struct {
        xcb_window_t window;

        bool needs_initial_paint;
        bool needs_frame_completion;
        unsigned width;
        unsigned height;
    } xcb;

    struct {
        EGLSurface surface;
    } egl;

    struct {
        GLint vertex_shader;
        GLint fragment_shader;
        GLint program;

        GLuint texture;

        GLint attr_pos;
        GLint attr_texture;
        GLint uniform_texture;
    } gl;

    struct {
        struct wpe_view_backend_exportable_fdo *exportable;
        struct wpe_view_backend *backend;

        struct wpe_fdo_egl_exported_image *image;
    } wpe;
};

static struct CogX11Display *s_display = NULL;
static struct CogX11Window *s_window = NULL;


static void
xcb_schedule_repaint (void)
{
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

static void
xcb_initial_paint (void)
{
    eglMakeCurrent (s_display->egl.display, s_window->egl.surface, s_window->egl.surface, s_display->egl.context);

    glViewport (0, 0, s_window->xcb.width, s_window->xcb.height);
    glClearColor (1, 1, 1, 1);
    glClear (GL_COLOR_BUFFER_BIT);

    eglSwapBuffers (s_display->egl.display, s_window->egl.surface);
}

static void
xcb_paint_image (struct wpe_fdo_egl_exported_image *image)
{
    static const float position_coords[4][2] = {
        { -1,  1 }, { 1,  1 },
        { -1, -1 }, { 1, -1 },
    };
    static const float texture_coords[4][2] = {
        { 0, 0 }, { 1, 0 },
        { 0, 1 }, { 1, 1 },
    };

    eglMakeCurrent (s_display->egl.display, s_window->egl.surface, s_window->egl.surface, s_display->egl.context);

    glViewport (0, 0, s_window->xcb.width, s_window->xcb.height);
    glClearColor (1, 1, 1, 1);
    glClear (GL_COLOR_BUFFER_BIT);

    glUseProgram (s_window->gl.program);

    glActiveTexture (GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, s_window->gl.texture);
    s_display->egl.image_target_texture (GL_TEXTURE_2D,
                                         wpe_fdo_egl_exported_image_get_egl_image (image));
    glUniform1i (s_window->gl.uniform_texture, 0);

    glVertexAttribPointer (s_window->gl.attr_pos, 2, GL_FLOAT, GL_FALSE, 0, position_coords);
    glVertexAttribPointer (s_window->gl.attr_texture, 2, GL_FLOAT, GL_FALSE, 0, texture_coords);

    glEnableVertexAttribArray (s_window->gl.attr_pos);
    glEnableVertexAttribArray (s_window->gl.attr_texture);

    glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);

    glDisableVertexAttribArray (s_window->gl.attr_pos);
    glDisableVertexAttribArray (s_window->gl.attr_texture);

    eglSwapBuffers (s_display->egl.display, s_window->egl.surface);

    s_window->wpe.image = image;
    s_window->xcb.needs_initial_paint = false;
    s_window->xcb.needs_frame_completion = true;
    xcb_schedule_repaint ();
}

static void
xcb_frame_completion (void)
{
    if (s_window->wpe.image) {
        wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image (s_window->wpe.exportable, s_window->wpe.image);
        s_window->wpe.image = NULL;
    }

    wpe_view_backend_exportable_fdo_dispatch_frame_complete (s_window->wpe.exportable);
}

static void
xcb_handle_key_press (xcb_key_press_event_t *event)
{
    uint32_t keysym = xkb_state_key_get_one_sym (s_display->xkb.state, event->detail);
    uint32_t unicode = xkb_state_key_get_utf32 (s_display->xkb.state, event->detail);

    struct wpe_input_keyboard_event input_event = {
        .time = event->time,
        .key_code = keysym,
        .hardware_key_code = unicode,
        .pressed = true,
        .modifiers = s_display->xkb.modifiers,
    };
    wpe_view_backend_dispatch_keyboard_event (s_window->wpe.backend, &input_event);
}

static void
xcb_handle_key_release (xcb_key_press_event_t *event)
{
    uint32_t keysym = xkb_state_key_get_one_sym (s_display->xkb.state, event->detail);
    uint32_t unicode = xkb_state_key_get_utf32 (s_display->xkb.state, event->detail);

    struct wpe_input_keyboard_event input_event = {
        .time = event->time,
        .key_code = keysym,
        .hardware_key_code = unicode,
        .pressed = false,
        .modifiers = s_display->xkb.modifiers,
    };
    wpe_view_backend_dispatch_keyboard_event (s_window->wpe.backend, &input_event);
}

static void
xcb_handle_axis (xcb_button_press_event_t *event, const int16_t axis_delta[2])
{
    struct wpe_input_axis_2d_event input_event = {
        .base = {
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
xcb_handle_button_press (xcb_button_press_event_t *event)
{
    static const int16_t axis_delta[4][2] = {
        {   0, -20 },
        {   0,  20 },
        { -20,   0 },
        {  20,   0 },
    };

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
on_export_fdo_egl_image(void *data, struct wpe_fdo_egl_exported_image *image)
{
    xcb_paint_image (image);
}

static void
xcb_process_events (void)
{
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
            xcb_schedule_repaint ();
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
                if (s_window->xcb.needs_initial_paint) {
                    xcb_initial_paint ();
                    s_window->xcb.needs_initial_paint = false;
                }

                if (s_window->xcb.needs_frame_completion) {
                    xcb_frame_completion ();
                    s_window->xcb.needs_frame_completion = false;
                }
                break;
            }
            break;
        }
        case XCB_KEY_PRESS:
            xcb_handle_key_press ((xcb_key_press_event_t *) event);
            break;
        case XCB_KEY_RELEASE:
            xcb_handle_key_release ((xcb_key_release_event_t *) event);
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
        default:
            break;
        }
    }
};

struct xcb_source {
    GSource source;
    GPollFD pfd;
    xcb_connection_t *connection;
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

    xcb_process_events ();
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
        XCB_EVENT_MASK_EXPOSURE |
        XCB_EVENT_MASK_STRUCTURE_NOTIFY |
        XCB_EVENT_MASK_KEY_PRESS |
        XCB_EVENT_MASK_KEY_RELEASE |
        XCB_EVENT_MASK_BUTTON_PRESS |
        XCB_EVENT_MASK_BUTTON_RELEASE |
        XCB_EVENT_MASK_POINTER_MOTION
    };

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

    s_window->xcb.needs_initial_paint = true;
    xcb_schedule_repaint ();

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

    s_display->xkb.state = xkb_x11_state_new_from_device (s_display->xkb.keymap, s_display->xcb.connection, s_display->xkb.device_id);
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
    s_display->egl.get_platform_display = load_egl_proc_address ("eglGetPlatformDisplayEXT");
    if (s_display->egl.get_platform_display) {
        s_display->egl.display = s_display->egl.get_platform_display (EGL_PLATFORM_X11_KHR, s_display->display, NULL);
    }

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
    s_display->egl.create_platform_window_surface = load_egl_proc_address ("eglCreatePlatformWindowSurfaceEXT");
    if (s_display->egl.create_platform_window_surface)
        s_window->egl.surface = s_display->egl.create_platform_window_surface (s_display->egl.display,
                                                                               s_display->egl.config,
                                                                               &win, NULL);
    if (s_window->egl.surface == EGL_NO_SURFACE)
        return FALSE;

    s_display->egl.image_target_texture = load_egl_proc_address ("glEGLImageTargetTexture2DOES");

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
init_gl (void)
{
    static const char *vertex_shader_source =
        "attribute vec2 pos;\n"
        "attribute vec2 texture;\n"
        "varying vec2 v_texture;\n"
        "void main() {\n"
        "  v_texture = texture;\n"
        "  gl_Position = vec4(pos, 0, 1);\n"
        "}\n";
    static const char *fragment_shader_source =
        "precision mediump float;\n"
        "uniform sampler2D u_texture;\n"
        "varying vec2 v_texture;\n"
        "void main() {\n"
        "  gl_FragColor = texture2D(u_texture, v_texture);\n"
        "}\n";

    s_window->gl.vertex_shader = glCreateShader (GL_VERTEX_SHADER);
    glShaderSource (s_window->gl.vertex_shader, 1, &vertex_shader_source, NULL);
    glCompileShader (s_window->gl.vertex_shader);

    GLint vertex_shader_compile_status = 0;
    glGetShaderiv (s_window->gl.vertex_shader, GL_COMPILE_STATUS, &vertex_shader_compile_status);
    if (!vertex_shader_compile_status) {
        GLsizei vertex_shader_info_log_length = 0;
        char vertex_shader_info_log[1024];
        glGetShaderInfoLog (s_window->gl.vertex_shader, 1023,
                            &vertex_shader_info_log_length, vertex_shader_info_log);
        vertex_shader_info_log[vertex_shader_info_log_length] = 0;
        g_warning ("Unable to compile vertex shader:\n%s", vertex_shader_info_log);
    }

    s_window->gl.fragment_shader = glCreateShader (GL_FRAGMENT_SHADER);
    glShaderSource (s_window->gl.fragment_shader, 1, &fragment_shader_source, NULL);
    glCompileShader (s_window->gl.fragment_shader);

    GLint fragment_shader_compile_status = 0;
    glGetShaderiv (s_window->gl.fragment_shader, GL_COMPILE_STATUS, &fragment_shader_compile_status);
    if (!fragment_shader_compile_status) {
        GLsizei fragment_shader_info_log_length = 0;
        char fragment_shader_info_log[1024];
        glGetShaderInfoLog (s_window->gl.fragment_shader, 1023,
                            &fragment_shader_info_log_length, fragment_shader_info_log);
        fragment_shader_info_log[fragment_shader_info_log_length] = 0;
        g_warning ("Unable to compile fragment shader:\n%s", fragment_shader_info_log);
    }

    s_window->gl.program = glCreateProgram ();
    glAttachShader (s_window->gl.program, s_window->gl.vertex_shader);
    glAttachShader (s_window->gl.program, s_window->gl.fragment_shader);
    glLinkProgram (s_window->gl.program);

    GLint link_status = 0;
    glGetProgramiv (s_window->gl.program, GL_LINK_STATUS, &link_status);
    if (!link_status) {
        GLsizei program_info_log_length = 0;
        char program_info_log[1024];
        glGetProgramInfoLog (s_window->gl.program, 1023,
                             &program_info_log_length, program_info_log);
        program_info_log[program_info_log_length] = 0;
        g_warning ("Unable to link program:\n%s", program_info_log);
        return FALSE;
    }

    s_window->gl.attr_pos = glGetAttribLocation (s_window->gl.program, "pos");
    s_window->gl.attr_texture = glGetAttribLocation (s_window->gl.program, "texture");
    s_window->gl.uniform_texture = glGetUniformLocation (s_window->gl.program, "u_texture");

    glGenTextures (1, &s_window->gl.texture);
    glBindTexture (GL_TEXTURE_2D, s_window->gl.texture);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, s_window->xcb.width, s_window->xcb.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture (GL_TEXTURE_2D, 0);

    return TRUE;
}

static void
clear_gl ()
{
    if (s_window->gl.texture)
        glDeleteTextures (1, &s_window->gl.texture);

    if (s_window->gl.vertex_shader)
        glDeleteShader (s_window->gl.vertex_shader);
    if (s_window->gl.fragment_shader)
        glDeleteShader (s_window->gl.fragment_shader);
    if (s_window->gl.program)
        glDeleteProgram (s_window->gl.program);
}

static gboolean
init_glib (void)
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

    if (!init_gl ()) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to initialize GL");
        return FALSE;
    }

    if (!init_glib ()) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to initialize GLib");
        return FALSE;
    }

    /* init WPE host data */
    wpe_fdo_initialize_for_egl_display (s_display->egl.display);

    return TRUE;
}

static void
cog_x11_platform_finalize(GObject *object)
{
    clear_glib ();
    clear_gl ();
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
