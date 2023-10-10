/*
 * cog-window.h
 * Copyright (C) 2023 Pablo Saavedra <psaavedra@igalia.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef COG_WINDOW_H
#define COG_WINDOW_H

#if !(defined(COG_INSIDE_COG__) && COG_INSIDE_COG__)
#    error "Do not include this header directly, use <cog.h> instead"
#endif

#include <glib-object.h>
#include <stdbool.h>
#include <stdint.h>

#define COG_WIN_DEFAULT_WIDTH  1024
#define COG_WIN_DEFAULT_HEIGHT 768

G_BEGIN_DECLS

typedef struct {
    uint32_t width;
    uint32_t height;
} CogWindowGeometry;

/*
 * CogWindow type declaration.
 */

#define COG_WINDOW_TYPE cog_window_get_type()

G_DECLARE_DERIVABLE_TYPE(CogWindow, cog_window, COG, WINDOW, GObject)

struct _CogWindowClass {
    GObjectClass parent_class;
};

void cog_window_geometry_configure(CogWindow *, uint32_t width, uint32_t height);

uint32_t cog_window_get_height(CogWindow *);
uint32_t cog_window_get_width(CogWindow *);

void cog_window_fullscreen_done(CogWindow *);
bool cog_window_fullscreen_is_resizing(CogWindow *);
void cog_window_fullscreen_set(CogWindow *, bool);

bool cog_window_is_fullscreen(CogWindow *);
void cog_window_set_fullscreen(CogWindow *, bool);

#if HAVE_FULLSCREEN_HANDLING
bool cog_window_is_was_fullscreen_requested_from_dom(CogWindow *);
void cog_window_set_was_fullscreen_requested_from_dom(CogWindow *, bool);
#endif

bool cog_window_is_maximized(CogWindow *);
void cog_window_set_maximized(CogWindow *, bool);

G_END_DECLS

#endif /* !COG_WINDOW_H */
