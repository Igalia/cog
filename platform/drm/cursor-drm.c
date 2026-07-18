/*
 * Copyright (C) 2021 Michal Artazov
 *
 * SPDX-License-Identifier: MIT
 */

#include <drm_fourcc.h>
#include <stdio.h>
#include "cursor-drm.h"

#define CURSOR_WIDTH 16
#define CURSOR_HEIGHT 16

static const uint8_t cursorData[CURSOR_WIDTH * CURSOR_HEIGHT * 4] = {
        "\370\370\370\231\0\0\0\0\0\0\0\0\377\377\377\2\0\0\0\0\0\0\0\0\0\0\0"
        "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\0\0\0\377\377\377\377\346\346\346\232\0\0\0\0\0\0\0\0\377\377\377\1"
        "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\0\0\0\0\0\0\0\0\0\0\316\316\316\373\267\267\267\377\363\363\363\222"
        "\0\0\0\0\377\377\377\1\200\200\200\2\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\326\326\326\376\0\0"
        "\0\374\314\314\314\377\360\360\360\211\0\0\0\0\377\377\377\2\0\0\0\1"
        "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\0\0\326\326\326\377\0\0\0\376\0\0\0\375\320\320\320\377\361\361\361"
        "\200\0\0\0\0\377\377\377\2\377\377\377\1\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\325\325\325\377\0\0\0\377'''\375"
        "\0\0\0\375\326\326\326\377\362\362\362x\0\0\0\0\377\377\377\3\0\0\0\1"
        "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\325\325\325"
        "\377\0\0\0\377000\377\26\26\26\375\0\0\0\376\326\326\326\377\357\357"
        "\357n\0\0\0\0\377\377\377\1\0\0\0\1\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\0\0\0\0\0\0\0\325\325\325\377\0\0\0\377!!!\377000\377III\375aaa\377"
        "\357\357\357\377\364\364\364s\0\0\0\0\377\377\377\2\0\0\0\0\0\0\0\0\0"
        "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\327\327\327\377\0\0\0\376\0\0\0\374\0"
        "\0\0\376\327\327\327\377\336\336\336\333\326\326\326\326\347\347\347"
        "\330\254\254\2547\0\0\0\0\377\377\377\2\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\0\0\0\0\0\320\320\320\376\0\0\0\377\344\344\344\377ooo\375\244\244\244"
        "\377\335\335\335\225\0\0\0\20\0\0\0\33KKK\21\0\0\0\0\0\0\0\0\0\0\0\0"
        "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\340\340\340\377\303\303\303\375\346"
        "\346\346\243\311\311\311\357\13\13\13\377\334\334\334\343\247\247\247"
        "\35\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\0\0\0\372\372\372\377\263\263\263\206\0\0\0\2\371\371\371\242\203\203"
        "\203\377\275\275\275\377\351\351\351t\0\0\0\0\377\377\377\4\0\0\0\0\0"
        "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\275\275\275]\0\0\0\17"
        "\0\0\0\0\335\335\335&\343\343\343\343\324\324\324\350\235\235\235N\0"
        "\0\0\0\377\377\377\1\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\0\0\0\0\0\0\0\0\377\377\377\1\377\377\377\2\0\0\0\0\252\252\252\33m"
        "mm*\0\0\0\5\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\0\0\0\0\0\0\0\377\377\377\2\0\0\0\0\0\0\0\0\377\377\377\1\0\0\0\0\0"
        "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\377\377\377\2\377"
        "\377\377\2\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\0\0\0\0\0\0\0\0\0\0\0",
};

static uint32_t convert_rgba_to_pixel_format(uint32_t rgba_pixel, uint32_t format)
{
    switch (format) {
        case DRM_FORMAT_RGBA8888:
            return rgba_pixel;

        case DRM_FORMAT_ARGB8888: {
            /* KMS blending (hardware cursors included) expects
             * PREMULTIPLIED alpha; the cursor image data is straight
             * alpha. Without premultiplication every pixel whose color
             * exceeds its alpha blends additively and washes out - on a
             * light page only the black outline stays visible, reducing
             * the arrow to a thin dotted line. */
            uint8_t alpha = rgba_pixel & 0xff;
            uint8_t r = ((rgba_pixel >> 24) & 0xff) * alpha / 255;
            uint8_t g = ((rgba_pixel >> 16) & 0xff) * alpha / 255;
            uint8_t b = ((rgba_pixel >> 8) & 0xff) * alpha / 255;
            return ((uint32_t) alpha << 24) | ((uint32_t) r << 16) | ((uint32_t) g << 8) | b;
        }

        default:
            return 0;
    }
}

void cog_drm_cursor_image_argb_premult(uint32_t *dst)
{
    for (int i = 0; i < CURSOR_WIDTH * CURSOR_HEIGHT; i++) {
        uint8_t r = cursorData[i * 4 + 0];
        uint8_t g = cursorData[i * 4 + 1];
        uint8_t b = cursorData[i * 4 + 2];
        uint8_t a = cursorData[i * 4 + 3];
        r = (uint16_t) r * a / 255;
        g = (uint16_t) g * a / 255;
        b = (uint16_t) b * a / 255;
        dst[i] = ((uint32_t) a << 24) | ((uint32_t) r << 16) | ((uint32_t) g << 8) | b;
    }
}

struct kms_framebuffer *create_cursor_framebuffer(struct kms_device *device, uint32_t format)
{
    struct kms_framebuffer *fb;
    uint32_t *buf;

    /* Hardware cursors on several drivers (radeon in particular) only
     * display buffers of exactly the size advertised by
     * DRM_CAP_CURSOR_WIDTH/HEIGHT - typically 64x64. Smaller uploads are
     * accepted by the ioctl but shown as nothing. Allocate 64x64 and
     * blit the cursor image into the top-left corner, rest transparent. */
    fb = kms_framebuffer_create(device, 64, 64, format);
    if (!fb)
        return NULL;

    if (kms_framebuffer_map(fb, (void *) &buf))
        return NULL;

    int index;
    uint32_t pixel;

    /* The legacy cursor engine ignores the BO's pitch and always reads
     * tightly packed WIDTHx4-byte rows (radeon aligns dumb-buffer
     * pitches far wider, e.g. 256 pixels - writing with that pitch
     * smears the image into a dotted vertical line). Lay the pixels
     * out with the hardware's fixed 64-pixel stride. */
    unsigned int stride_px = 64;

    for (unsigned int row = 0; row < fb->height; row++) {
        for (unsigned int column = 0; column < stride_px; column++) {
            if (row < CURSOR_HEIGHT && column < CURSOR_WIDTH) {
                index = (row * CURSOR_WIDTH * 4) + (column * 4);
                pixel = (cursorData[index] << 24) +
                        (cursorData[index + 1] << 16) +
                        (cursorData[index + 2] << 8) +
                        cursorData[index + 3];
                buf[row * stride_px + column] = convert_rgba_to_pixel_format(pixel, format);
            } else
                buf[row * stride_px + column] = 0;
        }
    }

    kms_framebuffer_unmap(fb);
    return fb;
}
