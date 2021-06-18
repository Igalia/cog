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

struct _CogHeadlessPlatformClass {
    CogPlatformClass parent_class;
};

struct _CogHeadlessPlatform {
    CogPlatform parent;
};

G_DECLARE_FINAL_TYPE(CogHeadlessPlatform, cog_headless_platform, COG, HEADLESS_PLATFORM, CogPlatform)

G_DEFINE_DYNAMIC_TYPE_EXTENDED(
    CogHeadlessPlatform,
    cog_headless_platform,
    COG_TYPE_PLATFORM,
    0,
    g_io_extension_point_implement(COG_MODULES_PLATFORM_EXTENSION_POINT, g_define_type_id, "headless", 100);)

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

static gboolean
cog_headless_platform_setup(CogPlatform* platform, CogShell* shell G_GNUC_UNUSED, const char* params, GError** error)
{
    setup_fdo_exportable(&win);

    // Maintain rendering at 30 FPS.
    win.tick_source = g_timeout_add(1000/30, G_SOURCE_FUNC(tick_callback), &win);

    return TRUE;
}

static void
cog_headless_platform_teardown(CogPlatform* platform)
{
    g_source_remove(win.tick_source);
    wpe_view_backend_exportable_fdo_destroy(win.exportable);
}

static WebKitWebViewBackend*
cog_headless_platform_get_view_backend(CogPlatform* platform, WebKitWebView* related_view, GError** error)
{
    g_assert_nonnull(win.view_backend);
    return win.view_backend;
}

static void
cog_headless_platform_class_init(CogHeadlessPlatformClass* klass)
{
    CogPlatformClass* platform_class = COG_PLATFORM_CLASS(klass);
    platform_class->setup = cog_headless_platform_setup;
    platform_class->teardown = cog_headless_platform_teardown;
    platform_class->get_view_backend = cog_headless_platform_get_view_backend;
}

static void
cog_headless_platform_class_finalize(CogHeadlessPlatformClass* klass G_GNUC_UNUSED)
{
}

static void
cog_headless_platform_init(CogHeadlessPlatform* self)
{
}

G_MODULE_EXPORT void
g_io_cogplatform_headless_load(GIOModule* module)
{
    GTypeModule* type_module = G_TYPE_MODULE(module);
    cog_headless_platform_register_type(type_module);
}

G_MODULE_EXPORT void
g_io_cogplatform_headless_unload(GIOModule* module G_GNUC_UNUSED)
{
}
