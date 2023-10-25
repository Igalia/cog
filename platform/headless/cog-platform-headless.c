/*
 * cog-platform-headless.c
 * Copyright (C) 2021 Igalia S.L
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../core/cog.h"
#include <errno.h>
#include <glib.h>
#include <wpe/fdo.h>
#include <wpe/unstable/fdo-shm.h>

typedef struct {
    bool                                    frame_ack_pending;
    struct wpe_view_backend_exportable_fdo *exportable;
} CogHeadlessView;

struct _CogHeadlessPlatformClass {
    CogPlatformClass parent_class;
};

struct _CogHeadlessPlatform {
    CogPlatform parent;

    unsigned max_fps;
    unsigned tick_source;

    /* TODO: Move elsewhere once multiple views are supported */
    CogHeadlessView view;
};

G_DECLARE_FINAL_TYPE(CogHeadlessPlatform, cog_headless_platform, COG, HEADLESS_PLATFORM, CogPlatform)

G_DEFINE_DYNAMIC_TYPE_EXTENDED(
    CogHeadlessPlatform,
    cog_headless_platform,
    COG_TYPE_PLATFORM,
    0,
    g_io_extension_point_implement(COG_MODULES_PLATFORM_EXTENSION_POINT, g_define_type_id, "headless", 100);)

static void on_export_shm_buffer(void* data, struct wpe_fdo_shm_exported_buffer* buffer)
{
    CogHeadlessView *view = data;
    wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(view->exportable, buffer);
    view->frame_ack_pending = true;
}

static gboolean
on_cog_headless_platform_tick(CogHeadlessPlatform *self)
{
    CogHeadlessView *view = &self->view;
    if (view->frame_ack_pending) {
        view->frame_ack_pending = false;
        wpe_view_backend_exportable_fdo_dispatch_frame_complete(view->exportable);
    }
    return G_SOURCE_CONTINUE;
}

static void
cog_headless_view_initialize(CogHeadlessView *self, CogHeadlessPlatform *platform)
{
    static const struct wpe_view_backend_exportable_fdo_client client = {
        .export_shm_buffer = on_export_shm_buffer,
    };

    self->exportable = wpe_view_backend_exportable_fdo_create(&client, self, 800, 600);
}

static gboolean
cog_headless_platform_setup(CogPlatform* platform, CogShell* shell G_GNUC_UNUSED, const char* params, GError** error)
{
    CogHeadlessPlatform *self = COG_HEADLESS_PLATFORM(platform);

    wpe_loader_init("libWPEBackend-fdo-1.0.so");
    wpe_fdo_initialize_shm();

    if (params && params[0] != '\0') {
        uint64_t value = g_ascii_strtoull(params, NULL, 0);
        if ((value == UINT64_MAX && errno == ERANGE) || value == 0 || value > UINT_MAX)
            g_warning("Invalid refresh rate value '%s', ignored", params);
        else
            self->max_fps = (unsigned) value;
    }
    g_debug("Maximum refresh rate: %u FPS", self->max_fps);

    cog_headless_view_initialize(&self->view, self);
    self->tick_source = g_timeout_add(1000.0 / self->max_fps, G_SOURCE_FUNC(on_cog_headless_platform_tick), self);
    return TRUE;
}

static void
cog_headless_platform_finalize(GObject* object)
{
    CogHeadlessPlatform *self = COG_HEADLESS_PLATFORM(object);

    g_clear_handle_id(&self->tick_source, g_source_remove);

    G_OBJECT_CLASS(cog_headless_platform_parent_class)->finalize(object);
}

static void
on_cog_headless_view_backend_destroy(CogHeadlessView *self)
{
    g_assert(self->exportable);
    g_clear_pointer(&self->exportable, wpe_view_backend_exportable_fdo_destroy);
}

static WebKitWebViewBackend*
cog_headless_platform_get_view_backend(CogPlatform* platform, WebKitWebView* related_view, GError** error)
{
    CogHeadlessPlatform *self = COG_HEADLESS_PLATFORM(platform);

    struct wpe_view_backend *view_backend = wpe_view_backend_exportable_fdo_get_view_backend(self->view.exportable);
    WebKitWebViewBackend    *wk_view_backend =
        webkit_web_view_backend_new(view_backend, (GDestroyNotify) on_cog_headless_view_backend_destroy, &self->view);

    g_assert(wk_view_backend);
    return wk_view_backend;
}

static void
cog_headless_platform_class_init(CogHeadlessPlatformClass* klass)
{
    GObjectClass* object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = cog_headless_platform_finalize;

    CogPlatformClass* platform_class = COG_PLATFORM_CLASS(klass);
    platform_class->setup = cog_headless_platform_setup;
    platform_class->get_view_backend = cog_headless_platform_get_view_backend;
}

static void
cog_headless_platform_class_finalize(CogHeadlessPlatformClass* klass G_GNUC_UNUSED)
{
}

static void
cog_headless_platform_init(CogHeadlessPlatform* self)
{
    self->max_fps = 30; /* Default value */
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
