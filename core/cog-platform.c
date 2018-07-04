/*
 * cog-platform.c
 * Copyright (C) 2018 Adrian Perez <aperez@igalia.com>
 * Copyright (C) 2018 Eduardo Lima <elima@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include <dlfcn.h>
#include "cog-platform.h"

/* @FIXME: Move this implementation to use a GIO extension point. */

struct _CogPlatform {
    void *so;
    GMainContext *main_context;
    GCancellable *cancellable;

    gboolean              (*setup)            (CogPlatform *platform,
                                               CogLauncher *launcher,
                                               const char  *params,
                                               GError     **error);

    void                  (*teardown)         (CogPlatform *platform);

    WebKitWebViewBackend* (*get_view_backend) (CogPlatform   *platform,
                                               WebKitWebView *related_view,
                                               GError       **error);
};

CogPlatform*
cog_platform_new (void)
{
    return g_slice_new0 (CogPlatform);
}

void
cog_platform_free (CogPlatform *platform)
{
    if (platform->cancellable)
        g_object_unref (platform->cancellable);

    g_clear_pointer (&platform->so, dlclose);
    g_slice_free (CogPlatform, platform);
}

void
cog_platform_teardown (CogPlatform *platform)
{
    g_return_if_fail (platform != NULL);

    if (platform->main_context)
        g_main_context_unref (platform->main_context);

    platform->teardown (platform);
}

gboolean
cog_platform_try_load (CogPlatform *platform,
                       const gchar *soname)
{
    g_return_val_if_fail (platform != NULL, FALSE);
    g_return_val_if_fail (soname != NULL, FALSE);

    g_assert_null (platform->so);
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

    return TRUE;

 err_out:
    g_clear_pointer (&platform->so, dlclose);
    return FALSE;
}

gboolean
cog_platform_setup (CogPlatform *platform,
                    CogLauncher *launcher,
                    const char  *params,
                    GError     **error)
{
    g_return_val_if_fail (platform != NULL, FALSE);
    g_return_val_if_fail (launcher != NULL, FALSE);

    platform->main_context = g_main_context_ref_thread_default ();

    return platform->setup (platform, launcher, params, error);
}

WebKitWebViewBackend*
cog_platform_get_view_backend (CogPlatform   *platform,
                               WebKitWebView *related_view,
                               GError       **error)
{
    g_return_val_if_fail (platform != NULL, NULL);

    return platform->get_view_backend (platform, related_view, error);
}

void
cog_platform_view_show (CogPlatform *platform,
                        GAsyncReadyCallback callback,
                        gpointer user_data)
{
    g_return_if_fail (platform != NULL);

    /* @FIXME: Check that cancellable is null (e.g, another action
     * is in progress.
     */
    GTask *task = g_task_new (NULL,
                              platform->cancellable,
                              callback,
                              user_data);

    /* @TODO: Add a show() method to the platform interface,
     * and call it here.
     */
}

gboolean
cog_platform_view_show_finish (CogPlatform *platform,
                               GAsyncResult *result,
                               GError **error)
{
    g_return_val_if_fail (platform != NULL, FALSE);

    /* @TODO: Add a show_finish() method to the platform interface,
     * and call it here.
     */
}

void
cog_platform_view_hide (CogPlatform *platform,
                        GAsyncReadyCallback callback,
                        gpointer user_data)
{
    g_return_if_fail (platform != NULL);

    /* @FIXME: Check that cancellable is null (e.g, another action
     * is in progress.
     */
    GTask *task = g_task_new (NULL,
                              platform->cancellable,
                              callback,
                              user_data);

    /* @TODO: Add a hide() method to the platform interface,
     * and call it here.
     */
}

gboolean
cog_platform_view_hide_finish (CogPlatform *platform,
                               GAsyncResult *result,
                               GError **error)
{
    g_return_val_if_fail (platform != NULL, FALSE);

    /* @TODO: Add a hide_finish() method to the platform interface,
     * and call it here.
     */
}
