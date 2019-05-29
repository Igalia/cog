/*
 * cog-platform-wlr.c
 * Copyright (C) 2019 Adrian Perez de Castro <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include <cog.h>
#include <wlr/backend.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>
#include <wpe/fdo.h>
#include <wpe/fdo-egl.h>
#include <wpe/webkit.h>
#include <xkbcommon/xkbcommon.h>


/* Global (ugh) state. */
static struct wl_display  *s_display  = NULL;
static struct wlr_backend *s_backend  = NULL;
static int32_t             s_width    = 160;
static int32_t             s_height   = 90;
static GSList             *s_backends = NULL;


typedef struct {
    struct wlr_output *output;
    struct wl_listener frame;
    struct wl_listener destroy;
} CogWlrOutput;

static void
cog_wlr_output_destroy (CogWlrOutput *self)
{
    g_debug("Freeing output %p.", self);
    wl_list_remove (&self->destroy.link);
    wl_list_remove (&self->frame.link);
    g_slice_free (CogWlrOutput, self);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (CogWlrOutput, cog_wlr_output_destroy)

static void
on_output_frame (struct wl_listener *listener, void *data)
{
    CogWlrOutput *self = wl_container_of (listener, self, frame);
    g_debug ("Frame for output %p.", self);

    wlr_output_make_current (self->output, NULL);
    // TODO: paint
    wlr_output_swap_buffers (self->output, NULL, NULL);
}

static void
on_output_destroy (struct wl_listener *listener, void *data)
{
    g_autoptr(CogWlrOutput) self = wl_container_of (listener, self, destroy);
    g_debug ("Removing output %p.", self);
}

static CogWlrOutput*
cog_wlr_output_new (struct wlr_output *output)
{
    g_autoptr(CogWlrOutput) self = g_slice_new0 (CogWlrOutput);
    self->output = g_steal_pointer (&output);
    self->frame.notify = on_output_frame;
    self->destroy.notify = on_output_destroy;

    if (!wl_list_empty (&self->output->modes)) {
        struct wlr_output_mode *mode =
            wl_container_of (self->output->modes.prev, mode, link);
        wlr_output_set_mode (self->output, mode);
        s_width = MAX (s_width, self->output->width);
        s_height = MAX (s_height, self->output->height);
    }
    wl_signal_add (&self->output->events.frame, &self->frame);
    wl_signal_add (&self->output->events.destroy, &self->destroy);

    g_debug ("Created new output %p, %" PRIi32 "x%" PRIi32 ".",
             self, self->output->width, self->output->height);
    return g_steal_pointer (&self);
}


typedef struct {
    struct wlr_input_device *device;
    struct wl_listener event;
    struct wl_listener destroy;
} CogWlrInput;

static void
cog_wlr_input_destroy (CogWlrInput *self)
{
    wl_list_remove (&self->event.link);
    wl_list_remove (&self->destroy.link);
    g_slice_free (CogWlrInput, self);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (CogWlrInput, cog_wlr_input_destroy)

static void
on_input_destroy (struct wl_listener *listener, void *data)
{
    g_autoptr(CogWlrInput) self = wl_container_of (listener, self, destroy);
    g_debug ("Removing input %p.", self);
}

static void
on_keyboard_key_event (struct wl_listener *listener, void *data)
{
    CogWlrInput *self = wl_container_of (listener, self, event);
    struct wlr_event_keyboard_key *event = data;

    const uint32_t keysym = xkb_state_key_get_one_sym (self->device->keyboard->xkb_state,
                                                       event->keycode);
    const uint32_t unicode = xkb_state_key_get_utf32 (self->device->keyboard->xkb_state,
                                                      event->keycode);

    /* TODO: Handle modifiers. */
    struct wpe_input_keyboard_event wpe_event = {
        .time = g_get_monotonic_time () / 1000,
        .key_code = keysym,
        .hardware_key_code = unicode,
        .pressed = (event->state == WLR_KEY_PRESSED),
    };

    for (GSList *item = s_backends; item; item = g_slist_next (item))
        wpe_view_backend_dispatch_keyboard_event (item->data, &wpe_event);
}

static CogWlrInput*
cog_wlr_input_new (struct wlr_input_device *device)
{
    g_autoptr(CogWlrInput) self = g_slice_new0 (CogWlrInput);
    self->device = g_steal_pointer (&device);
    self->destroy.notify = on_input_destroy;
    wl_signal_add (&self->device->events.destroy, &self->destroy);

    switch (self->device->type) {
        case WLR_INPUT_DEVICE_KEYBOARD:
            /* TODO: Support configuring keymaps. */
            self->event.notify = on_keyboard_key_event;
            wl_signal_add (&self->device->keyboard->events.key, &self->event);
            break;
    }

    g_debug ("Created new keyboard input %p.", self);
    return g_steal_pointer (&self);
}

static void
on_new_output (struct wl_listener *listener G_GNUC_UNUSED, void *data)
{
    cog_wlr_output_new (data);
}

static void
on_new_input (struct wl_listener *listener, void *data)
{
    cog_wlr_input_new (data);
}

static struct wlr_renderer*
on_create_renderer (struct wlr_egl *egl,
                    EGLenum         platform,
                    void           *remote_display,
                    EGLint         *config_attribs,
                    EGLint          visual_id)
{
    struct wlr_renderer *renderer = wlr_renderer_autocreate (egl,
                                                             platform,
                                                             remote_display,
                                                             config_attribs,
                                                             visual_id);
    g_debug ("Renderer created %p.", renderer);
    g_debug ("EGL version: %s.", eglQueryString (egl->display, EGL_VERSION));
    g_debug ("EGL vendor: %s.", eglQueryString (egl->display, EGL_VENDOR));

    if (wpe_fdo_initialize_for_egl_display (egl->display))
        return renderer;

    /* XXX: Is this a valid way of sinalling an error to wlroots? */
    wlr_renderer_destroy (renderer);
    return NULL;
}

gboolean
cog_platform_setup (CogPlatform *platform,
                    CogShell    *shell G_GNUC_UNUSED,
                    const char  *params G_GNUC_UNUSED,
                    GError     **error)
{
    g_assert (platform);

    if (!wpe_loader_init ("libWPEBackend-fdo-1.0.so")) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to set backend library name");
        return FALSE;
    }

    wlr_log_init (WLR_ERROR, NULL);
    s_display = wl_display_create ();

    if (!(s_backend = wlr_backend_autocreate (s_display, on_create_renderer))) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Cannot create wlroots backend");
        return FALSE;
    }

    static struct wl_listener new_output = { .notify = on_new_output };
    wl_signal_add (&s_backend->events.new_output, &new_output);

    static struct wl_listener new_input = { .notify = on_new_input };
    wl_signal_add (&s_backend->events.new_input, &new_input);

    if (!wlr_backend_start (s_backend)) {
        g_clear_pointer (&s_backend, wlr_backend_destroy);
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Cannot start wlroots backend");
        return FALSE;
    }

    return TRUE;
}

void
cog_platform_teardown (CogPlatform *platform)
{
    g_assert (platform);
    g_clear_pointer (&s_backend, wlr_backend_destroy);
    g_clear_pointer (&s_display, wl_display_destroy);
}

static void
on_export_egl_image (void *data, EGLImageKHR image)
{
}

static void
on_destroy_exportable (struct wpe_view_backend_exportable_fdo *exportable)
{
    struct wpe_view_backend *backend =
        wpe_view_backend_exportable_fdo_get_view_backend (exportable);
    g_debug ("Destroying exportable %p, backend %p.", exportable, backend);
    s_backends = g_slist_remove (s_backends, backend);
    wpe_view_backend_exportable_fdo_destroy (exportable);
}

WebKitWebViewBackend*
cog_platform_get_view_backend (CogPlatform   *platform,
                               WebKitWebView *related_view,
                               GError       **error)
{
    struct wpe_view_backend_exportable_fdo *exportable =
        wpe_view_backend_exportable_fdo_egl_create (&((struct wpe_view_backend_exportable_fdo_egl_client) { .export_egl_image = on_export_egl_image }),
                                                    NULL,
                                                    s_width,
                                                    s_height);
    struct wpe_view_backend *backend =
        wpe_view_backend_exportable_fdo_get_view_backend (exportable);
    s_backends = g_slist_prepend (s_backends, backend);

    g_debug ("Creating WebKitWebViewBackend for exportable %p, backend %p.",
             exportable, backend);

    return webkit_web_view_backend_new (backend,
                                        (GDestroyNotify) on_destroy_exportable,
                                        exportable);
}
