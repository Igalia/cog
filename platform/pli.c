/*
 * pli.c
 * Copyright (C) 2020 Adrian Perez de Castro <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
m*/

#include "pli.h"
#include <fcntl.h>
#include <gio/gio.h>
#include <glib/gstdio.h>


enum {
    N_TOUCH_POINTS = 10,
};


typedef struct _PliSource PliSource;


struct _PliContext {
    struct libinput *input;
    PliSource       *source;

    uint32_t                         touch_width;
    uint32_t                         touch_height;
    struct wpe_input_touch_event_raw touch_points[N_TOUCH_POINTS];
    enum wpe_input_touch_event_type  touch_last_type;
    int                              touch_last_id;

    void (*key_event_callback) (PliContext*, PliKeyEvent*, void*);
    void  *key_event_userdata;

    void (*touch_event_callback) (PliContext*, PliTouchEvent*, void*);
    void  *touch_event_userdata;
};


struct _PliSource {
    GSource     base;
    PliContext *context;
    void       *fd_tag;
};


static void pli_context_process_events (PliContext*);


static gboolean
pli_source_check (GSource *source)
{
    PliSource *self = (PliSource*) source;
    return !!g_source_query_unix_fd (source, self->fd_tag);
}


static gboolean
pli_source_dispatch (GSource    *source,
                     GSourceFunc callback,
                     void       *userdata)
{
    PliSource *self = (PliSource*) source;

    const GIOCondition events = g_source_query_unix_fd (source, self->fd_tag);

    if (events & (G_IO_ERR | G_IO_HUP))
        return G_SOURCE_REMOVE;

    if (events & G_IO_IN)
        pli_context_process_events (self->context);

    return G_SOURCE_CONTINUE;
}


static PliSource*
pli_source_new (PliContext *context)
{
    g_assert (context);

    static GSourceFuncs funcs = {
        .check = pli_source_check,
        .dispatch = pli_source_dispatch,
    };
    PliSource *self = (PliSource*) g_source_new (&funcs, sizeof (PliSource));
    self->context = context;
    self->fd_tag = g_source_add_unix_fd (&self->base,
                                         libinput_get_fd (context->input),
                                         G_IO_IN | G_IO_ERR | G_IO_HUP);
    g_source_set_name (&self->base, "Cog: libinput");
    g_source_set_can_recurse (&self->base, TRUE);

    g_debug ("%s: Created source @ %p.", G_STRFUNC, self);

    return self;
}


static int
pli_context_open_restricted (const char *path,
                             int         flags,
                             void       *data G_GNUC_UNUSED)
{
    int fd = g_open (path, flags, 0);
    g_debug ("%s: Device '%s' opened with fd=%d.", G_STRFUNC, path, fd);
    return fd;
}


static void
pli_context_close_restricted (int   fd,
                              void *data G_GNUC_UNUSED)
{
    g_debug ("%s: Closing device with fd=%d.", G_STRFUNC, fd);
    g_autoptr(GError) error = NULL;
    if (!g_close (fd, &error))
        g_warning ("Cannot close fd=%d: %s", fd, error->message);
}


PliContext*
pli_context_create (GError **error)
{
    g_autoptr(PliContext) self = g_slice_new0 (PliContext);

    struct udev *udev = udev_new();

    static const struct libinput_interface interface = {
        .open_restricted = pli_context_open_restricted,
        .close_restricted = pli_context_close_restricted,
    };
    self->input = libinput_udev_create_context (&interface, self, udev);
    g_clear_pointer (&udev, udev_unref);

    if (libinput_udev_assign_seat (self->input, "seat0")) {
        g_set_error_literal (error,
                             G_IO_ERROR,
                             G_IO_ERROR_FAILED,
                             "libinput: Cannot assign seat0");
        return NULL;
    }

    g_debug ("%s: Created @ %p.", G_STRFUNC, self);
    return g_steal_pointer (&self);
}


void
pli_context_destroy (PliContext *self)
{
    g_return_if_fail (self);

    if (self->source) {
        g_source_destroy (&self->source->base);
        g_source_unref (&self->source->base);
        self->source = NULL;
    }

    g_clear_pointer (&self->input, libinput_unref);

    g_slice_free (PliContext, self);

    g_debug ("%s: Destroyed @ %p.", G_STRFUNC, self);
}


void
pli_context_set_touch_size (PliContext *self,
                            uint32_t    w,
                            uint32_t    h)
{
    g_return_if_fail (self);

    self->touch_width = w;
    self->touch_height = h;
}


void
pli_context_attach_sources (PliContext   *self,
                            GMainContext *context)
{
    g_return_if_fail (self);

    if (!self->source) {
        self->source = pli_source_new (self);
        g_source_attach (&self->source->base, context);
    }
}


static void
pli_context_process_key_event (PliContext                     *self,
                               struct libinput_event_keyboard *event)
{
    if (G_UNLIKELY (!self->key_event_callback))
        return;

    /*
     * Explanation for the offset-by-8, copied from Weston:
     *    evdev XKB rules reflect X's  broken keycode system, which starts at 8
     */
    const uint32_t key = libinput_event_keyboard_get_key (event) + 8;
    const enum libinput_key_state key_state = libinput_event_keyboard_get_key_state (event);

    struct wpe_input_xkb_context *default_context = wpe_input_xkb_context_get_default ();
    struct xkb_state *state = wpe_input_xkb_context_get_state (default_context);

    (*self->key_event_callback) (self,
                                 &((PliKeyEvent) {
                                   .time = libinput_event_keyboard_get_time (event),
                                   .key_code = xkb_state_key_get_one_sym (state, key),
                                   .hardware_key_code = xkb_state_key_get_utf32 (state, key),
                                   .pressed = (key_state == LIBINPUT_KEY_STATE_PRESSED),
                                   }),
                                 self->key_event_userdata);
}


static void
pli_context_process_touch_event (PliContext                  *self,
                                 enum libinput_event_type     type,
                                 struct libinput_event_touch *event)
{
    const uint32_t timestamp = libinput_event_touch_get_time (event);

    enum wpe_input_touch_event_type ev_type = wpe_input_touch_event_type_null;
    switch (type) {
        case LIBINPUT_EVENT_TOUCH_DOWN:
            ev_type = wpe_input_touch_event_type_down;
            break;
        case LIBINPUT_EVENT_TOUCH_UP:
            ev_type = wpe_input_touch_event_type_up;
            break;
        case LIBINPUT_EVENT_TOUCH_MOTION:
            ev_type = wpe_input_touch_event_type_motion;
            break;
        case LIBINPUT_EVENT_TOUCH_FRAME: {
            if (G_LIKELY (self->touch_event_callback)) {
                (*self->touch_event_callback) (self,
                                               &((PliTouchEvent) {
                                                 .time = timestamp,
                                                 .type = self->touch_last_type,
                                                 .id = self->touch_last_id,
                                                 .touchpoints = self->touch_points,
                                                 .touchpoints_length = N_TOUCH_POINTS,
                                                 }),
                                               self->touch_event_userdata);
            }
            for (unsigned i = 0; i < N_TOUCH_POINTS; i++) {
                if (self->touch_points[i].type == wpe_input_touch_event_type_up) {
                    self->touch_points[i] = (struct wpe_input_touch_event_raw) {
                        .type = wpe_input_touch_event_type_null,
                    };
                }
            }
            return;
        }
        default:
            g_assert_not_reached ();
            return;
    }

    int id = libinput_event_touch_get_seat_slot (event);
    if (id < 0 || id >= N_TOUCH_POINTS)
        return;

    self->touch_points[id].time = timestamp;
    self->touch_points[id].type = self->touch_last_type = ev_type;
    self->touch_points[id].id = self->touch_last_id = id;

    if (type == LIBINPUT_EVENT_TOUCH_DOWN || type == LIBINPUT_EVENT_TOUCH_MOTION) {
        self->touch_points[id].x = libinput_event_touch_get_x_transformed (event, self->touch_width);
        self->touch_points[id].y = libinput_event_touch_get_y_transformed (event, self->touch_height);
    }
}


static void
pli_context_process_events (PliContext *self)
{
    g_assert (self);

    int status = libinput_dispatch (self->input);
    if (status < 0) {
        g_warning ("%s: %s", G_STRFUNC, strerror (-status));
        return;
    }

    for (;;) {
        struct libinput_event *event = libinput_get_event (self->input);
        if (!event)
            break;

        const enum libinput_event_type type = libinput_event_get_type (event);
        switch (type) {
            case LIBINPUT_EVENT_KEYBOARD_KEY:
                pli_context_process_key_event (self, libinput_event_get_keyboard_event (event));
                break;
            case LIBINPUT_EVENT_TOUCH_DOWN:
            case LIBINPUT_EVENT_TOUCH_UP:
            case LIBINPUT_EVENT_TOUCH_MOTION:
            case LIBINPUT_EVENT_TOUCH_FRAME:
                pli_context_process_touch_event (self, type, libinput_event_get_touch_event (event));
                break;
            default:
                /* TODO: Pointer events. */
                break;
        }

        libinput_event_destroy (event);
    }
}


void
pli_context_notify_key (PliContext *self,
                        void      (*callback)(PliContext*, PliKeyEvent*, void*),
                        void       *userdata)
{
    g_return_if_fail (self);
    self->key_event_callback = callback;
    self->key_event_userdata = userdata;
}


void
pli_context_notify_touch (PliContext *self,
                          void      (*callback) (PliContext*, PliTouchEvent*, void*),
                          void       *userdata)
{
    g_return_if_fail (self);
    self->touch_event_callback = callback;
    self->touch_event_userdata = userdata;
}
