/*
 * cog-view-private.h
 * Copyright (C) 2023 Igalia S.L.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "cog-view.h"
#include "cog-viewport.h"

G_BEGIN_DECLS

void cog_view_set_viewport(CogView *self, CogViewport *viewport);

G_END_DECLS
