/*
 * cog-modules.h
 * Copyright (C) 2021 Adrian Perez de Castro <aperez@igalia.com>
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#if !(defined(COG_INSIDE_COG__) && COG_INSIDE_COG__)
#    error "Do not include this header directly, use <cog.h> instead"
#endif

#include <gio/gio.h>

G_BEGIN_DECLS

#define COG_MODULES_PLATFORM_EXTENSION_POINT "com.igalia.Cog.Platform"

#define COG_MODULES_PLATFORM (cog_modules_get_platform_extension_point())

GIOExtensionPoint *cog_modules_get_platform_extension_point(void);

GType
cog_modules_get_preferred(GIOExtensionPoint *extension_point, const char *preferred_module, size_t is_supported_offset);

void cog_modules_foreach(GIOExtensionPoint *extension_point, void (*callback)(GIOExtension *, void *), void *userdata);

void cog_modules_add_directory(const char *directory_path);

G_END_DECLS
