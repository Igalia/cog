/*
 * Copyright (C) 2023 Igalia S.L.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#if !(defined(COG_INSIDE_COG__) && COG_INSIDE_COG__)
#    error "Do not include this header directly, use <cog.h> instead"
#endif

#include "cog-export.h"

#include <glib-object.h>

G_BEGIN_DECLS

typedef struct _CogView CogView;

#define COG_TYPE_VIEWPORT (cog_viewport_get_type())

COG_API
G_DECLARE_DERIVABLE_TYPE(CogViewport, cog_viewport, COG, VIEWPORT, GObject)

struct _CogViewportClass {
    GObjectClass parent_class;

    void *_padding[8];
};

#define COG_TYPE_VIEWPORT_IMPL (cog_viewport_get_impl_type())

COG_API
GType cog_viewport_get_impl_type(void);

COG_API
CogViewport *cog_viewport_new();

COG_API void     cog_viewport_add(CogViewport *self, CogView *view);
COG_API void     cog_viewport_remove(CogViewport *self, CogView *view);
COG_API gboolean cog_viewport_contains(CogViewport *self, CogView *view);
COG_API void     cog_viewport_foreach(CogViewport *self, GFunc func, void *userdata);
COG_API gsize    cog_viewport_get_n_views(CogViewport *self);
COG_API CogView *cog_viewport_get_nth_view(CogViewport *self, gsize index);
COG_API CogView *cog_viewport_get_visible_view(CogViewport *self);
COG_API void     cog_viewport_set_visible_view(CogViewport *self, CogView *view);

G_END_DECLS
