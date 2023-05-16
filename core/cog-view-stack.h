/*
 * cog-view-stack.h
 * Copyright (C) 2023 Igalia S.L.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#if !(defined(COG_INSIDE_COG__) && COG_INSIDE_COG__)
#    error "Do not include this header directly, use <cog.h> instead"
#endif

#include "cog-view-group.h"

G_BEGIN_DECLS

#define COG_TYPE_VIEW_STACK (cog_view_stack_get_type())

G_DECLARE_FINAL_TYPE(CogViewStack, cog_view_stack, COG, VIEW_STACK, CogViewGroup)

typedef struct _CogView CogView;

CogViewStack *cog_view_stack_new(void);
CogView      *cog_view_stack_get_visible_view(CogViewStack *self);
void          cog_view_stack_set_visible_view(CogViewStack *self, CogView *view);

G_END_DECLS
