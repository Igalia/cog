/*
 * cog-window.c
 * Copyright (C) 2023 Pablo Saavedra <psaavedra@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include <inttypes.h>

#include "cog-window.h"

enum {
    RESIZE_WINDOW,
    N_SIGNALS,
};

static int s_signals[N_SIGNALS] = {
    0,
};

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

static void cog_window_set_height(CogWindow *, uint32_t);
static void cog_window_set_width(CogWindow *, uint32_t);

/*
 * CogWindow instantiation.
 */

static void
cog_window_class_init(CogWindowClass *klass)
{

    /**
     * CogWindow::resize-window:
     * @self: The window to create the view for.
     * @user_data: User data.
     *
     * The `resize-window` signal is emitted when the is window resized window.
     *
     * Handling this signal allows to react to changes in the geometry of
     * the window.
     *
     * Returns: (void)
     */
    s_signals[RESIZE_WINDOW] =
        g_signal_new("resize-window", COG_WINDOW_TYPE, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
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

void
cog_window_geometry_configure(CogWindow *window, uint32_t width, uint32_t height)
{
    const char *env_var;
    if (width == 0) {
        env_var = g_getenv("COG_WIN_DEFAULT_WIDTH");
        if (env_var != NULL)
            width = (int32_t) g_ascii_strtod(env_var, NULL);
        else
            width = COG_WIN_DEFAULT_WIDTH;
    }
    if (height == 0) {
        env_var = g_getenv("COG_WIN_DEFAULT_HEIGHT");
        if (env_var != NULL)
            height = (int32_t) g_ascii_strtod(env_var, NULL);
        else
            height = COG_WIN_DEFAULT_HEIGHT;
    }

    if (cog_window_get_width(window) != width || cog_window_get_height(window) != height) {
        g_debug("Configuring new size: %" PRId32 "x%" PRId32, width, height);
        cog_window_set_width(window, width);
        cog_window_set_height(window, height);

        g_signal_emit(window, s_signals[RESIZE_WINDOW], 0);
    }
}

void
cog_window_fullscreen_done(CogWindow *self)
{
    CogWindowPrivate *priv = PRIV(self);
    priv->is_resizing_fullscreen = false;
    g_signal_emit(self, s_signals[RESIZE_WINDOW], 0);
}

bool
cog_window_fullscreen_is_resizing(CogWindow *self)
{
    CogWindowPrivate *priv = PRIV(self);
    return priv->is_resizing_fullscreen;
}

void
cog_window_fullscreen_set(CogWindow *self, bool value)
{
    CogWindowPrivate *priv = PRIV(self);
    if (value) {
        priv->is_resizing_fullscreen = true;
        priv->width_before_fullscreen = cog_window_get_width(self);
        priv->height_before_fullscreen = cog_window_get_height(self);
    } else {
        cog_window_geometry_configure(self, priv->width_before_fullscreen, priv->height_before_fullscreen);
    }
}

uint32_t
cog_window_get_height(CogWindow *self)
{
    CogWindowPrivate *priv = PRIV(self);
    return priv->height;
}

static void
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

static void
cog_window_set_width(CogWindow *self, uint32_t value)
{
    CogWindowPrivate *priv = PRIV(self);
    priv->width = value;
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
