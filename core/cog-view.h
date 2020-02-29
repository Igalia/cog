/*
 * cog-view.h
 * Copyright (C) 2019 Adrian Perez de Castro <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#pragma once

#include "cog-webkit-utils.h"

G_BEGIN_DECLS

typedef struct _CogShell CogShell;
struct wpe_view_backend;

#define COG_TYPE_VIEW  (cog_view_get_type ())

G_DECLARE_DERIVABLE_TYPE (CogView, cog_view, COG, VIEW, WebKitWebView)

struct _CogViewClass {
    /*< private >*/
    WebKitWebViewClass parent_class;
};

const char* cog_view_get_name      (CogView *view);
CogShell*   cog_view_get_shell     (CogView *view);

gboolean    cog_view_get_visible   (CogView *view);
void        cog_view_set_visible   (CogView *view,
                                    gboolean visible);

gboolean    cog_view_get_focused   (CogView *view);
void        cog_view_set_focused   (CogView *view,
                                    gboolean focused);

gboolean    cog_view_get_in_window (CogView *view);
void        cog_view_set_in_window (CogView *view,
                                    gboolean in_window);

struct wpe_view_backend*
            cog_view_get_backend   (CogView *view);

G_END_DECLS
