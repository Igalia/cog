/*
 * Copyright © 2014 NVIDIA Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <drm_fourcc.h>
#include "xf86drm.h"
#include "kms.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static const char *const connector_names[] = {
        "Unknown",
        "VGA",
        "DVI-I",
        "DVI-D",
        "DVI-A",
        "Composite",
        "SVIDEO",
        "LVDS",
        "Component",
        "9PinDIN",
        "DisplayPort",
        "HDMI-A",
        "HDMI-B",
        "TV",
        "eDP",
        "Virtual",
        "DSI",
};

static void kms_device_probe_screens(struct kms_device *device)
{
    unsigned int counts[ARRAY_SIZE(connector_names)];
    struct kms_screen *screen;
    drmModeRes *res;

    memset(counts, 0, sizeof(counts));

    res = drmModeGetResources(device->fd);
    if (!res)
        return;

    device->screens = calloc(res->count_connectors, sizeof(screen));
    if (!device->screens)
        goto err_free_resources;

    for (int i = 0; i < res->count_connectors; i++) {
        unsigned int *count;
        const char *type;
        int len;

        screen = kms_screen_create(device, res->connectors[i]);
        if (!screen)
            continue;

        /* assign a unique name to this screen */
        type = connector_names[screen->type];
        count = &counts[screen->type];

        len = snprintf(NULL, 0, "%s-%u", type, *count);

        screen->name = malloc(len + 1);
        if (!screen->name) {
            free(screen);
            continue;
        }

        snprintf(screen->name, len + 1, "%s-%u", type, *count);
        (*count)++;

        device->screens[i] = screen;
        device->num_screens++;
    }

    err_free_resources:
    drmModeFreeResources(res);
}

static void kms_device_probe_crtcs(struct kms_device *device)
{
    struct kms_crtc *crtc;
    drmModeRes *res;

    res = drmModeGetResources(device->fd);
    if (!res)
        return;

    device->crtcs = calloc(res->count_crtcs, sizeof(crtc));
    if (!device->crtcs)
        goto err_free_resources;

    for (int i = 0; i < res->count_crtcs; i++) {
        crtc = kms_crtc_create(device, res->crtcs[i]);
        if (!crtc)
            continue;

        device->crtcs[i] = crtc;
        device->num_crtcs++;
    }

    err_free_resources:
    drmModeFreeResources(res);
}

static void kms_device_probe_planes(struct kms_device *device)
{
    struct kms_plane *plane;
    drmModePlaneRes *res;

    res = drmModeGetPlaneResources(device->fd);
    if (!res)
        return;

    device->planes = calloc(res->count_planes, sizeof(plane));
    if (!device->planes)
        goto err_free_resources;

    for (int i = 0; i < res->count_planes; i++) {
        plane = kms_plane_create(device, res->planes[i]);
        if (!plane)
            continue;

        device->planes[i] = plane;
        device->num_planes++;
    }

    err_free_resources:
    drmModeFreePlaneResources(res);
}

static void kms_device_probe(struct kms_device *device)
{
    kms_device_probe_screens(device);
    kms_device_probe_crtcs(device);
    kms_device_probe_planes(device);
}

struct kms_device *kms_device_open(int fd)
{
    struct kms_device *device;

    device = calloc(1, sizeof(*device));
    if (!device)
        return NULL;

    device->fd = fd;
    kms_device_probe(device);
    return device;
}

void kms_device_free(struct kms_device *device)
{
    for (int i = 0; i < device->num_planes; i++)
        kms_plane_free(device->planes[i]);

    free(device->planes);

    for (int i = 0; i < device->num_crtcs; i++)
        kms_crtc_free(device->crtcs[i]);

    free(device->crtcs);

    for (int i = 0; i < device->num_screens; i++)
        kms_screen_free(device->screens[i]);

    free(device->screens);
    free(device);
}

struct kms_plane *kms_device_find_plane_by_type(struct kms_device *device, uint32_t type, unsigned int index)
{
    for (int i = 0; i < device->num_planes; i++) {
        if (device->planes[i]->type == type) {
            if (index == 0)
                return device->planes[i];

            index--;
        }
    }

    return NULL;
}

struct kms_crtc *kms_crtc_create(struct kms_device *device, uint32_t id)
{
    struct kms_crtc *crtc;

    crtc = calloc(1, sizeof(*crtc));
    if (!crtc)
        return NULL;

    crtc->device = device;
    crtc->id = id;

    return crtc;
}

void kms_crtc_free(struct kms_crtc *crtc)
{
    free(crtc);
}

struct kms_framebuffer *kms_framebuffer_create(struct kms_device *device,
                                               unsigned int width,
                                               unsigned int height,
                                               uint32_t format)
{
    uint32_t handles[4], pitches[4], offsets[4];
    struct drm_mode_create_dumb args;
    struct kms_framebuffer *fb;
    int err;

    fb = calloc(1, sizeof(*fb));
    if (!fb)
        return NULL;

    fb->device = device;
    fb->width = width;
    fb->height = height;
    fb->format = format;

    memset(&args, 0, sizeof(args));
    args.width = width;
    args.height = height;

    switch (format) {
        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_XBGR8888:
        case DRM_FORMAT_RGBA8888:
        case DRM_FORMAT_ARGB8888:
            args.bpp = 32;
            break;

        default:
            free(fb);
            return NULL;
    }

    err = drmIoctl(device->fd, DRM_IOCTL_MODE_CREATE_DUMB, &args);
    if (err < 0) {
        free(fb);
        return NULL;
    }

    fb->handle = args.handle;
    fb->pitch = args.pitch;
    fb->size = args.size;

    handles[0] = fb->handle;
    pitches[0] = fb->pitch;
    offsets[0] = 0;

    err = drmModeAddFB2(device->fd, width, height, format, handles,
                        pitches, offsets, &fb->id, 0);
    if (err < 0) {
        kms_framebuffer_free(fb);
        return NULL;
    }

    return fb;
}

void kms_framebuffer_free(struct kms_framebuffer *fb)
{
    struct kms_device *device = fb->device;
    struct drm_mode_destroy_dumb args;
    int err;

    if (fb->id) {
        err = drmModeRmFB(device->fd, fb->id);
        if (err < 0) {
            /* not much we can do now */
        }
    }

    memset(&args, 0, sizeof(args));
    args.handle = fb->handle;

    err = drmIoctl(device->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &args);
    if (err < 0) {
        /* not much we can do now */
    }

    free(fb);
}

int kms_framebuffer_map(struct kms_framebuffer *fb, void **ptrp)
{
    struct kms_device *device = fb->device;
    struct drm_mode_map_dumb args;
    void *ptr;
    int err;

    if (fb->ptr) {
        *ptrp = fb->ptr;
        return 0;
    }

    memset(&args, 0, sizeof(args));
    args.handle = fb->handle;

    err = drmIoctl(device->fd, DRM_IOCTL_MODE_MAP_DUMB, &args);
    if (err < 0)
        return -errno;

    ptr = mmap(0, fb->size, PROT_READ | PROT_WRITE, MAP_SHARED,
               device->fd, args.offset);
    if (ptr == MAP_FAILED)
        return -errno;

    *ptrp = fb->ptr = ptr;

    return 0;
}

void kms_framebuffer_unmap(struct kms_framebuffer *fb)
{
    if (fb->ptr) {
        munmap(fb->ptr, fb->size);
        fb->ptr = NULL;
    }
}

static void kms_screen_probe(struct kms_screen *screen)
{
    struct kms_device *device = screen->device;
    drmModeConnector *con;

    con = drmModeGetConnector(device->fd, screen->id);
    if (!con)
        return;

    screen->type = con->connector_type;

    if (con->connection == DRM_MODE_CONNECTED)
        screen->connected = true;
    else
        screen->connected = false;

    memcpy(&screen->mode, &con->modes[0], sizeof(drmModeModeInfo));
    screen->width = screen->mode.hdisplay;
    screen->height = screen->mode.vdisplay;

    drmModeFreeConnector(con);
}

struct kms_screen *kms_screen_create(struct kms_device *device, uint32_t id)
{
    struct kms_screen *screen;

    screen = calloc(1, sizeof(*screen));
    if (!screen)
        return NULL;

    screen->device = device;
    screen->id = id;

    kms_screen_probe(screen);

    return screen;
}

void kms_screen_free(struct kms_screen *screen)
{
    if (screen)
        free(screen->name);

    free(screen);
}

int kms_screen_set(struct kms_screen *screen, struct kms_crtc *crtc, struct kms_framebuffer *fb)
{
    struct kms_device *device = screen->device;
    int err;

    err = drmModeSetCrtc(device->fd, crtc->id, fb->id, 0, 0, &screen->id,
                         1, &screen->mode);
    if (err < 0)
        return -errno;

    return 0;
}

static int kms_plane_probe(struct kms_plane *plane)
{
    struct kms_device *device = plane->device;
    drmModeObjectPropertiesPtr props;
    drmModePlane *p;
    unsigned int i;

    p = drmModeGetPlane(device->fd, plane->id);
    if (!p)
        return -ENODEV;

    /* TODO: allow dynamic assignment to CRTCs */
    if (p->crtc_id == 0) {
        for (i = 0; i < device->num_crtcs; i++) {
            if (p->possible_crtcs & (1 << i)) {
                p->crtc_id = device->crtcs[i]->id;
                break;
            }
        }
    }

    for (i = 0; i < device->num_crtcs; i++) {
        if (device->crtcs[i]->id == p->crtc_id) {
            plane->crtc = device->crtcs[i];
            break;
        }
    }

    plane->formats = calloc(p->count_formats, sizeof(uint32_t));
    if (!plane->formats) {
        drmModeFreePlane(p);
        return -ENOMEM;
    }

    for (i = 0; i < p->count_formats; i++)
        plane->formats[i] = p->formats[i];

    plane->num_formats = p->count_formats;

    drmModeFreePlane(p);

    props = drmModeObjectGetProperties(device->fd, plane->id,
                                       DRM_MODE_OBJECT_PLANE);
    if (!props)
        return -ENODEV;

    for (i = 0; i < props->count_props; i++) {
        drmModePropertyPtr prop;

        prop = drmModeGetProperty(device->fd, props->props[i]);
        if (prop) {
            if (strcmp(prop->name, "type") == 0)
                plane->type = props->prop_values[i];

            drmModeFreeProperty(prop);
        }
    }

    drmModeFreeObjectProperties(props);

    return 0;
}

struct kms_plane *kms_plane_create(struct kms_device *device, uint32_t id)
{
    struct kms_plane *plane;

    plane = calloc(1, sizeof(*plane));
    if (!plane)
        return NULL;

    plane->device = device;
    plane->id = id;

    kms_plane_probe(plane);

    return plane;
}

void kms_plane_free(struct kms_plane *plane)
{
    free(plane);
}

int kms_plane_set(struct kms_plane *plane, struct kms_framebuffer *fb, unsigned int x, unsigned int y)
{
    struct kms_device *device = plane->device;
    int err;

    err = drmModeSetPlane(device->fd, plane->id, plane->crtc->id, fb->id, 0,
                          x, y, fb->width, fb->height,
                          0, 0, fb->width << 16, fb->height << 16);
    if (err < 0)
        return -errno;

    return 0;
}

bool kms_plane_supports_format(struct kms_plane *plane, uint32_t format)
{
    unsigned int i;

    for (i = 0; i < plane->num_formats; i++)
        if (plane->formats[i] == format)
            return true;

    return false;
}
