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

#define COG_TYPE_VIEW  (cog_view_get_type ())

G_DECLARE_DERIVABLE_TYPE (CogView, cog_view, COG, VIEW, WebKitWebView)

struct _CogViewClass {
    /*< private >*/
    WebKitWebViewClass parent_class;
};

const char* cog_view_get_name  (CogView *view);
CogShell*   cog_view_get_shell (CogView *view);

G_END_DECLS
