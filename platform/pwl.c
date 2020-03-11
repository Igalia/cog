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
#include <sys/mman.h>

#ifdef HAVE_IVI_SHELL
#include "ivi-application-client.h"
#endif /* HAVE_IVI_SHELL */

#if 0
# define TRACE(fmt, ...) g_debug ("%s: " fmt, G_STRFUNC, __VA_ARGS__)
#else
# define TRACE(fmt, ...) ((void) 0)
#endif


#ifndef EGL_WL_create_wayland_buffer_from_image
typedef struct wl_buffer * (EGLAPIENTRYP PFNEGLCREATEWAYLANDBUFFERFROMIMAGEWL) (EGLDisplay dpy, EGLImageKHR image);
#endif


G_DEFINE_QUARK (com_igalia_Cog_PwlError, pwl_error)


typedef struct {
    struct wl_output *output;
    int32_t output_name;
    int32_t device_scale;
    struct wl_list link;
} PwlOutput;


struct _PwlDisplay {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;

    struct wl_seat *seat;
    uint32_t        seat_version;

    struct egl_display *egl_display;
    PFNEGLCREATEWAYLANDBUFFERFROMIMAGEWL egl_createWaylandBufferFromImageWL;

    struct wl_pointer *pointer;
    PwlWindow *pointer_target;  /* Current target of pointer events. */

    struct wl_touch *touch;
    PwlWindow *touch_target;  /* Current target of touch events. */

    PwlXKBData          xkb_data;
    struct wl_keyboard *keyboard;
    PwlWindow          *keyboard_target; /* Current target of keyboard events. */
    int32_t             keyboard_repeat_rate;
    int32_t             keyboard_repeat_delay;
    uint32_t            keyboard_repeat_timestamp;
    uint32_t            keyboard_repeat_key;
    uint32_t            keyboard_repeat_state;
    GSource            *keyboard_repeat_source;

    struct xdg_wm_base *xdg_shell;
    struct zwp_fullscreen_shell_v1 *fshell;
    struct wl_shell *shell;

#ifdef HAVE_IVI_SHELL
    struct ivi_application *ivi_application;
#endif /* HAVE_IVI_SHELL */

    struct wl_list outputs; /* wl_list<PwlOutput> */

    char *application_id;
    char *window_title;
    GSource *event_src;

    bool (*on_capture_app_key) (PwlDisplay*, void *userdata);
    void *on_capture_app_key_userdata;
};


struct _PwlWindow {
    PwlDisplay *display;

    struct wl_surface *wl_surface;
    struct wl_surface_listener surface_listener;

    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    struct wl_shell_surface *shell_surface;

#ifdef HAVE_IVI_SHELL
    struct ivi_surface *ivi_surface;
    uint32_t surface_id;
#endif /* HAVE_IVI_SHELL */

    uint32_t width;
    uint32_t height;
    uint32_t device_scale;

    PwlKeyboard keyboard;
    PwlPointer pointer;
    PwlTouch touch;

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

    void (*on_touch_down) (PwlWindow*, const PwlTouch*, void *userdata);
    void *on_touch_down_userdata;
    void (*on_touch_up) (PwlWindow*, const PwlTouch*, void *userdata);
    void *on_touch_up_userdata;
    void (*on_touch_motion) (PwlWindow*, const PwlTouch*, void *userdata);
    void *on_touch_motion_userdata;

    void (*on_keyboard) (PwlWindow*, const PwlKeyboard*, void *userdata);
    void *on_keyboard_userdata;
};


struct pwl_event_source {
    GSource source;
    GPollFD pfd;
    struct wl_display* display;
};


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
        g_critical ("Unknown output %p.", output);
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
    g_debug ("Surface entered output %p with scale factor %i.", output, factor);

    if (factor < 1) {
        g_warning ("No scale factor available for output %p.", output);
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
window_dispatch_axis_event (PwlWindow *self)
{
    if (!(self->pointer.axis_x_delta || self->pointer.axis_y_delta)) {
        return;  /* There are no deltas to report. */
    }

    if (self->on_pointer_axis) {
        (*self->on_pointer_axis) (self,
                                  &self->pointer,
                                  self->on_pointer_axis_userdata);
    }

    self->pointer.axis_timestamp = 0;
    self->pointer.axis_x_delta = 0;
    self->pointer.axis_y_delta = 0;
    self->pointer.axis_x_discrete = false;
    self->pointer.axis_y_discrete = false;
}


static void
pointer_on_leave (void              *data,
                  struct wl_pointer *pointer,
                  uint32_t           serial,
                  struct wl_surface *surface)
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
        return;
    }

#ifdef WL_POINTER_FRAME_SINCE_VERSION
    if (display->seat_version < WL_POINTER_FRAME_SINCE_VERSION) {
        window_dispatch_axis_event (display->pointer_target);
    }
#endif /* WL_POINTER_FRAME_SINCE_VERSION */

    g_debug ("%s: Pointer left surface %p, window %p.",
             G_STRFUNC, surface, display->pointer_target);
}


static void
pointer_on_motion (void              *data,
                   struct wl_pointer *pointer,
                   uint32_t           timestamp,
                   wl_fixed_t         x,
                   wl_fixed_t         y)
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

    window->pointer.timestamp = timestamp;
    window->pointer.x = wl_fixed_to_int (x);
    window->pointer.y = wl_fixed_to_int (y);

    if (window->on_pointer_motion) {
        (*window->on_pointer_motion) (window,
                                      &window->pointer,
                                      window->on_pointer_motion_userdata);
    }
}

static void
pointer_on_button (void              *data,
                   struct wl_pointer *pointer,
                   uint32_t           serial,
                   uint32_t           timestamp,
                   uint32_t           button,
                   uint32_t           state)
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

    TRACE ("%s: timestamp=%" PRIu32 ", button=%" PRIu32 ", state=%" PRIi32 ".",
           G_STRFUNC, timestamp, button, state);

    window->pointer.button = !!state ? button : 0;
    window->pointer.state = state;
    window->pointer.timestamp = timestamp;

    if (window->on_pointer_button) {
        (*window->on_pointer_button) (window,
                                      &window->pointer,
                                      window->on_pointer_button_userdata);
    }
}


static void
pointer_on_axis (void              *data,
                 struct wl_pointer *pointer,
                 uint32_t           timestamp,
                 uint32_t           axis,
                 wl_fixed_t         value)
{
    TRACE ("%s: pointer @ %p, axis=%" PRIu32 ", value=%g.",
           G_STRFUNC, pointer, axis, wl_fixed_to_double (value));

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

    window->pointer.axis_timestamp = timestamp;
    switch (axis) {
        case WL_POINTER_AXIS_HORIZONTAL_SCROLL:
            window->pointer.axis_x_delta += value;
            break;
        case WL_POINTER_AXIS_VERTICAL_SCROLL:
            window->pointer.axis_y_delta += value;
            break;
        default:
            g_warning ("%s: Unhandled scroll axis %" PRIu32 ".", G_STRFUNC, axis);
            return;
    }

#ifdef WL_POINTER_FRAME_SINCE_VERSION
    if (display->seat_version < WL_POINTER_FRAME_SINCE_VERSION)
#endif /* !WL_POINTER_FRAME_SINCE_VERSION */
        window_dispatch_axis_event (window);
}


#ifdef WL_POINTER_FRAME_SINCE_VERSION
static void
pointer_on_frame (void              *data,
                  struct wl_pointer *pointer)
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

    window_dispatch_axis_event (window);
}


static void
pointer_on_axis_source (void              *data,
                        struct wl_pointer *pointer,
                        uint32_t           axis_source)
{
    TRACE ("%s: pointer @ %p, source=%" PRIu32 ".",
           G_STRFUNC, pointer, axis_source);
}


static void
pointer_on_axis_stop (void              *data,
                      struct wl_pointer *pointer,
                      uint32_t           timestamp,
                      uint32_t           axis)
{
    TRACE ("%s: pointer @ %p, axis=%" PRIu32 ".", G_STRFUNC, pointer, axis);

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

    switch (axis) {
        case WL_POINTER_AXIS_HORIZONTAL_SCROLL:
            window->pointer.axis_x_delta = 0;
            window->pointer.axis_x_discrete = false;
            break;
        case WL_POINTER_AXIS_VERTICAL_SCROLL:
            window->pointer.axis_y_delta = 0;
            window->pointer.axis_y_discrete = false;
            break;
    }
    window->pointer.axis_timestamp = timestamp;
}

static void
pointer_on_axis_discrete (void              *data,
                          struct wl_pointer *pointer,
                          uint32_t           axis,
                          int32_t            value)
{
    TRACE ("%s: pointer @ %p, axis=%" PRIu32 ", value=%" PRIi32 ".",
           G_STRFUNC, pointer, axis, value);

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

    switch (axis) {
        case WL_POINTER_AXIS_HORIZONTAL_SCROLL:
            window->pointer.axis_x_discrete = !!value;
            break;
        case WL_POINTER_AXIS_VERTICAL_SCROLL:
            window->pointer.axis_y_discrete = !!value;
            break;
    }
}
#endif /* WL_POINTER_FRAME_SINCE_VERSION */


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
    if (id < 0 || id >= PWL_N_TOUCH_POINTS)
        return;

    PwlWindow *window = wl_surface_get_user_data (surface);
    window->touch = (PwlTouch) { .id = id, .time = time, .x = x, .y = y };

    PwlDisplay *display = data;
    display->touch_target = window;

    if (window->on_touch_down) {
        (*window->on_touch_down) (window,
                                  &window->touch,
                                  window->on_touch_down_userdata);
    }
}

static void
touch_on_up (void *data,
             struct wl_touch *touch,
             uint32_t serial,
             uint32_t time,
             int32_t id)
{
    if (id < 0 || id >= PWL_N_TOUCH_POINTS)
        return;

    PwlDisplay *display = data;
    PwlWindow *window = display->touch_target;

    window->touch.id = id;
    window->touch.time = time;

    if (window->on_touch_up) {
        (*window->on_touch_up) (window,
                                &window->touch,
                                window->on_touch_up_userdata);
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
    if (id < 0 || id >= PWL_N_TOUCH_POINTS)
        return;

    PwlDisplay *display = data;
    PwlWindow *window = display->touch_target;
    window->touch = (PwlTouch) { .id = id, .time = time, .x = x, .y = y };

    if (window->on_touch_motion) {
        (*window->on_touch_motion) (window,
                                    &window->touch,
                                    window->on_touch_motion_userdata);
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

    const int map_mode = (display->seat_version > 6) ? MAP_PRIVATE : MAP_SHARED;
    void* mapping = mmap (NULL, size, PROT_READ, map_mode, fd, 0);
    close (fd);

    if (mapping == MAP_FAILED) {
        g_critical ("%s: Could not mmap new XKB keymap (%s).",
                    G_STRFUNC, strerror (errno));
        return;
    }

    struct xkb_keymap *keymap =
        xkb_keymap_new_from_string (display->xkb_data.context,
                                    mapping,
                                    XKB_KEYMAP_FORMAT_TEXT_V1,
                                    XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap (mapping, size);
    mapping = NULL;

    struct xkb_state *state = xkb_state_new (keymap);

    if (!keymap || !state) {
        g_critical ("%s: Could not initialize XKB keymap, keeping the old one.",
                    G_STRFUNC);
        g_clear_pointer (&state, xkb_state_unref);
        g_clear_pointer (&keymap, xkb_keymap_unref);
        return;
    }

    g_clear_pointer (&display->xkb_data.state, xkb_state_unref);
    g_clear_pointer (&display->xkb_data.keymap, xkb_keymap_unref);

    display->xkb_data.keymap = g_steal_pointer (&keymap);
    display->xkb_data.state = g_steal_pointer (&state);

    display->xkb_data.indexes.control = xkb_keymap_mod_get_index (display->xkb_data.keymap,
                                                                  XKB_MOD_NAME_CTRL);
    display->xkb_data.indexes.alt = xkb_keymap_mod_get_index (display->xkb_data.keymap,
                                                              XKB_MOD_NAME_ALT);
    display->xkb_data.indexes.shift = xkb_keymap_mod_get_index (display->xkb_data.keymap,
                                                                XKB_MOD_NAME_SHIFT);

    g_debug ("%s: XKB keymap updated.", G_STRFUNC);
}

void
keyboard_on_enter (void               *data,
                   struct wl_keyboard *keyboard,
                   uint32_t            serial,
                   struct wl_surface  *surface,
                   struct wl_array    *keys)
{
    PwlDisplay *display = data;
    if (keyboard != display->keyboard) {
        g_critical ("%s: Got keyboard %p, expected %p.",
                    G_STRFUNC, keyboard, display->keyboard);
        return;
    }

    PwlWindow *window = wl_surface_get_user_data (surface);
    display->keyboard_target = window;

    display->keyboard_target->keyboard.serial = serial;

    g_debug ("%s: Target surface %p, window %p.", G_STRFUNC, surface, window);
}

void
keyboard_on_leave (void               *data,
                   struct wl_keyboard *keyboard,
                   uint32_t            serial,
                   struct wl_surface  *surface)
{
    PwlDisplay *display = data;
    if (keyboard != display->keyboard) {
        g_critical ("%s: Got keyboard %p, expected %p.",
                    G_STRFUNC, keyboard, display->keyboard);
        return;
    }

    if (!display->keyboard_target) {
        g_critical ("%s: No current keyboard event target!", G_STRFUNC);
        return;
    }

    if (surface != display->keyboard_target->wl_surface) {
        g_critical ("%s: Got leave for surface %p, but current window %p "
                    "has surface %p.", G_STRFUNC, surface,
                    display->keyboard_target,
                    display->keyboard_target->wl_surface);
        return;
    }

    display->keyboard_target->keyboard.serial = serial;

    g_debug ("%s: Keyboard left surface %p, window %p.",
             G_STRFUNC, surface, display->keyboard_target);
}


static void
window_dispatch_key_event (PwlWindow  *self,
                           PwlXKBData *xkb_data,
                           uint32_t    timestamp,
                           uint32_t    key,
                           uint32_t    state)
{
    self->keyboard.state = state;
    self->keyboard.timestamp = timestamp;
    self->keyboard.unicode = xkb_state_key_get_utf32 (xkb_data->state, key);
    self->keyboard.keysym = xkb_state_key_get_one_sym (xkb_data->state, key);
    self->keyboard.modifiers = xkb_data->modifiers;

    if (xkb_data->compose_state &&
        state == WL_KEYBOARD_KEY_STATE_PRESSED &&
        xkb_compose_state_feed (xkb_data->compose_state, self->keyboard.keysym) == XKB_COMPOSE_FEED_ACCEPTED &&
        xkb_compose_state_get_status (xkb_data->compose_state) == XKB_COMPOSE_COMPOSED)
    {
        self->keyboard.keysym = xkb_compose_state_get_one_sym (xkb_data->compose_state);
        self->keyboard.unicode = xkb_keysym_to_utf32 (self->keyboard.keysym);
    }

    if (self->on_keyboard) {
        (*self->on_keyboard) (self,
                              &self->keyboard,
                              self->on_keyboard_userdata);
    }
}


static gboolean
display_on_keyboard_repeat_timeout (PwlDisplay *self)
{
    if (!self->keyboard_target) {
        g_critical ("%s: No keyboard event target window!", G_STRFUNC);
        /*
         * We cannot use display_stop_keyboard_repeat() here because it
         * would destroy the GSource while it is still being dispatched.
         */
        g_clear_pointer (&self->keyboard_repeat_source, g_source_unref);
        return G_SOURCE_REMOVE;
    }

    window_dispatch_key_event (self->keyboard_target,
                               &self->xkb_data,
                               self->keyboard_repeat_timestamp,
                               self->keyboard_repeat_key,
                               self->keyboard_repeat_state);
    return G_SOURCE_CONTINUE;
}


static void
display_stop_keyboard_repeat (PwlDisplay *self)
{
    if (!self->keyboard_repeat_source)
        return;

    g_source_destroy (self->keyboard_repeat_source);
    g_clear_pointer (&self->keyboard_repeat_source, g_source_unref);
}


static void
display_start_keyboard_repeat (PwlDisplay *self,
                               uint32_t    timestamp,
                               uint32_t    key,
                               uint32_t    state)
{
    display_stop_keyboard_repeat (self);

    self->keyboard_repeat_source =
        g_timeout_source_new (self->keyboard_repeat_delay);

    g_source_set_callback (self->keyboard_repeat_source,
                           (GSourceFunc) display_on_keyboard_repeat_timeout,
                           self,
                           NULL);

    g_source_attach (self->keyboard_repeat_source,
                     g_source_get_context (self->event_src));
}


void
keyboard_on_key (void               *data,
                 struct wl_keyboard *keyboard,
                 uint32_t            serial,
                 uint32_t            timestamp,
                 uint32_t            key,
                 uint32_t            state)
{
    PwlDisplay *display = data;
    if (keyboard != display->keyboard) {
        g_critical ("%s: Got keyboard %p, expected %p.",
                    G_STRFUNC, keyboard, display->keyboard);
        return;
    }

    PwlWindow *window = display->keyboard_target;
    if (!window) {
        g_critical ("%s: No current keyboard event target!", G_STRFUNC);
        return;
    }

    /* XXX: Investigate why is this necessary. */
    key += 8;

    window->keyboard.serial = serial;
    window_dispatch_key_event (window,
                               &display->xkb_data,
                               timestamp,
                               key,
                               state);

    if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        display_stop_keyboard_repeat (display);
        return;
    }

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED &&
        display->keyboard_repeat_rate &&
        xkb_keymap_key_repeats (display->xkb_data.keymap, key))
    {
        display_start_keyboard_repeat (display, timestamp, key, state);
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

    if (!display->xkb_data.state) {
        g_warning ("%s: XKB state not yet configured!", G_STRFUNC);
        return;
    }

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
keyboard_on_repeat_info (void               *data,
                         struct wl_keyboard *keyboard,
                         int32_t             rate,
                         int32_t             delay)
{
    PwlDisplay *display = data;
    if (keyboard != display->keyboard) {
        return;
    }

    display->keyboard_repeat_rate = rate;
    display->keyboard_repeat_delay = delay;

    if (!rate)
        display_stop_keyboard_repeat (display);

    g_debug ("%s: Set keyboard repeat rate=%" PRIi32 ", delay=%" PRIi32 ".",
             G_STRFUNC, rate, delay);
}


static void
display_on_seat_capabilities (void           *data,
                              struct wl_seat *seat,
                              uint32_t        capabilities)
{
    PwlDisplay *display = data;

    g_assert (display);
    g_assert (display->seat == seat);

    g_debug ("Enumerating seat capabilities:");

    /* Pointer */
    static const struct wl_pointer_listener pointer_listener = {
        .enter = pointer_on_enter,
        .leave = pointer_on_leave,
        .motion = pointer_on_motion,
        .button = pointer_on_button,
        .axis = pointer_on_axis,
#ifdef WL_POINTER_FRAME_SINCE_VERSION
        .frame = pointer_on_frame,
        .axis_source = pointer_on_axis_source,
        .axis_stop = pointer_on_axis_stop,
        .axis_discrete = pointer_on_axis_discrete,
#endif /* WL_POINTER_FRAME_SINCE_VERSION */
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
    if (has_keyboard && !display->keyboard) {
        display->keyboard = wl_seat_get_keyboard (display->seat);
        static const struct wl_keyboard_listener keyboard_listener = {
            .keymap = keyboard_on_keymap,
            .enter = keyboard_on_enter,
            .leave = keyboard_on_leave,
            .key = keyboard_on_key,
            .modifiers = keyboard_on_modifiers,
            .repeat_info = keyboard_on_repeat_info,
        };
        wl_keyboard_add_listener (display->keyboard, &keyboard_listener, display);
        g_debug ("  - New keyboard %p.", display->keyboard);
    } else if (!has_keyboard && display->keyboard) {
        g_debug ("  - Keyboard %p gone.", display->keyboard);
        g_clear_pointer (&display->keyboard, wl_keyboard_release);
    }

    /* Touch */
    const bool has_touch = capabilities & WL_SEAT_CAPABILITY_TOUCH;
    if (has_touch && !display->touch) {
        display->touch = wl_seat_get_touch (display->seat);
        static const struct wl_touch_listener touch_listener = {
            .down = touch_on_down,
            .up = touch_on_up,
            .motion = touch_on_motion,
            .frame = touch_on_frame,
            .cancel = touch_on_cancel,
        };
        wl_touch_add_listener (display->touch, &touch_listener, data);
        g_debug ("  - New touch input %p.", display->touch);
    } else if (!has_touch && display->touch) {
        g_debug ("  - Touch input %p gone.", display->touch);
        g_clear_pointer (&display->touch, wl_touch_release);
    }

    g_debug ("Done enumerating seat capabilities.");
}


static void
display_on_seat_name (void           *data G_GNUC_UNUSED,
                      struct wl_seat *seat G_GNUC_UNUSED,
                      const char     *name)
{
    g_debug ("Seat name: '%s'", name);
}


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

#if HAVE_IVI_SHELL
    if (strcmp (interface, ivi_application_interface.name) == 0) {
        display->ivi_application = wl_registry_bind (registry,
                                                     name,
                                                     &ivi_application_interface,
                                                     version);
    } else
#endif /* HAVE_IVI_SHELL */
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
        if (display->seat) {
            g_warning ("%s: Multiple seats are not supported, ignoring "
                       "wl_seat#%" PRIu32 ".", G_STRFUNC, name); 
            interface_used = FALSE;
        } else {
            display->seat_version = MIN (version, WL_POINTER_FRAME_SINCE_VERSION);
            display->seat = wl_registry_bind (registry,
                                              name,
                                              &wl_seat_interface,
                                              display->seat_version);
            static const struct wl_seat_listener seat_listener = {
                .capabilities = display_on_seat_capabilities,
                .name = display_on_seat_name,
            };
            wl_seat_add_listener (display->seat, &seat_listener, display);
        }
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
    } else {
        interface_used = FALSE;
    }
    g_debug ("%s '%s' interface obtained from the Wayland registry.",
             interface_used ? "Using" : "Ignoring", interface);
}

static void
registry_global_remove (void *data, struct wl_registry *registry, uint32_t name)
{
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
}



PwlDisplay*
pwl_display_connect (const char *name, GError **error)
{
    g_autoptr(PwlDisplay) self = g_slice_new0 (PwlDisplay);

    wl_list_init (&self->outputs);

    if (!(self->xkb_data.context = xkb_context_new (XKB_CONTEXT_NO_FLAGS))) {
        g_set_error_literal (error, PWL_ERROR, PWL_ERROR_UNAVAILABLE,
                             "Could not initialize XKB context");
        return NULL;
    }

    struct xkb_compose_table* compose_table =
        xkb_compose_table_new_from_locale (self->xkb_data.context,
                                           setlocale (LC_CTYPE, NULL),
                                           XKB_COMPOSE_COMPILE_NO_FLAGS);
    if (compose_table) {
        self->xkb_data.compose_state =
            xkb_compose_state_new (compose_table,
                                   XKB_COMPOSE_STATE_NO_FLAGS);
        g_clear_pointer (&compose_table, xkb_compose_table_unref);

        if (!self->xkb_data.compose_state) {
            g_set_error_literal (error, PWL_ERROR, PWL_ERROR_UNAVAILABLE,
                                 "Could not initialize XKB compose state");
            return NULL;
        }
    }

    /* Always have a default keymap, in case we never receive .keymap. */
    self->xkb_data.keymap =
        xkb_keymap_new_from_names (self->xkb_data.context,
                                   &((struct xkb_rule_names) { }),
                                   XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!self->xkb_data.keymap) {
        g_set_error_literal (error, PWL_ERROR, PWL_ERROR_UNAVAILABLE,
                             "Could not initialize XKB keymap");
        return NULL;
    }

    if (!(self->xkb_data.state = xkb_state_new (self->xkb_data.keymap))) {
        g_set_error_literal (error, PWL_ERROR, PWL_ERROR_UNAVAILABLE,
                             "Could not initialize XKB state");
        return NULL;
    }

    self->xkb_data.indexes.control = xkb_keymap_mod_get_index (self->xkb_data.keymap,
                                                               XKB_MOD_NAME_CTRL);
    self->xkb_data.indexes.alt = xkb_keymap_mod_get_index (self->xkb_data.keymap,
                                                           XKB_MOD_NAME_ALT);
    self->xkb_data.indexes.shift = xkb_keymap_mod_get_index (self->xkb_data.keymap,
                                                             XKB_MOD_NAME_SHIFT);

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

    if (!self->xdg_shell && !self->shell && !self->fshell
#ifdef HAVE_IVI_SHELL
        && !self->ivi_application
#endif /* HAVE_IVI_SHELL */
        )
    {
        g_set_error (error, PWL_ERROR, PWL_ERROR_WAYLAND,
                     "Wayland display does not support a shell "
                     "interface (%s, %s, "
#ifdef HAVE_IVI_SHELL
                     "%s, "
#endif /* HAVE_IVI_SHELL */
                     "or %s)",
                     xdg_wm_base_interface.name,
                     wl_shell_interface.name,
#ifdef HAVE_IVI_SHELL
                     ivi_application_interface.name,
#endif /* HAVE_IVI_SHELL */
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

    pwl_display_egl_deinit (self);

    if (self->display) {
        display_stop_keyboard_repeat (self);

        g_clear_pointer (&self->event_src, g_source_destroy);
        g_clear_pointer (&self->xdg_shell, xdg_wm_base_destroy);
        g_clear_pointer (&self->fshell, zwp_fullscreen_shell_v1_destroy);
        g_clear_pointer (&self->shell, wl_shell_destroy);
#ifdef HAVE_IVI_SHELL
        g_clear_pointer (&self->ivi_application, ivi_application_destroy);
#endif /* HAVE_IVI_SHELL */

        g_clear_pointer (&self->pointer, wl_pointer_destroy);
        g_clear_pointer (&self->keyboard, wl_keyboard_destroy);
        g_clear_pointer (&self->seat, wl_seat_destroy);
        g_clear_pointer (&self->compositor, wl_compositor_destroy);
        g_clear_pointer (&self->registry, wl_registry_destroy);

        wl_display_flush (self->display);
        g_clear_pointer (&self->display, wl_display_disconnect);
        g_clear_pointer (&self->application_id, g_free);
        g_clear_pointer (&self->window_title, g_free);

        g_clear_pointer (&self->xkb_data.state, xkb_state_unref);
        g_clear_pointer (&self->xkb_data.compose_state, xkb_compose_state_unref);
        g_clear_pointer (&self->xkb_data.keymap, xkb_keymap_unref);
        g_clear_pointer (&self->xkb_data.context, xkb_context_unref);
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
pwl_window_notify_touch_down (PwlWindow *self,
                              void (*callback) (PwlWindow*, const PwlTouch*, void*),
                              void *userdata)
{
    g_return_if_fail (self);

    self->on_touch_down = callback;
    self->on_touch_down_userdata = userdata;
}


void
pwl_window_notify_touch_up (PwlWindow *self,
                            void (*callback) (PwlWindow*, const PwlTouch*, void*),
                            void *userdata)
{
    g_return_if_fail (self);

    self->on_touch_up = callback;
    self->on_touch_up_userdata = userdata;
}


void
pwl_window_notify_touch_motion (PwlWindow *self,
                                void (*callback) (PwlWindow*, const PwlTouch*, void*),
                                void *userdata)
{
    g_return_if_fail (self);

    self->on_touch_motion = callback;
    self->on_touch_motion_userdata = userdata;
}


void
pwl_window_notify_keyboard (PwlWindow *self,
                            void (*callback) (PwlWindow*, const PwlKeyboard*, void*),
                            void *userdata)
{
    g_return_if_fail (self);

    self->on_keyboard = callback;
    self->on_keyboard_userdata = userdata;
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


#if 0
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
#endif


static inline void
resize_window (PwlWindow *window)
{
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

    return TRUE;
}


void pwl_display_egl_deinit (PwlDisplay *display)
{
    if (display->egl_display != EGL_NO_DISPLAY) {
        eglTerminate (display->egl_display);
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



PwlWindow*
pwl_window_create (PwlDisplay *display)
{
    g_return_val_if_fail (display, NULL);

    g_autoptr(PwlWindow) self = g_slice_new0 (PwlWindow);
    self->display = display;
    self->device_scale = 1;
    self->width = DEFAULT_WIDTH,
    self->height = DEFAULT_HEIGHT,
    self->wl_surface = wl_compositor_create_surface (display->compositor);
    wl_surface_set_user_data (self->wl_surface, self);

    static const struct wl_surface_listener surface_listener = {
        .enter = window_surface_on_enter,
        .leave = noop,
    };
    wl_surface_add_listener (self->wl_surface, &surface_listener, self);

#ifdef HAVE_IVI_SHELL
    if (display->ivi_application) {
        /* Do nothing, .ivi_surface is created when setting an ID. */
        configure_surface_geometry (self, 0, 0);
    } else
#endif /* HAVE_IVI_SHELL */
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

#ifdef HAVE_IVI_SHELL
    g_clear_pointer (&self->ivi_surface, ivi_surface_destroy);
#endif /* HAVE_IVI_SHELL */
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


#ifdef HAVE_IVI_SHELL
static void
window_ivi_surface_on_configure (void               *data,
                                 struct ivi_surface *surface G_GNUC_UNUSED,
                                 int32_t             width,
                                 int32_t             height)
{
    PwlWindow *self = data;
    g_assert (self->ivi_surface == surface);

    configure_surface_geometry (self, width, height);

    g_debug ("New ivi_surface configuration: (%" PRIi32 ", %" PRIi32 ")",
             width, height);

    resize_window (self);
}
#endif /* HAVE_IVI_SHELL */


void
pwl_window_set_id (PwlWindow *self,
                   uint32_t   id)
{
    g_return_if_fail (self);

#ifdef HAVE_IVI_SHELL
    if (self->display->ivi_application) {
        if (self->surface_id == id)
            return;

        g_clear_pointer (&self->ivi_surface, ivi_surface_destroy);
        self->surface_id = id;

        /*
         * For the invalid identifier, do not re-create the ivi_surface and
         * return early i.e. disable assigning a role to the Wayland surface.
         */
        if (self->surface_id == 0)
            return;

        self->ivi_surface =
            ivi_application_surface_create (self->display->ivi_application,
                                            self->surface_id,
                                            self->wl_surface);

        static const struct ivi_surface_listener listener = {
            .configure = window_ivi_surface_on_configure,
        };
        ivi_surface_add_listener (self->ivi_surface, &listener, self);

        return;
    }
#endif /* HAVE_IVI_SHELL */

    static bool warned = false;
    if (!warned) {
        warned = true;
        g_warning ("No available shell capable of using window identifiers.");
    }
}


uint32_t
pwl_window_get_id (const PwlWindow *self)
{
    g_return_val_if_fail (self, 0);

#ifdef HAVE_IVI_SHELL
    if (self->display->ivi_application)
        return self->surface_id;
#endif /* HAVE_IVI_SHELL */

    static bool warned = false;
    if (!warned) {
        warned = true;
        g_warning ("No available shell capable of using window identifiers.");
    }
    return 0;
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
    return self->device_scale;
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
