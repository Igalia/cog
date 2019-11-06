/*
 * cog-minimal-shell.c
 * Copyright (C) 2019 Adrian Perez de Castro <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#define COG_INTERNAL_COG__ 1

#include "cog-shell.h"
#include "cog-modules.h"
#include <wpe/wpe.h>

typedef struct {
    CogShellClass parent_class;
} CogMinimalShellClass;

typedef struct {
    CogShell parent;
} CogMinimalShell;

#define cog_minimal_shell_get_type _cog_minimal_shell_get_type
G_DEFINE_TYPE_WITH_CODE (CogMinimalShell, cog_minimal_shell, COG_TYPE_SHELL,
    _cog_modules_ensure_extension_points ();
    g_io_extension_point_implement (COG_MODULES_SHELL_EXTENSION_POINT,
                                    g_define_type_id, "minimal", 0))

static gboolean
cog_minimal_shell_is_supported (void)
{
    /* The minimal shell is always supported. */
    return TRUE;
}

static void
cog_minimal_shell_class_init (CogMinimalShellClass *klass)
{
    CogShellClass *shell_class = COG_SHELL_CLASS (klass);
    shell_class->is_supported = cog_minimal_shell_is_supported;
}

static void
cog_minimal_shell_init (CogMinimalShell *self)
{
    g_debug ("= %s", G_STRFUNC);

    /*
     * TODO: Instead of environment variables, provide some mechanism
     *       to pass parameters using the API to a CogShell implementation.
     */
    const char *backend_name = g_getenv ("COG_WPE_BACKEND_LIBRARY");

    bool ok;
    if (backend_name) {
        g_autofree char *backend_path = g_strconcat ("libWPEBackend-",
                                                     backend_name,
                                                     ".so",
                                                     NULL);
        g_debug ("%s: Using backend %s", G_STRFUNC, backend_path);
        ok = wpe_loader_init (backend_path);
    } else {
        g_debug ("%s: Using backend libWPEBackend-default.so", G_STRFUNC);
        ok = wpe_loader_init ("libWPEBackend-default.so");
    }

    g_debug ("%s: wpe_loader_init %s", G_STRFUNC, ok ? "suceeded" : "failed");
}
