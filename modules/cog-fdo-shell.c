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

    gboolean            single_window;
    struct wl_callback *frame_callback;
    PwlWindow          *window; /* Non-NULL in single window mode. */
    uint32_t            window_id;

    struct wpe_view_backend_exportable_fdo_egl_client exportable_client;
};

enum {
    SHELL_PROP_0,
    SHELL_PROP_SINGLE_WINDOW,
    SHELL_PROP_WINDOW_ID,
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

    struct wl_callback *frame_callback;
    struct wl_buffer   *last_buffer;
    GHashTable         *buffer_images; /* (wl_buffer, wpe_fdo_egl_exported_image) */

    PwlWindow *window; /* Non-NULL in multiple window mode. */
    uint32_t   window_id;
};

enum {
    VIEW_PROP_0,
    VIEW_PROP_WINDOW_ID,
    VIEW_N_PROPERTIES,
};

static GParamSpec *s_view_properties[VIEW_N_PROPERTIES] = { NULL, };


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
    g_assert (((CogFdoShell*) shell)->window == window);

    TRACE ("shell @ %p, new size %" PRIu32 "x%" PRIu32, shell, width, height);

    /* Apply the new size to all the views. */
    for (GList *item = cog_shell_get_views (shell); item; item = g_list_next (item)) {
        struct wpe_view_backend *backend = cog_view_get_backend (item->data);
        wpe_view_backend_dispatch_set_size (backend, width, height);
    }
}


static void
cog_fdo_view_on_export_fdo_egl_image_single_window (void*, struct wpe_fdo_egl_exported_image*);

static void
cog_fdo_view_on_export_fdo_egl_image_multi_window (void*, struct wpe_fdo_egl_exported_image*);


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

    if (!pwl_display_egl_init (s_display, error))
        return FALSE;

    /* TODO: Make the application identifier a CogShell property. */
    if (COG_DEFAULT_APPID && *COG_DEFAULT_APPID)
        pwl_display_set_default_application_id (s_display, COG_DEFAULT_APPID);

    /* TODO: Set later on the loaded page title as window title. */
    if (COG_DEFAULT_APPNAME && *COG_DEFAULT_APPNAME)
        pwl_display_set_default_window_title (s_display, COG_DEFAULT_APPNAME);

    pwl_display_attach_sources (s_display, g_main_context_get_thread_default ());
    wpe_fdo_initialize_for_egl_display (pwl_display_egl_get_display (s_display));

    CogFdoShell *self = COG_FDO_SHELL (initable);

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

        self->exportable_client.export_fdo_egl_image =
            cog_fdo_view_on_export_fdo_egl_image_single_window;
    } else {
        self->exportable_client.export_fdo_egl_image =
            cog_fdo_view_on_export_fdo_egl_image_multi_window;
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

    struct wpe_fdo_egl_exported_image *image;
    struct wl_buffer *lookup_buffer;

    if (g_hash_table_steal_extended (self->buffer_images,
                                     buffer,
                                     (void**) &lookup_buffer,
                                     (void**) &image))
    {
        g_assert (buffer == lookup_buffer);
        TRACE ("wl_buffer @ %p, image @ %p", buffer, image);
        wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image (self->exportable, image);
    } else {
        g_critical ("%s: Image not found for buffer %p.", G_STRFUNC, buffer);
    }

    wl_buffer_destroy (buffer);

    if (self->last_buffer == buffer)
        self->last_buffer = NULL;
}


static void
cog_fdo_view_on_surface_frame (void               *data,
                               struct wl_callback *callback,
                               uint32_t            timestamp G_GNUC_UNUSED)
{
    CogFdoShell *shell = (CogFdoShell*) cog_view_get_shell (data);
    CogFdoView *self = data;

    if (shell->single_window) {
        g_assert (shell->frame_callback == callback);
        g_clear_pointer (&shell->frame_callback, wl_callback_destroy);
    } else {
        g_assert (self->frame_callback == callback);
        g_clear_pointer (&self->frame_callback, wl_callback_destroy);
    }

    wpe_view_backend_exportable_fdo_dispatch_frame_complete (self->exportable);
}


static void
cog_fdo_view_request_frame (CogFdoView        *self,
                            struct wl_surface *window_surface)
{
    static const struct wl_callback_listener frame_listener = {
        .done = cog_fdo_view_on_surface_frame,
    };

    CogFdoShell *shell = (CogFdoShell*) cog_view_get_shell ((CogView*) self);
    if (shell->single_window) {
        if (!shell->frame_callback) {
            shell->frame_callback = wl_surface_frame (window_surface);
            wl_callback_add_listener (shell->frame_callback, &frame_listener, self);
        }
    } else {
        if (!self->frame_callback) {
            self->frame_callback = wl_surface_frame (window_surface);
            wl_callback_add_listener (self->frame_callback, &frame_listener, self);
        }
    }
}


static void
cog_fdo_view_update_window_contents (CogFdoView       *self,
                                     PwlWindow        *window,
                                     struct wl_buffer *buffer,
                                     gboolean          autorelease)
{
    if (autorelease) {
        static const struct wl_buffer_listener buffer_listener = {
            .release = cog_fdo_view_on_buffer_release,
        };
        wl_buffer_add_listener (buffer, &buffer_listener, self);
    }

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

    const uint32_t device_scale = pwl_window_get_device_scale (window);
    struct wl_surface *window_surface = pwl_window_get_surface (window);

    wl_surface_attach (window_surface, buffer, 0, 0);
    wl_surface_damage (window_surface, 0, 0,
                       width * device_scale,
                       height * device_scale);

    cog_fdo_view_request_frame (self, window_surface);
    wl_surface_commit (window_surface);
}


static void
cog_fdo_view_on_export_fdo_egl_image_single_window (void                              *data,
                                                    struct wpe_fdo_egl_exported_image *image)
{
    CogFdoView *self = data;

    struct wl_buffer *buffer =
        pwl_display_egl_create_buffer_from_image (s_display,
                                                  wpe_fdo_egl_exported_image_get_egl_image (image));
    g_hash_table_insert (self->buffer_images, buffer, image);
    TRACE ("wl_buffer @ %p, image @ %p", buffer, image);

    /* Release the last buffer and image before saving the new one. */
    if (self->last_buffer)
        cog_fdo_view_on_buffer_release (self, self->last_buffer);

    self->last_buffer = buffer;
    g_hash_table_insert (self->buffer_images, buffer, image);

    if (cog_shell_get_focused_view (cog_view_get_shell (data)) == (CogView*) self) {
        /* This view is currently being shown: attach buffer right away. */
        TRACE ("view %p focused, updating window.", data);
        CogFdoShell *shell = (CogFdoShell*) cog_view_get_shell (data);
        cog_fdo_view_update_window_contents (self, shell->window, buffer, FALSE);
    } else {
        TRACE ("view %p not focused, skipping window update.", data);
    }

    wpe_view_backend_exportable_fdo_dispatch_frame_complete (self->exportable);
}


static void
cog_fdo_view_on_export_fdo_egl_image_multi_window (void                              *data,
                                                   struct wpe_fdo_egl_exported_image *image)
{
    CogFdoView *self = data;

    struct wl_buffer *buffer =
        pwl_display_egl_create_buffer_from_image (s_display,
                                                  wpe_fdo_egl_exported_image_get_egl_image (image));
    g_hash_table_insert (self->buffer_images, buffer, image);
    TRACE ("wl_buffer @ %p, image @ %p", buffer, image);

    cog_fdo_view_update_window_contents (self, self->window, buffer, TRUE);

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

        pwl_window_get_size (self->window, &width, &height);
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

    if (!self->last_buffer) {
        g_debug ("%s: No last buffer to show, skipping update.", G_STRFUNC);
        return;
    }

    CogFdoShell *shell = (CogFdoShell*) cog_view_get_shell ((CogView*) self);
    cog_fdo_view_update_window_contents (self,
                                         shell->window,
                                         self->last_buffer,
                                         FALSE);
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

    struct wpe_fdo_egl_exported_image *image;
    struct wl_buffer *buffer;
    GHashTableIter iter;

    g_hash_table_iter_init (&iter, self->buffer_images);
    while (g_hash_table_iter_next (&iter, (void**) &buffer, (void**) &image)) {
        wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image (self->exportable,
                                                                             image);
        wl_buffer_destroy (buffer);
    }
    g_clear_pointer (&self->buffer_images, g_hash_table_destroy);

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
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}


static void
cog_fdo_shell_class_init (CogFdoShellClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->get_property = cog_fdo_shell_get_property;
    object_class->set_property = cog_fdo_shell_set_property;

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
