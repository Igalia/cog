/*
 * cog-modules.h
 * Copyright (C) 2019 Adrian Perez de Castro <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#pragma once

#if !(defined(COG_INSIDE_COG__) && COG_INSIDE_COG__)
#    error "Do not include this header directly, use <cog.h> instead"
#endif

#include <gio/gio.h>

G_BEGIN_DECLS

#define COG_MODULES_SHELL_EXTENSION_POINT "com.igalia.Cog.Shell"

#if (defined(COG_INTERNAL_COG__) && COG_INTERNAL_COG__)
typedef struct {
    GIOExtensionPoint *shell;
} G_GNUC_INTERNAL _CogExtensionPoints;

G_GNUC_INTERNAL
const _CogExtensionPoints* _cog_modules_ensure_extension_points (void);

G_GNUC_INTERNAL
GType _cog_modules_get_preferred_internal (const char        *func,
                                           GIOExtensionPoint *ep,
                                           const char        *preferred_module,
                                           size_t             is_supported_offset);

static inline GIOExtensionPoint*
_cog_modules_get_shell_extension_point (void)
{
    return _cog_modules_ensure_extension_points ()->shell;
}

/* Type getters for built-in "modules", see _cog_modules_ensure_loaded(). */
G_GNUC_INTERNAL GType _cog_minimal_shell_get_type (void);
#endif /* COG_INTERNAL_COG__ */

GType cog_modules_get_preferred (const char *extension_point,
                                 const char *preferred_module,
                                 size_t      is_supported_offset);

void  cog_modules_foreach       (const char *extension_point,
                                 void (*callback) (GIOExtension*, void*),
                                 void *userdata);

void  cog_modules_add_directory (const char *directory_path);

G_END_DECLS
