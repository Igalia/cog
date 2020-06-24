/*
 * cog-fdo-shell.c
 *
 * Copyright (C) 2020 Igalia S.L.
 * Copyright (C) 2018-2019 Adrian Perez de Castro <aperez@igalia.com>
 * Copyright (C) 2018 Eduardo Lima <elima@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "../platform/pwl.h"
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <cog.h>
#include <wpe/fdo.h>
#include <wpe/fdo-egl.h>


#if defined(WPE_CHECK_VERSION)
# define HAVE_2D_AXIS_EVENT (WPE_CHECK_VERSION (1, 5, 0) && (WEBKIT_CHECK_VERSION (2, 27, 4)))
#else /* !WPE_CHECK_VERSION */
# define HAVE_2D_AXIS_EVENT 0
#endif /* WPE_CHECK_VERSION */


#if 0
# define TRACE(fmt, ...) g_debug ("%s: " fmt, G_STRFUNC, ##__VA_ARGS__)
#else
# define TRACE(fmt, ...) ((void) 0)
#endif


static PwlDisplay *s_display = NULL;
static bool s_support_checked = false;
G_LOCK_DEFINE_STATIC (s_globals);


G_DECLARE_FINAL_TYPE (CogFdoShell, cog_fdo_shell, COG, FDO_SHELL, CogShell)
G_DECLARE_FINAL_TYPE (CogFdoView, cog_fdo_view, COG, FDO_VIEW, CogView)


enum RenderMode {
    RENDER_MODE_AUTO = 0,
    RENDER_MODE_ATTACH_BUFFER,
    RENDER_MODE_GLES_V2_PAINT,
};


struct _CogFdoShellClass {
    CogShellClass parent_class;
};

struct _CogFdoShell {
    CogShell    parent;

    enum RenderMode render_mode;
    gboolean        single_window;
    PwlWindow      *window; /* Non-NULL in single window mode. */
    uint32_t        window_id;

    struct wpe_view_backend_exportable_fdo_egl_client exportable_client;

    /* Used with RENDER_MODE_GLES_V2_PAINT */
    GLint egl_swap_interval;
    GLuint gl_program;
    GLuint gl_texture; /* Used in single window mode. */
    GLuint gl_texture_uniform;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC gl_eglImageTargetTexture2DOES;
};

enum {
    SHELL_PROP_0,
    SHELL_PROP_SINGLE_WINDOW,
    SHELL_PROP_WINDOW_ID,
    SHELL_PROP_RENDER_MODE,
    SHELL_PROP_SWAP_INTERVAL,
    SHELL_N_PROPERTIES,
};

static GParamSpec *s_shell_properties[SHELL_N_PROPERTIES] = { NULL, };


struct _CogFdoViewClass {
    CogViewClass parent_class;
};

struct _CogFdoView {
    CogView parent;

    struct wpe_view_backend_exportable_fdo *exportable;
    struct wpe_input_touch_event_raw touchpoints[PWL_N_TOUCH_POINTS];

    struct wpe_fdo_egl_exported_image *last_image;
    struct wl_buffer                  *last_buffer;

    PwlWindow *window; /* Non-NULL in multiple window mode. */
    uint32_t   window_id;

    /* Used with RENDER_MODE_GLES_V2_PAINT */
    GLuint gl_texture; /* Used in multiple window mode. */
};

enum {
    VIEW_PROP_0,
    VIEW_PROP_WINDOW_ID,
    VIEW_N_PROPERTIES,
};

static GParamSpec *s_view_properties[VIEW_N_PROPERTIES] = { NULL, };


static void
cog_fdo_gles_create_texture (GLuint  *texture,
                             uint32_t width,
                             uint32_t height)
{
    g_assert (texture);
    g_assert (*texture == 0);

    glGenTextures (1, texture);
    TRACE ("creating texture #%d, size %" PRIu32 "x%" PRIu32, *texture, width, height);

    glBindTexture (GL_TEXTURE_2D, *texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);
}

static inline void
cog_fdo_gles_destroy_texture (GLuint *texture)
{
    g_assert (texture);
    g_assert (*texture != 0);

    TRACE ("destroying texture #%d", *texture);

    glDeleteTextures (1, texture);
    *texture = 0;
}

static void
cog_fdo_shell_on_keyboard (PwlWindow         *window G_GNUC_UNUSED,
                           const PwlKeyboard *keyboard,
                           void              *shell)
{
    struct wpe_input_keyboard_event event = {
        .time = keyboard->timestamp,
        .key_code = keyboard->keysym,
        .hardware_key_code = keyboard->unicode,
        .pressed = !!keyboard->state,
        .modifiers = keyboard->modifiers,
    };
    CogView *view = cog_shell_get_focused_view (shell);
    wpe_view_backend_dispatch_keyboard_event (cog_view_get_backend (view),
                                              &event);
}


static inline void
cog_fdo_shell_dispatch_pointer_event (CogFdoShell                      *shell,
                                      const PwlPointer                 *pointer,
                                      enum wpe_input_pointer_event_type event_type)
{
    const uint32_t device_scale = pwl_window_get_device_scale (shell->window);
    struct wpe_input_pointer_event event = {
        .type = event_type,
        .time = pointer->timestamp,
        .x = pointer->x * device_scale,
        .y = pointer->y * device_scale,
        .button = pointer->button,
        .state = pointer->state,
        /* TODO: .modifiers */
    };
    CogView *view = cog_shell_get_focused_view (&shell->parent);
    wpe_view_backend_dispatch_pointer_event (cog_view_get_backend (view),
                                             &event);
}


static void
cog_fdo_shell_on_pointer_motion (PwlWindow        *window G_GNUC_UNUSED,
                                 const PwlPointer *pointer,
                                 void             *shell)
{
    g_assert (((CogFdoShell*) shell)->window == window);

    cog_fdo_shell_dispatch_pointer_event (shell,
                                          pointer,
                                          wpe_input_pointer_event_type_motion);
}


static void
cog_fdo_shell_on_pointer_button (PwlWindow        *window G_GNUC_UNUSED,
                                 const PwlPointer *pointer,
                                 void             *shell)
{
    g_assert (((CogFdoShell*) shell)->window == window);

    cog_fdo_shell_dispatch_pointer_event (shell,
                                          pointer,
                                          wpe_input_pointer_event_type_button);
}


static void
cog_fdo_shell_on_pointer_axis (PwlWindow        *window,
                               const PwlPointer *pointer,
                               void             *shell)
{
    CogFdoShell *self = shell;
    g_assert (self->window == window);

    const uint32_t device_scale = pwl_window_get_device_scale (window);

    CogView *view = cog_shell_get_focused_view (shell);
    struct wpe_view_backend *backend = cog_view_get_backend (view);

#if HAVE_2D_AXIS_EVENT
    struct wpe_input_axis_2d_event event = {
        .base = {
            .type = wpe_input_axis_event_type_mask_2d | wpe_input_axis_event_type_motion_smooth,
            .time = pointer->axis_timestamp,
            .x = pointer->x * device_scale,
            .y = pointer->y * device_scale,
        },
        .x_axis = wl_fixed_to_double (pointer->axis_x_delta) * device_scale,
        .y_axis = -wl_fixed_to_double (pointer->axis_y_delta) * device_scale,
    };
    wpe_view_backend_dispatch_axis_event (backend, &event.base);
#else /* !HAVE_2D_AXIS_EVENT */
    struct wpe_input_axis_event event = {
        .type = wpe_input_axis_event_type_motion,
        .time = pointer->axis_timestamp,
        .x = pointer->x * device_scale,
        .y = pointer->y * device_scale,
        /* TODO: .modifiers */
    };
    if (pointer->axis_x_delta) {
        event.axis = WL_POINTER_AXIS_HORIZONTAL_SCROLL;
        event.value = wl_fixed_to_int (pointer->axis_x_delta) > 0 ? 1 : -1;
        wpe_view_backend_dispatch_axis_event (backend, &event);
    };
    if (pointer->axis_y_delta) {
        event.axis = WL_POINTER_AXIS_VERTICAL_SCROLL;
        event.value = wl_fixed_to_int (pointer->axis_y_delta) > 0 ? -1 : 1;
        wpe_view_backend_dispatch_axis_event (backend, &event);
    }
#endif /* HAVE_2D_AXIS_EVENT */
}


static void
cog_fdo_shell_on_touch_down (PwlWindow      *window,
                             const PwlTouch *touch,
                             void           *shell)
{
    CogFdoShell *self = shell;
    CogFdoView *view = (CogFdoView*) cog_shell_get_focused_view (shell);

    const uint32_t device_scale = pwl_window_get_device_scale (self->window);
    view->touchpoints[touch->id] = (struct wpe_input_touch_event_raw) {
        .type = wpe_input_touch_event_type_down,
        .time = touch->time,
        .id = touch->id,
        .x = wl_fixed_to_int (touch->x) * device_scale,
        .y = wl_fixed_to_int (touch->y) * device_scale,
    };

    struct wpe_input_touch_event event = {
        .touchpoints = view->touchpoints,
        .touchpoints_length = PWL_N_TOUCH_POINTS,
        .type = wpe_input_touch_event_type_down,
        .id = touch->id,
        .time = touch->time,
        /* TODO: .modifiers */
    };

    struct wpe_view_backend *backend = cog_view_get_backend (&view->parent);
    wpe_view_backend_dispatch_touch_event (backend, &event);
}


static void
cog_fdo_shell_on_touch_up (PwlWindow      *window,
                           const PwlTouch *touch,
                           void           *shell)
{
    CogFdoView *view = (CogFdoView*) cog_shell_get_focused_view (shell);

    view->touchpoints[touch->id] = (struct wpe_input_touch_event_raw) {
        .type = wpe_input_touch_event_type_up,
        .time = touch->time,
        .id = touch->id,
        .x = view->touchpoints[touch->id].x,
        .y = view->touchpoints[touch->id].y,
    };

    struct wpe_input_touch_event event = {
        .touchpoints = view->touchpoints,
        .touchpoints_length = PWL_N_TOUCH_POINTS,
        .type = wpe_input_touch_event_type_up,
        .id = touch->id,
        .time = touch->time,
        /* TODO: .modifiers */
    };

    struct wpe_view_backend *backend = cog_view_get_backend (&view->parent);
    wpe_view_backend_dispatch_touch_event (backend, &event);

    memset (&view->touchpoints[touch->id], 0x00, sizeof (struct wpe_input_touch_event_raw));
}


static void
cog_fdo_shell_on_touch_motion (PwlWindow      *window,
                               const PwlTouch *touch,
                               void           *shell)
{
    CogFdoShell *self = shell;
    CogFdoView *view = (CogFdoView*) cog_shell_get_focused_view (shell);

    const uint32_t device_scale = pwl_window_get_device_scale (self->window);
    view->touchpoints[touch->id] = (struct wpe_input_touch_event_raw) {
        .type = wpe_input_touch_event_type_motion,
        .time = touch->time,
        .id = touch->id,
        .x = wl_fixed_to_int (touch->x) * device_scale,
        .y = wl_fixed_to_int (touch->y) * device_scale,
    };

    struct wpe_input_touch_event event = {
        .touchpoints = view->touchpoints,
        .touchpoints_length = PWL_N_TOUCH_POINTS,
        .type = wpe_input_touch_event_type_motion,
        .id = touch->id,
        .time = touch->time,
        /* TODO: .modifiers */
    };

    struct wpe_view_backend *backend = cog_view_get_backend (&view->parent);
    wpe_view_backend_dispatch_touch_event (backend, &event);
}


static void
cog_fdo_shell_on_device_scale (PwlWindow *window G_GNUC_UNUSED,
                               uint32_t   device_scale,
                               void      *shell)
{
    g_assert (((CogFdoShell*) shell)->window == window);

    TRACE ("shell @ %p, new device scale @%" PRIu32, shell, device_scale);

    /* Apply the scaling factor to all the views. */
    for (GList *item = cog_shell_get_views (shell); item; item = g_list_next (item)) {
        struct wpe_view_backend *backend = cog_view_get_backend (item->data);
        wpe_view_backend_dispatch_set_device_scale_factor (backend, device_scale);
    }
}


static void
cog_fdo_shell_on_window_resize (PwlWindow *window G_GNUC_UNUSED,
                                uint32_t   width,
                                uint32_t   height,
                                void      *shell)
{
    CogFdoShell *self = shell;
    g_assert (self->window == window);

    TRACE ("shell @ %p, new size %" PRIu32 "x%" PRIu32, shell, width, height);

    /* Apply the new size to all the views. */
    for (GList *item = cog_shell_get_views (shell); item; item = g_list_next (item)) {
        struct wpe_view_backend *backend = cog_view_get_backend (item->data);
        wpe_view_backend_dispatch_set_size (backend, width, height);
    }

    if (self->gl_texture) {
        cog_fdo_gles_destroy_texture (&self->gl_texture);
        cog_fdo_gles_create_texture (&self->gl_texture, width, height);
    }
}


static inline bool
cog_fdo_shell_has_gles_setup (const CogFdoShell *self)
{
    g_assert (self);

    return self->render_mode == RENDER_MODE_GLES_V2_PAINT && self->gl_program;
}

static void
cog_fdo_view_on_focus_change (PwlWindow *window,
                              PwlFocus   focus,
                              void      *view)
{
    TRACE ("view @ %p, new focus mask %#x", view, focus);
    g_assert (((CogFdoView*) view)->window == window);

    if (focus & PWL_FOCUS_KEYBOARD)
        cog_view_set_focused (view, TRUE);
}


static void
cog_fdo_shell_gles_cleanup (CogFdoShell *self)
{
    g_assert (self);

    if (!cog_fdo_shell_has_gles_setup (self))
        return;

    if (self->gl_texture) cog_fdo_gles_destroy_texture (&self->gl_texture);
    self->gl_texture_uniform = 0;
    g_clear_handle_id (&self->gl_program, glDeleteProgram);

    g_assert (!cog_fdo_shell_has_gles_setup (self));
}


static bool
cog_fdo_shell_gles_setup (CogFdoShell *self,
                          PwlWindow   *window,
                          GError     **error)
{
    g_assert (self);
    g_assert (window);

    if (cog_fdo_shell_has_gles_setup (self))
        return true;

    /*
     * We need an active context with a target surface to be able to create
     * a shader program and obtain th glEGLImageTargetTexture2DOES pointer.
     */
    if (!pwl_window_egl_make_current (window, error))
        return false;

    if (!eglSwapInterval (pwl_display_egl_get_display (s_display),
                          self->egl_swap_interval))
        g_warning ("Could not set EGL swap interval to %d (%#06x)",
                   self->egl_swap_interval, eglGetError ());

    self->gl_eglImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
        eglGetProcAddress ("glEGLImageTargetTexture2DOES");
    if (!self->gl_eglImageTargetTexture2DOES) {
        g_set_error (error, PWL_ERROR, PWL_ERROR_EGL,
                     "No glEGLImageTargetTexture2DOES pointer (%#06x)",
                     eglGetError ());
        return false;
    }

    static const char *vertex_shader_source =
        "attribute vec2 pos;\n"
        "attribute vec2 texture;\n"
        "varying vec2 v_texture;\n"
        "void main() {\n"
        "  v_texture = texture;\n"
        "  gl_Position = vec4(pos, 0, 1);\n"
        "}\n";

    static const char* fragment_shader_source =
        "precision mediump float;\n"
        "uniform sampler2D u_texture;\n"
        "varying vec2 v_texture;\n"
        "void main() {\n"
        "  gl_FragColor = texture2D(u_texture, v_texture);\n"
        "}\n";

    GLuint vertex_shader = glCreateShader (GL_VERTEX_SHADER);
    if (!vertex_shader) {
        g_set_error (error,
                     PWL_ERROR,
                     PWL_ERROR_GL,
                     "Cannot create vertex shader (%#06x)",
                     glGetError ());
        return false;
    }

    glShaderSource (vertex_shader, 1, &vertex_shader_source, NULL);
    glCompileShader (vertex_shader);

    GLuint fragment_shader = glCreateShader (GL_FRAGMENT_SHADER);
    if (!fragment_shader) {
        g_set_error (error,
                     PWL_ERROR,
                     PWL_ERROR_GL,
                     "Cannot create fragment shader (%#06x)",
                     glGetError ());
        glDeleteShader (vertex_shader);
        return false;
    }

    glShaderSource (fragment_shader, 1, &fragment_shader_source, NULL);
    glCompileShader (fragment_shader);

    GLuint program = glCreateProgram ();
    if (!program) {
        g_set_error (error,
                     PWL_ERROR,
                     PWL_ERROR_GL,
                     "Cannot create shader program (%#06x)",
                     glGetError ());
        glDeleteShader (vertex_shader);
        glDeleteShader (fragment_shader);
        return false;
    }

    glAttachShader (program, vertex_shader);
    glAttachShader (program, fragment_shader);

    glBindAttribLocation (program, 0, "pos");
    glBindAttribLocation (program, 1, "texture");

    glLinkProgram (program);

    /* Not needed anymore after the program has been linked. */
    glDeleteShader (vertex_shader);
    glDeleteShader (fragment_shader);

    GLint log_length;
    glGetProgramiv (program, GL_INFO_LOG_LENGTH, &log_length);

    char buffer[log_length + 1];
    glGetProgramInfoLog (program, log_length + 1, NULL, buffer);

    GLint succeeded = GL_FALSE;
    glGetProgramiv (program, GL_LINK_STATUS, &succeeded);
    if (succeeded == GL_FALSE) {
        g_set_error (error,
                     PWL_ERROR,
                     PWL_ERROR_GL,
                     "Cannot link shader program: %s",
                     buffer);
        return false;
    } else if (log_length) {
        g_info ("Shader program: %s.", buffer);
    }

    self->gl_texture_uniform = glGetUniformLocation (program, "u_texture");
    self->gl_program = program;

    return true;
}


static void
cog_fdo_view_on_export_fdo_egl_image_single_window_attach_buffer (void*, struct wpe_fdo_egl_exported_image*);

static void
cog_fdo_view_on_export_fdo_egl_image_single_window_gles_paint (void*, struct wpe_fdo_egl_exported_image*);

static void
cog_fdo_view_on_export_fdo_egl_image_multi_window_attach_buffer (void*, struct wpe_fdo_egl_exported_image*);

static void
cog_fdo_view_on_export_fdo_egl_image_multi_window_gles_paint (void*, struct wpe_fdo_egl_exported_image*);

static void
cog_fdo_shell_gles_paint (CogFdoShell*, GLuint, PwlWindow*, struct wpe_fdo_egl_exported_image*);

static gboolean
cog_fdo_shell_initable_init (GInitable    *initable,
                             GCancellable *cancellable,
                             GError      **error)
{
    if (!wpe_loader_init ("libWPEBackend-fdo-1.0.so")) {
        g_set_error_literal (error,
                             G_IO_ERROR,
                             G_IO_ERROR_FAILED,
                             "Could not initialize WPE loader.");
        return FALSE;
    }

    CogFdoShell *self = COG_FDO_SHELL (initable);

    /*
     * Prefer using wl_surface_attach(), but fall-back to using GLESv2
     * when a driver/compositor/etc. is known to have problems with it.
     */
    if (self->render_mode == RENDER_MODE_AUTO)
        self->render_mode =
            pwl_display_egl_has_broken_buffer_from_image (s_display)
                ? RENDER_MODE_GLES_V2_PAINT
                : RENDER_MODE_ATTACH_BUFFER;

    if (!pwl_display_egl_init (s_display,
                               self->render_mode == RENDER_MODE_GLES_V2_PAINT
                                ? PWL_EGL_CONFIG_FULL
                                : PWL_EGL_CONFIG_MINIMAL,
                               error))
        return FALSE;

    /* TODO: Make the application identifier a CogShell property. */
    if (COG_DEFAULT_APPID && *COG_DEFAULT_APPID)
        pwl_display_set_default_application_id (s_display, COG_DEFAULT_APPID);

    /* TODO: Set later on the loaded page title as window title. */
    if (COG_DEFAULT_APPNAME && *COG_DEFAULT_APPNAME)
        pwl_display_set_default_window_title (s_display, COG_DEFAULT_APPNAME);

    pwl_display_attach_sources (s_display, g_main_context_get_thread_default ());
    wpe_fdo_initialize_for_egl_display (pwl_display_egl_get_display (s_display));

    if (self->single_window) {
        self->window = pwl_window_create (s_display);
        pwl_window_set_id (self->window, self->window_id);

        pwl_window_notify_keyboard (self->window, cog_fdo_shell_on_keyboard, self);

        pwl_window_notify_pointer_motion (self->window, cog_fdo_shell_on_pointer_motion, self);
        pwl_window_notify_pointer_button (self->window, cog_fdo_shell_on_pointer_button, self);
        pwl_window_notify_pointer_axis (self->window, cog_fdo_shell_on_pointer_axis, self);

        pwl_window_notify_touch_down (self->window, cog_fdo_shell_on_touch_down, self);
        pwl_window_notify_touch_up (self->window, cog_fdo_shell_on_touch_up, self);
        pwl_window_notify_touch_motion (self->window, cog_fdo_shell_on_touch_motion, self);

        pwl_window_notify_device_scale (self->window, cog_fdo_shell_on_device_scale, self);
        pwl_window_notify_resize (self->window, cog_fdo_shell_on_window_resize, self);

        if (self->render_mode == RENDER_MODE_GLES_V2_PAINT) {
            if (!cog_fdo_shell_gles_setup (self, self->window, error))
                return FALSE;

            uint32_t width, height;
            pwl_window_get_size (self->window, &width, &height);
            cog_fdo_gles_create_texture (&self->gl_texture, width, height);
            self->exportable_client.export_fdo_egl_image =
                cog_fdo_view_on_export_fdo_egl_image_single_window_gles_paint;
        } else {
            self->exportable_client.export_fdo_egl_image =
                cog_fdo_view_on_export_fdo_egl_image_single_window_attach_buffer;
        }
    } else {
        self->exportable_client.export_fdo_egl_image =
            self->render_mode == RENDER_MODE_ATTACH_BUFFER
                ? cog_fdo_view_on_export_fdo_egl_image_multi_window_attach_buffer
                : cog_fdo_view_on_export_fdo_egl_image_multi_window_gles_paint;
    }

    return TRUE;
}


static void cog_fdo_shell_initable_iface_init (GInitableIface *iface)
{
    iface->init = cog_fdo_shell_initable_init;
}


G_DEFINE_DYNAMIC_TYPE_EXTENDED (CogFdoShell, cog_fdo_shell, COG_TYPE_SHELL, 0,
    g_io_extension_point_implement (COG_MODULES_SHELL_EXTENSION_POINT,
                                    g_define_type_id, "fdo", 100);
    G_IMPLEMENT_INTERFACE_DYNAMIC (G_TYPE_INITABLE, cog_fdo_shell_initable_iface_init))


G_DEFINE_DYNAMIC_TYPE (CogFdoView, cog_fdo_view, COG_TYPE_VIEW)


static void
cog_fdo_view_on_keyboard (PwlWindow         *window G_GNUC_UNUSED,
                          const PwlKeyboard *keyboard,
                          void              *view)
{
    struct wpe_input_keyboard_event event = {
        .time = keyboard->timestamp,
        .key_code = keyboard->keysym,
        .hardware_key_code = keyboard->unicode,
        .pressed = !!keyboard->state,
        .modifiers = keyboard->modifiers,
    };
    wpe_view_backend_dispatch_keyboard_event (cog_view_get_backend (view),
                                              &event);
}


static inline void
cog_fdo_view_dispatch_pointer_event (CogFdoView                       *self,
                                     const PwlPointer                 *pointer,
                                     enum wpe_input_pointer_event_type event_type)
{
    const uint32_t device_scale = pwl_window_get_device_scale (self->window);
    struct wpe_input_pointer_event event = {
        .type = event_type,
        .time = pointer->timestamp,
        .x = pointer->x * device_scale,
        .y = pointer->y * device_scale,
        .button = pointer->button,
        .state = pointer->state,
        /* TODO: .modifiers */
    };
    wpe_view_backend_dispatch_pointer_event (cog_view_get_backend (&self->parent),
                                             &event);
}


static void
cog_fdo_view_on_pointer_motion (PwlWindow        *window G_GNUC_UNUSED,
                                const PwlPointer *pointer,
                                void             *view)
{
    g_assert (((CogFdoView*) view)->window == window);

    cog_fdo_view_dispatch_pointer_event (view,
                                         pointer,
                                         wpe_input_pointer_event_type_motion);
}


static void
cog_fdo_view_on_pointer_button (PwlWindow        *window G_GNUC_UNUSED,
                                const PwlPointer *pointer,
                                void             *view)
{
    g_assert (((CogFdoView*) view)->window == window);

    cog_fdo_view_dispatch_pointer_event (view,
                                         pointer,
                                         wpe_input_pointer_event_type_button);
}


static void
cog_fdo_view_on_pointer_axis (PwlWindow        *window,
                              const PwlPointer *pointer,
                              void             *view)
{
    CogFdoView *self = view;
    g_assert (self->window == window);

    const uint32_t device_scale = pwl_window_get_device_scale (window);
    struct wpe_view_backend *backend = cog_view_get_backend (view);

#if HAVE_2D_AXIS_EVENT
    struct wpe_input_axis_2d_event event = {
        .base = {
            .type = wpe_input_axis_event_type_mask_2d | wpe_input_axis_event_type_motion_smooth,
            .time = pointer->axis_timestamp,
            .x = pointer->x * device_scale,
            .y = pointer->y * device_scale,
        },
        .x_axis = wl_fixed_to_double (pointer->axis_x_delta) * device_scale,
        .y_axis = -wl_fixed_to_double (pointer->axis_y_delta) * device_scale,
    };
    wpe_view_backend_dispatch_axis_event (backend, &event.base);
#else /* !HAVE_2D_AXIS_EVENT */
    struct wpe_input_axis_event event = {
        .type = wpe_input_axis_event_type_motion,
        .time = pointer->axis_timestamp,
        .x = pointer->x * device_scale,
        .y = pointer->y * device_scale,
        /* TODO: .modifiers */
    };
    if (pointer->axis_x_delta) {
        event.axis = WL_POINTER_AXIS_HORIZONTAL_SCROLL;
        event.value = wl_fixed_to_int (pointer->axis_x_delta) > 0 ? 1 : -1;
        wpe_view_backend_dispatch_axis_event (backend, &event);
    };
    if (pointer->axis_y_delta) {
        event.axis = WL_POINTER_AXIS_VERTICAL_SCROLL;
        event.value = wl_fixed_to_int (pointer->axis_y_delta) > 0 ? -1 : 1;
        wpe_view_backend_dispatch_axis_event (backend, &event);
    }
#endif /* HAVE_2D_AXIS_EVENT */
}


static void
cog_fdo_view_on_touch_down (PwlWindow      *window,
                            const PwlTouch *touch,
                            void           *view)
{
    CogFdoView *self = view;

    const uint32_t device_scale = pwl_window_get_device_scale (self->window);
    self->touchpoints[touch->id] = (struct wpe_input_touch_event_raw) {
        .type = wpe_input_touch_event_type_down,
        .time = touch->time,
        .id = touch->id,
        .x = wl_fixed_to_int (touch->x) * device_scale,
        .y = wl_fixed_to_int (touch->y) * device_scale,
    };

    struct wpe_input_touch_event event = {
        .touchpoints = self->touchpoints,
        .touchpoints_length = PWL_N_TOUCH_POINTS,
        .type = wpe_input_touch_event_type_down,
        .id = touch->id,
        .time = touch->time,
        /* TODO: .modifiers */
    };

    struct wpe_view_backend *backend = cog_view_get_backend (view);
    wpe_view_backend_dispatch_touch_event (backend, &event);
}


static void
cog_fdo_view_on_touch_up (PwlWindow      *window,
                          const PwlTouch *touch,
                          void           *view)
{
    CogFdoView *self = view;

    self->touchpoints[touch->id] = (struct wpe_input_touch_event_raw) {
        .type = wpe_input_touch_event_type_up,
        .time = touch->time,
        .id = touch->id,
        .x = self->touchpoints[touch->id].x,
        .y = self->touchpoints[touch->id].y,
    };

    struct wpe_input_touch_event event = {
        .touchpoints = self->touchpoints,
        .touchpoints_length = PWL_N_TOUCH_POINTS,
        .type = wpe_input_touch_event_type_up,
        .id = touch->id,
        .time = touch->time,
        /* TODO: .modifiers */
    };

    struct wpe_view_backend *backend = cog_view_get_backend (view);
    wpe_view_backend_dispatch_touch_event (backend, &event);

    memset (&self->touchpoints[touch->id], 0x00, sizeof (struct wpe_input_touch_event_raw));
}


static void
cog_fdo_view_on_touch_motion (PwlWindow      *window,
                              const PwlTouch *touch,
                              void           *view)
{
    CogFdoView *self = view;

    const uint32_t device_scale = pwl_window_get_device_scale (self->window);
    self->touchpoints[touch->id] = (struct wpe_input_touch_event_raw) {
        .type = wpe_input_touch_event_type_motion,
        .time = touch->time,
        .id = touch->id,
        .x = wl_fixed_to_int (touch->x) * device_scale,
        .y = wl_fixed_to_int (touch->y) * device_scale,
    };

    struct wpe_input_touch_event event = {
        .touchpoints = self->touchpoints,
        .touchpoints_length = PWL_N_TOUCH_POINTS,
        .type = wpe_input_touch_event_type_motion,
        .id = touch->id,
        .time = touch->time,
        /* TODO: .modifiers */
    };

    struct wpe_view_backend *backend = cog_view_get_backend (view);
    wpe_view_backend_dispatch_touch_event (backend, &event);
}


static void
cog_fdo_view_on_device_scale (PwlWindow *window G_GNUC_UNUSED,
                              uint32_t   device_scale,
                              void      *view)
{
    g_assert (((CogFdoView*) view)->window == window);

    TRACE ("view @ %p, new device scale @%" PRIu32, view, device_scale);

    struct wpe_view_backend *backend = cog_view_get_backend (view);
    wpe_view_backend_dispatch_set_device_scale_factor (backend, device_scale);
}


static void
cog_fdo_view_on_window_resize (PwlWindow *window G_GNUC_UNUSED,
                               uint32_t   width,
                               uint32_t   height,
                               void      *view)
{
    CogFdoView *self = view;
    g_assert (self->window == window);

    TRACE ("view @ %p, new size %" PRIu32 "x%" PRIu32, view, width, height);

    struct wpe_view_backend *backend = cog_view_get_backend (view);
    wpe_view_backend_dispatch_set_size (backend, width, height);

    if (self->gl_texture) {
        cog_fdo_gles_destroy_texture (&self->gl_texture);
        cog_fdo_gles_create_texture (&self->gl_texture, width, height);
    }
}

typedef struct {
    CogFdoView *view;
    struct wpe_fdo_egl_exported_image *image;
} CogFdoBufferData;

static void
cog_fdo_view_on_buffer_release (void             *data,
                                struct wl_buffer *buffer)
{
    CogFdoBufferData *buffer_data = data;

    if (buffer_data->view->last_buffer == buffer)
        buffer_data->view->last_buffer = NULL;

    wl_buffer_destroy (buffer);
    TRACE ("view @ %p, buffer @ %p destroyed", buffer_data->view, buffer);

    if (!buffer_data->view->last_image) {
        wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image (buffer_data->view->exportable,
                                                                             buffer_data->image);
        TRACE ("view @ %p, image @ %p destroyed",
               buffer_data->view, buffer_data->image);
    } else {
        TRACE ("view @ %p, image @ %p kept",
               buffer_data->view, buffer_data->image);
    }

    g_slice_free (CogFdoBufferData, buffer_data);
}


static void
cog_fdo_view_schedule_buffer_release (CogFdoView                        *self,
                                      struct wl_buffer                  *buffer,
                                      struct wpe_fdo_egl_exported_image *image)
{
    static const struct wl_buffer_listener listener = {
        .release = cog_fdo_view_on_buffer_release,
    };

    CogFdoBufferData *data = g_slice_new0 (CogFdoBufferData);
    data->view = self;
    data->image = image;
    wl_buffer_add_listener (buffer, &listener, data);

    TRACE ("view @ %p, buffer @ %p listener added", self, buffer);
}


static void
cog_fdo_view_update_window_contents (CogFdoView                        *self,
                                     PwlWindow                         *window,
                                     struct wpe_fdo_egl_exported_image *image)
{
    struct wl_buffer *buffer =
        pwl_display_egl_create_buffer_from_image (s_display,
                                                  wpe_fdo_egl_exported_image_get_egl_image (image));

    cog_fdo_view_schedule_buffer_release (self, buffer, image);

    uint32_t width, height;
    pwl_window_get_size (window, &width, &height);

    /*
     * If the web view background color does NOT have an alpha component, the
     * background will never be transparent: always set the opaque region to
     * hint to the compositor that it can skip blending the window surface.
     */
    WebKitColor bg_color;
    webkit_web_view_get_background_color ((WebKitWebView*) self, &bg_color);
    if (!bg_color.alpha || pwl_window_is_fullscreen (window))
        pwl_window_set_opaque_region (window, 0, 0, width, height);
    else
        pwl_window_unset_opaque_region (window);

    struct wl_surface *window_surface = pwl_window_get_surface (window);

    wl_surface_attach (window_surface, buffer, 0, 0);
    wl_surface_damage (window_surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit (window_surface);
}


static void
cog_fdo_view_on_export_fdo_egl_image_single_window_attach_buffer (void                              *data,
                                                                  struct wpe_fdo_egl_exported_image *image)
{
    CogFdoView *self = data;

    if (self->last_image) {
        wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image (self->exportable,
                                                                             self->last_image);
        TRACE ("view @ %p, image @ %p destroyed", self, self->last_image);
    }

    self->last_image = image;
    TRACE ("view @ %p, image @ %p saved", self, self->last_image);

    if (cog_shell_get_focused_view (cog_view_get_shell (data)) == (CogView*) self) {
        /* This view is currently being shown: attach buffer right away. */
        TRACE ("view %p focused, updating window.", data);
        CogFdoShell *shell = (CogFdoShell*) cog_view_get_shell (data);
        cog_fdo_view_update_window_contents (self, shell->window, self->last_image);
    } else {
        TRACE ("view %p not focused, skipping window update.", data);
    }

    wpe_view_backend_exportable_fdo_dispatch_frame_complete (self->exportable);
}


static void
cog_fdo_view_on_export_fdo_egl_image_multi_window_attach_buffer (void                              *data,
                                                                 struct wpe_fdo_egl_exported_image *image)
{
    CogFdoView *self = data;

    cog_fdo_view_update_window_contents (self, self->window, image);
    wpe_view_backend_exportable_fdo_dispatch_frame_complete (self->exportable);
}

static void
cog_fdo_shell_gles_paint (CogFdoShell                       *self,
                          GLuint                             texture,
                          PwlWindow                         *window,
                          struct wpe_fdo_egl_exported_image *image)
{
    TRACE ("shell @ %p, texture #%d, window @ %p, image @ %p",
           self, texture, window, image);

    uint32_t width, height;
    pwl_window_get_size (window, &width, &height);

    g_autoptr(GError) error = NULL;
    if (!pwl_window_egl_make_current (window, &error)) {
        g_critical ("Cannot activate EGL context: %s", error->message);
        return;
    }

    glViewport (0, 0, width, height);
    if (!image) {
        glClearColor (1, 1, 1, 1);  /* White. */
        glClear (GL_COLOR_BUFFER_BIT);
    } else {
        g_assert (texture != 0);

        glUseProgram (self->gl_program);
        glActiveTexture (GL_TEXTURE0);
        glBindTexture (GL_TEXTURE_2D, texture);
        self->gl_eglImageTargetTexture2DOES (GL_TEXTURE_2D,
            wpe_fdo_egl_exported_image_get_egl_image (image));
        glUniform1i (self->gl_texture_uniform, 0);

        static const GLfloat vertices[4][2] = {
            { -1.0,  1.0 },
            {  1.0,  1.0 },
            { -1.0, -1.0 },
            {  1.0, -1.0 },
        };
        static const GLfloat texture_pos[4][2] = {
            { 0, 0 },
            { 1, 0 },
            { 0, 1 },
            { 1, 1 },
        };

        glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
        glVertexAttribPointer (1, 2, GL_FLOAT, GL_FALSE, 0, texture_pos);

        glEnableVertexAttribArray (0);
        glEnableVertexAttribArray (1);

        glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);

        glDisableVertexAttribArray (0);
        glDisableVertexAttribArray (1);
    }

    if (!pwl_window_egl_swap_buffers (window, &error))
        g_warning ("Could not swap EGL buffers: %s", error->message);
}

static void
cog_fdo_view_on_export_fdo_egl_image_single_window_gles_paint (void                              *data,
                                                               struct wpe_fdo_egl_exported_image *image)
{
    CogFdoView *self = data;

    TRACE ("view @ %p, image @ %p", self, image);

    if (self->last_image)
        wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image (self->exportable,
                                                                             self->last_image);

    self->last_image = image;

    CogFdoShell *shell = (CogFdoShell*) cog_view_get_shell (data);
    if (cog_shell_get_focused_view ((CogShell*) shell) == (CogView*) self) {
        /* This view is currently being shown: attach buffer right away. */
        TRACE ("view %p focused, repainting window.", data);
        cog_fdo_shell_gles_paint (shell,
                                  shell->gl_texture,
                                  shell->window,
                                  self->last_image);
    } else {
        TRACE ("view %p not focused, skipping window update.", self);
    }

    wpe_view_backend_exportable_fdo_dispatch_frame_complete (self->exportable);
}


static void
cog_fdo_view_on_export_fdo_egl_image_multi_window_gles_paint (void                              *data,
                                                              struct wpe_fdo_egl_exported_image *image)
{
    CogFdoView *self = data;
    cog_fdo_shell_gles_paint ((CogFdoShell*) cog_view_get_shell (data),
                              self->gl_texture,
                              self->window,
                              image);

    wpe_view_backend_exportable_fdo_dispatch_frame_complete (self->exportable);
}


static void
cog_fdo_view_get_property (GObject    *object,
                           unsigned    prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
    CogFdoView *self = COG_FDO_VIEW (object);
    switch (prop_id) {
        case VIEW_PROP_WINDOW_ID:
            g_value_set_uint (value, self->window_id);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}


static void
cog_fdo_view_set_property (GObject      *object,
                           unsigned      prop_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
    CogFdoView *self = COG_FDO_VIEW (object);
    switch (prop_id) {
        case VIEW_PROP_WINDOW_ID:
            self->window_id = g_value_get_uint (value);
            /* The window might be NULL during construction. */
            if (self->window)
                pwl_window_set_id (self->window, self->window_id);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}


static void
cog_fdo_view_destroy_backend (void *userdata)
{
    CogFdoView *self = userdata;
    g_clear_pointer (&self->exportable, wpe_view_backend_exportable_fdo_destroy);
}


static void
cog_fdo_view_setup (CogView *view)
{
    CogFdoShell *shell = COG_FDO_SHELL (cog_view_get_shell (view));
    CogFdoView *self = COG_FDO_VIEW (view);

    uint32_t width, height;
    if (shell->single_window) {
        pwl_window_get_size (shell->window, &width, &height);
    } else {
        /* We need to create the window early to query the size. */
        self->window = pwl_window_create (s_display);
        pwl_window_set_id (self->window, self->window_id);

        pwl_window_notify_keyboard (self->window, cog_fdo_view_on_keyboard, self);

        pwl_window_notify_pointer_motion (self->window, cog_fdo_view_on_pointer_motion, self);
        pwl_window_notify_pointer_button (self->window, cog_fdo_view_on_pointer_button, self);
        pwl_window_notify_pointer_axis (self->window, cog_fdo_view_on_pointer_axis, self);

        pwl_window_notify_touch_down (self->window, cog_fdo_view_on_touch_down, self);
        pwl_window_notify_touch_up (self->window, cog_fdo_view_on_touch_up, self);
        pwl_window_notify_touch_motion (self->window, cog_fdo_view_on_touch_motion, self);

        pwl_window_notify_device_scale (self->window, cog_fdo_view_on_device_scale, self);
        pwl_window_notify_resize (self->window, cog_fdo_view_on_window_resize, self);

        pwl_window_notify_focus_change (self->window, cog_fdo_view_on_focus_change, self);

        pwl_window_get_size (self->window, &width, &height);

        if (shell->render_mode == RENDER_MODE_GLES_V2_PAINT) {
            if (!cog_fdo_shell_has_gles_setup (shell)) {
                g_autoptr(GError) error = NULL;
                if (!cog_fdo_shell_gles_setup (shell, self->window, &error))
                    g_error ("Could not setup GLES: %s", error->message);
            }
            cog_fdo_gles_create_texture (&self->gl_texture, width, height);
        }
    }

    self->exportable = wpe_view_backend_exportable_fdo_egl_create (&shell->exportable_client,
                                                                   self,
                                                                   width,
                                                                   height);
    WebKitWebViewBackend *backend =
        webkit_web_view_backend_new (wpe_view_backend_exportable_fdo_get_view_backend (self->exportable),
                                     cog_fdo_view_destroy_backend,
                                     self);

    GValue backend_value = G_VALUE_INIT;
    g_value_take_boxed (g_value_init (&backend_value,
                                      WEBKIT_TYPE_WEB_VIEW_BACKEND),
                        backend);
    g_object_set_property (G_OBJECT (self), "backend", &backend_value);

    g_debug ("%s: view @ %p, backend @ %p (%" PRIu32 "x%" PRIu32 "), "
             "exportable @ %p, %s window.",
             G_STRFUNC, self, backend, width, height, self->exportable,
             shell->single_window ? "shared" : "own");
}


static void
cog_fdo_view_on_notify_focused (CogFdoView *self,
                                GParamSpec *pspec)
{
    if (!cog_view_get_focused ((CogView*) self))
        return;

    if (!self->last_image) {
        g_debug ("%s: No last image to show, skipping update.", G_STRFUNC);
        return;
    }

    CogFdoShell *shell = (CogFdoShell*) cog_view_get_shell ((CogView*) self);
    cog_fdo_view_update_window_contents (self,
                                         shell->window,
                                         self->last_image);
}


static void
cog_fdo_view_constructed (GObject *self)
{
    G_OBJECT_CLASS (cog_fdo_view_parent_class)->constructed (self);

    CogFdoShell *shell = (CogFdoShell*) cog_view_get_shell ((CogView*) self);
    if (shell->single_window) {
        /* After a view has been focused, window contents must be updated. */
        g_signal_connect_after (self, "notify::focused",
                                G_CALLBACK (cog_fdo_view_on_notify_focused),
                                NULL);
        /*
         * Keep the "in-window" and "visible" synchronized with "focused":
         * in single window mode the focused view is the only one visible.
         */
        g_object_bind_property (self, "focused", self, "in-window",
                                G_BINDING_SYNC_CREATE | G_BINDING_DEFAULT);
        g_object_bind_property (self, "focused", self, "visible",
                                G_BINDING_SYNC_CREATE | G_BINDING_DEFAULT);
    } else {
        /* TODO: Provide a mechanism to update the "visible" property. */
        cog_view_set_visible (COG_VIEW (self), TRUE);

        /* The web view is always inside its own window. */
        cog_view_set_in_window (COG_VIEW (self), TRUE);
    }
}


static void
cog_fdo_view_dispose (GObject *object)
{
    CogFdoView *self = (CogFdoView*) object;

    if (self->gl_texture)
        cog_fdo_gles_destroy_texture (&self->gl_texture);

    g_clear_pointer (&self->last_buffer, wl_buffer_destroy);
    if (self->last_image) {
        wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image (self->exportable,
                                                                             self->last_image);
        self->last_image = NULL;
    }

    G_OBJECT_CLASS (cog_fdo_view_parent_class)->dispose (object);
}


static void
cog_fdo_view_class_init (CogFdoViewClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->get_property = cog_fdo_view_get_property;
    object_class->set_property = cog_fdo_view_set_property;
    object_class->constructed = cog_fdo_view_constructed;
    object_class->dispose = cog_fdo_view_dispose;

    CogViewClass *view_class = COG_VIEW_CLASS (klass);
    view_class->setup = cog_fdo_view_setup;

    s_view_properties[VIEW_PROP_WINDOW_ID] =
        g_param_spec_uint ("window-id",
                           "Window identifier",
                           "Window identifier for the view",
                           0, UINT32_MAX, 0,
                           G_PARAM_READWRITE |
                           G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class,
                                       VIEW_N_PROPERTIES,
                                       s_view_properties);
}


static void
cog_fdo_view_class_finalize (CogFdoViewClass *klass)
{
}


static void
cog_fdo_view_init (CogFdoView *self)
{
}


static gboolean
cog_fdo_shell_is_supported (void)
{
    gboolean result = FALSE;

    G_LOCK (s_globals);
    if (s_support_checked) {
        if (s_display)
            result = TRUE;
    } else {
        g_autoptr(GError) error = NULL;
        g_autoptr(PwlDisplay) display = pwl_display_connect (NULL, &error);
        if (!display) {
            g_debug ("%s: %s", G_STRFUNC, error->message);
        } else {
            s_display = g_steal_pointer (&display);
            result = TRUE;
        }
        s_support_checked = true;
    }
    G_UNLOCK (s_globals);

    return result;
}


static void
cog_fdo_shell_get_property (GObject    *object,
                            unsigned    prop_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
    CogFdoShell *shell = COG_FDO_SHELL (object);
    switch (prop_id) {
        case SHELL_PROP_SINGLE_WINDOW:
            g_value_set_boolean (value, shell->single_window);
            break;
        case SHELL_PROP_WINDOW_ID:
            g_value_set_uint (value, shell->window_id);
            break;
        case SHELL_PROP_RENDER_MODE:
            switch (shell->render_mode) {
                case RENDER_MODE_AUTO:
                    g_value_set_static_string (value, "auto");
                    break;
                case RENDER_MODE_ATTACH_BUFFER:
                    g_value_set_static_string (value, "attach-buffer");
                    break;
                case RENDER_MODE_GLES_V2_PAINT:
                    g_value_set_static_string (value, "gles-v2-paint");
                    break;
            }
            break;
        case SHELL_PROP_SWAP_INTERVAL:
            g_value_set_int (value, shell->egl_swap_interval);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}


static void
cog_fdo_shell_set_property (GObject      *object,
                            unsigned      prop_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
    CogFdoShell *shell = COG_FDO_SHELL (object);
    switch (prop_id) {
        case SHELL_PROP_SINGLE_WINDOW:
            shell->single_window = g_value_get_boolean (value);
            g_message ("%s: single_window = %s.", G_STRFUNC,
                       shell->single_window ? "TRUE" : FALSE);
            break;
        case SHELL_PROP_WINDOW_ID:
            shell->window_id = g_value_get_uint (value);
            /* The window might be NULL during construction. */
            if (shell->window)
                pwl_window_set_id (shell->window, shell->window_id);
            break;
        case SHELL_PROP_RENDER_MODE:
            if (g_strcmp0 (g_value_get_string (value), "auto") == 0) {
                shell->render_mode = RENDER_MODE_AUTO;
            } else if (g_strcmp0 (g_value_get_string (value), "attach-buffer") == 0) {
                shell->render_mode = RENDER_MODE_ATTACH_BUFFER;
            } else if (g_strcmp0 (g_value_get_string (value), "gles-v2-paint") == 0) {
                shell->render_mode = RENDER_MODE_GLES_V2_PAINT;
            } else {
                g_warning ("%s:%d: invalid value '%s' for property \"%s\" in '%s'",
                           __FILE__,
                           __LINE__,
                           g_value_get_string (value),
                           pspec->name,
                           G_OBJECT_TYPE_NAME (object));
            }
            break;
        case SHELL_PROP_SWAP_INTERVAL:
            shell->egl_swap_interval = g_value_get_int (value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}


static void
cog_fdo_shell_dispose (GObject *object)
{
    CogFdoShell *self = (CogFdoShell*) object;

    if (cog_fdo_shell_has_gles_setup (self))
        cog_fdo_shell_gles_cleanup (self);

    g_clear_pointer (&self->window, pwl_window_destroy);

    G_OBJECT_CLASS (cog_fdo_shell_parent_class)->dispose (object);
}


static void
cog_fdo_shell_class_init (CogFdoShellClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->get_property = cog_fdo_shell_get_property;
    object_class->set_property = cog_fdo_shell_set_property;
    object_class->dispose = cog_fdo_shell_dispose;

    CogShellClass *shell_class = COG_SHELL_CLASS (klass);
    shell_class->is_supported = cog_fdo_shell_is_supported;
    shell_class->get_view_class = cog_fdo_view_get_type;

    s_shell_properties[SHELL_PROP_SINGLE_WINDOW] =
        g_param_spec_boolean ("single-window",
                              "Single window mode",
                              "Use one window for stacking all views",
                              FALSE,
                              G_PARAM_READWRITE |
                              G_PARAM_CONSTRUCT_ONLY |
                              G_PARAM_STATIC_STRINGS);

    s_shell_properties[SHELL_PROP_WINDOW_ID] =
        g_param_spec_uint ("window-id",
                           "Window identifier",
                           "Window identifier when in single window mode",
                           0, UINT32_MAX, 0,
                           G_PARAM_READWRITE |
                           G_PARAM_STATIC_STRINGS);

    s_shell_properties[SHELL_PROP_RENDER_MODE] =
        g_param_spec_string ("render-mode",
                             "Rendering mode",
                             "Determines how to render view contents",
                             "auto",
                             G_PARAM_READWRITE |
                             G_PARAM_CONSTRUCT_ONLY |
                             G_PARAM_STATIC_STRINGS);

    s_shell_properties[SHELL_PROP_SWAP_INTERVAL] =
        g_param_spec_int ("swap-interval",
                          "Swap interval",
                          "EGL buffers swap interval, used in gles-v2-paint mode",
                          0, 2, 1,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class,
                                       SHELL_N_PROPERTIES,
                                       s_shell_properties);
}


static void
cog_fdo_shell_class_finalize (CogFdoShellClass *klass)
{
}


static void
cog_fdo_shell_init (CogFdoShell *shell)
{
}


G_MODULE_EXPORT
void
g_io_fdo_shell_load (GIOModule *module)
{
    GTypeModule *type_module = G_TYPE_MODULE (module);
    cog_fdo_shell_register_type (type_module);
    cog_fdo_view_register_type (type_module);
}

G_MODULE_EXPORT
void
g_io_fdo_shell_unload (GIOModule *module G_GNUC_UNUSED)
{
    G_LOCK (s_globals);
    g_clear_pointer (&s_display, pwl_display_destroy);
    G_UNLOCK (s_globals);
}
