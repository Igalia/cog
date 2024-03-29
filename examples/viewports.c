/*
 * viewports.c
 * Copyright (C) 2023 Igalia S.L.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../core/cog.h"

typedef struct {
    CogViewport *views;
    gsize        current_index;
} TimeoutData;

static gboolean
on_timeout_tick(TimeoutData *data)
{
    if (++data->current_index >= cog_viewport_get_n_views(data->views))
        data->current_index = 0;

    CogView *view = cog_viewport_get_nth_view(data->views, data->current_index);
    g_message("Set visible view %zu <%p>", data->current_index, view);
    cog_viewport_set_visible_view(data->views, view);

    return G_SOURCE_CONTINUE;
}

int
main(int argc, char *argv[])
{
    if (argc < 2) {
        g_printerr("Usage: %s <URL> [URL...]\n", argv[0]);
        return EXIT_FAILURE;
    }

    g_set_prgname("viewports");
    cog_init(NULL, NULL);

    g_autoptr(CogShell) shell = cog_shell_new(NULL, FALSE);
    g_autoptr(GError)   error = NULL;

    g_autoptr(CogPlatform) platform = cog_platform_get();
    if (!cog_platform_setup(platform, shell, NULL, &error))
        g_error("Cannot configure platform: %s", error->message);

    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);

    CogViewport *viewport0 = cog_viewport_new();

    for (int i = 1; i < argc; i++) {
        g_autoptr(CogView) view = cog_view_new(NULL);
        cog_platform_init_web_view(platform, WEBKIT_WEB_VIEW(view));
        cog_viewport_add(viewport0, view);
        webkit_web_view_load_uri(WEBKIT_WEB_VIEW(view), argv[i]);
        g_message("Viewport #0. Created view %p, URI %s", view, argv[i]);
    }

    TimeoutData data0 = {
        .views = viewport0,
        .current_index = SIZE_MAX,
    };
    g_timeout_add_seconds(5, (GSourceFunc) on_timeout_tick, &data0);
    on_timeout_tick(&data0);

    CogViewport *viewport1 = cog_viewport_new();

    for (int i = 1; i < argc; i++) {
        g_autoptr(CogView) view = cog_view_new(NULL);
        cog_platform_init_web_view(platform, WEBKIT_WEB_VIEW(view));
        cog_viewport_add(viewport1, view);
        webkit_web_view_load_uri(WEBKIT_WEB_VIEW(view), argv[i]);
        g_message("Viewport #1. Created view %p, URI %s", view, argv[i]);
    }

    TimeoutData data1 = {
        .views = viewport1,
        .current_index = SIZE_MAX,
    };
    g_timeout_add_seconds(7, (GSourceFunc) on_timeout_tick, &data1);
    on_timeout_tick(&data1);

    g_main_loop_run(loop);
    return EXIT_SUCCESS;
}
