/*
 * cog-utils-wl.c
 * Copyright (C) 2023 Pablo Saavedra <psaavedra@igalia.com>
 * Copyright (C) 2023 Adrian Perez de Castro <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef COG_PLATFORM_WL_WINDOW_C
#define COG_PLATFORM_WL_WINDOW_C

#include "../../core/cog.h"

#include "cog-window-wl.h"

G_DEFINE_TYPE(CogWlWindow, cog_wl_window, G_TYPE_OBJECT)

/*
 * CogWlWindow instantiation.
 */

static void
cog_wl_window_class_init(CogWlWindowClass *klass)
{
}

static void
cog_wl_window_init(CogWlWindow *self)
{
    self->width = COG_WL_WIN_DEFAULT_WIDTH;
    self->height = COG_WL_WIN_DEFAULT_HEIGHT;
    self->width_before_fullscreen = COG_WL_WIN_DEFAULT_WIDTH;
    self->height_before_fullscreen = COG_WL_WIN_DEFAULT_HEIGHT;
}

/*
 * Method definitions.
 */

CogWlWindow *
cog_wl_window_new(void)
{
    g_autoptr(CogWlWindow) self = g_object_new(COG_WL_WINDOW_TYPE, NULL);
    return g_steal_pointer(&self);
}

#endif /* !COG_PLATFORM_WL_WINDOW_C */
