/*
 * cog-platform-headless.c
 * Copyright (C) 2021 Igalia S.L
 *
 * Distributed under terms of the MIT license.
 */

#include <glib.h>
#include <wpe/fdo.h>
#include <wpe/unstable/fdo-shm.h>

#include "../../core/cog.h"

struct platform_window {
    guint tick_source;
    gboolean frame_complete;
    struct wpe_view_backend_exportable_fdo* exportable;
    WebKitWebViewBackend* view_backend;
};

static struct platform_window win = {
    .exportable = NULL, .frame_complete = FALSE
};

static void on_export_shm_buffer(void* data, struct wpe_fdo_shm_exported_buffer* buffer)
{
    struct platform_window* window = (struct platform_window*) data;
    wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(window->exportable, buffer);
    window->frame_complete = TRUE;
}

static void setup_fdo_exportable(struct platform_window* window)
{
    wpe_loader_init("libWPEBackend-fdo-1.0.so");
    wpe_fdo_initialize_shm();

    static const struct wpe_view_backend_exportable_fdo_client client = {
        .export_shm_buffer = on_export_shm_buffer,
    };

    window->exportable = wpe_view_backend_exportable_fdo_create(&client, window, 800, 600);
    struct wpe_view_backend* wpeViewBackend = wpe_view_backend_exportable_fdo_get_view_backend(window->exportable);
    window->view_backend = webkit_web_view_backend_new(wpeViewBackend, NULL, NULL);

    g_assert_nonnull(window->view_backend);
}

static gboolean tick_callback(gpointer data)
{
    struct platform_window* window = (struct platform_window*) data;
    if (window->frame_complete)
        wpe_view_backend_exportable_fdo_dispatch_frame_complete(window->exportable);
    return G_SOURCE_CONTINUE;
}

gboolean cog_platform_plugin_setup(CogPlatform* platform, CogShell* shell G_GNUC_UNUSED, const char* params, GError** error)
{
    g_assert_nonnull(platform);
    setup_fdo_exportable(&win);

    // Maintain rendering at 30 FPS.
    win.tick_source = g_timeout_add(1000/30, G_SOURCE_FUNC(tick_callback), &win);

    return TRUE;
}

void cog_platform_plugin_teardown(CogPlatform* platform)
{
    g_assert_nonnull(platform);
    g_source_remove(win.tick_source);
    wpe_view_backend_exportable_fdo_destroy(win.exportable);
}

WebKitWebViewBackend* cog_platform_plugin_get_view_backend(CogPlatform* platform, WebKitWebView* related_view, GError** error)
{
    g_assert_nonnull(platform);
    g_assert_nonnull(win.view_backend);
    return win.view_backend;
}

void cog_platform_plugin_init_web_view(CogPlatform* platform, WebKitWebView* view)
{
}
