/*
 * view-stack.c
 * Copyright (C) 2023 Igalia S.L.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../core/cog.h"

typedef struct {
    CogViewGroup *views;
    gsize         current_index;
} TimeoutData;

static gboolean
on_timeout_tick(TimeoutData *data)
{
    if (++data->current_index >= cog_view_group_get_n_views(data->views))
        data->current_index = 0;

    CogView *view = cog_view_group_get_nth_view(data->views, data->current_index);
    g_message("Set visible view %zu <%p>", data->current_index, view);
    cog_view_stack_set_visible_view(COG_VIEW_STACK(data->views), view);

    return G_SOURCE_CONTINUE;
}

int
main(int argc, char *argv[])
{
    g_set_prgname("view-stack");
    cog_modules_add_directory(g_getenv("COG_MODULEDIR") ?: COG_MODULEDIR);

    g_autoptr(GError)      error = NULL;
    g_autoptr(CogShell)    shell = cog_shell_new(g_get_prgname(), FALSE);
    g_autoptr(CogPlatform) platform = cog_platform_configure(NULL, NULL, "COG", shell, &error);
    if (!platform)
        g_error("Cannot configure platform: %s", error->message);
    cog_shell_startup(shell);

    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);

    CogViewGroup *view_group0 = COG_VIEW_GROUP(cog_shell_view_stack_lookup(shell, COG_VIEW_STACK_DEFAULT));
    g_assert(view_group0);

    CogViewGroup *view_group1 = COG_VIEW_GROUP(cog_shell_view_stack_new(shell, GUINT_TO_POINTER(1)));

    COG_VIEW_GROUP(cog_shell_view_stack_new(shell, GUINT_TO_POINTER(2)));
    bool deleted = cog_shell_view_stack_remove(shell, GUINT_TO_POINTER(2));
    g_assert(deleted);

    for (int i = 1; i < argc; i++) {
        g_autoptr(CogView) view = cog_view_new(NULL);
        cog_platform_init_web_view(platform, WEBKIT_WEB_VIEW(view));
        cog_view_group_add(view_group1, view);
        webkit_web_view_load_uri(WEBKIT_WEB_VIEW(view), argv[i]);
        g_message("Created view %p, URI %s in group 1", view, argv[i]);
    }

    TimeoutData data1 = {
        .views = view_group1,
        .current_index = SIZE_MAX,
    };

    g_timeout_add_seconds(3, (GSourceFunc) on_timeout_tick, &data1);
    on_timeout_tick(&data1);

    g_main_loop_run(loop);
    return EXIT_SUCCESS;
}
