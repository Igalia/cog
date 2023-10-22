/*
 * cog-view-wl.h
 * Copyright (C) 2023 Pablo Saavedra <psaavedra@igalia.com>
 * Copyright (C) 2023 Adrian Perez de Castro <aperez@igalia.com>
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "../../core/cog.h"

#include "cog-platform-wl.h"

G_BEGIN_DECLS

typedef struct _CogWlPlatform CogWlPlatform;

/*
 * CogWlView type declaration.
 */

struct _CogWlView {
    CogView parent;

    CogWlPlatform *platform;

    struct wpe_view_backend_exportable_fdo *exportable;
    struct wpe_fdo_egl_exported_image      *image;

    struct wl_callback *frame_callback;

    bool    should_update_opaque_region;
    int32_t scale_factor;

    struct wl_list shm_buffer_list;
};

G_DECLARE_FINAL_TYPE(CogWlView, cog_wl_view, COG, WL_VIEW, CogView)

/*
 * Method declarations.
 */

void cog_wl_view_enter_fullscreen(CogWlView *);

void cog_wl_view_register_type_exported(GTypeModule *type_module);

G_END_DECLS
