/*
 * cog-platform-nil.c
 * Copyright (C) 2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include <cog.h>
#include <errno.h>


#define BACKEND_LIB_PARAM_PREFIX      "backend="
#define BACKEND_LIB_PARAM_PREFIX_LEN  (sizeof (BACKEND_LIB_PARAM_PREFIX) - 1)


G_MODULE_EXPORT gboolean
cog_platform_setup (CogPlatform *platform G_GNUC_UNUSED,
                    CogLauncher *launcher G_GNUC_UNUSED,
                    const char  *params,
                    GError     **error)
{
    if (params) {
        if (g_str_has_prefix (params, BACKEND_LIB_PARAM_PREFIX)) {
            const char *libname = params + BACKEND_LIB_PARAM_PREFIX_LEN;
            if (libname && *libname) {
                if (!g_setenv ("WPE_BACKEND_LIBRARY", libname, TRUE)) {
                    g_set_error_literal (error,
                                         G_FILE_ERROR,
                                         g_file_error_from_errno (errno),
                                         "Cannot set WPE_BACKEND_LIBRARY environment variable");
                    return FALSE;
                }
            } else {
                g_warning ("platform-nil: Empty value specified for the "
                           BACKEND_LIB_PARAM_PREFIX " parameter");
            }
        } else {
            g_warning ("platform-nil: Invalid parameters: '%s'", params);
        }
    }

    return TRUE;
}


G_MODULE_EXPORT void
cog_platform_teardown (CogPlatform *platform G_GNUC_UNUSED)
{
}


G_MODULE_EXPORT WebKitWebViewBackend*
cog_platform_get_view_backend (CogPlatform   *platform G_GNUC_UNUSED,
                               WebKitWebView *related_view G_GNUC_UNUSED,
                               GError       **error G_GNUC_UNUSED)
{
    g_debug ("platform-nil: Instantiating default WPE backend.");
    return webkit_web_view_backend_new (wpe_view_backend_create (), NULL, NULL);
}

