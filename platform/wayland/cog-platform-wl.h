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

G_DECLARE_FINAL_TYPE(CogWlPlatform, cog_wl_platform, COG, WL_PLATFORM, CogPlatform)

struct _CogWlPlatformClass {
    CogPlatformClass parent_class;
};

struct _CogWlPlatform {
    CogPlatform   parent;
    CogWlDisplay *display;
    CogWlPopup   *popup;
    GHashTable   *viewports; /* (CogViewportKey, CogViewport) */
};

/*
 * Method declarations.
 */

/**
 * CogWlPlatform::cog_wl_platform_popup_create
 * @platform: Reference to the CogWlPlatform instance.
 * @options: Reference to the WebKitOptionMenu.
 *
 * Creates the CogWlPopup and set this inside the @platform.
 *
 * This function will fail if it is called twice without a
 * cog_wl_platform_popup_destroy() in between both calls.
 *
 * Returns: (void)
 */
void cog_wl_platform_popup_create(CogWlViewport *, WebKitOptionMenu *);
void cog_wl_platform_popup_destroy();
void cog_wl_platform_popup_update();

G_END_DECLS
