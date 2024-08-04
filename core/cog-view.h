/*
 * cog-view.h
 * Copyright (C) 2023 Igalia S.L.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#if !(defined(COG_INSIDE_COG__) && COG_INSIDE_COG__)
#    error "Do not include this header directly, use <cog.h> instead"
#endif

#include "cog-viewport.h"
#include "cog-webkit-utils.h"

G_BEGIN_DECLS

typedef struct _WebKitWebViewBackend WebKitWebViewBackend;
struct wpe_input_keyboard_event;
struct wpe_view_backend;

#define COG_TYPE_VIEW (cog_view_get_type())

COG_API
G_DECLARE_DERIVABLE_TYPE(CogView, cog_view, COG, VIEW, WebKitWebView)

struct _CogViewClass {
    /*< private >*/
    WebKitWebViewClass parent_class;

    /*< public >*/
    WebKitWebViewBackend *(*create_backend)(CogView *self);
    gboolean (*set_fullscreen)(CogView *self, gboolean enable);
    gboolean (*is_fullscreen)(CogView *self);
};

#define COG_TYPE_VIEW_IMPL (cog_view_get_impl_type())

COG_API
GType                    cog_view_get_impl_type(void);

COG_API
CogView                 *cog_view_new(const char *first_property_name, ...);

COG_API
struct wpe_view_backend *cog_view_get_backend(CogView *view);

COG_API
void cog_view_handle_key_event(CogView *self, const struct wpe_input_keyboard_event *event);

COG_API
void cog_view_set_use_key_bindings(CogView *self, gboolean enable);

COG_API
gboolean cog_view_get_use_key_bindings(CogView *self);

COG_API
CogViewport *cog_view_get_viewport(CogView *self);

COG_API
gboolean cog_view_is_visible(CogView *self);

COG_API
gboolean cog_view_set_visible(CogView *self);

COG_API gboolean cog_view_set_fullscreen(CogView *self, gboolean enable);
COG_API gboolean cog_view_is_fullscreen(CogView *self);

G_END_DECLS
