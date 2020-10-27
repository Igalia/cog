
#include <cog.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

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

#if defined(WPE_CHECK_VERSION)
# define HAVE_2D_AXIS_EVENT WPE_CHECK_VERSION(1, 5, 0) && WEBKIT_CHECK_VERSION(2, 27, 4)
#else
# define HAVE_2D_AXIS_EVENT 0
#endif /* WPE_CHECK_VERSION */

static struct {
    Display *display;
    xcb_connection_t *connection;
    xcb_screen_t *screen;
    xcb_window_t window;

    xcb_atom_t atom_wm_protocols;
    xcb_atom_t atom_wm_delete_window;
    xcb_atom_t atom_net_wm_name;
    xcb_atom_t atom_utf8_string;

    bool needs_initial_paint;
    bool needs_frame_completion;
    unsigned width;
    unsigned height;

    struct {
        int32_t x;
        int32_t y;
        uint32_t button;
        uint32_t state;
    } pointer;
} xcb_data = {
    .needs_initial_paint = false,
    .needs_frame_completion = false,
};

static struct {
    int32_t device_id;
    struct xkb_context *context;
    struct xkb_keymap *keymap;
    struct xkb_state *state;

    uint8_t modifiers;
} xkb_data = {
    .device_id = -1,
    .modifiers = 0,
};

static struct {
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display;
    PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC create_platform_window_surface;

    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture;

    EGLDisplay display;
    EGLConfig config;
    EGLContext context;
    EGLSurface surface;
} egl_data = {
    .display = EGL_NO_DISPLAY,
};

static struct {
    GLint vertex_shader;
    GLint fragment_shader;
    GLint program;

    GLuint texture;

    GLint attr_pos;
    GLint attr_texture;
    GLint uniform_texture;
} gl_data = {
};

static struct {
    GSource *xcb_source;
} glib_data;

static struct {
    struct wpe_view_backend *backend;
    struct wpe_fdo_egl_exported_image *image;
} wpe_view_data = {NULL, };

static struct {
    struct wpe_view_backend_exportable_fdo *exportable;
} wpe_host_data;

static void
xcb_schedule_repaint (void)
{
    xcb_client_message_event_t client_message = {
        .response_type = XCB_CLIENT_MESSAGE,
        .format = 32,
        .window = xcb_data.window,
        .type = XCB_ATOM_NOTICE,
    };

    xcb_send_event (xcb_data.connection, 0, xcb_data.window,
                    0, (char *) &client_message);
    xcb_flush (xcb_data.connection);
}

static void
xcb_initial_paint (void)
{
    eglMakeCurrent (egl_data.display, egl_data.surface, egl_data.surface, egl_data.context);

    glViewport (0, 0, xcb_data.width, xcb_data.height);
    glClearColor (1, 1, 1, 1);
    glClear (GL_COLOR_BUFFER_BIT);

    eglSwapBuffers (egl_data.display, egl_data.surface);
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

    eglMakeCurrent (egl_data.display, egl_data.surface, egl_data.surface, egl_data.context);

    glViewport (0, 0, xcb_data.width, xcb_data.height);
    glClearColor (1, 1, 1, 1);
    glClear (GL_COLOR_BUFFER_BIT);

    glUseProgram (gl_data.program);

    glActiveTexture (GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, gl_data.texture);
    egl_data.image_target_texture (GL_TEXTURE_2D,
                                   wpe_fdo_egl_exported_image_get_egl_image (image));
    glUniform1i (gl_data.uniform_texture, 0);

    glVertexAttribPointer (gl_data.attr_pos, 2, GL_FLOAT, GL_FALSE, 0, position_coords);
    glVertexAttribPointer (gl_data.attr_texture, 2, GL_FLOAT, GL_FALSE, 0, texture_coords);

    glEnableVertexAttribArray (gl_data.attr_pos);
    glEnableVertexAttribArray (gl_data.attr_texture);

    glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);

    glDisableVertexAttribArray (gl_data.attr_pos);
    glDisableVertexAttribArray (gl_data.attr_texture);

    eglSwapBuffers (egl_data.display, egl_data.surface);

    wpe_view_data.image = image;
    xcb_data.needs_initial_paint = false;
    xcb_data.needs_frame_completion = true;
    xcb_schedule_repaint ();
}

static void
xcb_frame_completion (void)
{
    if (wpe_view_data.image) {
        wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image (wpe_host_data.exportable, wpe_view_data.image);
        wpe_view_data.image = NULL;
    }

    wpe_view_backend_exportable_fdo_dispatch_frame_complete (wpe_host_data.exportable);
}

static void
xcb_handle_key_press (xcb_key_press_event_t *event)
{
    uint32_t keysym = xkb_state_key_get_one_sym (xkb_data.state, event->detail);
    uint32_t unicode = xkb_state_key_get_utf32 (xkb_data.state, event->detail);

    struct wpe_input_keyboard_event input_event = {
        .time = event->time,
        .key_code = keysym,
        .hardware_key_code = unicode,
        .pressed = true,
        .modifiers = xkb_data.modifiers,
    };
    wpe_view_backend_dispatch_keyboard_event (wpe_view_data.backend, &input_event);
}

static void
xcb_handle_key_release (xcb_key_press_event_t *event)
{
    uint32_t keysym = xkb_state_key_get_one_sym (xkb_data.state, event->detail);
    uint32_t unicode = xkb_state_key_get_utf32 (xkb_data.state, event->detail);

    struct wpe_input_keyboard_event input_event = {
        .time = event->time,
        .key_code = keysym,
        .hardware_key_code = unicode,
        .pressed = false,
        .modifiers = xkb_data.modifiers,
    };
    wpe_view_backend_dispatch_keyboard_event (wpe_view_data.backend, &input_event);
}

static void
xcb_handle_axis (xcb_button_press_event_t *event, const int16_t axis_delta[2])
{
#if HAVE_2D_AXIS_EVENT
    struct wpe_input_axis_2d_event input_event = {
        .base = {
            .type = wpe_input_axis_event_type_mask_2d | wpe_input_axis_event_type_motion_smooth,
            .time = event->time,
            .x = xcb_data.pointer.x,
            .y = xcb_data.pointer.y,
        },
        .x_axis = axis_delta[0],
        .y_axis = axis_delta[1],
    };

    wpe_view_backend_dispatch_axis_event (wpe_view_data.backend, &input_event.base);
#else
    assert (axis_delta[0] ^ axis_delta[1]);

    struct wpe_input_axis_event input_event = {
        .type = wpe_input_axis_event_type_motion,
        .time = event->time,
        .x = xcb_data.pointer.x,
        .y = xcb_data.pointer.y,
    };

    if (!!axis_delta[0]) {
        input_event.axis = 1;
        input_event.value = axis_delta[0];
    } else {
        input_event.axis = 0;
        input_event.value = axis_delta[1];
    }

    wpe_view_backend_dispatch_axis_event (wpe_view_data.backend, &input_event);
#endif
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
        xcb_data.pointer.button = event->detail;
        xcb_data.pointer.state = 1;
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
        .x = xcb_data.pointer.x,
        .y = xcb_data.pointer.y,
        .button = xcb_data.pointer.button,
        .state = xcb_data.pointer.state,
    };

    wpe_view_backend_dispatch_pointer_event (wpe_view_data.backend, &input_event);
}

static void
xcb_handle_button_release (xcb_button_release_event_t *event)
{
    switch (event->detail) {
    case 1:
    case 2:
    case 3:
        xcb_data.pointer.button = event->detail;
        xcb_data.pointer.state = 0;
        break;
    default:
        return;
    }

    struct wpe_input_pointer_event input_event = {
        .type = wpe_input_pointer_event_type_button,
        .time = event->time,
        .x = xcb_data.pointer.x,
        .y = xcb_data.pointer.y,
        .button = xcb_data.pointer.button,
        .state = xcb_data.pointer.state,
    };

    wpe_view_backend_dispatch_pointer_event (wpe_view_data.backend, &input_event);
}

static void
xcb_handle_motion_event (xcb_motion_notify_event_t *event)
{
    xcb_data.pointer.x = event->event_x;
    xcb_data.pointer.y = event->event_y;

    struct wpe_input_pointer_event input_event = {
        .type = wpe_input_pointer_event_type_motion,
        .time = event->time,
        .x = xcb_data.pointer.x,
        .y = xcb_data.pointer.y,
        .button = xcb_data.pointer.button,
        .state = xcb_data.pointer.state,
    };

    wpe_view_backend_dispatch_pointer_event (wpe_view_data.backend, &input_event);
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

    while ((event = xcb_poll_for_event (xcb_data.connection))) {
        switch (event->response_type & 0x7f) {
        case XCB_CONFIGURE_NOTIFY:
        {
            xcb_configure_notify_event_t *configure_notify = (xcb_configure_notify_event_t *) event;
            if (configure_notify->width == xcb_data.width
                && configure_notify->height == xcb_data.height)
                break;

            xcb_data.width = configure_notify->width;
            xcb_data.height = configure_notify->height;

            wpe_view_backend_dispatch_set_size (wpe_view_data.backend,
                                                xcb_data.width,
                                                xcb_data.height);
            xcb_schedule_repaint ();
            break;
        }
        case XCB_CLIENT_MESSAGE:
        {
            xcb_client_message_event_t *client_message = (xcb_client_message_event_t *) event;
            if (client_message->window != xcb_data.window)
                break;

            if (client_message->type == xcb_data.atom_wm_protocols
                && client_message->data.data32[0] == xcb_data.atom_wm_delete_window) {
                g_application_quit (g_application_get_default ());
                break;
            }

            if (client_message->type == XCB_ATOM_NOTICE) {
                if (xcb_data.needs_initial_paint) {
                    xcb_initial_paint ();
                    xcb_data.needs_initial_paint = false;
                }

                if (xcb_data.needs_frame_completion) {
                    xcb_frame_completion ();
                    xcb_data.needs_frame_completion = false;
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
    xcb_data.width = DEFAULT_WIDTH;
    xcb_data.height = DEFAULT_HEIGHT;

    xcb_data.display = XOpenDisplay (NULL);
    xcb_data.connection = XGetXCBConnection (xcb_data.display);
    if (xcb_connection_has_error (xcb_data.connection))
        return FALSE;

    xcb_data.window = xcb_generate_id (xcb_data.connection);

    const struct xcb_setup_t *setup = xcb_get_setup (xcb_data.connection);
    xcb_data.screen = xcb_setup_roots_iterator (setup).data;

    static const uint32_t window_values[] = {
        XCB_EVENT_MASK_EXPOSURE |
        XCB_EVENT_MASK_STRUCTURE_NOTIFY |
        XCB_EVENT_MASK_KEY_PRESS |
        XCB_EVENT_MASK_KEY_RELEASE |
        XCB_EVENT_MASK_BUTTON_PRESS |
        XCB_EVENT_MASK_BUTTON_RELEASE |
        XCB_EVENT_MASK_POINTER_MOTION
    };

    xcb_create_window (xcb_data.connection,
                       XCB_COPY_FROM_PARENT,
                       xcb_data.window,
                       xcb_data.screen->root,
                       0, 0, xcb_data.width, xcb_data.height,
                       0,
                       XCB_WINDOW_CLASS_INPUT_OUTPUT,
                       xcb_data.screen->root_visual,
                       XCB_CW_EVENT_MASK, window_values);

    xcb_data.atom_wm_protocols = get_atom (xcb_data.connection, "WM_PROTOCOLS");
    xcb_data.atom_wm_delete_window = get_atom (xcb_data.connection, "WM_DELETE_WINDOW");
    xcb_data.atom_net_wm_name = get_atom (xcb_data.connection, "_NET_WM_NAME");
    xcb_data.atom_utf8_string = get_atom (xcb_data.connection, "UTF8_STRING");
    xcb_change_property (xcb_data.connection,
                         XCB_PROP_MODE_REPLACE,
                         xcb_data.window,
                         xcb_data.atom_wm_protocols,
                         XCB_ATOM_ATOM,
                         32,
                         1, &xcb_data.atom_wm_delete_window);

    xcb_change_property (xcb_data.connection,
                         XCB_PROP_MODE_REPLACE,
                         xcb_data.window,
                         xcb_data.atom_net_wm_name,
                         xcb_data.atom_utf8_string,
                         8,
                         strlen("Cog"), "Cog");

    xcb_map_window (xcb_data.connection, xcb_data.window);
    xcb_flush (xcb_data.connection);

    xcb_data.needs_initial_paint = true;
    xcb_schedule_repaint ();

    return TRUE;
}

static void
clear_xcb (void)
{
    if (xcb_data.display)
        XCloseDisplay (xcb_data.display);
}

static gboolean
init_xkb (void)
{
    xkb_data.device_id = xkb_x11_get_core_keyboard_device_id (xcb_data.connection);
    if (xkb_data.device_id == -1)
        return FALSE;

    xkb_data.context = xkb_context_new (0);
    if (!xkb_data.context)
        return FALSE;

    xkb_data.keymap = xkb_x11_keymap_new_from_device (xkb_data.context, xcb_data.connection, xkb_data.device_id, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!xkb_data.keymap)
        return FALSE;

    xkb_data.state = xkb_x11_state_new_from_device (xkb_data.keymap, xcb_data.connection, xkb_data.device_id);
    if (!xkb_data.state)
        return FALSE;

    return TRUE;
}

static void
clear_xkb (void)
{
    if (xkb_data.state)
        xkb_state_unref (xkb_data.state);
    if (xkb_data.keymap)
        xkb_keymap_unref (xkb_data.keymap);
    if (xkb_data.context)
        xkb_context_unref (xkb_data.context);
}

static gboolean
init_egl (void)
{
    egl_data.get_platform_display = (void *) eglGetProcAddress ("eglGetPlatformDisplayEXT");
    if (egl_data.get_platform_display) {
        egl_data.display = egl_data.get_platform_display (EGL_PLATFORM_X11_KHR, xcb_data.display, NULL);
    }

    if (!eglInitialize (egl_data.display, NULL, NULL))
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

        if (!eglGetConfigs (egl_data.display, NULL, 0, &count) || count < 1)
            return FALSE;

        configs = g_new0 (EGLConfig, count);
        if (!eglChooseConfig (egl_data.display, config_attribs, configs, count, &matched) || !matched) {
            g_free (configs);
            return FALSE;
        }

        egl_data.config = configs[0];
        g_free (configs);
        if (!egl_data.config)
            return FALSE;
    }

    egl_data.context = eglCreateContext (egl_data.display,
                                         egl_data.config,
                                         EGL_NO_CONTEXT,
                                         context_attribs);
    if (egl_data.context == EGL_NO_CONTEXT)
        return FALSE;

    Window win = (Window) xcb_data.window;
    egl_data.create_platform_window_surface = (void *) eglGetProcAddress ("eglCreatePlatformWindowSurfaceEXT");
    if (egl_data.create_platform_window_surface)
        egl_data.surface = egl_data.create_platform_window_surface (egl_data.display,
                                                                    egl_data.config,
                                                                    &win, NULL);
    if (egl_data.surface == EGL_NO_SURFACE)
        return FALSE;

    egl_data.image_target_texture = (void *) eglGetProcAddress ("glEGLImageTargetTexture2DOES");

    eglMakeCurrent (egl_data.display, egl_data.surface, egl_data.surface, egl_data.context);
    return TRUE;
}

static void
clear_egl (void)
{
    if (egl_data.display != EGL_NO_DISPLAY) {
        eglTerminate (egl_data.display);
        egl_data.display = EGL_NO_DISPLAY;
    }

    eglReleaseThread ();
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

    gl_data.vertex_shader = glCreateShader (GL_VERTEX_SHADER);
    glShaderSource (gl_data.vertex_shader, 1, &vertex_shader_source, NULL);
    glCompileShader (gl_data.vertex_shader);

    GLint vertex_shader_compile_status = 0;
    glGetShaderiv (gl_data.vertex_shader, GL_COMPILE_STATUS, &vertex_shader_compile_status);
    if (!vertex_shader_compile_status) {
        GLsizei vertex_shader_info_log_length = 0;
        char vertex_shader_info_log[1024];
        glGetShaderInfoLog (gl_data.vertex_shader, 1023,
            &vertex_shader_info_log_length, vertex_shader_info_log);
        vertex_shader_info_log[vertex_shader_info_log_length] = 0;
        g_warning ("Unable to compile vertex shader:\n%s", vertex_shader_info_log);
    }

    gl_data.fragment_shader = glCreateShader (GL_FRAGMENT_SHADER);
    glShaderSource (gl_data.fragment_shader, 1, &fragment_shader_source, NULL);
    glCompileShader (gl_data.fragment_shader);

    GLint fragment_shader_compile_status = 0;
    glGetShaderiv (gl_data.fragment_shader, GL_COMPILE_STATUS, &fragment_shader_compile_status);
    if (!fragment_shader_compile_status) {
        GLsizei fragment_shader_info_log_length = 0;
        char fragment_shader_info_log[1024];
        glGetShaderInfoLog (gl_data.fragment_shader, 1023,
            &fragment_shader_info_log_length, fragment_shader_info_log);
        fragment_shader_info_log[fragment_shader_info_log_length] = 0;
        g_warning ("Unable to compile fragment shader:\n%s", fragment_shader_info_log);
    }

    gl_data.program = glCreateProgram ();
    glAttachShader (gl_data.program, gl_data.vertex_shader);
    glAttachShader (gl_data.program, gl_data.fragment_shader);
    glLinkProgram (gl_data.program);

    GLint link_status = 0;
    glGetProgramiv (gl_data.program, GL_LINK_STATUS, &link_status);
    if (!link_status) {
        GLsizei program_info_log_length = 0;
        char program_info_log[1024];
        glGetProgramInfoLog (gl_data.program, 1023,
            &program_info_log_length, program_info_log);
        program_info_log[program_info_log_length] = 0;
        g_warning ("Unable to link program:\n%s", program_info_log);
        return FALSE;
    }

    gl_data.attr_pos = glGetAttribLocation (gl_data.program, "pos");
    gl_data.attr_texture = glGetAttribLocation (gl_data.program, "texture");
    gl_data.uniform_texture = glGetUniformLocation (gl_data.program, "u_texture");

    glGenTextures (1, &gl_data.texture);
    glBindTexture (GL_TEXTURE_2D, gl_data.texture);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, xcb_data.width, xcb_data.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture (GL_TEXTURE_2D, 0);

    return TRUE;
}

static void
clear_gl ()
{
    if (gl_data.texture)
        glDeleteTextures (1, &gl_data.texture);

    if (gl_data.vertex_shader)
        glDeleteShader (gl_data.vertex_shader);
    if (gl_data.fragment_shader)
        glDeleteShader (gl_data.fragment_shader);
    if (gl_data.program)
        glDeleteProgram (gl_data.program);
}

static gboolean
init_glib (void)
{
    static GSourceFuncs xcb_source_funcs = {
        .check = xcb_source_check,
        .dispatch = xcb_source_dispatch,
    };

    glib_data.xcb_source = g_source_new (&xcb_source_funcs,
                                         sizeof (struct xcb_source));
    {
        struct xcb_source *source = (struct xcb_source *) glib_data.xcb_source;
        source->connection = xcb_data.connection;

        source->pfd.fd = xcb_get_file_descriptor (xcb_data.connection);
        source->pfd.events = G_IO_IN | G_IO_ERR | G_IO_HUP;
        source->pfd.revents = 0;
        g_source_add_poll (glib_data.xcb_source, &source->pfd);

        g_source_set_name (glib_data.xcb_source, "cog-x11: xcb");
        g_source_set_can_recurse (glib_data.xcb_source, TRUE);
        g_source_attach (glib_data.xcb_source, g_main_context_get_thread_default ());
    }

    return TRUE;
}

static void
clear_glib (void)
{
    if (glib_data.xcb_source)
        g_source_destroy (glib_data.xcb_source);
    g_clear_pointer (&glib_data.xcb_source, g_source_unref);
}

gboolean
cog_platform_plugin_setup (CogPlatform *platform,
                           CogShell    *shell G_GNUC_UNUSED,
                           const char  *params,
                           GError     **error)
{
    g_assert (platform);
    g_return_val_if_fail (COG_IS_SHELL (shell), FALSE);

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
    wpe_fdo_initialize_for_egl_display (egl_data.display);

    return TRUE;
}

void
cog_platform_plugin_teardown (CogPlatform *platform)
{
    g_assert (platform);

    clear_glib ();
    clear_gl ();
    clear_egl ();
    clear_xkb ();
    clear_xcb ();
}

WebKitWebViewBackend*
cog_platform_plugin_get_view_backend (CogPlatform   *platform,
                                      WebKitWebView *related_view,
                                      GError       **error)
{
    static struct wpe_view_backend_exportable_fdo_egl_client exportable_egl_client = {
        .export_fdo_egl_image = on_export_fdo_egl_image,
    };

    wpe_host_data.exportable =
        wpe_view_backend_exportable_fdo_egl_create (&exportable_egl_client,
                                                    NULL,
                                                    DEFAULT_WIDTH,
                                                    DEFAULT_HEIGHT);
    g_assert (wpe_host_data.exportable);

    /* init WPE view backend */
    wpe_view_data.backend =
        wpe_view_backend_exportable_fdo_get_view_backend (wpe_host_data.exportable);
    g_assert (wpe_view_data.backend);

    WebKitWebViewBackend *wk_view_backend =
        webkit_web_view_backend_new (wpe_view_data.backend,
                       (GDestroyNotify) wpe_view_backend_exportable_fdo_destroy,
                                     wpe_host_data.exportable);
    g_assert (wk_view_backend);

    return wk_view_backend;
}
