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
#include <cog.h>
#include <wpe/fdo.h>
#include <wpe/fdo-egl.h>


#if defined(WPE_CHECK_VERSION)
# define HAVE_2D_AXIS_EVENT (WPE_CHECK_VERSION (1, 5, 0) && (WEBKIT_CHECK_VERSION (2, 27, 4)))
#else /* !WPE_CHECK_VERSION */
# define HAVE_2D_AXIS_EVENT 0
#endif /* WPE_CHECK_VERSION */


#if 0
# define TRACE(fmt, ...) g_debug ("%s: " fmt, G_STRFUNC, __VA_ARGS__)
#else
# define TRACE(fmt, ...) ((void) 0)
#endif


static PwlDisplay *s_display = NULL;
static bool s_support_checked = false;
G_LOCK_DEFINE_STATIC (s_globals);


G_DECLARE_FINAL_TYPE (CogFdoShell, cog_fdo_shell, COG, FDO_SHELL, CogShell)
G_DECLARE_FINAL_TYPE (CogFdoView, cog_fdo_view, COG, FDO_VIEW, CogView)


struct _CogFdoShellClass {
    CogShellClass parent_class;
};

struct _CogFdoShell {
    CogShell    parent;
    CogFdoView *focused_view;
};


struct _CogFdoViewClass {
    CogViewClass parent_class;
};

struct _CogFdoView {
    CogView parent;
    PwlWindow *window;
    struct wpe_view_backend_exportable_fdo *exportable;
    struct wl_callback *frame_callback;
    GHashTable *buffer_images; /* (wl_buffer, wpe_fdo_egl_exported_image) */
    struct wpe_input_touch_event_raw touchpoints[PWL_N_TOUCH_POINTS];
};


static gboolean
cog_fdo_shell_initable_init (GInitable *initable,
                             GCancellable *cancellable,
                             GError **error)
{
    if (!wpe_loader_init ("libWPEBackend-fdo-1.0.so")) {
        g_set_error_literal (error,
                             G_IO_ERROR,
                             G_IO_ERROR_FAILED,
                             "Could not initialize WPE loader.");
        return FALSE;
    }

    if (!pwl_display_egl_init (s_display, error))
        return FALSE;

    /* TODO: Make the application identifier a CogShell property. */
    if (COG_DEFAULT_APPID && *COG_DEFAULT_APPID)
        pwl_display_set_default_application_id (s_display, COG_DEFAULT_APPID);

    /* TODO: Set later on the loaded page title as window title. */
    if (COG_DEFAULT_APPNAME && *COG_DEFAULT_APPNAME)
        pwl_display_set_default_window_title (s_display, COG_DEFAULT_APPNAME);

    setup_wayland_event_source (g_main_context_get_thread_default (), s_display);

    wpe_fdo_initialize_for_egl_display (pwl_display_egl_get_display (s_display));
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
    wpe_view_backend_dispatch_axis_event (backend, &event);
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
    g_assert (((CogFdoView*) view)->window == window);

    TRACE ("view @ %p, new size %" PRIu32 "x%" PRIu32, view, width, height);

    struct wpe_view_backend *backend = cog_view_get_backend (view);
    wpe_view_backend_dispatch_set_size (backend, width, height);
}


static void
cog_fdo_view_on_buffer_release (void             *data,
                                struct wl_buffer *buffer)
{
    CogFdoView *self = data;

    struct wpe_fdo_egl_exported_image *image =
        g_hash_table_lookup (self->buffer_images, buffer);
    g_hash_table_remove (self->buffer_images, buffer);

    TRACE ("wl_buffer @ %p, image @ %p", buffer, image);

    g_assert (image);
    wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image (self->exportable, image);

    wl_buffer_destroy (buffer);
}


static void
cog_fdo_view_on_surface_frame (void               *data,
                               struct wl_callback *callback,
                               uint32_t            timestamp G_GNUC_UNUSED)
{
    CogFdoView *self = data;

    g_assert (self->frame_callback == callback);
    g_clear_pointer (&self->frame_callback, wl_callback_destroy);

    wpe_view_backend_exportable_fdo_dispatch_frame_complete (self->exportable);
}


static void
cog_fdo_view_request_frame (CogFdoView        *self,
                            struct wl_surface *window_surface)
{
    if (!self->frame_callback) {
        self->frame_callback = wl_surface_frame (window_surface);
        static const struct wl_callback_listener frame_listener = {
            .done = cog_fdo_view_on_surface_frame,
        };
        wl_callback_add_listener (self->frame_callback, &frame_listener, self);
    }
}


static void
cog_fdo_view_on_export_fdo_egl_image (void                              *data,
                                      struct wpe_fdo_egl_exported_image *image)
{
    CogFdoView *self = data;

    uint32_t width, height;
    pwl_window_get_size (self->window, &width, &height);

    if (pwl_window_is_fullscreen (self->window))
        pwl_window_set_opaque_region (self->window, 0, 0, width, height);
    else
        pwl_window_unset_opaque_region (self->window);

    struct wl_buffer *buffer =
        pwl_display_egl_create_buffer_from_image (s_display,
                                                  wpe_fdo_egl_exported_image_get_egl_image (image));
    g_hash_table_insert (self->buffer_images, buffer, image);
    TRACE ("wl_buffer @ %p, image @ %p", buffer, image);

    static const struct wl_buffer_listener buffer_listener = {
        .release = cog_fdo_view_on_buffer_release,
    };
    wl_buffer_add_listener (buffer, &buffer_listener, self);

    const uint32_t device_scale = pwl_window_get_device_scale (self->window);
    struct wl_surface *window_surface = pwl_window_get_surface (self->window);
    wl_surface_attach (window_surface, buffer, 0, 0);
    wl_surface_damage (window_surface, 0, 0, width * device_scale, height * device_scale);

    cog_fdo_view_request_frame (self, window_surface);

    wl_surface_commit (window_surface);
}


static void
cog_fdo_view_destroy_backend (void *userdata)
{
    CogFdoView *self = userdata;
    g_clear_pointer (&self->exportable, wpe_view_backend_exportable_fdo_destroy);
}


/*
 * Here be dragons.
 *
 * We want to use the CogFdoView as userdata for the FDO backend client,
 * but we need to create the WebKitWebViewBackend from it before the
 * WebKitWebView is constructed. While this seems like a chicken-and-egg
 * issue, in reality the backend is not used until the .constructed
 */
static GObject*
cog_fdo_view_constructor (GType                  type,
                          unsigned               n_properties,
                          GObjectConstructParam *properties)
{
    GObject *obj = G_OBJECT_CLASS (cog_fdo_view_parent_class)->constructor (type,
                                                                            n_properties,
                                                                            properties);

    /* We need to create the window early to query the size. */
    CogFdoView *self = (CogFdoView*) obj;

    self->window = pwl_window_create (s_display);

    pwl_window_notify_keyboard (self->window, cog_fdo_view_on_keyboard, self);

    pwl_window_notify_pointer_motion (self->window, cog_fdo_view_on_pointer_motion, self);
    pwl_window_notify_pointer_button (self->window, cog_fdo_view_on_pointer_button, self);
    pwl_window_notify_pointer_axis (self->window, cog_fdo_view_on_pointer_axis, self);

    pwl_window_notify_touch_down (self->window, cog_fdo_view_on_touch_down, self);
    pwl_window_notify_touch_up (self->window, cog_fdo_view_on_touch_up, self);
    pwl_window_notify_touch_motion (self->window, cog_fdo_view_on_touch_motion, self);

    pwl_window_notify_device_scale (self->window, cog_fdo_view_on_device_scale, self);
    pwl_window_notify_resize (self->window, cog_fdo_view_on_window_resize, self);

    uint32_t width, height;
    pwl_window_get_size (self->window, &width, &height);

    static const struct wpe_view_backend_exportable_fdo_egl_client client = {
        .export_fdo_egl_image = cog_fdo_view_on_export_fdo_egl_image,
    };
    self->exportable = wpe_view_backend_exportable_fdo_egl_create (&client,
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
    g_object_set_property (obj, "backend", &backend_value);

    TRACE ("Created @ %p, WebKitWebViewBackend @ %p, exportable @ %p.",
           self, backend, self->exportable);
    return obj;
}


static void
cog_fdo_view_dispose (GObject *object)
{
    CogFdoView *view = (CogFdoView*) object;

    g_clear_pointer (&view->buffer_images, g_hash_table_destroy);

    G_OBJECT_CLASS (cog_fdo_view_parent_class)->dispose (object);
}


static void
cog_fdo_view_class_init (CogFdoViewClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->constructor = cog_fdo_view_constructor;
    object_class->dispose = cog_fdo_view_dispose;
}


static void
cog_fdo_view_class_finalize (CogFdoViewClass *klass)
{
}


static void
cog_fdo_view_init (CogFdoView *self)
{
    self->buffer_images = g_hash_table_new (NULL, NULL);
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
cog_fdo_shell_class_init (CogFdoShellClass *klass)
{
    CogShellClass *shell_class = COG_SHELL_CLASS (klass);
    shell_class->is_supported = cog_fdo_shell_is_supported;
    shell_class->get_view_class = cog_fdo_view_get_type;
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
