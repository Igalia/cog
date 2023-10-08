/*
 * cog-window.c
 * Copyright (C) 2023 Pablo Saavedra <psaavedra@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "cog-window.h"

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t width_before_fullscreen;
    uint32_t height_before_fullscreen;

    bool is_fullscreen;
#if HAVE_FULLSCREEN_HANDLING
    bool was_fullscreen_requested_from_dom;
#endif
    bool is_resizing_fullscreen;
    bool is_maximized;
} CogWindowPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(CogWindow, cog_window, G_TYPE_OBJECT)

#define PRIV(obj) ((CogWindowPrivate *) cog_window_get_instance_private(COG_WINDOW(obj)))
/*
 * CogWindow instantiation.
 */

static void
cog_window_class_init(CogWindowClass *klass)
{
}

static void
cog_window_init(CogWindow *self)
{
    CogWindowPrivate *priv = PRIV(self);
    priv->width = COG_WIN_DEFAULT_WIDTH;
    priv->height = COG_WIN_DEFAULT_HEIGHT;
    priv->width_before_fullscreen = COG_WIN_DEFAULT_WIDTH;
    priv->height_before_fullscreen = COG_WIN_DEFAULT_HEIGHT;
    priv->is_fullscreen = false;
#if HAVE_FULLSCREEN_HANDLING
    priv->was_fullscreen_requested_from_dom = false;
#endif
    priv->is_resizing_fullscreen = false;
    priv->is_maximized = false;
}

uint32_t
cog_window_get_height(CogWindow *self)
{
    CogWindowPrivate *priv = PRIV(self);
    return priv->height;
}

void
cog_window_set_height(CogWindow *self, uint32_t value)
{
    CogWindowPrivate *priv = PRIV(self);
    priv->height = value;
}

uint32_t
cog_window_get_width(CogWindow *self)
{
    CogWindowPrivate *priv = PRIV(self);
    return priv->width;
}

void
cog_window_set_width(CogWindow *self, uint32_t value)
{
    CogWindowPrivate *priv = PRIV(self);
    priv->width = value;
}

uint32_t
cog_window_get_width_before_fullscreen(CogWindow *self)
{
    CogWindowPrivate *priv = PRIV(self);
    return priv->width_before_fullscreen;
}

void
cog_window_set_width_before_fullscreen(CogWindow *self, uint32_t value)
{
    CogWindowPrivate *priv = PRIV(self);
    priv->width_before_fullscreen = value;
}

uint32_t
cog_window_get_height_before_fullscreen(CogWindow *self)
{
    CogWindowPrivate *priv = PRIV(self);
    return priv->height_before_fullscreen;
}

void
cog_window_set_height_before_fullscreen(CogWindow *self, uint32_t value)
{
    CogWindowPrivate *priv = PRIV(self);
    priv->height_before_fullscreen = value;
}

bool
cog_window_is_fullscreen(CogWindow *self)
{
    CogWindowPrivate *priv = PRIV(self);
    return priv->is_fullscreen;
}

void
cog_window_set_fullscreen(CogWindow *self, bool value)
{
    CogWindowPrivate *priv = PRIV(self);
    priv->is_fullscreen = value;
}

#if HAVE_FULLSCREEN_HANDLING
bool
cog_window_is_was_fullscreen_requested_from_dom(CogWindow *self)
{
    CogWindowPrivate *priv = PRIV(self);
    return priv->was_fullscreen_requested_from_dom;
}

void
cog_window_set_was_fullscreen_requested_from_dom(CogWindow *self, bool value)
{
    CogWindowPrivate *priv = PRIV(self);
    priv->was_fullscreen_requested_from_dom = value;
}
#endif

bool
cog_window_is_resizing_fullscreen(CogWindow *self)
{
    CogWindowPrivate *priv = PRIV(self);
    return priv->is_resizing_fullscreen;
}

void
cog_window_set_resizing_fullscreen(CogWindow *self, bool value)
{
    CogWindowPrivate *priv = PRIV(self);
    priv->is_resizing_fullscreen = value;
}

bool
cog_window_is_maximized(CogWindow *self)
{
    CogWindowPrivate *priv = PRIV(self);
    return priv->is_maximized;
}

void
cog_window_set_maximized(CogWindow *self, bool value)
{
    CogWindowPrivate *priv = PRIV(self);
    priv->is_maximized = value;
}
