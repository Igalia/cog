/*
 * pwl.c
 * Copyright (C) 2019 Adrian Perez de Castro <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "pwl.h"

#include <errno.h>

/* for mmap */
#include <sys/mman.h>

#include <gio/gio.h>

// TODO: #if defined(WPE_CHECK_VERSION) && WPE_CHECK_VERSION(1, 3, 0)
#if 1
# define HAVE_DEVICE_SCALING 1
#else
# define HAVE_DEVICE_SCALING 0
#endif /* WPE_CHECK_VERSION */


PwlData wl_data = {
    .current_output.scale = 1,
};

PwlEGLData egl_data;

PwlWinData win_data = {
    .egl_surface = EGL_NO_SURFACE,
    .width = DEFAULT_WIDTH,
    .height = DEFAULT_HEIGHT,
    .is_fullscreen = false,
    .is_maximized = false,
};

PwlXKBData xkb_data = {NULL, };


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
                            struct wl_display *display)
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
    wl_source->display = display;
    wl_source->pfd.fd = wl_display_get_fd (display);
    wl_source->pfd.events = G_IO_IN | G_IO_ERR | G_IO_HUP;
    wl_source->pfd.revents = 0;
    g_source_add_poll (&wl_source->source, &wl_source->pfd);

    g_source_set_can_recurse (&wl_source->source, TRUE);
    g_source_attach (&wl_source->source, g_main_context_get_thread_default());

    g_source_unref (&wl_source->source);

    return &wl_source->source;
}

static void
configure_surface_geometry (int32_t width, int32_t height)
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

    win_data.width = width;
    win_data.height = height;
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
    configure_surface_geometry (width, height);

    g_debug ("New wl_shell configuration: (%" PRIu32 ", %" PRIu32 ")", width, height);

    wl_data.resize_window (data);
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
    gboolean interface_used = TRUE;

    if (strcmp (interface, wl_compositor_interface.name) == 0) {
        wl_data.compositor = wl_registry_bind (registry,
                                               name,
                                               &wl_compositor_interface,
                                               version);
    } else if (strcmp (interface, wl_shell_interface.name) == 0) {
        wl_data.shell = wl_registry_bind (registry,
                                          name,
                                          &wl_shell_interface,
                                          version);
    } else if (strcmp (interface, xdg_wm_base_interface.name) == 0) {
        wl_data.xdg_shell = wl_registry_bind (registry,
                                              name,
                                              &xdg_wm_base_interface,
                                              version);
        g_assert (wl_data.xdg_shell);
        xdg_wm_base_add_listener (wl_data.xdg_shell, &xdg_shell_listener, NULL);
    } else if (strcmp (interface,
                       zwp_fullscreen_shell_v1_interface.name) == 0) {
        wl_data.fshell = wl_registry_bind (registry,
                                           name,
                                           &zwp_fullscreen_shell_v1_interface,
                                           version);
    } else if (strcmp (interface, wl_seat_interface.name) == 0) {
        wl_data.seat = wl_registry_bind (registry,
                                         name,
                                         &wl_seat_interface,
                                         version);
#if HAVE_DEVICE_SCALING
    } else if (strcmp (interface, wl_output_interface.name) == 0) {
        struct wl_output* output = wl_registry_bind (registry,
                                                     name,
                                                     &wl_output_interface,
                                                     version);
        wl_output_add_listener (output, &wl_data.output_listener, NULL);
        bool inserted = false;
        for (int i = 0; i < G_N_ELEMENTS (wl_data.metrics); i++)
        {
            if (wl_data.metrics[i].output == NULL) {
                wl_data.metrics[i].output = output;
                wl_data.metrics[i].name = name;
                inserted = true;
                break;
            }
        }
        if (!inserted) {
            g_warning ("Exceeded %" G_GSIZE_FORMAT " connected outputs(!)", G_N_ELEMENTS (wl_data.metrics));
        }
#endif /* HAVE_DEVICE_SCALING */
    } else {
        interface_used = FALSE;
    }
    g_debug ("%s '%s' interface obtained from the Wayland registry.",
             interface_used ? "Using" : "Ignoring", interface);
}

#if HAVE_DEVICE_SCALING
static void
registry_global_remove (void *data, struct wl_registry *registry, uint32_t name)
{
    for (int i = 0; i < G_N_ELEMENTS (wl_data.metrics); i++)
    {
        if (wl_data.metrics[i].name == name) {
            wl_data.metrics[i].output = NULL;
            wl_data.metrics[i].name = 0;
            g_debug ("Removed output %i\n", name);
            break;
        }
    }
}
#endif /* HAVE_DEVICE_SCALING */

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
#if HAVE_DEVICE_SCALING
    .global_remove = registry_global_remove
#endif /* HAVE_DEVICE_SCALING */
};

gboolean init_egl (GError **error)
{
    g_debug ("Initializing EGL...");

    egl_data.display = eglGetDisplay ((EGLNativeDisplayType) wl_data.display);
    if (egl_data.display == EGL_NO_DISPLAY) {
        // TODO: ERR_EGL (error, "Could not open EGL display");
        return FALSE;
    }

    EGLint major, minor;
    if (!eglInitialize (egl_data.display, &major, &minor)) {
        // TODO: ERR_EGL (error, "Could not initialize  EGL");
        clear_egl ();
        return FALSE;
    }
    g_info ("EGL version %d.%d initialized.", major, minor);

    if (!eglBindAPI (EGL_OPENGL_ES_API)) {
        // TODO: ERR_EGL (error, "Could not bind OpenGL ES API to EGL");
        clear_egl ();
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
    if (!eglChooseConfig (egl_data.display,
                          config_attribs,
                          &egl_data.egl_config,
                          1,
                          &num_configs)) {
        // TODO: ERR_EGL (error, "Could not find a suitable EGL configuration");
        clear_egl ();
        return FALSE;
    }
    g_assert (num_configs > 0);

    egl_data.context = eglCreateContext (egl_data.display,
                                         egl_data.egl_config,
                                         EGL_NO_CONTEXT,
                                         context_attribs);
    if (egl_data.context == EGL_NO_CONTEXT) {
        // TODO: ERR_EGL (error, "Could not create EGL context");
        clear_egl ();
        return FALSE;
    }

    return TRUE;
}

void clear_egl (void)
{
    if (egl_data.display != EGL_NO_DISPLAY) {
        if (egl_data.context != EGL_NO_CONTEXT) {
            eglDestroyContext (egl_data.display, egl_data.context);
        }
        eglTerminate (egl_data.display);
        egl_data.context = EGL_NO_CONTEXT;
        egl_data.display = EGL_NO_DISPLAY;
    }
    eglReleaseThread ();
}


gboolean init_wayland (GError **error)
{
    g_debug ("Initializing Wayland...");

    if (!(wl_data.display = wl_display_connect (NULL))) {
        g_set_error (error,
                     G_FILE_ERROR,
                     g_file_error_from_errno (errno),
                     "Could not open Wayland display");
        return FALSE;
    }

    wl_data.registry = wl_display_get_registry (wl_data.display);
    g_assert (wl_data.registry);
    wl_registry_add_listener (wl_data.registry,
                              &registry_listener,
                              NULL);
    wl_display_roundtrip (wl_data.display);

    g_assert (wl_data.compositor);
    g_assert (wl_data.xdg_shell != NULL ||
              wl_data.shell != NULL ||
              wl_data.fshell != NULL);

    return TRUE;
}

void clear_wayland (void)
{
    g_source_destroy (wl_data.event_src);

    if (wl_data.xdg_shell != NULL)
        xdg_wm_base_destroy (wl_data.xdg_shell);
    if (wl_data.fshell != NULL)
        zwp_fullscreen_shell_v1_destroy (wl_data.fshell);
    if (wl_data.shell != NULL)
        wl_shell_destroy (wl_data.shell);

    if (wl_data.compositor != NULL)
        wl_compositor_destroy (wl_data.compositor);

    wl_registry_destroy (wl_data.registry);
    wl_display_flush (wl_data.display);
    wl_display_disconnect (wl_data.display);
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
    configure_surface_geometry (width, height);

    g_debug ("New XDG toplevel configuration: (%" PRIu32 ", %" PRIu32 ")", width, height);

    wl_data.resize_window(data);
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


gboolean create_window (void *data, GError **error)
{
    g_debug ("Creating Wayland surface...");

    win_data.wl_surface = wl_compositor_create_surface (wl_data.compositor);
    g_assert (win_data.wl_surface);

#if HAVE_DEVICE_SCALING
    wl_surface_add_listener (win_data.wl_surface, &win_data.surface_listener, data);
#endif /* HAVE_DEVICE_SCALING */

    if (wl_data.xdg_shell != NULL) {
        win_data.xdg_surface =
            xdg_wm_base_get_xdg_surface (wl_data.xdg_shell,
                                         win_data.wl_surface);
        g_assert (win_data.xdg_surface);

        xdg_surface_add_listener (win_data.xdg_surface, &xdg_surface_listener,
                                  NULL);
        win_data.xdg_toplevel =
            xdg_surface_get_toplevel (win_data.xdg_surface);
        g_assert (win_data.xdg_toplevel);

        xdg_toplevel_add_listener (win_data.xdg_toplevel,
                                   &xdg_toplevel_listener, data);
        // TODO: xdg_toplevel_set_title (win_data.xdg_toplevel, COG_DEFAULT_APPNAME);

        const char *app_id = NULL;
        GApplication *app = g_application_get_default ();
        if (app) {
            app_id = g_application_get_application_id (app);
        }
        if (!app_id) {
            // TODO: app_id = COG_DEFAULT_APPID;
        }
        xdg_toplevel_set_app_id (win_data.xdg_toplevel, app_id);
        wl_surface_commit(win_data.wl_surface);
    } else if (wl_data.fshell != NULL) {
        zwp_fullscreen_shell_v1_present_surface (wl_data.fshell,
                                                 win_data.wl_surface,
                                                 ZWP_FULLSCREEN_SHELL_V1_PRESENT_METHOD_DEFAULT,
                                                 NULL);
    } else if (wl_data.shell != NULL) {
        win_data.shell_surface = wl_shell_get_shell_surface (wl_data.shell,
                                                             win_data.wl_surface);
        g_assert (win_data.shell_surface);

        wl_shell_surface_add_listener (win_data.shell_surface,
                                       &shell_surface_listener,
                                       data);
        wl_shell_surface_set_toplevel (win_data.shell_surface);

        /* wl_shell needs an initial surface configuration. */
        configure_surface_geometry (0, 0);
    }

    const char* env_var;
    if ((env_var = g_getenv ("COG_PLATFORM_FDO_VIEW_FULLSCREEN")) &&
        g_ascii_strtoll (env_var, NULL, 10) > 0)
    {
        win_data.is_maximized = false;
        win_data.is_fullscreen = true;

        if (wl_data.xdg_shell != NULL) {
            xdg_toplevel_set_fullscreen (win_data.xdg_toplevel, NULL);
        } else if (wl_data.shell != NULL) {
            wl_shell_surface_set_fullscreen (win_data.shell_surface,
                                             WL_SHELL_SURFACE_FULLSCREEN_METHOD_SCALE,
                                             0,
                                             NULL);
        } else {
            g_warning ("No available shell capable of fullscreening.");
            win_data.is_fullscreen = false;
        }
    }
    else if ((env_var = g_getenv ("COG_PLATFORM_FDO_VIEW_MAXIMIZE")) &&
             g_ascii_strtoll (env_var, NULL, 10) > 0)
    {
        win_data.is_maximized = true;
        win_data.is_fullscreen = false;

        if (wl_data.xdg_shell != NULL) {
            xdg_toplevel_set_maximized (win_data.xdg_toplevel);
        } else if (wl_data.shell != NULL) {
            wl_shell_surface_set_maximized (win_data.shell_surface, NULL);
        } else {
            g_warning ("No available shell capable of maximizing.");
            win_data.is_maximized = false;
        }
    }

    return TRUE;
}
void destroy_window (void)
{
    if (egl_data.display != EGL_NO_DISPLAY) {
        eglMakeCurrent (egl_data.display,
                        EGL_NO_SURFACE,
                        EGL_NO_SURFACE,
                        EGL_NO_CONTEXT);

        if (win_data.egl_surface) {
            eglDestroySurface (egl_data.display, win_data.egl_surface);
            win_data.egl_surface = EGL_NO_DISPLAY;
        }
    }

    g_clear_pointer (&win_data.egl_window, wl_egl_window_destroy);
    g_clear_pointer (&win_data.xdg_toplevel, xdg_toplevel_destroy);
    g_clear_pointer (&win_data.xdg_surface, xdg_surface_destroy);
    g_clear_pointer (&win_data.shell_surface, wl_shell_surface_destroy);
    g_clear_pointer (&win_data.wl_surface, wl_surface_destroy);
}

/* Keyboard */
void
keyboard_on_keymap (void *data,
                    struct wl_keyboard *wl_keyboard,
                    uint32_t format,
                    int32_t fd,
                    uint32_t size)
{
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close (fd);
        return;
    }

    void* mapping = mmap (NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        close (fd);
        return;
    }

    xkb_data.keymap =
        xkb_keymap_new_from_string (xkb_data.context,
                                    (char *) mapping,
                                    XKB_KEYMAP_FORMAT_TEXT_V1,
                                    XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap (mapping, size);
    close (fd);

    if (xkb_data.keymap == NULL)
        return;

    xkb_data.state = xkb_state_new (xkb_data.keymap);
    if (xkb_data.state == NULL)
        return;

    xkb_data.indexes.control = xkb_keymap_mod_get_index (xkb_data.keymap,
                                                         XKB_MOD_NAME_CTRL);
    xkb_data.indexes.alt = xkb_keymap_mod_get_index (xkb_data.keymap,
                                                     XKB_MOD_NAME_ALT);
    xkb_data.indexes.shift = xkb_keymap_mod_get_index (xkb_data.keymap,
                                                       XKB_MOD_NAME_SHIFT);
}

void
keyboard_on_enter (void *data,
                   struct wl_keyboard *wl_keyboard,
                   uint32_t serial,
                   struct wl_surface *surface,
                   struct wl_array *keys)
{
    g_assert (surface == win_data.wl_surface);
    wl_data.keyboard.serial = serial;
}

void
keyboard_on_leave (void *data,
                   struct wl_keyboard *wl_keyboard,
                   uint32_t serial,
                   struct wl_surface *surface)
{
    wl_data.keyboard.serial = serial;
}

gboolean
repeat_delay_timeout (void *data)
{
    wl_data.handle_key_event (data, wl_data.keyboard.repeat_data.key,
                      wl_data.keyboard.repeat_data.state,
                      wl_data.keyboard.repeat_data.time);

    wl_data.keyboard.repeat_data.event_source =
        g_timeout_add (wl_data.keyboard.repeat_info.rate,
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
    /* @FIXME: investigate why is this necessary */
    // IDK.
    key += 8;

    wl_data.keyboard.serial = serial;
    wl_data.handle_key_event (data, key, state, time);

    if (wl_data.keyboard.repeat_info.rate == 0)
        return;

    if (state == WL_KEYBOARD_KEY_STATE_RELEASED
        && wl_data.keyboard.repeat_data.key == key) {
        if (wl_data.keyboard.repeat_data.event_source)
            g_source_remove (wl_data.keyboard.repeat_data.event_source);

        memset (&wl_data.keyboard.repeat_data,
                0x00,
                sizeof (wl_data.keyboard.repeat_data));
    } else if (state == WL_KEYBOARD_KEY_STATE_PRESSED
               && xkb_keymap_key_repeats (xkb_data.keymap, key)) {
        if (wl_data.keyboard.repeat_data.event_source)
            g_source_remove (wl_data.keyboard.repeat_data.event_source);

        wl_data.keyboard.repeat_data.key = key;
        wl_data.keyboard.repeat_data.time = time;
        wl_data.keyboard.repeat_data.state = state;
        wl_data.keyboard.repeat_data.event_source =
            g_timeout_add (wl_data.keyboard.repeat_info.delay,
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
    xkb_state_update_mask (xkb_data.state,
                           mods_depressed,
                           mods_latched,
                           mods_locked,
                           0,
                           0,
                           group);

    xkb_data.modifiers = 0;
    uint32_t component
        = (XKB_STATE_MODS_DEPRESSED | XKB_STATE_MODS_LATCHED);

    if (xkb_state_mod_index_is_active (xkb_data.state,
                                       xkb_data.indexes.control,
                                       component)) {
        xkb_data.modifiers |= xkb_data.modifier.control;
    }
    if (xkb_state_mod_index_is_active (xkb_data.state,
                                       xkb_data.indexes.alt,
                                       component)) {
        xkb_data.modifiers |= xkb_data.modifier.alt;
    }
    if (xkb_state_mod_index_is_active (xkb_data.state,
                                       xkb_data.indexes.shift,
                                       component)) {
        xkb_data.modifiers |= xkb_data.modifier.shift;
    }
}

void
keyboard_on_repeat_info (void *data,
                         struct wl_keyboard *wl_keyboard,
                         int32_t rate,
                         int32_t delay)
{
    wl_data.keyboard.repeat_info.rate = rate;
    wl_data.keyboard.repeat_info.delay = delay;

    /* a rate of zero disables any repeating. */
    if (rate == 0 && wl_data.keyboard.repeat_data.event_source > 0) {
        g_source_remove(wl_data.keyboard.repeat_data.event_source);
        memset (&wl_data.keyboard.repeat_data,
                0x00,
                sizeof (wl_data.keyboard.repeat_data));
    }
}


static void
seat_on_capabilities (void* data, struct wl_seat* seat, uint32_t capabilities)
{
    g_debug ("Enumerating seat capabilities:");

    /* Pointer */
    const bool has_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
    if (has_pointer && wl_data.pointer.obj == NULL) {
        wl_data.pointer.obj = wl_seat_get_pointer (wl_data.seat);
        g_assert (wl_data.pointer.obj);
        wl_pointer_add_listener (wl_data.pointer.obj, &wl_data.pointer.listener, data);
        g_debug ("  - Pointer");
    } else if (! has_pointer && wl_data.pointer.obj != NULL) {
        wl_pointer_release (wl_data.pointer.obj);
        wl_data.pointer.obj = NULL;
    }

    /* Keyboard */
    const bool has_keyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;
    if (has_keyboard && wl_data.keyboard.obj == NULL) {
        wl_data.keyboard.obj = wl_seat_get_keyboard (wl_data.seat);
        g_assert (wl_data.keyboard.obj);
        wl_keyboard_add_listener (wl_data.keyboard.obj, &wl_data.keyboard.listener, data);
        g_debug ("  - Keyboard");
    } else if (! has_keyboard && wl_data.keyboard.obj != NULL) {
        wl_keyboard_release (wl_data.keyboard.obj);
        wl_data.keyboard.obj = NULL;
    }

    /* Touch */
    const bool has_touch = capabilities & WL_SEAT_CAPABILITY_TOUCH;
    if (has_touch && wl_data.touch.obj == NULL) {
        wl_data.touch.obj = wl_seat_get_touch (wl_data.seat);
        g_assert (wl_data.touch.obj);
        wl_touch_add_listener (wl_data.touch.obj, &wl_data.touch.listener, data);
        g_debug ("  - Touch");
    } else if (! has_touch && wl_data.touch.obj != NULL) {
        wl_touch_release (wl_data.touch.obj);
        wl_data.touch.obj = NULL;
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


gboolean init_input (void *data, GError **error)
{
    if (wl_data.seat != NULL) {
        wl_seat_add_listener (wl_data.seat, &seat_listener, data);

        xkb_data.context = xkb_context_new (XKB_CONTEXT_NO_FLAGS);
        g_assert (xkb_data.context);
        xkb_data.compose_table =
            xkb_compose_table_new_from_locale (xkb_data.context,
                                               setlocale (LC_CTYPE, NULL),
                                               XKB_COMPOSE_COMPILE_NO_FLAGS);
        if (xkb_data.compose_table != NULL) {
            xkb_data.compose_state =
                xkb_compose_state_new (xkb_data.compose_table,
                                       XKB_COMPOSE_STATE_NO_FLAGS);
        }
    }

    return TRUE;
}

void clear_input (void)
{
    g_clear_pointer (&wl_data.pointer.obj, wl_pointer_destroy);
    g_clear_pointer (&wl_data.keyboard.obj, wl_keyboard_destroy);
    g_clear_pointer (&wl_data.seat, wl_seat_destroy);

    g_clear_pointer (&xkb_data.state, xkb_state_unref);
    g_clear_pointer (&xkb_data.compose_state, xkb_compose_state_unref);
    g_clear_pointer (&xkb_data.compose_table, xkb_compose_table_unref);
    g_clear_pointer (&xkb_data.keymap, xkb_keymap_unref);
    g_clear_pointer (&xkb_data.context, xkb_context_unref);
}
