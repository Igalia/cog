/*
 * cog-platform-headless.c
 * Copyright (C) 2021, 2023 Igalia S.L
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../core/cog.h"
#include <errno.h>
#include <glib.h>
#include <wpe/fdo.h>
#include <wpe/unstable/fdo-shm.h>

struct _CogHeadlessView {
    CogView parent;

    bool                                    frame_ack_pending;
    struct wpe_view_backend_exportable_fdo *exportable;
};

G_DECLARE_FINAL_TYPE(CogHeadlessView, cog_headless_view, COG, HEADLESS_VIEW, CogView)
G_DEFINE_DYNAMIC_TYPE(CogHeadlessView, cog_headless_view, COG_TYPE_VIEW)

struct _CogHeadlessPlatformClass {
    CogPlatformClass parent_class;
};

struct _CogHeadlessPlatform {
    CogPlatform parent;

    unsigned max_fps;
    unsigned tick_source;

    GPtrArray *viewports; /* CogViewport */
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

static void
on_cog_headless_view_backend_destroy(CogHeadlessView *self)
{
    g_debug("%s: view %p, exportable %p", G_STRFUNC, self, self->exportable);
    g_assert(self->exportable);
    g_clear_pointer(&self->exportable, wpe_view_backend_exportable_fdo_destroy);
}

static WebKitWebViewBackend *
cog_headless_view_create_backend(CogView *view)
{
    CogHeadlessView *self = COG_HEADLESS_VIEW(view);

    static const struct wpe_view_backend_exportable_fdo_client client = {
        .export_shm_buffer = on_export_shm_buffer,
    };

    self->exportable = wpe_view_backend_exportable_fdo_create(&client, self, 800, 600);

    struct wpe_view_backend *view_backend = wpe_view_backend_exportable_fdo_get_view_backend(self->exportable);
    return webkit_web_view_backend_new(view_backend, (GDestroyNotify) on_cog_headless_view_backend_destroy, self);
}

static void
cog_headless_view_class_init(CogHeadlessViewClass *klass)
{
    CogViewClass *view_class = COG_VIEW_CLASS(klass);
    view_class->create_backend = cog_headless_view_create_backend;
}

static void
cog_headless_view_class_finalize(CogHeadlessViewClass *klass G_GNUC_UNUSED)
{
}

static void
cog_headless_view_init(CogHeadlessView *self G_GNUC_UNUSED)
{
}

static void
cog_headless_view_tick(CogHeadlessView *view)
{
    if (view->frame_ack_pending) {
        view->frame_ack_pending = false;
        wpe_view_backend_exportable_fdo_dispatch_frame_complete(view->exportable);
    }
}

static void
cog_headless_viewport_tick(CogViewport *viewport)
{
    cog_viewport_foreach(viewport, (GFunc) cog_headless_view_tick, NULL);
}

static gboolean
on_cog_headless_platform_tick(CogHeadlessPlatform *self)
{
    g_ptr_array_foreach(self->viewports, (GFunc) cog_headless_viewport_tick, NULL);
    return G_SOURCE_CONTINUE;
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

    self->tick_source = g_timeout_add(1000.0 / self->max_fps, G_SOURCE_FUNC(on_cog_headless_platform_tick), self);
    return TRUE;
}

static void
cog_headless_platform_finalize(GObject* object)
{
    CogHeadlessPlatform *self = COG_HEADLESS_PLATFORM(object);

    g_clear_handle_id(&self->tick_source, g_source_remove);
    g_clear_pointer(&self->viewports, g_ptr_array_unref);

    G_OBJECT_CLASS(cog_headless_platform_parent_class)->finalize(object);
}

static void
cog_headless_viewport_created(CogPlatform *platform, CogViewport *viewport)
{
    CogHeadlessPlatform *self = COG_HEADLESS_PLATFORM(platform);

    g_assert(!g_ptr_array_find(self->viewports, viewport, NULL));
    g_ptr_array_add(self->viewports, viewport);

    g_debug("%s: new viewport %p", G_STRFUNC, viewport);
}

static void
cog_headless_viewport_disposed(CogPlatform *platform, CogViewport *viewport)
{
    CogHeadlessPlatform *self = COG_HEADLESS_PLATFORM(platform);

    gboolean removed G_GNUC_UNUSED = g_ptr_array_remove_fast(self->viewports, viewport);
    g_assert(removed);

    g_debug("%s: removed viewport %p", G_STRFUNC, viewport);
}

static void
cog_headless_platform_class_init(CogHeadlessPlatformClass* klass)
{
    GObjectClass* object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = cog_headless_platform_finalize;

    CogPlatformClass* platform_class = COG_PLATFORM_CLASS(klass);
    platform_class->setup = cog_headless_platform_setup;
    platform_class->get_view_type = cog_headless_view_get_type;
    platform_class->viewport_created = cog_headless_viewport_created;
    platform_class->viewport_disposed = cog_headless_viewport_disposed;
}

static void
cog_headless_platform_class_finalize(CogHeadlessPlatformClass* klass G_GNUC_UNUSED)
{
}

static void
cog_headless_platform_init(CogHeadlessPlatform* self)
{
    self->viewports = g_ptr_array_sized_new(3);
    self->max_fps = 30; /* Default value */
}

G_MODULE_EXPORT void
g_io_cogplatform_headless_load(GIOModule* module)
{
    GTypeModule* type_module = G_TYPE_MODULE(module);
    cog_headless_platform_register_type(type_module);
    cog_headless_view_register_type(type_module);
}

G_MODULE_EXPORT void
g_io_cogplatform_headless_unload(GIOModule* module G_GNUC_UNUSED)
{
}
