/*
 * pwl.c
 *
 * Copyright (C) 2020 Igalia S.L.
 * Copyright (C) 2019 Adrian Perez de Castro <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "pwl.h"
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <errno.h>
#include <gio/gio.h>
#include <string.h>
#include <sys/mman.h>

/* Needs to be included after wayland-egl.h */
#include "egl-proc-address.h"

#ifdef HAVE_XDG_SHELL
#include "xdg-shell-client.h"
#endif /* HAVE_XDG_SHELL */

#ifdef HAVE_XDG_SHELL_UNSTABLE_V6
#include "xdg-shell-unstable-v6-client.h"
#endif /* HAVE_XDG_SHELL_UNSTABLE_V6 */

#ifdef HAVE_FULLSCREEN_SHELL_UNSTABLE_V1
#include "fullscreen-shell-unstable-v1-client.h"
#endif /* HAVE_FULLSCREEN_SHELL_UNSTABLE_V1 */

#ifdef HAVE_IVI_APPLICATION
#include "ivi-application-client.h"
#endif /* HAVE_IVI_APPLICATION */

#if 0
# define TRACE(fmt, ...) g_debug ("%s: " fmt, G_STRFUNC, ##__VA_ARGS__)
#else
# define TRACE(fmt, ...) ((void) 0)
#endif


#ifndef EGL_CAST
#define EGL_CAST(type, value) ((type) (value))
#endif /* !EGL_CAST */

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


typedef struct {
    PwlDisplay         *display;  /* Reference to the PwlDisplay*/

    struct wl_seat     *seat;
    uint32_t            seat_name;
    uint32_t            seat_version;

    struct wl_pointer  *pointer;
    PwlWindow          *pointer_target;  /* Current target of pointer events. */

    struct wl_touch    *touch;
    PwlWindow          *touch_target;  /* Current target of touch events. */

    PwlXKBData          xkb_data;
    struct wl_keyboard *keyboard;
    PwlWindow          *keyboard_target; /* Current target of keyboard events. */
    int32_t             keyboard_repeat_rate;
    int32_t             keyboard_repeat_delay;
    uint32_t            keyboard_repeat_timestamp;
    uint32_t            keyboard_repeat_key;
    uint32_t            keyboard_repeat_state;
    GSource            *keyboard_repeat_source;

    struct wl_list link;
} PwlSeat;


struct _PwlDisplay {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;

    EGLDisplay                               egl_display;
    PFNEGLCREATEWAYLANDBUFFERFROMIMAGEWL     egl_createWaylandBufferFromImageWL;
    EGLConfig                                egl_config;               /* Lazy init. */
    EGLContext                               egl_context;              /* Lazy init. */

    struct wl_shell *shell;

#ifdef HAVE_XDG_SHELL
    struct xdg_wm_base *xdg_shell;
#endif /* HAVE_XDG_SHELL */

#ifdef HAVE_XDG_SHELL_UNSTABLE_V6
    struct zxdg_shell_v6 *zxdg_shell_v6;
#endif /* HAVE_XDG_SHELL_UNSTABLE_V6 */

#ifdef HAVE_FULLSCREEN_SHELL_UNSTABLE_V1
    struct zwp_fullscreen_shell_v1 *fshell;
#endif /* HAVE_FULLSCREEN_SHELL_UNSTABLE_V1 */

#ifdef HAVE_IVI_APPLICATION
    struct ivi_application *ivi_application;
#endif /* HAVE_IVI_APPLICATION */

    struct wl_list outputs; /* wl_list<PwlOutput> */
    struct wl_list seats; /* wl_list<PwlSeat> */

    char *application_id;
    char *window_title;
    GSource *source;

    bool wl_viv_present;

    bool (*on_capture_app_key) (PwlDisplay*, void *userdata);
    void *on_capture_app_key_userdata;
};

struct _PwlWindow {
    PwlDisplay *display;

    EGLNativeWindowType egl_window;
    EGLSurface          egl_window_surface;

    struct wl_surface *wl_surface;
    struct wl_surface_listener surface_listener;
    struct { int32_t x, y, w, h; } opaque_region;

    struct wl_shell_surface *shell_surface;

#ifdef HAVE_XDG_SHELL
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
#endif /* HAVE_XDG_SHELL */

#ifdef HAVE_XDG_SHELL_UNSTABLE_V6
    struct zxdg_surface_v6 *zxdg_surface_v6;
    struct zxdg_toplevel_v6 *zxdg_toplevel_v6;
#endif /* HAVE_XDG_SHELL_UNSTABLE_V6 */

#ifdef HAVE_IVI_APPLICATION
    struct ivi_surface *ivi_surface;
    uint32_t surface_id;
#endif /* HAVE_IVI_APPLICATION */

    uint32_t width;
    uint32_t height;
    uint32_t device_scale;

    PwlKeyboard keyboard;
    PwlPointer pointer;
    PwlTouch touch;
    PwlFocus focus;

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

    void (*focus_change_callback) (PwlWindow*, PwlFocus, void *userdata);
    void *focus_change_userdata;
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
    PwlSeat *seat = data;
    if (pointer != seat->pointer) {
        g_critical ("%s: Got pointer %p, expected %p.",
                    G_STRFUNC, pointer, seat->pointer);
        return;
    }

    PwlWindow *window = wl_surface_get_user_data (surface);
    seat->pointer_target = window;

    window->focus |= PWL_FOCUS_MOUSE;
    if (window->focus_change_callback) {
        (*window->focus_change_callback) (window,
                                          window->focus,
                                          window->focus_change_userdata);
    }

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
    PwlSeat *seat = data;
    if (pointer != seat->pointer) {
        g_critical ("%s: Got pointer %p, expected %p.",
                    G_STRFUNC, pointer, seat->pointer);
        return;
    }

    if (!seat->pointer_target) {
        g_critical ("%s: No current pointer event target!", G_STRFUNC);
        return;
    }

    if (surface != seat->pointer_target->wl_surface) {
        g_critical ("%s: Got leave for surface %p, but current window %p "
                    "has surface %p.", G_STRFUNC, surface,
                    seat->pointer_target,
                    seat->pointer_target->wl_surface);
        return;
    }

#ifdef WL_POINTER_FRAME_SINCE_VERSION
    if (seat->seat_version < WL_POINTER_FRAME_SINCE_VERSION) {
        window_dispatch_axis_event (seat->pointer_target);
    }
#endif /* WL_POINTER_FRAME_SINCE_VERSION */

    PwlWindow *window = seat->pointer_target;

    window->focus &= ~PWL_FOCUS_MOUSE;
    if (window->focus_change_callback) {
        (*window->focus_change_callback) (window,
                                          window->focus,
                                          window->focus_change_userdata);
    }

    g_debug ("%s: Pointer left surface %p, window %p.",
             G_STRFUNC, surface, seat->pointer_target);
}


static void
pointer_on_motion (void              *data,
                   struct wl_pointer *pointer,
                   uint32_t           timestamp,
                   wl_fixed_t         x,
                   wl_fixed_t         y)
{
    PwlSeat *seat = data;
    if (pointer != seat->pointer) {
        g_critical ("%s: Got pointer %p, expected %p.",
                    G_STRFUNC, pointer, seat->pointer);
        return;
    }

    PwlWindow *window = seat->pointer_target;
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
    PwlSeat *seat = data;
    if (pointer != seat->pointer) {
        g_critical ("%s: Got pointer %p, expected %p.",
                    G_STRFUNC, pointer, seat->pointer);
        return;
    }

    PwlWindow *window = seat->pointer_target;
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

    PwlSeat *seat = data;
    if (pointer != seat->pointer) {
        g_critical ("%s: Got pointer %p, expected %p.",
                    G_STRFUNC, pointer, seat->pointer);
        return;
    }

    PwlWindow *window = seat->pointer_target;
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
    if (seat->seat_version < WL_POINTER_FRAME_SINCE_VERSION)
#endif /* !WL_POINTER_FRAME_SINCE_VERSION */
        window_dispatch_axis_event (window);
}


#ifdef WL_POINTER_FRAME_SINCE_VERSION
static void
pointer_on_frame (void              *data,
                  struct wl_pointer *pointer)
{
    PwlSeat *seat = data;
    if (pointer != seat->pointer) {
        g_critical ("%s: Got pointer %p, expected %p.",
                    G_STRFUNC, pointer, seat->pointer);
        return;
    }

    PwlWindow *window = seat->pointer_target;
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

    PwlSeat *seat = data;
    if (pointer != seat->pointer) {
        g_critical ("%s: Got pointer %p, expected %p.",
                    G_STRFUNC, pointer, seat->pointer);
        return;
    }

    PwlWindow *window = seat->pointer_target;
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

    PwlSeat *seat = data;
    if (pointer != seat->pointer) {
        g_critical ("%s: Got pointer %p, expected %p.",
                    G_STRFUNC, pointer, seat->pointer);
        return;
    }

    PwlWindow *window = seat->pointer_target;
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

    PwlSeat *seat = data;
    seat->touch_target = window;

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

    PwlSeat *seat = data;
    PwlWindow *window = seat->touch_target;

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

    PwlSeat *seat = data;
    PwlWindow *window = seat->touch_target;
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
    PwlSeat *seat = data;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close (fd);
        return;
    }

    const int map_mode = (seat->seat_version > 6) ? MAP_PRIVATE : MAP_SHARED;
    void* mapping = mmap (NULL, size, PROT_READ, map_mode, fd, 0);
    close (fd);

    if (mapping == MAP_FAILED) {
        g_critical ("%s: Could not mmap new XKB keymap (%s).",
                    G_STRFUNC, strerror (errno));
        return;
    }

    struct xkb_keymap *keymap =
        xkb_keymap_new_from_string (seat->xkb_data.context,
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

    g_clear_pointer (&seat->xkb_data.state, xkb_state_unref);
    g_clear_pointer (&seat->xkb_data.keymap, xkb_keymap_unref);

    seat->xkb_data.keymap = g_steal_pointer (&keymap);
    seat->xkb_data.state = g_steal_pointer (&state);

    seat->xkb_data.indexes.control = xkb_keymap_mod_get_index (seat->xkb_data.keymap,
                                                               XKB_MOD_NAME_CTRL);
    seat->xkb_data.indexes.alt = xkb_keymap_mod_get_index (seat->xkb_data.keymap,
                                                           XKB_MOD_NAME_ALT);
    seat->xkb_data.indexes.shift = xkb_keymap_mod_get_index (seat->xkb_data.keymap,
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
    PwlSeat *seat = data;
    if (keyboard != seat->keyboard) {
        g_critical ("%s: Got keyboard %p, expected %p.",
                    G_STRFUNC, keyboard, seat->keyboard);
        return;
    }

    PwlWindow *window = wl_surface_get_user_data (surface);
    seat->keyboard_target = window;

    seat->keyboard_target->keyboard.serial = serial;

    window->focus |= PWL_FOCUS_KEYBOARD;
    if (window->focus_change_callback) {
        (*window->focus_change_callback) (window,
                                          window->focus,
                                          window->focus_change_userdata);
    }

    g_debug ("%s: Target surface %p, window %p.", G_STRFUNC, surface, window);
}

void
keyboard_on_leave (void               *data,
                   struct wl_keyboard *keyboard,
                   uint32_t            serial,
                   struct wl_surface  *surface)
{
    PwlSeat *seat = data;
    if (keyboard != seat->keyboard) {
        g_critical ("%s: Got keyboard %p, expected %p.",
                    G_STRFUNC, keyboard, seat->keyboard);
        return;
    }

    if (!seat->keyboard_target) {
        g_critical ("%s: No current keyboard event target!", G_STRFUNC);
        return;
    }

    if (surface != seat->keyboard_target->wl_surface) {
        g_critical ("%s: Got leave for surface %p, but current window %p "
                    "has surface %p.", G_STRFUNC, surface,
                    seat->keyboard_target,
                    seat->keyboard_target->wl_surface);
        return;
    }

    PwlWindow *window = seat->keyboard_target;
    window->keyboard.serial = serial;

    window->focus &= ~PWL_FOCUS_KEYBOARD;
    if (window->focus_change_callback) {
        (*window->focus_change_callback) (window,
                                          window->focus,
                                          window->focus_change_userdata);
    }

    g_debug ("%s: Keyboard left surface %p, window %p.",
             G_STRFUNC, surface, seat->keyboard_target);
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
seat_on_keyboard_repeat_timeout (PwlSeat *self)
{
    if (!self->keyboard_target) {
        g_critical ("%s: No keyboard event target window!", G_STRFUNC);
        /*
         * We cannot use seat_stop_keyboard_repeat() here because it
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
seat_stop_keyboard_repeat (PwlSeat *self)
{
    g_debug ("Seat @ %p: Stopping the keyboard repeat timer.",
             self->seat);

    if (!self->keyboard_repeat_source)
        return;

    g_source_destroy (self->keyboard_repeat_source);
    g_clear_pointer (&self->keyboard_repeat_source, g_source_unref);
}


static void
seat_start_keyboard_repeat (PwlSeat    *self,
                            uint32_t    timestamp,
                            uint32_t    key,
                            uint32_t    state)
{
    seat_stop_keyboard_repeat (self);

    self->keyboard_repeat_source =
        g_timeout_source_new (self->keyboard_repeat_delay);

    g_source_set_callback (self->keyboard_repeat_source,
                           (GSourceFunc) seat_on_keyboard_repeat_timeout,
                           self,
                           NULL);

    GMainContext *context;
    if (self->display->source) {
        context = g_source_get_context (self->display->source);
    } else {
        static bool warned = false;
        if (!warned) {
            g_warning ("pwl_display_attach_sources() was never called for"
                       " display @ %p, using the default GMainContext as"
                       " fall-back for handling key repeat.", self->display);
            warned = true;
        }
        context = g_main_context_default ();
    }
    g_source_attach (self->keyboard_repeat_source, context);
}


void
keyboard_on_key (void               *data,
                 struct wl_keyboard *keyboard,
                 uint32_t            serial,
                 uint32_t            timestamp,
                 uint32_t            key,
                 uint32_t            state)
{
    PwlSeat *seat = data;
    if (keyboard != seat->keyboard) {
        g_critical ("%s: Got keyboard %p, expected %p.",
                    G_STRFUNC, keyboard, seat->keyboard);
        return;
    }

    PwlWindow *window = seat->keyboard_target;
    if (!window) {
        g_critical ("%s: No current keyboard event target!", G_STRFUNC);
        return;
    }

    /* XXX: Investigate why is this necessary. */
    key += 8;

    window->keyboard.serial = serial;
    window_dispatch_key_event (window,
                               &seat->xkb_data,
                               timestamp,
                               key,
                               state);

    if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        seat_stop_keyboard_repeat (seat);
        return;
    }

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED &&
        seat->keyboard_repeat_rate &&
        xkb_keymap_key_repeats (seat->xkb_data.keymap, key))
    {
        seat_start_keyboard_repeat (seat, timestamp, key, state);
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
    PwlSeat *seat = data;

    if (!seat->xkb_data.state) {
        g_warning ("%s: XKB state not yet configured!", G_STRFUNC);
        return;
    }

    xkb_state_update_mask (seat->xkb_data.state,
                           mods_depressed,
                           mods_latched,
                           mods_locked,
                           0,
                           0,
                           group);

    seat->xkb_data.modifiers = 0;
    uint32_t component
        = (XKB_STATE_MODS_DEPRESSED | XKB_STATE_MODS_LATCHED);

    if (xkb_state_mod_index_is_active (seat->xkb_data.state,
                                       seat->xkb_data.indexes.control,
                                       component)) {
        seat->xkb_data.modifiers |= seat->xkb_data.modifier.control;
    }
    if (xkb_state_mod_index_is_active (seat->xkb_data.state,
                                       seat->xkb_data.indexes.alt,
                                       component)) {
        seat->xkb_data.modifiers |= seat->xkb_data.modifier.alt;
    }
    if (xkb_state_mod_index_is_active (seat->xkb_data.state,
                                       seat->xkb_data.indexes.shift,
                                       component)) {
        seat->xkb_data.modifiers |= seat->xkb_data.modifier.shift;
    }
}

void
keyboard_on_repeat_info (void               *data,
                         struct wl_keyboard *keyboard,
                         int32_t             rate,
                         int32_t             delay)
{
    PwlSeat *seat = data;
    if (keyboard != seat->keyboard) {
        return;
    }

    seat->keyboard_repeat_rate = rate;
    seat->keyboard_repeat_delay = delay;

    if (!rate)
        seat_stop_keyboard_repeat (seat);

    g_debug ("%s: Set keyboard repeat rate=%" PRIi32 ", delay=%" PRIi32 ".",
             G_STRFUNC, rate, delay);
}


static void
display_on_seat_capabilities (void           *data,
                              struct wl_seat *wl_seat,
                              uint32_t        capabilities)
{
    PwlSeat *seat = data;

    g_assert (seat);
    g_assert (seat->seat == wl_seat);

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
        if (!seat->pointer) {
            seat->pointer = wl_seat_get_pointer (wl_seat);
            wl_pointer_add_listener (seat->pointer, &pointer_listener, seat);
            g_debug ("  - New pointer %p.", seat->pointer);
        } else {
            g_warning ("Multiple pointers per seat are currently unsupported.");
        }
    } else if (seat->pointer) {
        g_debug ("  - Pointer %p gone.", seat->pointer);
        g_clear_pointer (&seat->pointer, wl_pointer_release);
    }

    /* Keyboard */
    const bool has_keyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;
    if (has_keyboard) {
        seat->keyboard = wl_seat_get_keyboard (wl_seat);
        static const struct wl_keyboard_listener keyboard_listener = {
            .keymap = keyboard_on_keymap,
            .enter = keyboard_on_enter,
            .leave = keyboard_on_leave,
            .key = keyboard_on_key,
            .modifiers = keyboard_on_modifiers,
            .repeat_info = keyboard_on_repeat_info,
        };
        wl_keyboard_add_listener (seat->keyboard, &keyboard_listener, seat);
        g_debug ("  - New keyboard %p.", seat->keyboard);
    } else if (!has_keyboard && seat->keyboard) {
        g_debug ("  - Keyboard %p gone.", seat->keyboard);
        g_clear_pointer (&seat->keyboard, wl_keyboard_release);
    }

    /* Touch */
    const bool has_touch = capabilities & WL_SEAT_CAPABILITY_TOUCH;
    if (has_touch) {
        seat->touch = wl_seat_get_touch (wl_seat);
        static const struct wl_touch_listener touch_listener = {
            .down = touch_on_down,
            .up = touch_on_up,
            .motion = touch_on_motion,
            .frame = touch_on_frame,
            .cancel = touch_on_cancel,
        };
        wl_touch_add_listener (seat->touch, &touch_listener, seat);
        g_debug ("  - New touch input %p.", seat->touch);
    } else if (!has_touch && seat->touch) {
        g_debug ("  - Touch input %p gone.", seat->touch);
        g_clear_pointer (&seat->touch, wl_touch_release);
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


#ifdef HAVE_XDG_SHELL
static void
xdg_shell_ping (void *data, struct xdg_wm_base *shell, uint32_t serial)
{
    xdg_wm_base_pong (shell, serial);
}
#endif /* HAVE_XDG_SHELL */


#ifdef HAVE_XDG_SHELL_UNSTABLE_V6
static void
zxdg_shell_v6_ping (void                 *data G_GNUC_UNUSED,
                    struct zxdg_shell_v6 *shell,
                    uint32_t              serial)
{
    zxdg_shell_v6_pong (shell, serial);
}
#endif /* HAVE_XDG_SHELL_UNSTABLE_V6 */


static void
display_add_seat (PwlDisplay *display,
                  struct wl_seat *wl_seat,
                  uint32_t name,
                  uint32_t version)
{
    g_assert (display);

    PwlSeat *seat = g_slice_new0 (PwlSeat);
    seat->seat = wl_seat;
    seat->seat_name = name;
    seat->seat_version = version;
    seat->display = display;
    wl_list_init (&seat->link);
    wl_list_insert (&display->seats, &seat->link);

    static const struct wl_seat_listener seat_listener = {
        .capabilities = display_on_seat_capabilities,
        .name = display_on_seat_name,
    };
    wl_seat_add_listener (wl_seat, &seat_listener, seat);

    if (!(seat->xkb_data.context = xkb_context_new (XKB_CONTEXT_NO_FLAGS))) {
        g_error ("Could not initialize XKB context");
        return;
    }

    struct xkb_compose_table* compose_table =
        xkb_compose_table_new_from_locale (seat->xkb_data.context,
                                           setlocale (LC_CTYPE, NULL),
                                           XKB_COMPOSE_COMPILE_NO_FLAGS);
    if (compose_table) {
        seat->xkb_data.compose_state =
            xkb_compose_state_new (compose_table,
                                   XKB_COMPOSE_STATE_NO_FLAGS);
        g_clear_pointer (&compose_table, xkb_compose_table_unref);

        if (!seat->xkb_data.compose_state) {
            g_error ("Could not initialize XKB compose state");
            return;
        }
    }

    /* Always have a default keymap, in case we never receive .keymap. */
    seat->xkb_data.keymap =
        xkb_keymap_new_from_names (seat->xkb_data.context,
                                   &((struct xkb_rule_names) { }),
                                   XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!seat->xkb_data.keymap) {
        g_error ("Could not initialize XKB keymap");
        return;
    }

    if (!(seat->xkb_data.state = xkb_state_new (seat->xkb_data.keymap))) {
        g_error ("Could not initialize XKB state");
        return;
    }

    seat->xkb_data.indexes.control = xkb_keymap_mod_get_index (seat->xkb_data.keymap,
                                                               XKB_MOD_NAME_CTRL);
    seat->xkb_data.indexes.alt = xkb_keymap_mod_get_index (seat->xkb_data.keymap,
                                                           XKB_MOD_NAME_ALT);
    seat->xkb_data.indexes.shift = xkb_keymap_mod_get_index (seat->xkb_data.keymap,
                                                             XKB_MOD_NAME_SHIFT);
}

static void
pwl_seat_destroy (PwlSeat *self)
{
    g_assert (self != NULL);

    seat_stop_keyboard_repeat (self);

    g_debug ("%s: Destroying @ %p", G_STRFUNC, self);
    g_clear_pointer (&self->pointer, wl_pointer_destroy);
    g_clear_pointer (&self->keyboard, wl_keyboard_destroy);
    g_clear_pointer (&self->seat, wl_seat_destroy);

    g_clear_pointer (&self->xkb_data.state, xkb_state_unref);
    g_clear_pointer (&self->xkb_data.compose_state, xkb_compose_state_unref);
    g_clear_pointer (&self->xkb_data.keymap, xkb_keymap_unref);
    g_clear_pointer (&self->xkb_data.context, xkb_context_unref);

    wl_list_remove (&self->link);
    g_slice_free (PwlSeat, self);
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

#ifdef HAVE_XDG_SHELL
    if (strcmp (interface, xdg_wm_base_interface.name) == 0) {
        display->xdg_shell = wl_registry_bind (registry,
                                               name,
                                               &xdg_wm_base_interface,
                                               version);
        g_assert (display->xdg_shell);
        static const struct xdg_wm_base_listener xdg_shell_listener = {
            .ping = xdg_shell_ping,
        };
        xdg_wm_base_add_listener (display->xdg_shell, &xdg_shell_listener, NULL);
    } else
#endif /* HAVE_XDG_SHELL */
#if HAVE_XDG_SHELL_UNSTABLE_V6
    if (strcmp (interface, zxdg_shell_v6_interface.name) == 0) {
        display->zxdg_shell_v6 = wl_registry_bind (registry,
                                                   name,
                                                   &zxdg_shell_v6_interface,
                                                   version);
        static const struct zxdg_shell_v6_listener zxdg_shell_v6_listener = {
            .ping = zxdg_shell_v6_ping,
        };
        zxdg_shell_v6_add_listener (display->zxdg_shell_v6,
                                    &zxdg_shell_v6_listener,
                                    NULL);
    } else
#endif /* HAVE_XDG_SHELL_UNSTABLE_V6 */
#ifdef HAVE_FULLSCREEN_SHELL_UNSTABLE_V1
    if (strcmp (interface,
                zwp_fullscreen_shell_v1_interface.name) == 0) {
        display->fshell = wl_registry_bind (registry,
                                            name,
                                            &zwp_fullscreen_shell_v1_interface,
                                            version);
    } else 
#endif /* HAVE_FULLSCREEN_SHELL_UNSTABLE_V1 */
#ifdef HAVE_IVI_APPLICATION
    if (strcmp (interface, ivi_application_interface.name) == 0) {
        display->ivi_application = wl_registry_bind (registry,
                                                     name,
                                                     &ivi_application_interface,
                                                     version);
    } else
#endif /* HAVE_IVI_APPLICATION */
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
    } else if (strcmp (interface, wl_seat_interface.name) == 0) {
        uint32_t seat_version = MIN (version, WL_POINTER_FRAME_SINCE_VERSION);
        struct wl_seat *wl_seat = wl_registry_bind (registry,
                                                    name,
                                                    &wl_seat_interface,
                                                    seat_version);

        display_add_seat(display, wl_seat, name, seat_version);
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
        if (strcmp (interface, "wl_viv") == 0)
            display->wl_viv_present = true;

        interface_used = FALSE;
    }
    g_debug ("%s '%s' interface obtained from the Wayland registry.",
             interface_used ? "Using" : "Ignoring", interface);
}

static void
registry_global_remove (void *data, struct wl_registry *registry, uint32_t name)
{
    PwlDisplay *display = data;
    PwlOutput *output, *tmp_output;
    wl_list_for_each_safe (output, tmp_output, &display->outputs, link) {
        if (output->output_name == name) {
            g_debug ("Output #%"PRIi32" @ %p: Removed.",
                     output->output_name, output->output);
            g_clear_pointer (&output->output, wl_output_release);
            wl_list_remove (&output->link);
            g_slice_free (PwlOutput, output);
            break;
        }
    }
    PwlSeat *seat, *tmp_seat;
    wl_list_for_each_safe (seat, tmp_seat, &display->seats, link) {
        if (seat->seat_name == name) {
            pwl_seat_destroy(seat);
            break;
        }
    }
}


PwlDisplay*
pwl_display_connect (const char *name, GError **error)
{
    g_autoptr(PwlDisplay) self = g_slice_new0 (PwlDisplay);

    wl_list_init (&self->outputs);
    wl_list_init (&self->seats);

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

    if (!self->shell
#ifdef HAVE_XDG_SHELL
        && !self->xdg_shell
#endif /* HAVE_XDG_SHELL */
#ifdef HAVE_XDG_SHELL_UNSTABLE_V6
        && !self->zxdg_shell_v6
#endif /* HAVE_XDG_SHELL_UNSTABLE_V6 */
#ifdef HAVE_FULLSCREEN_SHELL_UNSTABLE_V1
        && !self->fshell
#endif /* HAVE_FULLSCREEN_SHELL_UNSTABLE_V1 */
#ifdef HAVE_IVI_APPLICATION
        && !self->ivi_application
#endif /* HAVE_IVI_APPLICATION */
        )
    {
        g_set_error_literal (error, PWL_ERROR, PWL_ERROR_WAYLAND,
                             "Wayland display does not support a shell "
                             "interface (supported: "
#ifdef HAVE_XDG_SHELL
                             "xdg_wm_base, "
#endif /* HAVE_XDG_SHELL */
#ifdef HAVE_XDG_SHELL_UNSTABLE_V6
                             "zxdg_shell_v6, "
#endif /* HAVE_XDG_SHELL_UNSTABLE_V6 */
#ifdef HAVE_FULLSCREEN_SHELL_UNSTABLE_V1
                             "fullscreen_shell, "
#endif /* HAVE_FULLSCREEN_SHELL_UNSTABLE_V1 */
#ifdef HAVE_IVI_APPLICATION
                             "ivi_application, "
#endif /* HAVE_IVI_APPLICATION */
                             "wl_shell)");
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
        if (self->source) {
            g_source_destroy (self->source);
            g_clear_pointer (&self->source, g_source_unref);
        }
        g_clear_pointer (&self->shell, wl_shell_destroy);

#ifdef HAVE_XDG_SHELL
        g_clear_pointer (&self->xdg_shell, xdg_wm_base_destroy);
#endif /* HAVE_XDG_SHELL */

#ifdef HAVE_XDG_SHELL_UNSTABLE_V6
        g_clear_pointer (&self->zxdg_shell_v6, zxdg_shell_v6_destroy);
#endif /* HAVE_XDG_SHELL_UNSTABLE_V6 */

#ifdef HAVE_FULLSCREEN_SHELL_UNSTABLE_V1
        g_clear_pointer (&self->fshell, zwp_fullscreen_shell_v1_destroy);
#endif /* HAVE_FULLSCREEN_SHELL_UNSTABLE_V1 */

#ifdef HAVE_IVI_APPLICATION
        g_clear_pointer (&self->ivi_application, ivi_application_destroy);
#endif /* HAVE_IVI_APPLICATION */

        PwlSeat *item, *tmp;
        wl_list_for_each_safe (item, tmp, &self->seats, link) {
            pwl_seat_destroy(item);
        }

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


typedef struct {
    GSource            base;
    struct wl_display *display;
    void              *fd_tag;
    bool               reading;
    int                error;
} PwlSource;


static gboolean
pwl_source_prepare (GSource *source,
                    int     *timeout)
{
    PwlSource *self = (PwlSource*) source;

    *timeout = -1;

    if (self->reading)
        return FALSE;

    if (wl_display_prepare_read (self->display) != 0)
        return TRUE;

    if (wl_display_flush (self->display) < 0) {
        g_warning ("%s: wl_display_flush: %s",
                   G_STRFUNC, strerror (self->error = errno));
        return TRUE;
    }

    self->reading = true;
    return FALSE;
}

static gboolean
pwl_source_check (GSource *source)
{
    PwlSource *self = (PwlSource*) source;

    if (self->error) {
        TRACE ("has error, returning early.");
        return TRUE;
    }

    const GIOCondition events = g_source_query_unix_fd (source, self->fd_tag);

    if (self->reading) {
        if (events & G_IO_IN) {
            if (wl_display_read_events (self->display) < 0)
                g_warning ("%s: wl_display_read_events: %s",
                           G_STRFUNC, strerror (self->error = errno));
        } else {
            wl_display_cancel_read (self->display);
        }
        self->reading = false;
    }

    return events;
}

static gboolean
pwl_source_dispatch (GSource    *source,
                     GSourceFunc callback G_GNUC_UNUSED,
                     void       *userdata G_GNUC_UNUSED)
{
    PwlSource *self = (PwlSource*) source;

    const GIOCondition events = g_source_query_unix_fd (source, self->fd_tag);

    if (self->error || (events & G_IO_ERR)) {
        if (self->error) {
            g_critical ("%s: removing source due to error: %s",
                        G_STRFUNC, strerror (self->error));
        } else {
            g_critical ("%s: removing source due to unknown error.", G_STRFUNC);
        }
        return G_SOURCE_REMOVE;
    }

    if (events & G_IO_HUP) {
        TRACE ("removing source due to hangup");
        return G_SOURCE_REMOVE;
    }

    if (wl_display_dispatch_pending (self->display) < 0) {
        g_critical ("%s: wl_display_dispatch_pending: %s", G_STRFUNC, strerror (errno));
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

static GSource*
pwl_source_new (struct wl_display *display)
{
    g_assert (display);

    static GSourceFuncs funcs = {
        .prepare  = pwl_source_prepare,
        .check    = pwl_source_check,
        .dispatch = pwl_source_dispatch,
    };
    PwlSource *self = (PwlSource*) g_source_new (&funcs, sizeof (PwlSource));
    self->reading = false;
    self->display = display;
    self->fd_tag = g_source_add_unix_fd (&self->base,
                                         wl_display_get_fd (display),
                                         G_IO_IN | G_IO_ERR | G_IO_HUP);
    g_source_set_priority (&self->base, G_PRIORITY_HIGH + 30);
    g_source_set_can_recurse (&self->base, TRUE);
    g_source_set_name (&self->base, "PwlSource");

    g_debug ("%s: Created PwlSource @ %p, fd#%d.",
             G_STRFUNC, self, wl_display_get_fd (display));
    return &self->base;
}

void
pwl_display_attach_sources (PwlDisplay   *self,
                            GMainContext *context)
{
    g_return_if_fail (self);

    if (!self->source) {
        self->source = pwl_source_new (self->display);
        g_source_attach (self->source, context);
        TRACE ("display @ %p, attached source @ %p to context @ %p.",
               self, self->source, context);
    }
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
    if (window->egl_window) {
        wl_egl_window_resize (window->egl_window,
                              window->width,
                              window->height,
                              0,
                              0);
    }

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


bool
pwl_display_egl_init (PwlDisplay  *display,
                      PwlEglConfig config,
                      GError     **error)
{
    g_return_val_if_fail (config == PWL_EGL_CONFIG_MINIMAL ||
                          config == PWL_EGL_CONFIG_FULL,
                          false);

    g_debug ("Initializing EGL...");

    display->egl_display = eglGetDisplay ((EGLNativeDisplayType) display->display);
    if (display->egl_display == EGL_NO_DISPLAY) {
        set_egl_error (error, eglGetError (), "Could not get display");
        return false;
    }

    EGLint major, minor;
    if (!eglInitialize (display->egl_display, &major, &minor)) {
        set_egl_error (error, eglGetError (), "Initialization failed");
        pwl_display_egl_deinit (display);
        return false;
    }
    g_info ("EGL version %d.%d initialized.", major, minor);

    /*
     * TODO: Use eglQueryString() to find out whether required extensions are
     *       available before trying to actually use functions obtained below
     *       with eglGetProcAddress().
     */

    display->egl_createWaylandBufferFromImageWL = (PFNEGLCREATEWAYLANDBUFFERFROMIMAGEWL)
        load_egl_proc_address ("eglCreateWaylandBufferFromImageWL");
    if (!display->egl_createWaylandBufferFromImageWL) {
        set_egl_error (error, eglGetError (),
                       "eglCreateWaylandBufferFromImageWL unsupported");
        pwl_display_egl_deinit (display);
        return false;
    }

    if (config == PWL_EGL_CONFIG_FULL) {
        if (!eglBindAPI (EGL_OPENGL_ES_API)) {
            set_egl_error (error, eglGetError (), "Cannot bind OpenGL ES API");
            pwl_display_egl_deinit (display);
            return false;
        }

        static const EGLint config_attrs[] = {
            EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
            EGL_RED_SIZE,        8,
            EGL_GREEN_SIZE,      8,
            EGL_BLUE_SIZE,       8,
            EGL_ALPHA_SIZE,      8,
            EGL_DEPTH_SIZE,      0,
            EGL_STENCIL_SIZE,    0,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_SAMPLES,         0,
            EGL_NONE,
        };

        EGLint count = 0;
        if (!eglChooseConfig (display->egl_display,
                              config_attrs,
                              &display->egl_config,
                              1,
                              &count) || count == 0) {
            set_egl_error (error, eglGetError (),
                           "Could not choose a suitable  configuration");
            pwl_display_egl_deinit (display);
            return false;
        }

        static const EGLint context_attrs[] = {
            EGL_CONTEXT_CLIENT_VERSION, 2,
            EGL_NONE,
        };

        display->egl_context = eglCreateContext (display->egl_display,
                                                 display->egl_config,
                                                 EGL_NO_CONTEXT,
                                                 context_attrs);
        if (display->egl_context == EGL_NO_CONTEXT) {
            set_egl_error (error, eglGetError (),
                           "Could not create EGL context");
            pwl_display_egl_deinit (display);
            return false;
        }
    }

    return true;
}


void pwl_display_egl_deinit (PwlDisplay *display)
{
    if (display->egl_display != EGL_NO_DISPLAY) {
        eglTerminate (display->egl_display);
        display->egl_display = EGL_NO_DISPLAY;
    }
    eglReleaseThread ();
}


static bool
pwl_window_egl_create_surface (PwlWindow *self,
                               GError   **error)
{
    g_return_val_if_fail (self, false);
    g_return_val_if_fail (self->egl_window_surface == EGL_NO_SURFACE, false);

    self->egl_window_surface = eglCreateWindowSurface (self->display->egl_display,
                                                       self->display->egl_config,
                                                       self->egl_window,
                                                       NULL);
    if (self->egl_window_surface == EGL_NO_SURFACE) {
        set_egl_error (error, eglGetError (),
                       "Cannot create EGL window surface");
        return false;
    }

    return true;
}


bool
pwl_window_egl_make_current (PwlWindow *self,
                             GError   **error)
{
    g_return_val_if_fail (self, false);
    g_return_val_if_fail (self->egl_window_surface != EGL_NO_SURFACE, false);

    if (eglMakeCurrent (self->display->egl_display,
                        self->egl_window_surface,
                        self->egl_window_surface,
                        self->display->egl_context))
        return true;

    set_egl_error (error, eglGetError (), "Cannot enable EGL context");
    return false;
}


bool
pwl_window_egl_swap_buffers (PwlWindow *self,
                             GError   **error)
{
    g_return_val_if_fail (self, false);
    g_return_val_if_fail (self->egl_window_surface != EGL_NO_SURFACE, false);

    if (eglSwapBuffers (self->display->egl_display,
                        self->egl_window_surface))
        return true;

    set_egl_error (error, eglGetError (), "Cannot swap EGL buffers");
    return false;
}


EGLDisplay
pwl_display_egl_get_display (const PwlDisplay *self)
{
    g_return_val_if_fail (self, EGL_NO_DISPLAY);
    return self->egl_display;
}


EGLContext
pwl_display_egl_get_context (const PwlDisplay *self)
{
    g_return_val_if_fail (self, EGL_NO_CONTEXT);
    return self->egl_context;
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


bool
pwl_display_egl_has_broken_buffer_from_image (const PwlDisplay *self)
{
    g_return_val_if_fail (self, false);

    /*
     * The Vivante proprietary driver has a bug that causes file
     * descriptors for buffers backing EGLImages to be leaked after
     * using eglCreateWaylandBufferFromImageWL().
     */
    return self->wl_viv_present;
}


#ifdef HAVE_XDG_SHELL
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
#endif /* HAVE_XDG_SHELL */


#ifdef HAVE_XDG_SHELL_UNSTABLE_V6
static void
zxdg_surface_v6_on_configure (void                   *data G_GNUC_UNUSED,
                              struct zxdg_surface_v6 *surface,
                              uint32_t                serial)
{
    zxdg_surface_v6_ack_configure (surface, serial);
}

static void
zxdg_toplevel_v6_on_configure (void                    *data,
                               struct zxdg_toplevel_v6 *toplevel G_GNUC_UNUSED,
                               int32_t                  width,
                               int32_t                  height,
                               struct wl_array         *states G_GNUC_UNUSED)
{
    PwlWindow *window = data;
    g_assert (window->zxdg_toplevel_v6 == toplevel);

    configure_surface_geometry (window, width, height);

    g_debug ("New ZXDGv6 toplevel configuration: (%" PRIi32 ", %" PRIi32 ")",
             width, height);

    resize_window (window);
}

static void
zxdg_toplevel_v6_on_close (void                    *data,
                           struct zxdg_toplevel_v6 *toplevel G_GNUC_UNUSED)
{
    PwlWindow *window = data;
    g_assert (window->zxdg_toplevel_v6 == toplevel);

    g_warning ("%s: Unimplemented.", G_STRFUNC);
}
#endif /* HAVE_XDG_SHELL_UNSTABLE_V6 */


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

#ifdef HAVE_IVI_APPLICATION
    if (display->ivi_application) {
        /* Do nothing, .ivi_surface is created when setting an ID. */
        configure_surface_geometry (self, 0, 0);
    } else
#endif /* HAVE_IVI_APPLICATION */
#ifdef HAVE_XDG_SHELL
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
    } else
#endif /* HAVE_XDG_SHELL */
#ifdef HAVE_XDG_SHELL_UNSTABLE_V6
    if (display->zxdg_shell_v6) {
        self->zxdg_surface_v6 =
            zxdg_shell_v6_get_xdg_surface (display->zxdg_shell_v6,
                                           self->wl_surface);
        static const struct zxdg_surface_v6_listener surface_listener = {
            .configure = zxdg_surface_v6_on_configure,
        };
        zxdg_surface_v6_add_listener (self->zxdg_surface_v6,
                                      &surface_listener,
                                      self);

        self->zxdg_toplevel_v6 =
            zxdg_surface_v6_get_toplevel (self->zxdg_surface_v6);
        static const struct zxdg_toplevel_v6_listener toplevel_listener = {
            .configure = zxdg_toplevel_v6_on_configure,
            .close = zxdg_toplevel_v6_on_close,
        };
        zxdg_toplevel_v6_add_listener (self->zxdg_toplevel_v6,
                                       &toplevel_listener,
                                       self);
    } else
#endif /* HAVE_XDG_SHELL_UNSTABLE_V6 */
#ifdef HAVE_FULLSCREEN_SHELL_UNSTABLE_V1
    if (display->fshell) {
        zwp_fullscreen_shell_v1_present_surface (display->fshell,
                                                 self->wl_surface,
                                                 ZWP_FULLSCREEN_SHELL_V1_PRESENT_METHOD_DEFAULT,
                                                 NULL);
        self->is_fullscreen = true;
    } else
#endif /* HAVE_FULLSCREEN_SHELL_UNSTABLE_V1 */
    if (display->shell) {
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

    if (display->egl_context != EGL_NO_CONTEXT) {
        self->egl_window = wl_egl_window_create (self->wl_surface,
                                                 self->width,
                                                 self->height);

        g_autoptr(GError) error = NULL;
        if (!self->egl_window) {
            g_error ("Cannot create Wayland-EGL window");
        } else if (!pwl_window_egl_create_surface (self, &error)) {
            g_error ("Cannot create EGL window surface: %s", error->message);
        }
    }

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
    PwlDisplay *display = self->display;
    PwlSeat *seat, *tmp_seat;
    wl_list_for_each_safe (seat, tmp_seat, &display->seats, link) {
        if (seat->keyboard_target == self)
            seat->keyboard_target = NULL;

        if (seat->pointer_target == self)
            seat->pointer_target = NULL;

    }
    if (self->egl_window_surface != EGL_NO_SURFACE) {
        eglDestroySurface (self->display->egl_display,
                           self->egl_window_surface);
        self->egl_window_surface = EGL_NO_SURFACE;
    }

    g_clear_pointer (&self->egl_window, wl_egl_window_destroy);

#ifdef HAVE_IVI_APPLICATION
    g_clear_pointer (&self->ivi_surface, ivi_surface_destroy);
#endif /* HAVE_IVI_APPLICATION */

#ifdef HAVE_XDG_SHELL
    g_clear_pointer (&self->xdg_toplevel, xdg_toplevel_destroy);
    g_clear_pointer (&self->xdg_surface, xdg_surface_destroy);
#endif /* HAVE_XDG_SHELL */

#ifdef HAVE_XDG_SHELL_UNSTABLE_V6
    g_clear_pointer (&self->zxdg_toplevel_v6, zxdg_toplevel_v6_destroy);
    g_clear_pointer (&self->zxdg_surface_v6, zxdg_surface_v6_destroy);
#endif /* HAVE_XDG_SHELL_UNSTABLE_V6 */

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

#ifdef HAVE_XDG_SHELL
    if (self->display->xdg_shell) {
        xdg_toplevel_set_app_id (self->xdg_toplevel, application_id);
    } else
#endif /* HAVE_XDG_SHELL */
#ifdef HAVE_XDG_SHELL_UNSTABLE_V6
    if (self->display->zxdg_shell_v6) {
        zxdg_toplevel_v6_set_app_id (self->zxdg_toplevel_v6, application_id);
    } else
#endif /* HAVE_XDG_SHELL_UNSTABLE_V6 */
    if (self->display->shell) {
        wl_shell_surface_set_class (self->shell_surface, application_id);
    }
}


#ifdef HAVE_IVI_APPLICATION
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
#endif /* HAVE_IVI_APPLICATION */


void
pwl_window_set_id (PwlWindow *self,
                   uint32_t   id)
{
    g_return_if_fail (self);

#ifdef HAVE_IVI_APPLICATION
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
#endif /* HAVE_IVI_APPLICATION */

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

#ifdef HAVE_IVI_APPLICATION
    if (self->display->ivi_application)
        return self->surface_id;
#endif /* HAVE_IVI_APPLICATION */

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

#ifdef HAVE_XDG_SHELL
    if (self->display->xdg_shell) {
        xdg_toplevel_set_title (self->xdg_toplevel, title);
    } else
#endif /* HAVE_XDG_SHELL */
#ifdef HAVE_XDG_SHELL_UNSTABLE_V6
    if (self->display->zxdg_shell_v6) {
        zxdg_toplevel_v6_set_title (self->zxdg_toplevel_v6, title);
    } else
#endif /* HAVE_XDG_SHELL_UNSTABLE_V6 */
    if (self->display->shell) {
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

#ifdef HAVE_XDG_SHELL
    if (self->display->xdg_shell) {
        if ((self->is_fullscreen = fullscreen)) {
            xdg_toplevel_set_fullscreen (self->xdg_toplevel, NULL);
        } else {
            xdg_toplevel_unset_fullscreen (self->xdg_toplevel);
        }
    } else
#endif /* HAVE_XDG_SHELL */
#ifdef HAVE_XDG_SHELL_UNSTABLE_V6
    if (self->display->zxdg_shell_v6) {
        if ((self->is_fullscreen = fullscreen)) {
            zxdg_toplevel_v6_set_fullscreen (self->zxdg_toplevel_v6, NULL);
        } else {
            zxdg_toplevel_v6_unset_fullscreen (self->zxdg_toplevel_v6);
        }
    } else
#endif /* HAVE_XDG_SHELL_UNSTABLE_V6 */
    if (self->display->shell) {
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

#ifdef HAVE_XDG_SHELL
    if (self->display->xdg_shell) {
        if ((self->is_maximized = maximized)) {
            xdg_toplevel_set_maximized (self->xdg_toplevel);
        } else {
            xdg_toplevel_unset_maximized (self->xdg_toplevel);
        }
    } else
#endif /* HAVE_XDG_SHELL */
#ifdef HAVE_XDG_SHELL_UNSTABLE_V6
    if (self->display->zxdg_shell_v6) {
        if ((self->is_maximized = maximized)) {
            zxdg_toplevel_v6_set_maximized (self->zxdg_toplevel_v6);
        } else {
            zxdg_toplevel_v6_unset_maximized (self->zxdg_toplevel_v6);
        }
    } else
#endif /* HAVE_XDG_SHELL_UNSTABLE_V6 */
    if (self->display->shell) {
        if ((self->is_maximized = maximized)) {
            wl_shell_surface_set_maximized (self->shell_surface, NULL);
        } else {
            wl_shell_surface_set_toplevel (self->shell_surface);
        }
    } else {
        g_warning ("No available shell capable of setting maximization status.");
    }
}


static inline bool
pwl_window_opaque_region_matches (const PwlWindow *self,
                                  uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    return (self->opaque_region.x == x &&
            self->opaque_region.y == y &&
            self->opaque_region.w == w &&
            self->opaque_region.h == h);
}


void
pwl_window_set_opaque_region (PwlWindow *self,
                              uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    g_return_if_fail (self);

    if (G_LIKELY (pwl_window_opaque_region_matches (self, x, y, w, h)))
        return;

    struct wl_region *region = wl_compositor_create_region (self->display->compositor);
    wl_region_add (region, x, y, w, h);
    wl_surface_set_opaque_region (self->wl_surface, region);
    wl_region_destroy (region);

    self->opaque_region.x = x;
    self->opaque_region.y = y;
    self->opaque_region.w = w;
    self->opaque_region.h = h;
}


void
pwl_window_unset_opaque_region (PwlWindow *self)
{
    g_return_if_fail (self);

    if (G_LIKELY (pwl_window_opaque_region_matches (self, 0, 0, 0, 0)))
        return;

    wl_surface_set_opaque_region (self->wl_surface, NULL);

    self->opaque_region.x = self->opaque_region.y =
        self->opaque_region.w = self->opaque_region.h = 0;
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


void
pwl_window_notify_focus_change (PwlWindow *self,
                                void (*callback) (PwlWindow*, PwlFocus, void*),
                                void *userdata)
{
    g_return_if_fail (self);

    self->focus_change_callback = callback;
    self->focus_change_userdata = userdata;
}
