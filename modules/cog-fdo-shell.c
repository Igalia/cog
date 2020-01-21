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
static PwlWinData* s_win_data = NULL;
static bool s_support_checked = false;
G_LOCK_DEFINE_STATIC (s_globals);

static void resize_window (void *data);
static void handle_key_event (void *data, uint32_t key, uint32_t state, uint32_t time);

typedef struct {
    CogShellClass parent_class;
} CogFdoShellClass;

typedef struct {
    CogShell parent;
} CogFdoShell;

static void cog_fdo_shell_initable_iface_init (GInitableIface *iface);

struct wpe_input_touch_event_raw touch_points[10];

extern PwlData wl_data;
extern PwlXKBData xkb_data;

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

/* Output scale */
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
    bool found = false;
    for (int i = 0; i < G_N_ELEMENTS (wl_data.metrics); i++)
    {
        if (wl_data.metrics[i].output == output) {
            found = true;
            wl_data.metrics[i].scale = factor;
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

struct wpe_view_backend*
cog_shell_get_active_wpe_backend (CogShell *shell)
{
    WebKitWebView *webview = WEBKIT_WEB_VIEW(cog_shell_get_active_view (shell));

    return webkit_web_view_backend_get_wpe_backend (webkit_web_view_get_backend (webview));
}

static void
on_surface_enter (PwlDisplay* display, void *userdata)
{
    CogShell *shell = (CogShell*) userdata;
    struct wpe_view_backend* backend = cog_shell_get_active_wpe_backend (shell);
    wpe_view_backend_dispatch_set_device_scale_factor (backend, wl_data.current_output.scale);
}
#endif /* HAVE_DEVICE_SCALING */

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
    PwlDisplay *display = (PwlDisplay*) data;
    CogShell *shell = (CogShell*) display->userdata;

    struct wpe_view_backend* backend = cog_shell_get_active_wpe_backend (shell);

    wl_data.pointer.x = wl_fixed_to_int (fixed_x);
    wl_data.pointer.y = wl_fixed_to_int (fixed_y);
    struct wpe_input_pointer_event event = {
        wpe_input_pointer_event_type_motion,
        time,
        wl_data.pointer.x * wl_data.current_output.scale,
        wl_data.pointer.y * wl_data.current_output.scale,
        wl_data.pointer.button,
        wl_data.pointer.state
    };

    wpe_view_backend_dispatch_pointer_event (backend, &event);
}

static void
pointer_on_button (void* data,
                   struct wl_pointer *pointer,
                   uint32_t serial,
                   uint32_t time,
                   uint32_t button,
                   uint32_t state)
{
    PwlDisplay *display = (PwlDisplay*) data;
    CogShell *shell = (CogShell*) display->userdata;

    struct wpe_view_backend* backend = cog_shell_get_active_wpe_backend (shell);

    /* @FIXME: what is this for?
    if (button >= BTN_MOUSE)
        button = button - BTN_MOUSE + 1;
    else
        button = 0;
    */

    wl_data.pointer.button = !!state ? button : 0;
    wl_data.pointer.state = state;

    struct wpe_input_pointer_event event = {
        wpe_input_pointer_event_type_button,
        time,
        wl_data.pointer.x * wl_data.current_output.scale,
        wl_data.pointer.y * wl_data.current_output.scale,
        wl_data.pointer.button,
        wl_data.pointer.state,
    };

    wpe_view_backend_dispatch_pointer_event (backend, &event);
}

static void
pointer_on_axis (void* data,
                 struct wl_pointer *pointer,
                 uint32_t time,
                 uint32_t axis,
                 wl_fixed_t value)
{
    PwlDisplay *display = (PwlDisplay*) data;
    CogShell *shell = (CogShell*) display->userdata;

    struct wpe_view_backend* backend = cog_shell_get_active_wpe_backend (shell);

    struct wpe_input_axis_event event = {
        wpe_input_axis_event_type_motion,
        time,
        wl_data.pointer.x * wl_data.current_output.scale,
        wl_data.pointer.y * wl_data.current_output.scale,
        axis,
        wl_fixed_to_int(value) > 0 ? -1 : 1,
    };

    wpe_view_backend_dispatch_axis_event (backend, &event);
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

    PwlDisplay *display = (PwlDisplay*) data;
    CogShell *shell = (CogShell*) display->userdata;

    struct wpe_view_backend* backend = cog_shell_get_active_wpe_backend (shell);

    struct wpe_input_touch_event_raw raw_event = {
        wpe_input_touch_event_type_down,
        time,
        id,
        wl_fixed_to_int (x) * wl_data.current_output.scale,
        wl_fixed_to_int (y) * wl_data.current_output.scale,
    };

    memcpy (&touch_points[id],
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
touch_on_up (void *data,
             struct wl_touch *touch,
             uint32_t serial,
             uint32_t time,
             int32_t id)
{
    if (id < 0 || id >= 10)
        return;

    PwlDisplay *display = (PwlDisplay*) data;
    CogShell *shell = (CogShell*) display->userdata;

    struct wpe_view_backend* backend = cog_shell_get_active_wpe_backend (shell);

    struct wpe_input_touch_event_raw raw_event = {
        wpe_input_touch_event_type_up,
        time,
        id,
        touch_points[id].x,
        touch_points[id].y,
    };

    memcpy (&touch_points[id],
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

    memset (&touch_points[id],
            0x00,
            sizeof (struct wpe_input_touch_event_raw));
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
    CogShell *shell = (CogShell*) display->userdata;

    struct wpe_view_backend* backend = cog_shell_get_active_wpe_backend (shell);

    struct wpe_input_touch_event_raw raw_event = {
        wpe_input_touch_event_type_motion,
        time,
        id,
        wl_fixed_to_int (x) * wl_data.current_output.scale,
        wl_fixed_to_int (y) * wl_data.current_output.scale,
    };

    memcpy (&touch_points[id],
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
touch_on_frame (void *data, struct wl_touch *touch)
{
    /* @FIXME: buffer touch events and handle them here */
}

static void
touch_on_cancel (void *data, struct wl_touch *touch)
{
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

    wl_data.resize_window = resize_window;
    wl_data.handle_key_event = handle_key_event;
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
    clear_input (s_pdisplay);

    destroy_window (s_pdisplay, s_win_data);
    pwl_display_egl_deinit (s_pdisplay);
}


static void
resize_window (void *data)
{
    PwlWinData *win_data = (PwlWinData*) data;
    CogShell *shell = (CogShell*) win_data->display->userdata;

    int32_t pixel_width = win_data->width * wl_data.current_output.scale;
    int32_t pixel_height = win_data->height * wl_data.current_output.scale;

    struct wpe_view_backend* backend = cog_shell_get_active_wpe_backend (shell);

    if (win_data->egl_window)
        wl_egl_window_resize (win_data->egl_window,
			                pixel_width,
			                pixel_height,
			                0, 0);

    wpe_view_backend_dispatch_set_size (backend,
                                        win_data->width,
                                        win_data->height);
    g_debug ("Resized EGL buffer to: (%u, %u) @%ix\n",
            pixel_width, pixel_height, wl_data.current_output.scale);
}


static bool
capture_app_key_bindings (uint32_t keysym,
                          uint32_t unicode,
                          uint32_t state,
                          uint8_t modifiers)
{
    CogLauncher *launcher = cog_launcher_get_default ();
    WebKitWebView *web_view = WEBKIT_WEB_VIEW(cog_shell_get_active_view (cog_launcher_get_shell (launcher)));

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        /* fullscreen */
        if (modifiers == 0 && unicode == 0 && keysym == XKB_KEY_F11) {
            if (! s_win_data->is_fullscreen)
                xdg_toplevel_set_fullscreen (s_win_data->xdg_toplevel, NULL);
            else
                xdg_toplevel_unset_fullscreen (s_win_data->xdg_toplevel);
            s_win_data->is_fullscreen = ! s_win_data->is_fullscreen;
            return true;
        }
        /* Ctrl+W, exit the application */
        else if (modifiers == wpe_input_keyboard_modifier_control &&
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


static void
handle_key_event (void *data, uint32_t key, uint32_t state, uint32_t time)
{
    PwlDisplay *display = (PwlDisplay*) data;
    CogShell *shell = (CogShell*) display->userdata;

    struct wpe_view_backend* backend = cog_shell_get_active_wpe_backend (shell);

    uint32_t keysym = xkb_state_key_get_one_sym (xkb_data.state, key);
    uint32_t unicode = xkb_state_key_get_utf32 (xkb_data.state, key);

    /* Capture app-level key-bindings here */
    if (capture_app_key_bindings (keysym, unicode, state, xkb_data.modifiers))
        return;

    if (xkb_data.compose_state != NULL
            && state == WL_KEYBOARD_KEY_STATE_PRESSED
            && xkb_compose_state_feed (xkb_data.compose_state, keysym)
            == XKB_COMPOSE_FEED_ACCEPTED
            && xkb_compose_state_get_status (xkb_data.compose_state)
            == XKB_COMPOSE_COMPOSED) {
        keysym = xkb_compose_state_get_one_sym (xkb_data.compose_state);
        unicode = xkb_keysym_to_utf32 (keysym);
    }

    struct wpe_input_keyboard_event event = {
        time,
        keysym,
        unicode,
        state == true,
        xkb_data.modifiers
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
    struct wl_callback *frame_callback = wl_surface_frame (s_win_data->wl_surface);
    wl_callback_add_listener (frame_callback,
                              &frame_listener,
                              exportable);
}

static void
on_buffer_release (void* data, struct wl_buffer* buffer)
{
    WpeViewBackendData *backend_data = (WpeViewBackendData*) data;

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
    WpeViewBackendData *backend_data = (WpeViewBackendData*) data;
    g_assert (backend_data);

    backend_data->image = image;
    wpe_view_data.image = image;
    wpe_view_backend_add_activity_state (backend_data->backend, wpe_view_activity_state_initiated);


    if (s_win_data->is_fullscreen) {
        struct wl_region *region;
        region = wl_compositor_create_region (s_pdisplay->compositor);
        wl_region_add (region, 0, 0, s_win_data->width, s_win_data->height);
        wl_surface_set_opaque_region (s_win_data->wl_surface, region);
        wl_region_destroy (region);
    } else {
        wl_surface_set_opaque_region (s_win_data->wl_surface, NULL);
    }

    static PFNEGLCREATEWAYLANDBUFFERFROMIMAGEWL
        s_eglCreateWaylandBufferFromImageWL;
    if (s_eglCreateWaylandBufferFromImageWL == NULL) {
        s_eglCreateWaylandBufferFromImageWL = (PFNEGLCREATEWAYLANDBUFFERFROMIMAGEWL)
            eglGetProcAddress ("eglCreateWaylandBufferFromImageWL");
        g_assert (s_eglCreateWaylandBufferFromImageWL);
    }

    struct wl_buffer *buffer = s_eglCreateWaylandBufferFromImageWL (s_pdisplay->egl_display, wpe_fdo_egl_exported_image_get_egl_image (image));
    g_assert (buffer);
    wl_buffer_add_listener (buffer, &buffer_listener, data);

    wl_surface_attach (s_win_data->wl_surface, buffer, 0, 0);
    wl_surface_damage (s_win_data->wl_surface,
                       0, 0,
                       s_win_data->width * wl_data.current_output.scale,
                       s_win_data->height * wl_data.current_output.scale);
    request_frame (backend_data->exportable);

    wl_surface_commit (s_win_data->wl_surface);
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

    if (!wl_data.event_src) {
        wl_data.event_src =
            setup_wayland_event_source (g_main_context_get_thread_default (),
                                        s_pdisplay);
    }

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

    TRACE ("");
    s_pdisplay->userdata = initable;
    s_win_data = g_new0 (PwlWinData, 1);

#if HAVE_DEVICE_SCALING
    const struct wl_output_listener output_listener = {
        .geometry = noop,
        .mode = noop,
        .done = noop,
        .scale = output_handle_scale,
    };
    wl_data.output_listener = output_listener;

    s_pdisplay->on_surface_enter = on_surface_enter;
#endif /* HAVE_DEVICE_SCALING */

    if (!init_wayland (s_pdisplay, error))
        return FALSE;

    if (!pwl_display_egl_init (s_pdisplay, error))
        return FALSE;

    if (!create_window (s_pdisplay, s_win_data, error)) {
        pwl_display_egl_deinit (s_pdisplay);
        return FALSE;
    }

    const struct wl_pointer_listener pointer_listener = {
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
    wl_data.pointer.listener = pointer_listener;

    xkb_data.modifier.control = wpe_input_keyboard_modifier_control;
    xkb_data.modifier.alt = wpe_input_keyboard_modifier_alt;
    xkb_data.modifier.shift = wpe_input_keyboard_modifier_shift;

    const struct wl_touch_listener touch_listener = {
        .down = touch_on_down,
        .up = touch_on_up,
        .motion = touch_on_motion,
        .frame = touch_on_frame,
        .cancel = touch_on_cancel,
    };
    wl_data.touch.listener = touch_listener;

    if (!init_input (s_pdisplay, error)) {
        destroy_window (s_pdisplay, s_win_data);
        pwl_display_egl_deinit (s_pdisplay);
        return FALSE;
    }

    /* init WPE host data */
    wpe_fdo_initialize_for_egl_display (s_pdisplay->egl_display);

    return TRUE;
}

static void cog_fdo_shell_initable_iface_init (GInitableIface *iface)
{
    iface->init = cog_fdo_shell_initable_init;
}
