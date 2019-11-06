/*
 * pwl.h
 * Copyright (C) 2019 Adrian Perez de Castro <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#pragma once

#include <glib.h>
#include <wayland-client.h>

G_BEGIN_DECLS

typedef struct {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
} PwlDisplay;

PwlDisplay* pwl_display_connect (const char *name, GError**);
void        pwl_display_destroy (PwlDisplay*);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (PwlDisplay, pwl_display_destroy)

G_END_DECLS
