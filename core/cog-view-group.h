/*
 * cog-view-group.h
 * Copyright (C) 2023 Igalia S.L.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#if !(defined(COG_INSIDE_COG__) && COG_INSIDE_COG__)
#    error "Do not include this header directly, use <cog.h> instead"
#endif

#include <glib-object.h>

G_BEGIN_DECLS

typedef struct _CogView CogView;

#define COG_TYPE_VIEW_GROUP (cog_view_group_get_type())

G_DECLARE_DERIVABLE_TYPE(CogViewGroup, cog_view_group, COG, VIEW_GROUP, GObject)

struct _CogViewGroupClass {
    GObjectClass parent_class;

    void *_padding[8];
};

CogViewGroup *cog_view_group_new(void);
void          cog_view_group_add(CogViewGroup *self, CogView *view);
void          cog_view_group_remove(CogViewGroup *self, CogView *view);
gboolean      cog_view_group_contains(CogViewGroup *self, CogView *view);
void          cog_view_group_foreach(CogViewGroup *self, GFunc func, void *userdata);
gsize         cog_view_group_get_n_views(CogViewGroup *self);
CogView      *cog_view_group_get_nth_view(CogViewGroup *self, gsize index);

G_END_DECLS
