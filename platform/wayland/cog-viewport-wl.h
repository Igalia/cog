/*
 * cog-viewport-wl.h
 * Copyright (C) 2023 Pablo Saavedra <psaavedra@igalia.com>
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "../../core/cog.h"

#include "cog-platform-wl.h"
#include "cog-utils-wl.h"

G_BEGIN_DECLS

/*
 * CogWlViewport type declaration.
 */

struct _CogWlViewport {
    CogViewport parent;

    CogWlWindow window;
};

G_DECLARE_FINAL_TYPE(CogWlViewport, cog_wl_viewport, COG, WL_VIEWPORT, CogViewport)

/*
 * Method declarations.
 */

void     cog_wl_viewport_configure_geometry(CogWlViewport *, int32_t width, int32_t height);
gboolean cog_wl_viewport_create_window(CogWlViewport *, GError **error);
void     cog_wl_viewport_resize_to_largest_output(CogWlViewport *);
bool     cog_wl_viewport_set_fullscreen(CogWlViewport *, bool fullscreen);

void cog_wl_viewport_register_type_exported(GTypeModule *type_module);

G_END_DECLS
