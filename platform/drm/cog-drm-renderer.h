/*
 * cog-drm-renderer.h
 * Copyright (C) 2021 Igalia S.L.
 *
 * Distributed under terms of the MIT license.
 */

#pragma once

#include <glib.h>
#include <inttypes.h>
#include <stdbool.h>

struct gbm_device;
struct wpe_view_backend_exportable_fdo;
typedef struct _drmModeModeInfo drmModeModeInfo;
typedef struct _CogDrmRenderer  CogDrmRenderer;

struct _CogDrmRenderer {
    const char *name;

    bool (*initialize)(CogDrmRenderer *, GError **);
    void (*destroy)(CogDrmRenderer *);

    struct wpe_view_backend_exportable_fdo *(*create_exportable)(CogDrmRenderer *, uint32_t width, uint32_t height);
};

void cog_drm_renderer_destroy(CogDrmRenderer *self);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(CogDrmRenderer, cog_drm_renderer_destroy)

static inline bool
cog_drm_renderer_initialize(CogDrmRenderer *self, GError **error)
{
    if (self->initialize)
        return self->initialize(self, error);
    return true;
}

static inline struct wpe_view_backend_exportable_fdo *
cog_drm_renderer_create_exportable(CogDrmRenderer *self, uint32_t width, uint32_t height)
{
    return self->create_exportable(self, width, height);
}

CogDrmRenderer *cog_drm_modeset_renderer_new(struct gbm_device     *dev,
                                             uint32_t               plane_id,
                                             uint32_t               crtc_id,
                                             uint32_t               connector_id,
                                             const drmModeModeInfo *mode,
                                             bool                   atomic_modesetting);
