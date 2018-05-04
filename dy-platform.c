/*
 * dy-platform.c
 * Copyright (C) 2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include <dlfcn.h>
#include "dy-platform.h"

/* @FIXME: Move this implementation to use a GIO extension point. */

struct _DyPlatform {
    void *so;

    gboolean              (*setup)            (DyPlatform  *platform,
                                               DyLauncher  *launcher,
                                               const char  *params,
                                               GError     **error);

    void                  (*teardown)         (DyPlatform *platform);

    WebKitWebViewBackend* (*get_view_backend) (DyPlatform     *platform,
                                               WebKitWebView  *related_view,
                                               GError        **error);
};

DyPlatform*
dy_platform_new (void)
{
    return g_slice_new0 (DyPlatform);
}

void
dy_platform_free (DyPlatform *platform)
{
    if (platform->so)
        dlclose (platform->so);

    g_slice_free (DyPlatform, platform);
}

void
dy_platform_teardown (DyPlatform *platform)
{
    g_return_if_fail (platform != NULL);

    platform->teardown (platform);
}

gboolean
dy_platform_try_load (DyPlatform  *platform,
                      const gchar *soname)
{
    g_return_val_if_fail (platform != NULL, FALSE);
    g_return_val_if_fail (soname != NULL, FALSE);

    g_assert_null (platform->so);
    platform->so = dlopen (soname, RTLD_LAZY);
    if (!platform->so)
        return FALSE;

    platform->setup = dlsym (platform->so, "dy_platform_setup");
    if (!platform->setup)
        goto err_out;

    platform->teardown = dlsym (platform->so, "dy_platform_teardown");
    if (!platform->teardown)
        goto err_out;

    platform->get_view_backend = dlsym (platform->so,
                                        "dy_platform_get_view_backend");
    if (!platform->get_view_backend)
        goto err_out;

    return TRUE;

 err_out:
    dlclose (platform->so);
    platform->so = NULL;

    return FALSE;
}

gboolean
dy_platform_setup (DyPlatform  *platform,
                   DyLauncher  *launcher,
                   const char  *params,
                   GError     **error)
{
    g_return_val_if_fail (platform != NULL, FALSE);
    g_return_val_if_fail (launcher != NULL, FALSE);

    return platform->setup (platform, launcher, params, error);
}

WebKitWebViewBackend*
dy_platform_get_view_backend (DyPlatform     *platform,
                              WebKitWebView  *related_view,
                              GError        **error)
{
    g_return_val_if_fail (platform != NULL, NULL);

    return platform->get_view_backend (platform, related_view, error);
}
