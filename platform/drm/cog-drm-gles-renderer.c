/*
 * cog-drm-gles-renderer.c
 * Copyright (C) 2021 Igalia S.L.
 *
 * Distributed under terms of the MIT license.
 */

#include "cog-drm-renderer.h"
#include <epoxy/gl.h>
#include <wayland-util.h>
#include <wpe/fdo-egl.h>
#include <wpe/fdo.h>

typedef struct {
    CogDrmRenderer base;

    struct wpe_view_backend_exportable_fdo *exportable;
} CogDrmGlesRenderer;

static void
cog_drm_gles_renderer_handle_egl_image(void *data, struct wpe_fdo_egl_exported_image *image)
{
    CogDrmGlesRenderer *self = data;

    /* TODO: Actually do display the image. */
    wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(self->exportable, image);

    wpe_view_backend_exportable_fdo_dispatch_frame_complete(self->exportable);
}

static bool
cog_drm_gles_renderer_initialize(CogDrmRenderer *renderer, GError **error)
{
    CogDrmGlesRenderer *self = wl_container_of(renderer, self, base);
    return true;
}

static void
cog_drm_gles_renderer_destroy(CogDrmRenderer *renderer)
{
    CogDrmGlesRenderer *self = wl_container_of(renderer, self, base);
}

static struct wpe_view_backend_exportable_fdo *
cog_drm_gles_renderer_create_exportable(CogDrmRenderer *renderer, uint32_t width, uint32_t height)
{
    static const struct wpe_view_backend_exportable_fdo_egl_client client = {
        .export_fdo_egl_image = cog_drm_gles_renderer_handle_egl_image,
    };

    CogDrmGlesRenderer *self = wl_container_of(renderer, self, base);
    return (self->exportable = wpe_view_backend_exportable_fdo_egl_create(&client, renderer, width, height));
}

CogDrmRenderer *
cog_drm_gles_renderer_new(EGLDisplay display)
{
    g_assert(display != EGL_NO_DISPLAY);

    CogDrmGlesRenderer *self = g_slice_new(CogDrmGlesRenderer);
    *self = (CogDrmGlesRenderer){
        .base.name = "gles",
        .base.initialize = cog_drm_gles_renderer_initialize,
        .base.destroy = cog_drm_gles_renderer_destroy,
        .base.create_exportable = cog_drm_gles_renderer_create_exportable,
    };

    return &self->base;
}
