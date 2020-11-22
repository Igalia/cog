
#ifndef COG_CURSOR_DRM_H
#define COG_CURSOR_DRM_H

#include "kms.h"

struct kms_framebuffer *create_cursor_framebuffer(struct kms_device *device, uint32_t format);

#endif //COG_CURSOR_DRM_H
