/*
 * cog-platform-wl.h
 * Copyright (C) 2023 Pablo Saavedra <psaavedra@igalia.com>
 * Copyright (C) 2023 Adrian Perez de Castro <aperez@igalia.com>
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "../../core/cog.h"

#include "cog-utils-wl.h"
#include "cog-view-wl.h"

G_BEGIN_DECLS

typedef struct _CogWlView CogWlView;

#define COG_WL_PLATFORM_TYPE cog_wl_platform_get_type()

G_DECLARE_FINAL_TYPE(CogWlPlatform, cog_wl_platform, COG, WL_PLATFORM, CogPlatform)

struct _CogWlPlatformClass {
    CogPlatformClass parent_class;
};

struct _CogWlPlatform {
    CogPlatform   parent;
    CogWlDisplay *display;
    CogWlView    *view;
    CogWlWindow   window;
};

/*
 * Method declarations.
 */

void cog_wl_platform_popup_create(CogWlPlatform *platform, WebKitOptionMenu *);

bool cog_wl_platform_set_fullscreen(CogWlPlatform *platform, bool fullscreen);

G_END_DECLS
