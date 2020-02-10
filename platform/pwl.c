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
#include <gio/gio.h>
#include <string.h>
#include <sys/mman.h>


#ifndef EGL_WL_create_wayland_buffer_from_image
typedef struct wl_buffer * (EGLAPIENTRYP PFNEGLCREATEWAYLANDBUFFERFROMIMAGEWL) (EGLDisplay dpy, EGLImageKHR image);
#endif


G_DEFINE_QUARK (com_igalia_Cog_PwlError, pwl_error)


#if HAVE_DEVICE_SCALING
typedef struct {
    struct wl_output *output;
    int32_t output_name;
    int32_t device_scale;
    struct wl_list link;
} PwlOutput;
#endif /* HAVE_DEVICE_SCALING */


struct _PwlDisplay {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_seat *seat;

    struct egl_display *egl_display;
    EGLContext egl_context;
    EGLConfig egl_config;
    PFNEGLCREATEWAYLANDBUFFERFROMIMAGEWL egl_createWaylandBufferFromImageWL;

    struct wl_pointer *pointer;
    PwlWindow *pointer_target;  /* Current target of pointer events. */

    PwlKeyboard keyboard;
    PwlTouch touch;

    PwlXKBData xkb_data;

    struct xdg_wm_base *xdg_shell;
    struct zwp_fullscreen_shell_v1 *fshell;
    struct wl_shell *shell;

#if HAVE_DEVICE_SCALING
    struct wl_list outputs; /* wl_list<PwlOutput> */
#endif /* HAVE_DEVICE_SCALING */

    char *application_id;
    char *window_title;
    GSource *event_src;

    void (*on_touch_on_down)     (PwlDisplay*, const PwlTouch*, void *userdata);
    void *on_touch_on_down_userdata;
    void (*on_touch_on_up)       (PwlDisplay*, const PwlTouch*, void *userdata);
    void *on_touch_on_up_userdata;
    void (*on_touch_on_motion)   (PwlDisplay*, const PwlTouch*, void *userdata);
    void *on_touch_on_motion_userdata;

    void (*on_key_event) (PwlDisplay*, const PwlKeyboard*, void *userdata);
    void *on_key_event_userdata;
    bool (*on_capture_app_key) (PwlDisplay*, void *userdata);
    void *on_capture_app_key_userdata;
};


struct _PwlWindow {
    PwlDisplay *display;

    struct wl_surface *wl_surface;
    struct wl_surface_listener surface_listener;
    struct wl_egl_window *egl_window;
    EGLSurface egl_surface;

    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    struct wl_shell_surface *shell_surface;

    uint32_t width;
    uint32_t height;

#if HAVE_DEVICE_SCALING
    uint32_t device_scale;
#endif /* HAVE_DEVICE_SCALING */

    PwlPointer pointer;

    /*
     * XXX: Should this be en enum? Can a window be maximized *and*
     *      fullscreened at the same time?
     */
    bool is_fullscreen;
    bool is_maximized;

    void (*on_window_resize) (PwlWindow*,
                              uint32_t width,
                              uint32_t height,
                              void *userdata);
    void *on_window_resize_userdata;

    void (*device_scale_callback) (PwlWindow*, uint32_t scale, void* userdata);
    void *device_scale_userdata;

    void (*on_pointer_motion) (PwlWindow*, const PwlPointer*, void *userdata);
    void *on_pointer_motion_userdata;
    void (*on_pointer_button) (PwlWindow*, const PwlPointer*, void *userdata);
    void *on_pointer_button_userdata;
    void (*on_pointer_axis) (PwlWindow*, const PwlPointer*, void *userdata);
    void *on_pointer_axis_userdata;
};


struct pwl_event_source {
    GSource source;
    GPollFD pfd;
    struct wl_display* display;
};


#if HAVE_DEVICE_SCALING
static void
noop()
{
}

static void
display_output_on_scale (void *data,
                         struct wl_output *output,
                         int32_t factor)
{
    PwlDisplay *self = data;
    bool found = false;

    PwlOutput *item;
    wl_list_for_each (item, &self->outputs, link) {
        if (item->output == output) {
            found = true;
            break;
        }
    }

    if (!found) {
        g_critical ("Unknown output %p\n", output);
        return;
    }

    if (item->device_scale == factor) {
        g_debug ("Output #%"PRIi32 " @ %p: Scaling factor %"PRIi32"x unchanged",
                 item->output_name, output, factor);
    } else {
        /* TODO: Notify scale changes to windows in this output.  */
        g_debug ("Output #%"PRIi32" @ %p: New scaling factor %"PRIi32"x",
                 item->output_name, item->output, factor);
        item->device_scale = factor;
    }
}


static void
window_surface_on_enter (void *data, struct wl_surface *surface, struct wl_output *output)
{
    PwlWindow *self = data;

    g_assert (self->wl_surface == surface);

    int32_t factor = -1;
    PwlOutput *item;
    wl_list_for_each (item, &self->display->outputs, link) {
        if (item->output == output) {
            factor = item->device_scale;
            break;
        }
    }
    g_debug ("Surface entered output %p with scale factor %i\n", output, factor);

    if (factor < 1) {
        g_warning ("No scale factor available for output %p\n", output);
        return;
    }

    if (factor == self->device_scale) {
        g_debug ("Device scaling factor unchanged for window %p.", self);
        return;
    }

    wl_surface_set_buffer_scale (surface, factor);

    if (self->device_scale_callback) {
        (*self->device_scale_callback) (self, (uint32_t) factor,
                                        self->device_scale_userdata);
    }

    self->device_scale = (uint32_t) factor;
}
#endif /* HAVE_DEVICE_SCALING */


static void
xdg_shell_ping (void *data, struct xdg_wm_base *shell, uint32_t serial)
{
    xdg_wm_base_pong (shell, serial);
}


static void
registry_global (void               *data,
                 struct wl_registry *registry,
                 uint32_t            name,
                 const char         *interface,
                 uint32_t            version)
{
    PwlDisplay *display = data;
    gboolean interface_used = TRUE;

    if (strcmp (interface, wl_compositor_interface.name) == 0) {
        display->compositor = wl_registry_bind (registry,
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
        static const struct xdg_wm_base_listener xdg_shell_listener = {
            .ping = xdg_shell_ping,
        };
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
        PwlOutput *item = g_slice_new0 (PwlOutput);
        item->output = wl_registry_bind (registry, name,
                                         &wl_output_interface,
                                         version);
        item->output_name = name;
        item->device_scale = 1;
        wl_list_init (&item->link);
        wl_list_insert (&display->outputs, &item->link);

        static const struct wl_output_listener output_listener = {
            .geometry = noop,
            .mode = noop,
            .done = noop,
            .scale = display_output_on_scale,
        };
        wl_output_add_listener (item->output, &output_listener, display);

        g_debug ("Output #%"PRIi32" @ %p: Added with scaling factor 1x",
                 item->output_name, item->output);
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
#if HAVE_DEVICE_SCALING
    PwlDisplay *display = data;
    PwlOutput *item, *tmp;
    wl_list_for_each_safe (item, tmp, &display->outputs, link) {
        if (item->output_name == name) {
            g_debug ("Output #%"PRIi32" @ %p: Removed.",
                     item->output_name, item->output);
            g_clear_pointer (&item->output, wl_output_release);
            wl_list_remove (&item->link);
            g_slice_free (PwlOutput, item);
            break;
        }
    }
#endif /* HAVE_DEVICE_SCALING */
}



PwlDisplay*
pwl_display_connect (const char *name, GError **error)
{
    g_autoptr(PwlDisplay) self = g_slice_new0 (PwlDisplay);

#if HAVE_DEVICE_SCALING
    wl_list_init (&self->outputs);
#endif /* HAVE_DEVICE_SCALING */

    if (!(self->display = wl_display_connect (name))) {
        g_set_error_literal (error, G_FILE_ERROR,
                             g_file_error_from_errno (errno),
                             "Could not open Wayland display");
        return NULL;
    }

    self->registry = wl_display_get_registry (self->display);
    static const struct wl_registry_listener registry_listener = {
        .global = registry_global,
        .global_remove = registry_global_remove
    };
    wl_registry_add_listener (self->registry, &registry_listener, self);
    wl_display_roundtrip (self->display);

    if (!self->compositor) {
        g_set_error (error, PWL_ERROR, PWL_ERROR_WAYLAND,
                     "Wayland display does not support %s",
                     wl_compositor_interface.name);
        return NULL;
    }

    if (!self->xdg_shell && !self->shell && !self->fshell) {
        g_set_error (error, PWL_ERROR, PWL_ERROR_WAYLAND,
                     "Wayland display does not support a shell "
                     "interface (%s, %s, or %s)",
                     xdg_wm_base_interface.name,
                     wl_shell_interface.name,
                     zwp_fullscreen_shell_v1_interface.name);
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
        g_clear_pointer (&self->application_id, g_free);
        g_clear_pointer (&self->window_title, g_free);
    }

    g_slice_free (PwlDisplay, self);
}


void
pwl_display_set_default_application_id (PwlDisplay *self,
                                        const char *application_id)
{
    g_return_if_fail (self);
    g_return_if_fail (application_id);
    g_return_if_fail (*application_id != '\0');

    g_clear_pointer (&self->application_id, g_free);
    self->application_id = g_strdup (application_id);
}


void
pwl_display_set_default_window_title (PwlDisplay *self,
                                      const char *title)
{
    g_return_if_fail (self);
    g_return_if_fail (title);
    g_return_if_fail (*title != '\0');

    g_clear_pointer (&self->window_title, g_free);
    self->window_title = g_strdup (title);
}


void
pwl_window_notify_pointer_motion (PwlWindow *self,
                                  void (*callback) (PwlWindow*, const PwlPointer*, void*),
                                  void *userdata)
{
    g_return_if_fail (self);

    self->on_pointer_motion = callback;
    self->on_pointer_motion_userdata = userdata;
}


void
pwl_window_notify_pointer_button (PwlWindow *self,
                                  void (*callback) (PwlWindow*, const PwlPointer*, void*),
                                  void *userdata)
{
    g_return_if_fail (self);

    self->on_pointer_button = callback;
    self->on_pointer_button_userdata = userdata;
}


void
pwl_window_notify_pointer_axis (PwlWindow *self,
                                void (*callback) (PwlWindow*, const PwlPointer*, void*),
                                void *userdata)
{
    g_return_if_fail (self);

    self->on_pointer_axis = callback;
    self->on_pointer_axis_userdata = userdata;
}


void
pwl_display_notify_touch_down (PwlDisplay *self,
                               void (*callback) (PwlDisplay*, const PwlTouch*, void*),
                               void *userdata)
{
    g_return_if_fail (self);

    self->on_touch_on_down = callback;
    self->on_touch_on_down_userdata = userdata;
}


void
pwl_display_notify_touch_up (PwlDisplay *self,
                             void (*callback) (PwlDisplay*, const PwlTouch*, void*),
                             void *userdata)
{
    g_return_if_fail (self);

    self->on_touch_on_up = callback;
    self->on_touch_on_up_userdata = userdata;
}


void
pwl_display_notify_touch_motion (PwlDisplay *self,
                                 void (*callback) (PwlDisplay*, const PwlTouch*, void*),
                                 void *userdata)
{
    g_return_if_fail (self);

    self->on_touch_on_motion = callback;
    self->on_touch_on_motion_userdata = userdata;
}


void
pwl_display_notify_key_event (PwlDisplay *self,
                              void (*callback) (PwlDisplay*, const PwlKeyboard*, void*),
                              void *userdata)
{
    g_return_if_fail (self);

    self->on_key_event = callback;
    self->on_key_event_userdata = userdata;
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


void
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
    g_source_attach (&wl_source->source, main_context);

    g_source_unref (&wl_source->source);

    display->event_src = &wl_source->source;
}


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
        (*display->on_key_event) (display,
                                  &display->keyboard,
                                  display->on_key_event_userdata);
    }
}

static void
resize_window (PwlWindow *window)
{
    const uint32_t device_scale = pwl_window_get_device_scale (window);

    int32_t pixel_width = window->width * device_scale;
    int32_t pixel_height = window->height * device_scale;

    if (window->egl_window) {
        wl_egl_window_resize (window->egl_window,
                              pixel_width,
                              pixel_height,
                              0, 0);
    }
    g_debug ("Resized EGL buffer to: (%"PRIi32", %"PRIi32") @%"PRIu32"x\n",
             pixel_width, pixel_height, device_scale);

    if (window->on_window_resize) {
        (*window->on_window_resize) (window,
                                     window->width,
                                     window->height,
                                     window->on_window_resize_userdata);
    }
}


static void
configure_surface_geometry (PwlWindow *window, int32_t width, int32_t height)
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

    window->width = width;
    window->height = height;
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
    PwlWindow *window = data;
    configure_surface_geometry (window, width, height);

    g_debug ("New wl_shell configuration: (%" PRIu32 ", %" PRIu32 ")", width, height);

    resize_window (window);
}


static void
set_egl_error (GError **error, EGLint code, const char *message)
{
    g_assert (message);

    const char *code_string = NULL;
    switch (code) {
#define ERRCASE(v) case EGL_ ## v: code_string = #v; break
        ERRCASE (SUCCESS);
        ERRCASE (NOT_INITIALIZED);
        ERRCASE (BAD_ACCESS);
        ERRCASE (BAD_ALLOC);
        ERRCASE (BAD_ATTRIBUTE);
        ERRCASE (BAD_CONTEXT);
        ERRCASE (BAD_CONFIG);
        ERRCASE (BAD_CURRENT_SURFACE);
        ERRCASE (BAD_DISPLAY);
        ERRCASE (BAD_SURFACE);
        ERRCASE (BAD_MATCH);
        ERRCASE (BAD_PARAMETER);
        ERRCASE (BAD_NATIVE_PIXMAP);
        ERRCASE (BAD_NATIVE_WINDOW);
        ERRCASE (CONTEXT_LOST);
#undef ERRCASE
        default:
            code_string = "UNKNOWN";
    }

    g_set_error (error, PWL_ERROR, PWL_ERROR_EGL,
                 "EGL: %s - #%06x %s", message, code, code_string);
}


gboolean
pwl_display_egl_init (PwlDisplay *display, GError **error)
{
    g_debug ("Initializing EGL...");

    display->egl_display = eglGetDisplay ((EGLNativeDisplayType) display->display);
    if (display->egl_display == EGL_NO_DISPLAY) {
        set_egl_error (error, eglGetError (), "Could not get display");
        return FALSE;
    }

    EGLint major, minor;
    if (!eglInitialize (display->egl_display, &major, &minor)) {
        set_egl_error (error, eglGetError (), "Initialization failed");
        pwl_display_egl_deinit (display);
        return FALSE;
    }
    g_info ("EGL version %d.%d initialized.", major, minor);

    /*
     * TODO: Use eglQueryString() to find out whether required extensions are
     *       available before trying to actually use functions obtained below
     *       with eglGetProcAddress().
     */

    display->egl_createWaylandBufferFromImageWL = (PFNEGLCREATEWAYLANDBUFFERFROMIMAGEWL)
        eglGetProcAddress ("eglCreateWaylandBufferFromImageWL");
    if (!display->egl_createWaylandBufferFromImageWL) {
        set_egl_error (error, eglGetError (),
                       "eglCreateWaylandBufferFromImageWL unsupported");
        pwl_display_egl_deinit (display);
        return FALSE;
    }

    if (!eglBindAPI (EGL_OPENGL_ES_API)) {
        set_egl_error (error, eglGetError (), "Could not bind OpenEGL ES API");
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
        set_egl_error (error, eglGetError (),
                       "Could not find a suitable configuration");
        pwl_display_egl_deinit (display);
        return FALSE;
    }
    g_assert (num_configs > 0);

    display->egl_context = eglCreateContext (display->egl_display,
                                         display->egl_config,
                                         EGL_NO_CONTEXT,
                                         context_attribs);
    if (display->egl_context == EGL_NO_CONTEXT) {
        set_egl_error (error, eglGetError (), "Could not create context");
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


EGLDisplay
pwl_display_egl_get_display (const PwlDisplay *self)
{
    g_return_val_if_fail (self, EGL_NO_DISPLAY);
    return self->egl_display;
}


struct wl_buffer*
pwl_display_egl_create_buffer_from_image (const PwlDisplay *self,
                                          EGLImage image)
{
    g_return_val_if_fail (self, NULL);
    g_return_val_if_fail (image != EGL_NO_IMAGE, NULL);
    g_assert (self->egl_createWaylandBufferFromImageWL);

    return (*self->egl_createWaylandBufferFromImageWL) (self->egl_display, image);
}


PwlXKBData*
pwl_display_xkb_get_data (PwlDisplay *self)
{
    g_return_val_if_fail (self, NULL);
    return &self->xkb_data;
}


static void
xdg_surface_on_configure (void *data,
                          struct xdg_surface *surface,
                          uint32_t serial)
{
    xdg_surface_ack_configure (surface, serial);
}

static void
xdg_toplevel_on_configure (void *data,
                           struct xdg_toplevel *toplevel,
                           int32_t width, int32_t height,
                           struct wl_array *states)
{
    PwlWindow *window = data;
    configure_surface_geometry (window, width, height);

    g_debug ("New XDG toplevel configuration: (%" PRIu32 ", %" PRIu32 ")", width, height);

    resize_window (data);
}

static void
xdg_toplevel_on_close (void *data, struct xdg_toplevel *xdg_toplevel)
{
    // TODO g_application_quit (g_application_get_default ());
}

/*
 * TODO: Library code must not use environment variables. This needs to be
 *       moved to cog.c, adding methods in CogShell which get overriden in
 *       CogFdoShell, and that in turn calls the appropriate platform
 *       functions.
 */
static void
create_window (PwlDisplay* display, PwlWindow* window)
{
    const char* env_var;
    if ((env_var = g_getenv ("COG_PLATFORM_FDO_VIEW_FULLSCREEN")) &&
        g_ascii_strtoll (env_var, NULL, 10) > 0)
    {
        pwl_window_set_fullscreen (window, true);
    }
    else if ((env_var = g_getenv ("COG_PLATFORM_FDO_VIEW_MAXIMIZE")) &&
             g_ascii_strtoll (env_var, NULL, 10) > 0)
    {
        pwl_window_set_maximized (window, true);
    }
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
    PwlDisplay *display = data;
    if (pointer != display->pointer) {
        g_critical ("%s: Got pointer %p, expected %p.",
                    G_STRFUNC, pointer, display->pointer);
        return;
    }

    PwlWindow *window = wl_surface_get_user_data (surface);
    display->pointer_target = window;

    g_debug ("%s: Target surface %p, window %p.", G_STRFUNC, surface, window);
}

static void
pointer_on_leave (void* data,
                  struct wl_pointer *pointer,
                  uint32_t serial,
                  struct wl_surface* surface)
{
    PwlDisplay *display = data;
    if (pointer != display->pointer) {
        g_critical ("%s: Got pointer %p, expected %p.",
                    G_STRFUNC, pointer, display->pointer);
        return;
    }

    if (!display->pointer_target) {
        g_critical ("%s: No current pointer event target!", G_STRFUNC);
        return;
    }

    if (surface != display->pointer_target->wl_surface) {
        g_critical ("%s: Got leave for surface %p, but current window %p "
                    "has surface %p.", G_STRFUNC, surface,
                    display->pointer_target,
                    display->pointer_target->wl_surface);
    }

    display->pointer_target = NULL;
}

static void
pointer_on_motion (void* data,
                   struct wl_pointer *pointer,
                   uint32_t time,
                   wl_fixed_t fixed_x,
                   wl_fixed_t fixed_y)
{
    PwlDisplay *display = data;
    if (pointer != display->pointer) {
        g_critical ("%s: Got pointer %p, expected %p.",
                    G_STRFUNC, pointer, display->pointer);
        return;
    }

    PwlWindow *window = display->pointer_target;
    if (!window) {
        g_critical ("%s: No current pointer event target!", G_STRFUNC);
        return;
    }

    window->pointer.time = time;
    window->pointer.x = wl_fixed_to_int (fixed_x);
    window->pointer.y = wl_fixed_to_int (fixed_y);

    if (window->on_pointer_motion) {
        (*window->on_pointer_motion) (window,
                                      &window->pointer,
                                      window->on_pointer_motion_userdata);
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
    if (pointer != display->pointer) {
        g_critical ("%s: Got pointer %p, expected %p.",
                    G_STRFUNC, pointer, display->pointer);
        return;
    }

    PwlWindow *window = display->pointer_target;
    if (!window) {
        g_critical ("%s: No current pointer event target!", G_STRFUNC);
        return;
    }

    /* @FIXME: what is this for?
    if (button >= BTN_MOUSE)
        button = button - BTN_MOUSE + 1;
    else
        button = 0;
    */

    window->pointer.button = !!state ? button : 0;
    window->pointer.state = state;
    window->pointer.time = time;

    if (window->on_pointer_button) {
        (*window->on_pointer_button) (window,
                                      &window->pointer,
                                      window->on_pointer_button_userdata);
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
    if (pointer != display->pointer) {
        g_critical ("%s: Got pointer %p, expected %p.",
                    G_STRFUNC, pointer, display->pointer);
        return;
    }

    PwlWindow *window = display->pointer_target;
    if (!window) {
        g_critical ("%s: No current pointer event target!", G_STRFUNC);
        return;
    }

    window->pointer.axis = axis;
    window->pointer.time = time;
    window->pointer.value = wl_fixed_to_int(value) > 0 ? -1 : 1;

    if (window->on_pointer_axis) {
        (*window->on_pointer_axis) (window,
                                    &window->pointer,
                                    window->on_pointer_axis_userdata);
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
        (*display->on_touch_on_down) (display,
                                      &display->touch,
                                      display->on_touch_on_down_userdata);
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
        (*display->on_touch_on_up) (display,
                                    &display->touch,
                                    display->on_touch_on_up_userdata);
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
    PwlDisplay *display = data;
    display->touch.id = id;
    display->touch.time = time;
    display->touch.x = x;
    display->touch.y = y;
    if (display->on_touch_on_motion) {
        (*display->on_touch_on_motion) (display,
                                        &display->touch,
                                        display->on_touch_on_motion_userdata);
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
    PwlDisplay *display = data;

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
    PwlDisplay *display = data;
    display->keyboard.serial = serial;
}

void
keyboard_on_leave (void *data,
                   struct wl_keyboard *wl_keyboard,
                   uint32_t serial,
                   struct wl_surface *surface)
{
    PwlDisplay *display = data;
    display->keyboard.serial = serial;
}

gboolean
repeat_delay_timeout (void *data)
{
    PwlDisplay *display = data;
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
    PwlDisplay *display = data;
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
    PwlDisplay *display = data;
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
    PwlDisplay *display = data;
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
    PwlDisplay *display = data;
    g_assert (display);

    if (seat != display->seat) {
        g_critical ("%s: Multiple seats are not supported.", G_STRFUNC);
        return;
    }

    g_debug ("Enumerating seat capabilities:");


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

    if (capabilities & WL_SEAT_CAPABILITY_POINTER) {
        if (!display->pointer) {
            display->pointer = wl_seat_get_pointer (display->seat);
            wl_pointer_add_listener (display->pointer, &pointer_listener, display);
            g_debug ("  - New pointer %p.", display->pointer);
        } else {
            g_warning ("Multiple pointers per seat are currently unsupported.");
        }
    } else if (display->pointer) {
        g_debug ("  - Pointer %p gone.", display->pointer);
        g_clear_pointer (&display->pointer, wl_pointer_release);
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


PwlWindow*
pwl_window_create (PwlDisplay *display)
{
    g_return_val_if_fail (display, NULL);

    g_autoptr(PwlWindow) self = g_slice_new0 (PwlWindow);
    self->display = display;
    self->egl_surface = EGL_NO_SURFACE,
    self->width = DEFAULT_WIDTH,
    self->height = DEFAULT_HEIGHT,
    self->wl_surface = wl_compositor_create_surface (display->compositor);
    wl_surface_set_user_data (self->wl_surface, self);

#if HAVE_DEVICE_SCALING
    self->device_scale = 1;

    static const struct wl_surface_listener surface_listener = {
        .enter = window_surface_on_enter,
        .leave = noop,
    };
    wl_surface_add_listener (self->wl_surface, &surface_listener, self);
#endif /* HAVE_DEVICE_SCALING */

    if (display->xdg_shell) {
        self->xdg_surface = xdg_wm_base_get_xdg_surface (display->xdg_shell,
                                                         self->wl_surface);
        static const struct xdg_surface_listener xdg_surface_listener = {
            .configure = xdg_surface_on_configure,
        };
        xdg_surface_add_listener (self->xdg_surface,
                                  &xdg_surface_listener,
                                  NULL);

        self->xdg_toplevel = xdg_surface_get_toplevel (self->xdg_surface);
        static const struct xdg_toplevel_listener xdg_toplevel_listener = {
            .configure = xdg_toplevel_on_configure,
            .close = xdg_toplevel_on_close,
        };
        xdg_toplevel_add_listener (self->xdg_toplevel,
                                   &xdg_toplevel_listener,
                                   self);
    } else if (display->fshell) {
        zwp_fullscreen_shell_v1_present_surface (display->fshell,
                                                 self->wl_surface,
                                                 ZWP_FULLSCREEN_SHELL_V1_PRESENT_METHOD_DEFAULT,
                                                 NULL);
        self->is_fullscreen = true;
    } else if (display->shell) {
        self->shell_surface = wl_shell_get_shell_surface (display->shell,
                                                          self->wl_surface);
        static const struct wl_shell_surface_listener shell_surface_listener = {
            .ping = shell_surface_ping,
            .configure = shell_surface_configure,
        };
        wl_shell_surface_add_listener (self->shell_surface,
                                       &shell_surface_listener,
                                       self);
        wl_shell_surface_set_toplevel (self->shell_surface);

        /* wl_shell needs an initial surface configuration. */
        configure_surface_geometry (self, 0, 0);
    }

    /* Apply default title/appid, if configured. */
    if (display->window_title) {
        pwl_window_set_title (self, display->window_title);
    }
    if (display->application_id) {
        pwl_window_set_application_id (self, display->application_id);
    }
    wl_surface_commit(self->wl_surface);

    /*
     * TODO: Remove this call to create_window() and instead have proper
     *       methods to set maximization and fullscreen states.
     */
    create_window (display, self);

    return g_steal_pointer (&self);
}


void
pwl_window_destroy (PwlWindow *self)
{
    g_return_if_fail (self);

    if (self->display->egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent (self->display->egl_display,
                        EGL_NO_SURFACE,
                        EGL_NO_SURFACE,
                        EGL_NO_CONTEXT);

        if (self->egl_surface != EGL_NO_SURFACE) {
            eglDestroySurface (self->display->egl_display, self->egl_surface);
            self->egl_surface = EGL_NO_DISPLAY;
        }
    }

    g_clear_pointer (&self->egl_window, wl_egl_window_destroy);
    g_clear_pointer (&self->xdg_toplevel, xdg_toplevel_destroy);
    g_clear_pointer (&self->xdg_surface, xdg_surface_destroy);
    g_clear_pointer (&self->shell_surface, wl_shell_surface_destroy);
    g_clear_pointer (&self->wl_surface, wl_surface_destroy);

    g_slice_free (PwlWindow, self);
}


void
pwl_window_set_application_id (PwlWindow *self, const char *application_id)
{
    g_return_if_fail (self);
    g_return_if_fail (application_id);
    g_return_if_fail (*application_id != '\0');

    if (self->display->xdg_shell) {
        xdg_toplevel_set_app_id (self->xdg_toplevel, application_id);
    } else if (self->display->shell) {
        wl_shell_surface_set_class (self->shell_surface, application_id);
    }
}


void
pwl_window_set_title (PwlWindow *self, const char *title)
{
    g_return_if_fail (self);
    g_return_if_fail (title);
    g_return_if_fail (*title != '\0');

    if (self->display->xdg_shell) {
        xdg_toplevel_set_title (self->xdg_toplevel, title);
    } else if (self->display->shell) {
        wl_shell_surface_set_title (self->shell_surface, title);
    }
}


struct wl_surface*
pwl_window_get_surface (const PwlWindow *self)
{
    g_return_val_if_fail (self, NULL);
    return self->wl_surface;
}


void
pwl_window_get_size (const PwlWindow *self, uint32_t *w, uint32_t *h)
{
    g_return_if_fail (self);
    g_return_if_fail (w || h);

    if (w) *w = self->width;
    if (h) *h = self->height;
}


uint32_t
pwl_window_get_device_scale (const PwlWindow *self)
{
    g_return_val_if_fail (self, 1);

#if HAVE_DEVICE_SCALING
    return self->device_scale;
#else
    return 1;
#endif /* HAVE_DEVICE_SCALING */
}


bool
pwl_window_is_fullscreen (const PwlWindow *self)
{
    g_return_val_if_fail (self, false);
    return self->is_fullscreen;
}


void
pwl_window_set_fullscreen (PwlWindow *self, bool fullscreen)
{
    g_return_if_fail (self);

    /* Return early if already in the desired state. */
    if (self->is_fullscreen == fullscreen)
        return;

    if (self->display->xdg_shell) {
        if ((self->is_fullscreen = fullscreen)) {
            xdg_toplevel_set_fullscreen (self->xdg_toplevel, NULL);
        } else {
            xdg_toplevel_unset_fullscreen (self->xdg_toplevel);
        }
    } else if (self->display->shell) {
        if ((self->is_fullscreen = fullscreen)) {
            wl_shell_surface_set_fullscreen (self->shell_surface,
                                             WL_SHELL_SURFACE_FULLSCREEN_METHOD_SCALE,
                                             0, NULL);
        } else {
            wl_shell_surface_set_toplevel (self->shell_surface);
        }
    } else {
        g_warning ("No available shell capable of setting fullscreen status.");
    }
}


bool
pwl_window_is_maximized (const PwlWindow *self)
{
    g_return_val_if_fail (self, false);
    return self->is_maximized;
}


void
pwl_window_set_maximized (PwlWindow *self, bool maximized)
{
    g_return_if_fail (self);

    /* Return early if already in the desired state. */
    if (self->is_maximized == maximized)
        return;

    if (self->display->xdg_shell) {
        if ((self->is_maximized = maximized)) {
            xdg_toplevel_set_maximized (self->xdg_toplevel);
        } else {
            xdg_toplevel_unset_maximized (self->xdg_toplevel);
        }
    } else if (self->display->shell) {
        if ((self->is_maximized = maximized)) {
            wl_shell_surface_set_maximized (self->shell_surface, NULL);
        } else {
            wl_shell_surface_set_toplevel (self->shell_surface);
        }
    } else {
        g_warning ("No available shell capable of setting maximization status.");
    }
}


void
pwl_window_set_opaque_region (const PwlWindow *self,
                              uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    g_return_if_fail (self);

    struct wl_region *region = wl_compositor_create_region (self->display->compositor);
    wl_region_add (region, x, y, w, h);
    wl_surface_set_opaque_region (self->wl_surface, region);
    wl_region_destroy (region);
}


void
pwl_window_unset_opaque_region (const PwlWindow *self)
{
    g_return_if_fail (self);
    wl_surface_set_opaque_region (self->wl_surface, NULL);
}


void
pwl_window_notify_resize (PwlWindow *self,
                          void (*callback) (PwlWindow*, uint32_t w, uint32_t h, void*),
                          void *userdata)
{
    g_return_if_fail (self);

    self->on_window_resize = callback;
    self->on_window_resize_userdata = userdata;
}


void
pwl_window_notify_device_scale (PwlWindow *self,
                                void (*callback) (PwlWindow*, uint32_t scale, void*),
                                void *userdata)
{
    g_return_if_fail (self);

    self->device_scale_callback = callback;
    self->device_scale_userdata = userdata;
}


bool
pwl_display_input_init (PwlDisplay *self, GError **error)
{
    /* TODO: Return "false" if input is not available or initialization fails. */

    if (self->seat != NULL) {
        wl_seat_add_listener (self->seat, &seat_listener, self);

        self->xkb_data.context = xkb_context_new (XKB_CONTEXT_NO_FLAGS);
        g_assert (self->xkb_data.context);
        self->xkb_data.compose_table =
            xkb_compose_table_new_from_locale (self->xkb_data.context,
                                               setlocale (LC_CTYPE, NULL),
                                               XKB_COMPOSE_COMPILE_NO_FLAGS);
        if (self->xkb_data.compose_table != NULL) {
            self->xkb_data.compose_state =
                xkb_compose_state_new (self->xkb_data.compose_table,
                                       XKB_COMPOSE_STATE_NO_FLAGS);
        }
    }

    return true;
}


void
pwl_display_input_deinit (PwlDisplay* self)
{
    g_clear_pointer (&self->pointer, wl_pointer_destroy);
    g_clear_pointer (&self->keyboard.obj, wl_keyboard_destroy);
    g_clear_pointer (&self->seat, wl_seat_destroy);

    g_clear_pointer (&self->xkb_data.state, xkb_state_unref);
    g_clear_pointer (&self->xkb_data.compose_state, xkb_compose_state_unref);
    g_clear_pointer (&self->xkb_data.compose_table, xkb_compose_table_unref);
    g_clear_pointer (&self->xkb_data.keymap, xkb_keymap_unref);
    g_clear_pointer (&self->xkb_data.context, xkb_context_unref);
}
