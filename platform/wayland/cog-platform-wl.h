/*
 * cog-platform-wl.h
 * Copyright (C) 2023 Pablo Saavedra <psaavedra@igalia.com>
 * Copyright (C) 2023 Adrian Perez de Castro <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#pragma once

#include "../../core/cog.h"

G_BEGIN_DECLS

G_DECLARE_FINAL_TYPE(CogWlPlatform, cog_wl_platform, COG, WL_PLATFORM, CogPlatform)

struct _CogWlPlatformClass {
    CogPlatformClass parent_class;
};

struct _CogWlPlatform {
    CogPlatform    parent;
    WebKitWebView *web_view;
};

G_END_DECLS
