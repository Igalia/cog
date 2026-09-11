/*
 * Copyright (C) 2021 Michal Artazov
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef COG_CURSOR_DRM_H
#define COG_CURSOR_DRM_H

#include "kms.h"
#include <stdint.h>

/* scale enlarges the 16x16 cursor artwork (nearest-neighbour) so it keeps
 * pace with the view's device scale factor; the 64x64 hardware cursor
 * buffer bounds it to 4. */
struct kms_framebuffer *create_cursor_framebuffer(struct kms_device *device, uint32_t format, unsigned scale);

/* The built-in cursor image as premultiplied ARGB8888, for renderers
 * that composite a software cursor (broken hardware cursors exist,
 * see RADEON_DCE41_CURSOR_NOTES). dst must hold (SIZE*scale)^2 pixels,
 * scale in [1, MAX_SCALE]. */
#define COG_DRM_CURSOR_IMAGE_SIZE 16
#define COG_DRM_CURSOR_IMAGE_MAX_SCALE 4
void cog_drm_cursor_image_argb_premult(uint32_t *dst, unsigned scale);

#endif //COG_CURSOR_DRM_H
