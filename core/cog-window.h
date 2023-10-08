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

/*
 * CogWindow type declaration.
 */

#define COG_WINDOW_TYPE cog_wl_window_get_type()

G_DECLARE_DERIVABLE_TYPE(CogWindow, cog_window, COG, WINDOW, GObject)

struct _CogWindowClass {
    GObjectClass parent_class;
};

uint32_t cog_window_get_height(CogWindow *);
void     cog_window_set_height(CogWindow *, uint32_t);

uint32_t cog_window_get_width(CogWindow *);
void     cog_window_set_width(CogWindow *, uint32_t);

uint32_t cog_window_get_height_before_fullscreen(CogWindow *);
void     cog_window_set_height_before_fullscreen(CogWindow *, uint32_t);

uint32_t cog_window_get_width_before_fullscreen(CogWindow *);
void     cog_window_set_width_before_fullscreen(CogWindow *, uint32_t);

bool cog_window_is_fullscreen(CogWindow *);
void cog_window_set_fullscreen(CogWindow *, bool);

#if HAVE_FULLSCREEN_HANDLING
bool cog_window_is_was_fullscreen_requested_from_dom(CogWindow *);
void cog_window_set_was_fullscreen_requested_from_dom(CogWindow *, bool);
#endif

bool cog_window_is_resizing_fullscreen(CogWindow *);
void cog_window_set_resizing_fullscreen(CogWindow *, bool);

bool cog_window_is_maximized(CogWindow *);
void cog_window_set_maximized(CogWindow *, bool);

G_END_DECLS

#endif /* !COG_WINDOW_H */
