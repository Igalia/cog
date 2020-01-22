/*
 * pwl.c
 *
 * Copyright (C) 2020 Igalia S.L.
 * Copyright (C) 2019 Adrian Perez de Castro <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "pwl.h"

#include <errno.h>

/* for mmap */
#include <sys/mman.h>

#include <gio/gio.h>


PwlDisplay*
pwl_display_connect (const char *name, GError **error)
{
    g_autoptr(PwlDisplay) self = g_slice_new0 (PwlDisplay);

    if (!(self->display = wl_display_connect (name))) {
        g_set_error_literal (error, G_FILE_ERROR,
                             g_file_error_from_errno (errno),
                             "Could not open Wayland display");
        return NULL;
    }

    g_debug ("%s: Created @ %p", G_STRFUNC, self);
    return g_steal_pointer (&self);
}


void
pwl_display_destroy (PwlDisplay *self)
{
    g_assert (self != NULL);

    g_debug ("%s: Destroying @ %p", G_STRFUNC, self);

    if (self->display) {
        g_clear_pointer (&(self->event_src), g_source_destroy);
        g_clear_pointer (&(self->xdg_shell), xdg_wm_base_destroy);
        g_clear_pointer (&(self->fshell), zwp_fullscreen_shell_v1_destroy);
        g_clear_pointer (&(self->shell), wl_shell_destroy);

        g_clear_pointer (&self->compositor, wl_compositor_destroy);
        g_clear_pointer (&self->registry, wl_registry_destroy);

        wl_display_flush (self->display);
        g_clear_pointer (&self->display, wl_display_disconnect);
    }

    g_slice_free (PwlDisplay, self);
}


static gboolean
wl_src_prepare (GSource *base, gint *timeout)
{
    struct pwl_event_source *src = (struct pwl_event_source *) base;

    *timeout = -1;

    while (wl_display_prepare_read (src->display) != 0) {
        if (wl_display_dispatch_pending (src->display) < 0)
            return false;
    }
    wl_display_flush (src->display);

    return false;
}

static gboolean
wl_src_check (GSource *base)
{
    struct pwl_event_source *src = (struct pwl_event_source *) base;

    if (src->pfd.revents & G_IO_IN) {
        if (wl_display_read_events(src->display) < 0)
            return false;
        return true;
    } else {
        wl_display_cancel_read(src->display);
        return false;
    }
}


static gboolean
wl_src_dispatch (GSource *base, GSourceFunc callback, gpointer user_data)
{
    struct pwl_event_source *src = (struct pwl_event_source *) base;

    if (src->pfd.revents & G_IO_IN) {
        if (wl_display_dispatch_pending(src->display) < 0)
            return false;
    }

    if (src->pfd.revents & (G_IO_ERR | G_IO_HUP))
        return false;

    src->pfd.revents = 0;

    return true;
}

void
wl_src_finalize (GSource *base)
{
}

GSource *
setup_wayland_event_source (GMainContext *main_context,
                            PwlDisplay *display)
{
    static GSourceFuncs wl_src_funcs = {
        .prepare = wl_src_prepare,
        .check = wl_src_check,
        .dispatch = wl_src_dispatch,
        .finalize = wl_src_finalize,
    };

    struct pwl_event_source *wl_source =
        (struct pwl_event_source *) g_source_new (&wl_src_funcs,
                                               sizeof (struct pwl_event_source));
    wl_source->display = display->display;
    wl_source->pfd.fd = wl_display_get_fd (display->display);
    wl_source->pfd.events = G_IO_IN | G_IO_ERR | G_IO_HUP;
    wl_source->pfd.revents = 0;
    g_source_add_poll (&wl_source->source, &wl_source->pfd);

    g_source_set_can_recurse (&wl_source->source, TRUE);
    g_source_attach (&wl_source->source, g_main_context_get_thread_default());

    g_source_unref (&wl_source->source);

    return &wl_source->source;
}

#if HAVE_DEVICE_SCALING
static void
noop()
{
}

static void
output_handle_scale (void *data,
                     struct wl_output *output,
                     int32_t factor)
{
    PwlDisplay *display = (PwlDisplay*) data;
    bool found = false;
    for (int i = 0; i < G_N_ELEMENTS (display->metrics); i++)
    {
        if (display->metrics[i].output == output) {
            found = true;
            display->metrics[i].scale = factor;
            break;
        }
    }
    if (!found)
    {
        g_warning ("Unknown output %p\n", output);
        return;
    }
    g_info ("Got scale factor %i for output %p\n", factor, output);
}

static void
surface_handle_enter (void *data, struct wl_surface *surface, struct wl_output *output)
{
    PwlDisplay *display = data;
    int32_t scale_factor = -1;

    for (int i=0; i < G_N_ELEMENTS (display->metrics); i++)
    {
        if (display->metrics[i].output == output) {
            scale_factor = display->metrics[i].scale;
        }
    }
    if (scale_factor == -1) {
        g_warning ("No scale factor available for output %p\n", output);
        return;
    }
    g_debug ("Surface entered output %p with scale factor %i\n", output, scale_factor);
    wl_surface_set_buffer_scale (surface, scale_factor);
    display->current_output.scale = scale_factor;
    if (display->on_surface_enter) {
        display->on_surface_enter(display, display->on_surface_enter_userdata);
    }
}
#endif /* HAVE_DEVICE_SCALING */


static bool
capture_app_key_bindings (PwlDisplay *display,
                          uint32_t keysym,
                          uint32_t unicode,
                          uint32_t state,
                          uint8_t modifiers)
{
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        /* fullscreen */
        /* TODO: Requires disengage window, display and WL seat on capabilities
        if (modifiers == 0 && unicode == 0 && keysym == XKB_KEY_F11) {
            if (! s_win_data->is_fullscreen)
                xdg_toplevel_set_fullscreen (s_win_data->xdg_toplevel, NULL);
            else
                xdg_toplevel_unset_fullscreen (s_win_data->xdg_toplevel);
            s_win_data->is_fullscreen = ! s_win_data->is_fullscreen;
            return true;
        } */
    }

    if (display->on_capture_app_key) {
        return (*display->on_capture_app_key) (display, display->on_capture_app_key_userdata);
    }

    return false;
}

static void
handle_key_event (void *data, uint32_t key, uint32_t state, uint32_t time)
{
    PwlDisplay *display = data;

    display->keyboard.event.state = state;
    display->keyboard.event.timestamp = time;
    display->keyboard.event.unicode = xkb_state_key_get_utf32 (display->xkb_data.state, key);
    display->keyboard.event.keysym = xkb_state_key_get_one_sym (display->xkb_data.state, key);
    display->keyboard.event.modifiers = display->xkb_data.modifiers;
    /* Capture app-level key-bindings here */
    if (capture_app_key_bindings (display,
                                  display->keyboard.event.keysym,
                                  display->keyboard.event.unicode,
                                  state,
                                  display->keyboard.event.modifiers))
        return;

    if (display->xkb_data.compose_state != NULL
            && state == WL_KEYBOARD_KEY_STATE_PRESSED
            && xkb_compose_state_feed (display->xkb_data.compose_state, display->keyboard.event.keysym)
            == XKB_COMPOSE_FEED_ACCEPTED
            && xkb_compose_state_get_status (display->xkb_data.compose_state)
            == XKB_COMPOSE_COMPOSED) {
        display->keyboard.event.keysym = xkb_compose_state_get_one_sym (display->xkb_data.compose_state);
        display->keyboard.event.unicode = xkb_keysym_to_utf32 (display->keyboard.event.keysym);
    }

    if (display->on_key_event) {
        (*display->on_key_event) (display, display->on_key_event_userdata);
    }
}

static void
resize_window (PwlWinData *win_data)
{
    int32_t pixel_width = win_data->width * win_data->display->current_output.scale;
    int32_t pixel_height = win_data->height * win_data->display->current_output.scale;

    if (win_data->egl_window)
        wl_egl_window_resize (win_data->egl_window,
			                pixel_width,
			                pixel_height,
			                0, 0);

    g_debug ("Resized EGL buffer to: (%u, %u) @%ix\n",
            pixel_width, pixel_height, win_data->display->current_output.scale);

    if (win_data->on_window_resize) {
        (*win_data->on_window_resize) (win_data, win_data->on_window_resize_userdata);
    }
}


static void
configure_surface_geometry (PwlWinData *win_data, int32_t width, int32_t height)
{
    const char* env_var;
    if (width == 0) {
        env_var = g_getenv("COG_PLATFORM_FDO_VIEW_WIDTH");
        if (env_var != NULL)
            width = (int32_t) g_ascii_strtod(env_var, NULL);
        else
            width = DEFAULT_WIDTH;
    }
    if (height == 0) {
        env_var = g_getenv("COG_PLATFORM_FDO_VIEW_HEIGHT");
        if (env_var != NULL)
            height = (int32_t) g_ascii_strtod(env_var, NULL);
        else
            height = DEFAULT_HEIGHT;
    }

    win_data->width = width;
    win_data->height = height;
}

static void
shell_surface_ping (void *data,
                    struct wl_shell_surface *shell_surface,
                    uint32_t serial)
{
    wl_shell_surface_pong (shell_surface, serial);
}

static void
shell_surface_configure (void *data,
                         struct wl_shell_surface *shell_surface,
                         uint32_t edges,
                         int32_t width, int32_t height)
{
    PwlWinData *win_data = (PwlWinData*) data;
    configure_surface_geometry (win_data, width, height);

    g_debug ("New wl_shell configuration: (%" PRIu32 ", %" PRIu32 ")", width, height);

    resize_window (win_data);
}

static const struct wl_shell_surface_listener shell_surface_listener = {
    .ping = shell_surface_ping,
    .configure = shell_surface_configure,
};

static void
xdg_shell_ping (void *data, struct xdg_wm_base *shell, uint32_t serial)
{
    xdg_wm_base_pong(shell, serial);
}

static const struct xdg_wm_base_listener xdg_shell_listener = {
    .ping = xdg_shell_ping,
};

static void
registry_global (void               *data,
                 struct wl_registry *registry,
                 uint32_t            name,
                 const char         *interface,
                 uint32_t            version)
{
    PwlDisplay *display = (PwlDisplay*) data;
    gboolean interface_used = TRUE;

    if (strcmp (interface, wl_compositor_interface.name) == 0) {
        ((PwlDisplay *) data)->compositor = wl_registry_bind (registry,
                                               name,
                                               &wl_compositor_interface,
                                               version);
    } else if (strcmp (interface, wl_shell_interface.name) == 0) {
        display->shell = wl_registry_bind (registry,
                                          name,
                                          &wl_shell_interface,
                                          version);
    } else if (strcmp (interface, xdg_wm_base_interface.name) == 0) {
        display->xdg_shell = wl_registry_bind (registry,
                                              name,
                                              &xdg_wm_base_interface,
                                              version);
        g_assert (display->xdg_shell);
        xdg_wm_base_add_listener (display->xdg_shell, &xdg_shell_listener, NULL);
    } else if (strcmp (interface,
                       zwp_fullscreen_shell_v1_interface.name) == 0) {
        display->fshell = wl_registry_bind (registry,
                                           name,
                                           &zwp_fullscreen_shell_v1_interface,
                                           version);
    } else if (strcmp (interface, wl_seat_interface.name) == 0) {
        display->seat = wl_registry_bind (registry,
                                         name,
                                         &wl_seat_interface,
                                         version);
#if HAVE_DEVICE_SCALING
    } else if (strcmp (interface, wl_output_interface.name) == 0) {
        struct wl_output* output = wl_registry_bind (registry,
                                                     name,
                                                     &wl_output_interface,
                                                     version);
        static const struct wl_output_listener output_listener = {
            .geometry = noop,
            .mode = noop,
            .done = noop,
            .scale = output_handle_scale,
        };
        wl_output_add_listener (output, &output_listener, display);
        bool inserted = false;
        for (int i = 0; i < G_N_ELEMENTS (display->metrics); i++)
        {
            if (display->metrics[i].output == NULL) {
                display->metrics[i].output = output;
                display->metrics[i].name = name;
                inserted = true;
                break;
            }
        }
        if (!inserted) {
            g_warning ("Exceeded %" G_GSIZE_FORMAT " connected outputs(!)", G_N_ELEMENTS (display->metrics));
        }
#endif /* HAVE_DEVICE_SCALING */
    } else {
        interface_used = FALSE;
    }
    g_debug ("%s '%s' interface obtained from the Wayland registry.",
             interface_used ? "Using" : "Ignoring", interface);
}

static void
registry_global_remove (void *data, struct wl_registry *registry, uint32_t name)
{
    PwlDisplay *display = (PwlDisplay*) data;
    for (int i = 0; i < G_N_ELEMENTS (display->metrics); i++)
    {
        if (display->metrics[i].name == name) {
            display->metrics[i].output = NULL;
            display->metrics[i].name = 0;
            g_debug ("Removed output %i\n", name);
            break;
        }
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
#if HAVE_DEVICE_SCALING
    .global_remove = registry_global_remove
#endif /* HAVE_DEVICE_SCALING */
};

gboolean
pwl_display_egl_init (PwlDisplay *display, GError **error)
{
    g_debug ("Initializing EGL...");

    display->egl_display = eglGetDisplay ((EGLNativeDisplayType) display->display);
    if (display->egl_display == EGL_NO_DISPLAY) {
        // TODO: ERR_EGL (error, "Could not open EGL display");
        return FALSE;
    }

    EGLint major, minor;
    if (!eglInitialize (display->egl_display, &major, &minor)) {
        // TODO: ERR_EGL (error, "Could not initialize  EGL");
        pwl_display_egl_deinit (display);
        return FALSE;
    }
    g_info ("EGL version %d.%d initialized.", major, minor);

    if (!eglBindAPI (EGL_OPENGL_ES_API)) {
        // TODO: ERR_EGL (error, "Could not bind OpenGL ES API to EGL");
        pwl_display_egl_deinit (display);
        return FALSE;
    }

    static const EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    static const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE,     8,
        EGL_GREEN_SIZE,   8,
        EGL_BLUE_SIZE,    8,
        EGL_ALPHA_SIZE,   0,
        EGL_DEPTH_SIZE,   0,
        EGL_STENCIL_SIZE, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SAMPLES, 0,
        EGL_NONE
    };

    EGLint num_configs;
    if (!eglChooseConfig (display->egl_display,
                          config_attribs,
                          &(display->egl_config),
                          1,
                          &num_configs)) {
        // TODO: ERR_EGL (error, "Could not find a suitable EGL configuration");
        pwl_display_egl_deinit (display);
        return FALSE;
    }
    g_assert (num_configs > 0);

    display->egl_context = eglCreateContext (display->egl_display,
                                         display->egl_config,
                                         EGL_NO_CONTEXT,
                                         context_attribs);
    if (display->egl_context == EGL_NO_CONTEXT) {
        // TODO: ERR_EGL (error, "Could not create EGL context");
        pwl_display_egl_deinit (display);
        return FALSE;
    }

    return TRUE;
}

void pwl_display_egl_deinit (PwlDisplay *display)
{
    if (display->egl_display != EGL_NO_DISPLAY) {
        if (display->egl_context != EGL_NO_CONTEXT) {
            eglDestroyContext (display->egl_display, display->egl_context);
        }
        eglTerminate (display->egl_display);
        display->egl_context = EGL_NO_CONTEXT;
        display->egl_display = EGL_NO_DISPLAY;
    }
    eglReleaseThread ();
}


gboolean
init_wayland (PwlDisplay *display, GError **error)
{
    g_debug ("Initializing Wayland...");

    if (!(display->display = wl_display_connect (NULL))) {
        g_set_error (error,
                     G_FILE_ERROR,
                     g_file_error_from_errno (errno),
                     "Could not open Wayland display");
        return FALSE;
    }

    display->current_output.scale = 1;
    display->registry = wl_display_get_registry (display->display);
    g_assert (display->registry);
    wl_registry_add_listener (display->registry,
                              &registry_listener,
                              display);
    wl_display_roundtrip (display->display);

    g_assert (display->compositor);
    g_assert (display->xdg_shell != NULL ||
              display->shell != NULL ||
              display->fshell != NULL);

    return TRUE;
}

static void
xdg_surface_on_configure (void *data,
                          struct xdg_surface *surface,
                          uint32_t serial)
{
    xdg_surface_ack_configure (surface, serial);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_on_configure
};

static void
xdg_toplevel_on_configure (void *data,
                           struct xdg_toplevel *toplevel,
                           int32_t width, int32_t height,
                           struct wl_array *states)
{
    PwlWinData *win_data = (PwlWinData*) data;
    configure_surface_geometry (win_data, width, height);

    g_debug ("New XDG toplevel configuration: (%" PRIu32 ", %" PRIu32 ")", width, height);

    resize_window (data);
}

static void
xdg_toplevel_on_close (void *data, struct xdg_toplevel *xdg_toplevel)
{
    // TODO g_application_quit (g_application_get_default ());
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_on_configure,
    .close = xdg_toplevel_on_close,
};


gboolean create_window (PwlDisplay* display, PwlWinData* win_data, GError **error)
{
    g_debug ("Creating Wayland surface...");

    win_data->display = display,

    win_data->egl_surface = EGL_NO_SURFACE,
    win_data->width = DEFAULT_WIDTH,
    win_data->height = DEFAULT_HEIGHT,
    win_data->is_fullscreen = false,
    win_data->is_maximized = false,

    win_data->wl_surface = wl_compositor_create_surface (display->compositor);
    g_assert (win_data->wl_surface);

#if HAVE_DEVICE_SCALING
    static const struct wl_surface_listener surface_listener = {
        .enter = surface_handle_enter,
        .leave = noop,
    };
    wl_surface_add_listener (win_data->wl_surface, &surface_listener, display);
#endif /* HAVE_DEVICE_SCALING */

    if (display->xdg_shell != NULL) {
        win_data->xdg_surface =
            xdg_wm_base_get_xdg_surface (display->xdg_shell,
                                         win_data->wl_surface);
        g_assert (win_data->xdg_surface);

        xdg_surface_add_listener (win_data->xdg_surface, &xdg_surface_listener,
                                  NULL);
        win_data->xdg_toplevel =
            xdg_surface_get_toplevel (win_data->xdg_surface);
        g_assert (win_data->xdg_toplevel);

        xdg_toplevel_add_listener (win_data->xdg_toplevel,
                                   &xdg_toplevel_listener, win_data);
        // TODO: xdg_toplevel_set_title (win_data.xdg_toplevel, COG_DEFAULT_APPNAME);

        const char *app_id = NULL;
        GApplication *app = g_application_get_default ();
        if (app) {
            app_id = g_application_get_application_id (app);
        }
        if (!app_id) {
            // TODO: app_id = COG_DEFAULT_APPID;
        }
        xdg_toplevel_set_app_id (win_data->xdg_toplevel, app_id);
        wl_surface_commit(win_data->wl_surface);
    } else if (display->fshell != NULL) {
        zwp_fullscreen_shell_v1_present_surface (display->fshell,
                                                 win_data->wl_surface,
                                                 ZWP_FULLSCREEN_SHELL_V1_PRESENT_METHOD_DEFAULT,
                                                 NULL);
    } else if (display->shell != NULL) {
        win_data->shell_surface = wl_shell_get_shell_surface (display->shell,
                                                             win_data->wl_surface);
        g_assert (win_data->shell_surface);

        wl_shell_surface_add_listener (win_data->shell_surface,
                                       &shell_surface_listener,
                                       win_data);
        wl_shell_surface_set_toplevel (win_data->shell_surface);

        /* wl_shell needs an initial surface configuration. */
        configure_surface_geometry (win_data, 0, 0);
    }

    const char* env_var;
    if ((env_var = g_getenv ("COG_PLATFORM_FDO_VIEW_FULLSCREEN")) &&
        g_ascii_strtoll (env_var, NULL, 10) > 0)
    {
        win_data->is_maximized = false;
        win_data->is_fullscreen = true;

        if (display->xdg_shell != NULL) {
            xdg_toplevel_set_fullscreen (win_data->xdg_toplevel, NULL);
        } else if (display->shell != NULL) {
            wl_shell_surface_set_fullscreen (win_data->shell_surface,
                                             WL_SHELL_SURFACE_FULLSCREEN_METHOD_SCALE,
                                             0,
                                             NULL);
        } else {
            g_warning ("No available shell capable of fullscreening.");
            win_data->is_fullscreen = false;
        }
    }
    else if ((env_var = g_getenv ("COG_PLATFORM_FDO_VIEW_MAXIMIZE")) &&
             g_ascii_strtoll (env_var, NULL, 10) > 0)
    {
        win_data->is_maximized = true;
        win_data->is_fullscreen = false;

        if (display->xdg_shell != NULL) {
            xdg_toplevel_set_maximized (win_data->xdg_toplevel);
        } else if (display->shell != NULL) {
            wl_shell_surface_set_maximized (win_data->shell_surface, NULL);
        } else {
            g_warning ("No available shell capable of maximizing.");
            win_data->is_maximized = false;
        }
    }

    return TRUE;
}

void destroy_window (PwlDisplay *display, PwlWinData *win_data)
{
    if (display->egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent (display->egl_display,
                        EGL_NO_SURFACE,
                        EGL_NO_SURFACE,
                        EGL_NO_CONTEXT);

        if (win_data->egl_surface) {
            eglDestroySurface (display->egl_display, win_data->egl_surface);
            win_data->egl_surface = EGL_NO_DISPLAY;
        }
    }

    g_clear_pointer (&(win_data->egl_window), wl_egl_window_destroy);
    g_clear_pointer (&(win_data->xdg_toplevel), xdg_toplevel_destroy);
    g_clear_pointer (&(win_data->xdg_surface), xdg_surface_destroy);
    g_clear_pointer (&(win_data->shell_surface), wl_shell_surface_destroy);
    g_clear_pointer (&(win_data->wl_surface), wl_surface_destroy);
}


/* Pointer */

static void
pointer_on_enter (void* data,
                  struct wl_pointer* pointer,
                  uint32_t serial,
                  struct wl_surface* surface,
                  wl_fixed_t fixed_x,
                  wl_fixed_t fixed_y)
{
}

static void
pointer_on_leave (void* data,
                  struct wl_pointer *pointer,
                  uint32_t serial,
                  struct wl_surface* surface)
{
}

static void
pointer_on_motion (void* data,
                   struct wl_pointer *pointer,
                   uint32_t time,
                   wl_fixed_t fixed_x,
                   wl_fixed_t fixed_y)
{
    PwlDisplay *display = data;
    display->pointer.time = time;
    display->pointer.x = wl_fixed_to_int (fixed_x);
    display->pointer.y = wl_fixed_to_int (fixed_y);
    if (display->on_pointer_on_motion) {
        display->on_pointer_on_motion (display, display->on_pointer_on_motion_userdata);
    }
}

static void
pointer_on_button (void* data,
                   struct wl_pointer *pointer,
                   uint32_t serial,
                   uint32_t time,
                   uint32_t button,
                   uint32_t state)
{
    PwlDisplay *display = data;

    /* @FIXME: what is this for?
    if (button >= BTN_MOUSE)
        button = button - BTN_MOUSE + 1;
    else
        button = 0;
    */

    display->pointer.button = !!state ? button : 0;
    display->pointer.state = state;
    display->pointer.time = time;

    if (display->on_pointer_on_button) {
        display->on_pointer_on_button (display, display->on_pointer_on_button_userdata);
    }
}

static void
pointer_on_axis (void* data,
                 struct wl_pointer *pointer,
                 uint32_t time,
                 uint32_t axis,
                 wl_fixed_t value)
{
    PwlDisplay *display = data;
    display->pointer.axis = axis;
    display->pointer.time = time;
    display->pointer.value = wl_fixed_to_int(value) > 0 ? -1 : 1;
    if (display->on_pointer_on_axis) {
        display->on_pointer_on_axis (display, display->on_pointer_on_axis_userdata);
    }
}
#if WAYLAND_1_10_OR_GREATER

static void
pointer_on_frame (void* data,
                  struct wl_pointer *pointer)
{
    /* @FIXME: buffer pointer events and handle them in frame. That's the
     * recommended usage of this interface.
     */
}

static void
pointer_on_axis_source (void *data,
                        struct wl_pointer *wl_pointer,
                        uint32_t axis_source)
{
}

static void
pointer_on_axis_stop (void *data,
                      struct wl_pointer *wl_pointer,
                      uint32_t time,
                      uint32_t axis)
{
}

static void
pointer_on_axis_discrete (void *data,
                          struct wl_pointer *wl_pointer,
                          uint32_t axis,
                          int32_t discrete)
{
}

#endif /* WAYLAND_1_10_OR_GREATER */


/* Touch */
static void
touch_on_down (void *data,
               struct wl_touch *touch,
               uint32_t serial,
               uint32_t time,
               struct wl_surface *surface,
               int32_t id,
               wl_fixed_t x,
               wl_fixed_t y)
{
    if (id < 0 || id >= 10)
        return;

    PwlDisplay *display = data;
    display->touch.id = id;
    display->touch.time = time;
    display->touch.x = x;
    display->touch.y = y;
    if (display->on_touch_on_down) {
        display->on_touch_on_down (display, display->on_touch_on_down_userdata);
    }
}

static void
touch_on_up (void *data,
             struct wl_touch *touch,
             uint32_t serial,
             uint32_t time,
             int32_t id)
{
    if (id < 0 || id >= 10)
        return;
    PwlDisplay *display = data;
    display->touch.id = id;
    display->touch.time = time;
    if (display->on_touch_on_up) {
        display->on_touch_on_up (display, display->on_touch_on_up_userdata);
    }
}

static void
touch_on_motion (void *data,
                 struct wl_touch *touch,
                 uint32_t time,
                 int32_t id,
                 wl_fixed_t x,
                 wl_fixed_t y)
{
    if (id < 0 || id >= 10)
        return;
    PwlDisplay *display = (PwlDisplay*) data;
    display->touch.id = id;
    display->touch.time = time;
    display->touch.x = x;
    display->touch.y = y;
    if (display->on_touch_on_motion) {
        display->on_touch_on_motion (display, display->on_touch_on_motion_userdata);
    }
}

static void
touch_on_frame (void *data, struct wl_touch *touch)
{
    /* @FIXME: buffer touch events and handle them here */
}

static void
touch_on_cancel (void *data, struct wl_touch *touch)
{
}

/* Keyboard */
void
keyboard_on_keymap (void *data,
                    struct wl_keyboard *wl_keyboard,
                    uint32_t format,
                    int32_t fd,
                    uint32_t size)
{
    PwlDisplay *display = (PwlDisplay*) data;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close (fd);
        return;
    }

    void* mapping = mmap (NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        close (fd);
        return;
    }

    display->xkb_data.keymap =
        xkb_keymap_new_from_string (display->xkb_data.context,
                                    (char *) mapping,
                                    XKB_KEYMAP_FORMAT_TEXT_V1,
                                    XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap (mapping, size);
    close (fd);

    if (display->xkb_data.keymap == NULL)
        return;

    display->xkb_data.state = xkb_state_new (display->xkb_data.keymap);
    if (display->xkb_data.state == NULL)
        return;

    display->xkb_data.indexes.control = xkb_keymap_mod_get_index (display->xkb_data.keymap,
                                                         XKB_MOD_NAME_CTRL);
    display->xkb_data.indexes.alt = xkb_keymap_mod_get_index (display->xkb_data.keymap,
                                                     XKB_MOD_NAME_ALT);
    display->xkb_data.indexes.shift = xkb_keymap_mod_get_index (display->xkb_data.keymap,
                                                       XKB_MOD_NAME_SHIFT);
}

void
keyboard_on_enter (void *data,
                   struct wl_keyboard *wl_keyboard,
                   uint32_t serial,
                   struct wl_surface *surface,
                   struct wl_array *keys)
{
    PwlDisplay *display = (PwlDisplay*) data;
    display->keyboard.serial = serial;
}

void
keyboard_on_leave (void *data,
                   struct wl_keyboard *wl_keyboard,
                   uint32_t serial,
                   struct wl_surface *surface)
{
    PwlDisplay *display = (PwlDisplay*) data;
    display->keyboard.serial = serial;
}

gboolean
repeat_delay_timeout (void *data)
{
    PwlDisplay *display = (PwlDisplay*) data;
    handle_key_event (data, display->keyboard.repeat_data.key,
                      display->keyboard.repeat_data.state,
                      display->keyboard.repeat_data.time);

    display->keyboard.repeat_data.event_source =
        g_timeout_add (display->keyboard.repeat_info.rate,
                       (GSourceFunc) repeat_delay_timeout, data);

    return G_SOURCE_REMOVE;
}

void
keyboard_on_key (void *data,
                 struct wl_keyboard *wl_keyboard,
                 uint32_t serial,
                 uint32_t time,
                 uint32_t key,
                 uint32_t state)
{
    PwlDisplay *display = (PwlDisplay*) data;
    /* @FIXME: investigate why is this necessary */
    // IDK.
    key += 8;

    display->keyboard.serial = serial;
    handle_key_event (data, key, state, time);

    if (display->keyboard.repeat_info.rate == 0)
        return;

    if (state == WL_KEYBOARD_KEY_STATE_RELEASED
        && display->keyboard.repeat_data.key == key) {
        if (display->keyboard.repeat_data.event_source)
            g_source_remove (display->keyboard.repeat_data.event_source);

        memset (&(display->keyboard.repeat_data),
                0x00,
                sizeof (display->keyboard.repeat_data));
    } else if (state == WL_KEYBOARD_KEY_STATE_PRESSED
               && xkb_keymap_key_repeats (display->xkb_data.keymap, key)) {
        if (display->keyboard.repeat_data.event_source)
            g_source_remove (display->keyboard.repeat_data.event_source);

        display->keyboard.repeat_data.key = key;
        display->keyboard.repeat_data.time = time;
        display->keyboard.repeat_data.state = state;
        display->keyboard.repeat_data.event_source =
            g_timeout_add (display->keyboard.repeat_info.delay,
                           (GSourceFunc) repeat_delay_timeout, data);
    }
}

void
keyboard_on_modifiers (void *data,
                       struct wl_keyboard *wl_keyboard,
                       uint32_t serial,
                       uint32_t mods_depressed,
                       uint32_t mods_latched,
                       uint32_t mods_locked,
                       uint32_t group)
{
    PwlDisplay *display = (PwlDisplay*) data;
    xkb_state_update_mask (display->xkb_data.state,
                           mods_depressed,
                           mods_latched,
                           mods_locked,
                           0,
                           0,
                           group);

    display->xkb_data.modifiers = 0;
    uint32_t component
        = (XKB_STATE_MODS_DEPRESSED | XKB_STATE_MODS_LATCHED);

    if (xkb_state_mod_index_is_active (display->xkb_data.state,
                                       display->xkb_data.indexes.control,
                                       component)) {
        display->xkb_data.modifiers |= display->xkb_data.modifier.control;
    }
    if (xkb_state_mod_index_is_active (display->xkb_data.state,
                                       display->xkb_data.indexes.alt,
                                       component)) {
        display->xkb_data.modifiers |= display->xkb_data.modifier.alt;
    }
    if (xkb_state_mod_index_is_active (display->xkb_data.state,
                                       display->xkb_data.indexes.shift,
                                       component)) {
        display->xkb_data.modifiers |= display->xkb_data.modifier.shift;
    }
}

void
keyboard_on_repeat_info (void *data,
                         struct wl_keyboard *wl_keyboard,
                         int32_t rate,
                         int32_t delay)
{
    PwlDisplay *display = (PwlDisplay*) data;
    display->keyboard.repeat_info.rate = rate;
    display->keyboard.repeat_info.delay = delay;

    /* a rate of zero disables any repeating. */
    if (rate == 0 && display->keyboard.repeat_data.event_source > 0) {
        g_source_remove(display->keyboard.repeat_data.event_source);
        memset (&(display->keyboard.repeat_data),
                0x00,
                sizeof (display->keyboard.repeat_data));
    }
}


static void
seat_on_capabilities (void* data, struct wl_seat* seat, uint32_t capabilities)
{
    g_debug ("Enumerating seat capabilities:");
    PwlDisplay *display = (PwlDisplay*) data;

    /* Pointer */
    static const struct wl_pointer_listener pointer_listener = {
        .enter = pointer_on_enter,
        .leave = pointer_on_leave,
        .motion = pointer_on_motion,
        .button = pointer_on_button,
        .axis = pointer_on_axis,
    #if WAYLAND_1_10_OR_GREATER
        .frame = pointer_on_frame,
        .axis_source = pointer_on_axis_source,
        .axis_stop = pointer_on_axis_stop,
        .axis_discrete = pointer_on_axis_discrete,
    #endif /* WAYLAND_1_10_OR_GREATER */
    };
    const bool has_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
    if (has_pointer && display->pointer.obj == NULL) {
        display->pointer.obj = wl_seat_get_pointer (display->seat);
        g_assert (display->pointer.obj);
        g_assert (data);
        wl_pointer_add_listener (display->pointer.obj, &pointer_listener, data);
        g_debug ("  - Pointer");
    } else if (! has_pointer && display->pointer.obj != NULL) {
        wl_pointer_release (display->pointer.obj);
        display->pointer.obj = NULL;
    }

    /* Keyboard */
    const bool has_keyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;
    if (has_keyboard && display->keyboard.obj == NULL) {
        display->keyboard.obj = wl_seat_get_keyboard (display->seat);
        g_assert (display->keyboard.obj);
        static const struct wl_keyboard_listener keyboard_listener = {
            .keymap = keyboard_on_keymap,
            .enter = keyboard_on_enter,
            .leave = keyboard_on_leave,
            .key = keyboard_on_key,
            .modifiers = keyboard_on_modifiers,
            .repeat_info = keyboard_on_repeat_info,
        };
        wl_keyboard_add_listener (display->keyboard.obj, &keyboard_listener, data);
        g_debug ("  - Keyboard");
    } else if (! has_keyboard && display->keyboard.obj != NULL) {
        wl_keyboard_release (display->keyboard.obj);
        display->keyboard.obj = NULL;
    }

    /* Touch */
    const bool has_touch = capabilities & WL_SEAT_CAPABILITY_TOUCH;
    if (has_touch && display->touch.obj == NULL) {
        display->touch.obj = wl_seat_get_touch (display->seat);
        g_assert (display->touch.obj);
        static const struct wl_touch_listener touch_listener = {
            .down = touch_on_down,
            .up = touch_on_up,
            .motion = touch_on_motion,
            .frame = touch_on_frame,
            .cancel = touch_on_cancel,
        };
        wl_touch_add_listener (display->touch.obj, &touch_listener, data);
        g_debug ("  - Touch");
    } else if (! has_touch && display->touch.obj != NULL) {
        wl_touch_release (display->touch.obj);
        display->touch.obj = NULL;
    }

    g_debug ("Done enumerating seat capabilities.");
}

static void
seat_on_name (void *data, struct wl_seat *seat, const char *name)
{
    g_debug ("Seat name: '%s'", name);
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_on_capabilities,
    .name = seat_on_name,
};


gboolean init_input (PwlDisplay *display, GError **error)
{
    if (display->seat != NULL) {
        wl_seat_add_listener (display->seat, &seat_listener, display);

        display->xkb_data.context = xkb_context_new (XKB_CONTEXT_NO_FLAGS);
        g_assert (display->xkb_data.context);
        display->xkb_data.compose_table =
            xkb_compose_table_new_from_locale (display->xkb_data.context,
                                               setlocale (LC_CTYPE, NULL),
                                               XKB_COMPOSE_COMPILE_NO_FLAGS);
        if (display->xkb_data.compose_table != NULL) {
            display->xkb_data.compose_state =
                xkb_compose_state_new (display->xkb_data.compose_table,
                                       XKB_COMPOSE_STATE_NO_FLAGS);
        }
    }

    return TRUE;
}

void clear_input (PwlDisplay* display)
{
    g_clear_pointer (&(display->pointer.obj), wl_pointer_destroy);
    g_clear_pointer (&(display->keyboard.obj), wl_keyboard_destroy);
    g_clear_pointer (&(display->seat), wl_seat_destroy);

    g_clear_pointer (&(display->xkb_data.state), xkb_state_unref);
    g_clear_pointer (&(display->xkb_data.compose_state), xkb_compose_state_unref);
    g_clear_pointer (&(display->xkb_data.compose_table), xkb_compose_table_unref);
    g_clear_pointer (&(display->xkb_data.keymap), xkb_keymap_unref);
    g_clear_pointer (&(display->xkb_data.context), xkb_context_unref);
}
