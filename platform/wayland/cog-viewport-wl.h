/*
 * cog-viewport-wl.h
 * Copyright (C) 2023 Pablo Saavedra <psaavedra@igalia.com>
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "../../core/cog.h"

#include "cog-platform-wl.h"

G_BEGIN_DECLS

/*
 * CogWlViewport type declaration.
 */

struct _CogWlViewport {
    CogViewport parent;
};

G_DECLARE_FINAL_TYPE(CogWlViewport, cog_wl_viewport, COG, WL_VIEWPORT, CogViewport)

/*
 * Method declarations.
 */

void cog_wl_viewport_register_type_exported(GTypeModule *type_module);

G_END_DECLS
