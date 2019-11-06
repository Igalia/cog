/*
 * pwl.c
 * Copyright (C) 2019 Adrian Perez de Castro <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "pwl.h"

#include <errno.h>

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
