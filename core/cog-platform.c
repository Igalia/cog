/*
 * cog-platform.c
 * Copyright (C) 2018 Adrian Perez <aperez@igalia.com>
 * Copyright (C) 2018 Eduardo Lima <elima@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "cog-platform.h"

#define __USE_GNU
#include <dlfcn.h>


G_DEFINE_QUARK (COG_PLATFORM_EGL_ERROR, cog_platform_egl_error)
G_DEFINE_QUARK (COG_PLATFORM_WPE_ERROR, cog_platform_wpe_error)


/* @FIXME: Move this implementation to use a GIO extension point. */

struct _CogPlatform {
    void *so;

    gboolean                  (*setup)             (CogPlatform *platform,
                                                    CogShell    *shell,
                                                    const char  *params,
                                                    GError     **error);

    void                      (*teardown)          (CogPlatform *platform);

    WebKitWebViewBackend*     (*get_view_backend)  (CogPlatform   *platform,
                                                    WebKitWebView *related_view,
                                                    GError       **error);
    WebKitInputMethodContext* (*create_im_context) (CogPlatform   *platform);
};

CogPlatform*
cog_platform_new (void)
{
    return g_slice_new0 (CogPlatform);
}

void
cog_platform_free (CogPlatform *platform)
{
    g_clear_pointer (&platform->so, dlclose);
    g_slice_free (CogPlatform, platform);
}

void
cog_platform_teardown (CogPlatform *platform)
{
    g_return_if_fail (platform != NULL);

    platform->teardown (platform);
}

gboolean
cog_platform_try_load (CogPlatform *platform,
                       const gchar *soname)
{
    g_return_val_if_fail (platform != NULL, FALSE);
    g_return_val_if_fail (soname != NULL, FALSE);

    g_assert (!platform->so);
    platform->so = dlopen (soname, RTLD_LAZY);
    if (!platform->so)
        return FALSE;

    platform->setup = dlsym (platform->so, "cog_platform_setup");
    if (!platform->setup)
        goto err_out;

    platform->teardown = dlsym (platform->so, "cog_platform_teardown");
    if (!platform->teardown)
        goto err_out;

    platform->get_view_backend = dlsym (platform->so,
                                        "cog_platform_get_view_backend");
    if (!platform->get_view_backend)
        goto err_out;

    platform->create_im_context = dlsym (RTLD_NEXT,
                                         "cog_platform_create_im_context");

    return TRUE;

 err_out:
    g_clear_pointer (&platform->so, dlclose);
    return FALSE;
}

gboolean
cog_platform_setup (CogPlatform *platform,
                    CogShell    *shell,
                    const char  *params,
                    GError     **error)
{
    g_return_val_if_fail (platform != NULL, FALSE);
    g_return_val_if_fail (COG_IS_SHELL (shell), FALSE);

    return platform->setup (platform, shell, params, error);
}

WebKitWebViewBackend*
cog_platform_get_view_backend (CogPlatform   *platform,
                               WebKitWebView *related_view,
                               GError       **error)
{
    g_return_val_if_fail (platform != NULL, NULL);

    return platform->get_view_backend (platform, related_view, error);
}

WebKitInputMethodContext*
cog_platform_create_im_context (CogPlatform *platform)
{
#if COG_IM_API_SUPPORTED
    g_return_val_if_fail (platform != NULL, NULL);

    if (platform->create_im_context)
        return platform->create_im_context (platform);
#endif

    return NULL;
}
