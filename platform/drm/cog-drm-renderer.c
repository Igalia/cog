/*
 * cog-drm-renderer.c
 * Copyright (C) 2021 Igalia S.L.
 *
 * SPDX-License-Identifier: MIT
 */

#include "cog-drm-renderer.h"

void
cog_drm_renderer_destroy(CogDrmRenderer *self)
{
    if (self) {
        g_assert(self->destroy);
        self->destroy(self);
    }
}
