/*
 * Copyright (C) 2021 Michal Artazov
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef COG_CURSOR_DRM_H
#define COG_CURSOR_DRM_H

#include "kms.h"
#include <stdint.h>

struct kms_framebuffer *create_cursor_framebuffer(struct kms_device *device, uint32_t format);

/* The built-in cursor image as premultiplied ARGB8888, for renderers
 * that composite a software cursor (broken hardware cursors exist,
 * see RADEON_DCE41_CURSOR_NOTES). dst must hold SIZE*SIZE pixels. */
#define COG_DRM_CURSOR_IMAGE_SIZE 16
void cog_drm_cursor_image_argb_premult(uint32_t *dst);

#endif //COG_CURSOR_DRM_H
