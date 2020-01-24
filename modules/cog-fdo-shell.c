/*
 * cog-fdo-shell.c
 *
 * Copyright (C) 2020 Igalia S.L.
 * Copyright (C) 2018-2019 Adrian Perez de Castro <aperez@igalia.com>
 * Copyright (C) 2018 Eduardo Lima <elima@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include <cog.h>
#include <wpe/wpe.h>
#include "../platform/pwl.h"

#include <errno.h>
#include <stdint.h>
#include <wpe/webkit.h>
#include <wpe/fdo.h>
#include <wpe/fdo-egl.h>
#include <stdlib.h>
#include <stdio.h>
#include <glib.h>
#include <string.h>


#define DEFAULT_ZOOM_STEP 0.1f

#if 1
# define TRACE(fmt, ...) g_debug ("%s: " fmt, G_STRFUNC, ##__VA_ARGS__)
#else
# define TRACE(fmt, ...) ((void) 0)
#endif

const static int wpe_view_activity_state_initiated = 1 << 4;

static PwlDisplay *s_pdisplay = NULL;
static PwlWindow* s_pwindow = NULL;
static bool s_support_checked = false;
G_LOCK_DEFINE_STATIC (s_globals);

typedef struct {
    CogShellClass parent_class;
} CogFdoShellClass;

typedef struct {
    CogShell parent;
} CogFdoShell;

static void cog_fdo_shell_initable_iface_init (GInitableIface *iface);

struct wpe_input_touch_event_raw touch_points[10];

GSList* wpe_host_data_exportable = NULL;

static struct {
    struct wpe_fdo_egl_exported_image *image;
} wpe_view_data = {NULL, };

typedef struct {
    struct wpe_view_backend_exportable_fdo *exportable;
    struct wpe_view_backend *backend;
    struct wpe_fdo_egl_exported_image *image;
} WpeViewBackendData;

void
wpe_view_backend_data_free (WpeViewBackendData *data)
{
    wpe_view_backend_exportable_fdo_destroy (data->exportable);
    /* TODO: Either free data->backend or make sure it is freed elsewhere. */
    /* TODO: I think the data->image is actually freed already when it's not needed anymore. */
    g_free (data);
}

struct wpe_view_backend*
cog_shell_get_active_wpe_backend (CogShell *shell)
{
    WebKitWebView *webview = WEBKIT_WEB_VIEW(cog_shell_get_active_view (shell));

    return webkit_web_view_backend_get_wpe_backend (webkit_web_view_get_backend (webview));
}

/* Output scale */
static void
window_on_device_scale (PwlWindow* self, uint32_t device_scale, void *userdata)
{
    /*
     * TODO: Device scale must be handled for the backend of the window,
     *       and *not* for the current view backend!
     */
    CogShell *shell = userdata;
    struct wpe_view_backend* backend = cog_shell_get_active_wpe_backend (shell);
    wpe_view_backend_dispatch_set_device_scale_factor (backend, device_scale);
}


/* Pointer */
static void
on_pointer_on_motion (PwlDisplay* display,
                      const PwlPointer *pointer,
                      void *userdata)
{
    CogShell *shell = userdata;
    struct wpe_view_backend* backend = cog_shell_get_active_wpe_backend (shell);
    /* TODO: Device scale must be handled per window! */
    const uint32_t device_scale = pwl_window_get_device_scale (s_pwindow);
    struct wpe_input_pointer_event event = {
        wpe_input_pointer_event_type_motion,
        pointer->time,
        pointer->x * device_scale,
        pointer->y * device_scale,
        pointer->button,
        pointer->state
    };
    wpe_view_backend_dispatch_pointer_event (backend, &event);
}

static void
on_pointer_on_button (PwlDisplay* display,
                      const PwlPointer *pointer,
                      void *userdata)
{
    CogShell *shell = userdata;
    struct wpe_view_backend* backend = cog_shell_get_active_wpe_backend (shell);
    /* TODO: Device scale must be handled per window! */
    const uint32_t device_scale = pwl_window_get_device_scale (s_pwindow);
    struct wpe_input_pointer_event event = {
        wpe_input_pointer_event_type_button,
        pointer->time,
        pointer->x * device_scale,
        pointer->y * device_scale,
        pointer->button,
        pointer->state,
    };
    wpe_view_backend_dispatch_pointer_event (backend, &event);
}

static void
on_pointer_on_axis (PwlDisplay* display,
                    const PwlPointer *pointer,
                    void *userdata)
{
    CogShell *shell = userdata;
    struct wpe_view_backend* backend = cog_shell_get_active_wpe_backend (shell);
    /* TODO: Device scale must be handled per window! */
    const uint32_t device_scale = pwl_window_get_device_scale (s_pwindow);
    struct wpe_input_axis_event event = {
        wpe_input_axis_event_type_motion,
        pointer->time,
        pointer->x * device_scale,
        pointer->y * device_scale,
        pointer->axis,
        pointer->value,
    };
    wpe_view_backend_dispatch_axis_event (backend, &event);
}

/* Touch */
static void
on_touch_on_down (PwlDisplay* display,
                  const PwlTouch* touch,
                  void *userdata)
{
    CogShell *shell = userdata;
    struct wpe_view_backend* backend = cog_shell_get_active_wpe_backend (shell);
    /* TODO: Device scale must be handled per window! */
    const uint32_t device_scale = pwl_window_get_device_scale (s_pwindow);
    struct wpe_input_touch_event_raw raw_event = {
        wpe_input_touch_event_type_down,
        touch->time,
        touch->id,
        wl_fixed_to_int (touch->x) * device_scale,
        wl_fixed_to_int (touch->y) * device_scale,
    };

    memcpy (&touch_points[touch->id],
            &raw_event,
            sizeof (struct wpe_input_touch_event_raw));

    struct wpe_input_touch_event event = {
        touch_points,
        10,
        raw_event.type,
        raw_event.id,
        raw_event.time
    };

    wpe_view_backend_dispatch_touch_event (backend, &event);
}

static void
on_touch_on_up (PwlDisplay* display,
                const PwlTouch *touch,
                void *userdata)
{
    CogShell *shell = userdata;
    struct wpe_view_backend* backend = cog_shell_get_active_wpe_backend (shell);
    struct wpe_input_touch_event_raw raw_event = {
        wpe_input_touch_event_type_up,
        touch->time,
        touch->id,
        touch_points[touch->id].x,
        touch_points[touch->id].y,
    };

    memcpy (&touch_points[touch->id],
            &raw_event,
            sizeof (struct wpe_input_touch_event_raw));

    struct wpe_input_touch_event event = {
        touch_points,
        10,
        raw_event.type,
        raw_event.id,
        raw_event.time
    };

    wpe_view_backend_dispatch_touch_event (backend, &event);

    memset (&touch_points[touch->id],
            0x00,
            sizeof (struct wpe_input_touch_event_raw));
}

static void
on_touch_on_motion (PwlDisplay* display,
                    const PwlTouch *touch,
                    void *userdata)
{
    CogShell *shell = userdata;
    struct wpe_view_backend* backend = cog_shell_get_active_wpe_backend (shell);
    /* TODO: Device scale must be handled per window! */
    const uint32_t device_scale = pwl_window_get_device_scale (s_pwindow);
    struct wpe_input_touch_event_raw raw_event = {
        wpe_input_touch_event_type_motion,
        touch->time,
        touch->id,
        wl_fixed_to_int (touch->x) * device_scale,
        wl_fixed_to_int (touch->y) * device_scale,
    };

    memcpy (&touch_points[touch->id],
            &raw_event,
            sizeof (struct wpe_input_touch_event_raw));

    struct wpe_input_touch_event event = {
        touch_points,
        10,
        raw_event.type,
        raw_event.id,
        raw_event.time
    };

    wpe_view_backend_dispatch_touch_event (backend, &event);
}

G_DEFINE_DYNAMIC_TYPE_EXTENDED (CogFdoShell, cog_fdo_shell, COG_TYPE_SHELL, 0,
    g_io_extension_point_implement (COG_MODULES_SHELL_EXTENSION_POINT,
                                    g_define_type_id, "fdo", 100);
    G_IMPLEMENT_INTERFACE_DYNAMIC (G_TYPE_INITABLE, cog_fdo_shell_initable_iface_init))

G_MODULE_EXPORT
void
g_io_fdo_shell_load (GIOModule *module)
{
    TRACE ("");
    cog_fdo_shell_register_type (G_TYPE_MODULE (module));
}

G_MODULE_EXPORT
void
g_io_fdo_shell_unload (GIOModule *module G_GNUC_UNUSED)
{
    TRACE ("");

    G_LOCK (s_globals);
    g_clear_pointer (&s_pdisplay, pwl_display_destroy);
    G_UNLOCK (s_globals);
}

static gboolean
cog_fdo_shell_is_supported (void)
{
    gboolean result;

    G_LOCK (s_globals);

    if (!s_support_checked) {
        g_autoptr(GError) error = NULL;
        s_pdisplay = pwl_display_connect (NULL, &error);
        if (!s_pdisplay)
            g_debug ("%s: %s", G_STRFUNC, error->message);
        s_support_checked = true;
    }
    TRACE ("PwlDisplay @ %p", s_pdisplay);
    result = s_pdisplay ? TRUE : FALSE;

    G_UNLOCK (s_globals);

    return result;
}

WebKitWebViewBackend* cog_shell_new_fdo_view_backend (CogShell *shell);
WebKitWebViewBackend* cog_shell_fdo_resume_active_views (CogShell *shell);

static void
cog_fdo_shell_class_init (CogFdoShellClass *klass)
{
    TRACE ("");
    CogShellClass *shell_class = COG_SHELL_CLASS (klass);
    shell_class->is_supported = cog_fdo_shell_is_supported;
    shell_class->cog_shell_new_view_backend = cog_shell_new_fdo_view_backend;
    shell_class->cog_shell_resume_active_views = cog_shell_fdo_resume_active_views;
}

static void
cog_fdo_shell_class_finalize (CogFdoShellClass *klass)
{
    TRACE ("");
    /* TODO
    g_assert (platform);

    // free WPE view data
    if (wpe_view_data.frame_callback != NULL)
        wl_callback_destroy (wpe_view_data.frame_callback);
    if (wpe_view_data.image != NULL) {
        wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image (wpe_host_data.exportable,
                                                                             wpe_view_data.image);
    }
    g_clear_pointer (&wpe_view_data.buffer, wl_buffer_destroy);

    / * @FIXME: check why this segfaults
    wpe_view_backend_destroy (wpe_view_data.backend);
    * /

    / * free WPE host data * /
    / * @FIXME: check why this segfaults
    wpe_view_backend_exportable_fdo_destroy (wpe_host_data.exportable);
    * /
*/

    g_clear_pointer (&s_pwindow, pwl_window_destroy);

    pwl_display_input_deinit (s_pdisplay);
    pwl_display_egl_deinit (s_pdisplay);
}


static void
on_window_resize (PwlWindow *window, uint32_t width, uint32_t height, void *userdata)
{
    CogShell *shell = userdata;
    struct wpe_view_backend* backend = cog_shell_get_active_wpe_backend (shell);
    wpe_view_backend_dispatch_set_size (backend, width, height);
}


/* TODO: Keybinding capture should be moved into CogShell. */
#if 0
static bool
on_capture_app_key (PwlDisplay *display, void *userdata)
{
    CogLauncher *launcher = cog_launcher_get_default ();
    WebKitWebView *web_view = WEBKIT_WEB_VIEW(cog_shell_get_active_view (cog_launcher_get_shell (launcher)));

    uint32_t keysym = display->keyboard.event.keysym;
    uint32_t unicode = display->keyboard.event.unicode;
    uint32_t state = display->keyboard.event.state;
    uint8_t modifiers =display->keyboard.event.modifiers;

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        /* Ctrl+W, exit the application */
        if (modifiers == wpe_input_keyboard_modifier_control &&
                 unicode == 0x17 && keysym == 0x77) {
            // TODO: A loadable module in a library, we shouldn't be causing
            // the application to quit, this should be decided by the application.
            // Not even sure that it's a good idea to handle any key shortcuts.
            g_application_quit (G_APPLICATION (launcher));
            return true;
        }
        /* Ctrl+Plus, zoom in */
        else if (modifiers == wpe_input_keyboard_modifier_control &&
                 unicode == XKB_KEY_equal && keysym == XKB_KEY_equal) {
            const double level = webkit_web_view_get_zoom_level (web_view);
            webkit_web_view_set_zoom_level (web_view,
                                            level + DEFAULT_ZOOM_STEP);
            return true;
        }
        /* Ctrl+Minus, zoom out */
        else if (modifiers == wpe_input_keyboard_modifier_control &&
                 unicode == 0x2D && keysym == 0x2D) {
            const double level = webkit_web_view_get_zoom_level (web_view);
            webkit_web_view_set_zoom_level (web_view,
                                            level - DEFAULT_ZOOM_STEP);
            return true;
        }
        /* Ctrl+0, restore zoom level to 1.0 */
        else if (modifiers == wpe_input_keyboard_modifier_control &&
                 unicode == XKB_KEY_0 && keysym == XKB_KEY_0) {
            webkit_web_view_set_zoom_level (web_view, 1.0f);
            return true;
        }
        /* Alt+Left, navigate back */
        else if (modifiers == wpe_input_keyboard_modifier_alt &&
                 unicode == 0 && keysym == XKB_KEY_Left) {
            webkit_web_view_go_back (web_view);
            return true;
        }
        /* Alt+Right, navigate forward */
        else if (modifiers == wpe_input_keyboard_modifier_alt &&
                 unicode == 0 && keysym == XKB_KEY_Right) {
            webkit_web_view_go_forward (web_view);
            return true;
        }
    }

    return false;
}
#endif


static void
on_key_event (PwlDisplay* display,
              const PwlKeyboard *kbd,
              void *userdata)
{
    CogShell *shell = userdata;
    struct wpe_view_backend* backend = cog_shell_get_active_wpe_backend (shell);
    struct wpe_input_keyboard_event event = {
        kbd->event.timestamp,
        kbd->event.keysym,
        kbd->event.unicode,
        kbd->event.state == true,
        kbd->event.modifiers
    };
    wpe_view_backend_dispatch_keyboard_event (backend, &event);
}


static void
on_surface_frame (void *data, struct wl_callback *callback, uint32_t time)
{
    wl_callback_destroy (callback);
    struct wpe_view_backend_exportable_fdo *exportable = (struct wpe_view_backend_exportable_fdo*) data;
    wpe_view_backend_exportable_fdo_dispatch_frame_complete (exportable);
}

static const struct wl_callback_listener frame_listener = {
    .done = on_surface_frame
};

static void
request_frame (struct wpe_view_backend_exportable_fdo *exportable)
{
    struct wl_callback *frame_callback = wl_surface_frame (pwl_window_get_surface (s_pwindow));
    wl_callback_add_listener (frame_callback,
                              &frame_listener,
                              exportable);
}

static void
on_buffer_release (void* data, struct wl_buffer* buffer)
{
    WpeViewBackendData *backend_data = data;

    /* TODO: These asserts might be unnecessary, but having for now. */
    g_assert (backend_data);

    // Corner case: dispaching the last generated image. Required when the
    // active view changes.
    if (wpe_view_data.image && wpe_view_data.image != backend_data->image) {
        wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image (backend_data->exportable, wpe_view_data.image);
    }
    wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image (backend_data->exportable, backend_data->image);
    g_clear_pointer (&buffer, wl_buffer_destroy);
}

static const struct wl_buffer_listener buffer_listener = {
    .release = on_buffer_release,
};

static void
on_export_fdo_egl_image (void *data, struct wpe_fdo_egl_exported_image *image)
{
    /* TODO: Pass the corresponding CogFdoView/PwlWindow as userdata. */
    WpeViewBackendData *backend_data = data;
    g_assert (backend_data);

    backend_data->image = image;
    wpe_view_data.image = image;
    wpe_view_backend_add_activity_state (backend_data->backend, wpe_view_activity_state_initiated);

    if (pwl_window_is_fullscreen (s_pwindow)) {
        uint32_t width, height;
        pwl_window_get_size (s_pwindow, &width, &height);
        pwl_window_set_opaque_region (s_pwindow, 0, 0, width, height);
    } else {
        pwl_window_unset_opaque_region (s_pwindow);
    }

    struct wl_buffer *buffer =
        pwl_display_egl_create_buffer_from_image (s_pdisplay,
                                                  wpe_fdo_egl_exported_image_get_egl_image (image));
    g_assert (buffer);
    wl_buffer_add_listener (buffer, &buffer_listener, data);

    uint32_t width, height;
    pwl_window_get_size (s_pwindow, &width, &height);
    struct wl_surface *surface = pwl_window_get_surface (s_pwindow);
    uint32_t device_scale = pwl_window_get_device_scale (s_pwindow);

    wl_surface_attach (surface, buffer, 0, 0);
    wl_surface_damage (surface, 0, 0, width * device_scale, height * device_scale);
    request_frame (backend_data->exportable);

    wl_surface_commit (surface);
}

WebKitWebViewBackend*
cog_shell_new_fdo_view_backend (CogShell *shell)
{
    static struct wpe_view_backend_exportable_fdo_egl_client exportable_egl_client = {
        .export_fdo_egl_image = on_export_fdo_egl_image,
    };

    WpeViewBackendData *data = g_new0 (WpeViewBackendData, 1);

    data->exportable = wpe_view_backend_exportable_fdo_egl_create (&exportable_egl_client,
                                                                   data,
                                                                   DEFAULT_WIDTH,
                                                                   DEFAULT_HEIGHT);
    g_assert (data->exportable);

    /* init WPE view backend */
    data->backend =
        wpe_view_backend_exportable_fdo_get_view_backend (data->exportable);
    g_assert (data->backend);

    WebKitWebViewBackend *wk_view_backend =
        webkit_web_view_backend_new (data->backend,
                                     (GDestroyNotify) wpe_view_backend_data_free,
                                     data);
    g_assert (wk_view_backend);

    setup_wayland_event_source (g_main_context_get_thread_default (), s_pdisplay);

    return wk_view_backend;
}


WebKitWebViewBackend*
cog_shell_fdo_resume_active_views (CogShell *shell)
{
    GSList* iterator = NULL;
    for (iterator = wpe_host_data_exportable; iterator; iterator =  iterator->next) {
        struct wpe_view_backend_exportable_fdo *exportable = iterator->data;
        struct wpe_view_backend *backend = wpe_view_backend_exportable_fdo_get_view_backend (exportable);
        if (wpe_view_backend_get_activity_state (backend) & wpe_view_activity_state_visible) {
            if (wpe_view_backend_get_activity_state (backend) & wpe_view_activity_state_initiated) {
                g_debug("cog_shell_resume_active_views: %p", exportable);
                request_frame (exportable);
            }
        }
    }
    WebKitWebView *webview = WEBKIT_WEB_VIEW(cog_shell_get_active_view (shell));
    return webkit_web_view_get_backend(webview);
}

static void
cog_fdo_shell_init (CogFdoShell *shell)
{
    TRACE ("");
}

gboolean
cog_fdo_shell_initable_init (GInitable *initable,
                             GCancellable *cancellable,
                             GError **error)
{
    if (cancellable != NULL) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                             "Cancellable initialization not supported");
        return FALSE;
    }

    if (!wpe_loader_init ("libWPEBackend-fdo-1.0.so")) {
        g_debug ("%s: Could not initialize libwpe.", G_STRFUNC);
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                             "Couldn't initialize the FDO backend.");
        return FALSE;
    }

    if (!pwl_display_egl_init (s_pdisplay, error)) {
        g_critical ("pwl_display_egl_init failed");
        return FALSE;
    }

    /* TODO: Make the application identifier a CogShell property. */
    if (COG_DEFAULT_APPID && *COG_DEFAULT_APPID) {
        pwl_display_set_default_application_id (s_pdisplay, COG_DEFAULT_APPID);
    }

    s_pwindow = pwl_window_create (s_pdisplay);

    pwl_display_notify_pointer_motion (s_pdisplay, on_pointer_on_motion, initable);
    pwl_display_notify_pointer_button (s_pdisplay, on_pointer_on_button, initable);
    pwl_display_notify_pointer_axis (s_pdisplay, on_pointer_on_axis, initable);

    pwl_display_notify_touch_down (s_pdisplay, on_touch_on_down, initable);
    pwl_display_notify_touch_up (s_pdisplay, on_touch_on_up, initable);
    pwl_display_notify_touch_motion (s_pdisplay, on_touch_on_motion, initable);

    pwl_display_notify_key_event (s_pdisplay, on_key_event, initable);

    /* TODO: Move to CogShell. */
#if 0
    s_pdisplay->on_capture_app_key = on_capture_app_key;
    s_pdisplay->on_capture_app_key_userdata = initable;
#endif

    pwl_window_notify_resize (s_pwindow, on_window_resize, initable);
    pwl_window_notify_device_scale (s_pwindow, window_on_device_scale, initable);

    PwlXKBData *xkb_data = pwl_display_xkb_get_data (s_pdisplay);
    xkb_data->modifier.control = wpe_input_keyboard_modifier_control;
    xkb_data->modifier.alt = wpe_input_keyboard_modifier_alt;
    xkb_data->modifier.shift = wpe_input_keyboard_modifier_shift;

    if (!pwl_display_input_init (s_pdisplay, error)) {
        g_critical ("init_input failed");
        g_clear_pointer (&s_pwindow, pwl_window_destroy);
        pwl_display_egl_deinit (s_pdisplay);
        return FALSE;
    }

    /* init WPE host data */
    wpe_fdo_initialize_for_egl_display (pwl_display_egl_get_display (s_pdisplay));

    return TRUE;
}

static void cog_fdo_shell_initable_iface_init (GInitableIface *iface)
{
    iface->init = cog_fdo_shell_initable_init;
}
