/*
 * cog-platform-headless.c
 * Copyright (C) 2021 Igalia S.L
 *
 * Distributed under terms of the MIT license.
 */

#include <glib.h>

#include "../../core/cog.h"

struct platform_window {
    guint tick_source;
    gboolean frame_complete;
    WebKitWebViewBackend* view_backend;
};

static struct platform_window win = {
    .view_backend = NULL, .frame_complete = FALSE
};

static void*
create_backend(void* backend, struct wpe_view_backend* view_backend)
{
    return NULL;
}

static void
destroy_backend(void* backend)
{
    
}

static void
init_backend(void* backend)
{
    
}

static int
get_renderer_host_fd(void* backend)
{
    return -1;
}

static void setup_fdo_exportable(struct platform_window* window)
{
    static struct wpe_view_backend_interface s_backend_interface = {
        .create = create_backend,
        .destroy = destroy_backend,
        .initialize = init_backend,
        .get_renderer_host_fd = get_renderer_host_fd
    };

    wpe_loader_init("libWPEBackend-headless.so");
    window->view_backend = webkit_web_view_backend_new(
        wpe_view_backend_create_with_backend_interface(&s_backend_interface, NULL),
        NULL, NULL);

    g_printerr("yo\n");
    g_assert_nonnull(window->view_backend);
}

/* static gboolean tick_callback(gpointer data) */
/* { */
/*     struct platform_window* window = (struct platform_window*) data; */
/*     if (window->frame_complete) */
/*         wpe_view_backend_exportable_fdo_dispatch_frame_complete(window->exportable); */
/*     return G_SOURCE_CONTINUE; */
/* } */

gboolean cog_platform_plugin_setup(CogPlatform* platform, CogShell* shell G_GNUC_UNUSED, const char* params, GError** error)
{
    g_assert_nonnull(platform);
    setup_fdo_exportable(&win);

    // Maintain rendering at 30 FPS.
    /* win.tick_source = g_timeout_add(1000/30, G_SOURCE_FUNC(tick_callback), &win); */

    return TRUE;
}

void cog_platform_plugin_teardown(CogPlatform* platform)
{
    g_assert_nonnull(platform);
    g_source_remove(win.tick_source);
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
