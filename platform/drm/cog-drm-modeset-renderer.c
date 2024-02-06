/*
 * cog-drm-modeset-renderer.c
 * Copyright (C) 2021 Igalia S.L.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../core/cog.h"
#include "cog-drm-renderer.h"
#include <errno.h>
#include <gbm.h>
#include <wayland-server.h>
#include <wpe/fdo.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

typedef struct {
    GSource         base;
    GPollFD         pfd;
    drmEventContext event_context;
} DrmEventSource;

static gboolean
drm_source_check(GSource *source)
{
    DrmEventSource *self = wl_container_of(source, self, base);
    return !!self->pfd.revents;
}

static gboolean
drm_source_dispatch(GSource *source, GSourceFunc callback, gpointer user_data)
{
    DrmEventSource *self = wl_container_of(source, self, base);
    if (self->pfd.revents & (G_IO_ERR | G_IO_HUP))
        return FALSE;

    if (self->pfd.revents & G_IO_IN)
        drmHandleEvent(self->pfd.fd, &self->event_context);
    self->pfd.revents = 0;
    return TRUE;
}

static void drm_page_flip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void *);

static GSource *
drm_event_source_new(int fd)
{
    static GSourceFuncs funcs = {
        .check = drm_source_check,
        .dispatch = drm_source_dispatch,
    };

    DrmEventSource *self = (DrmEventSource *) g_source_new(&funcs, sizeof(DrmEventSource));
    self->event_context = (drmEventContext){
        .version = DRM_EVENT_CONTEXT_VERSION,
        .page_flip_handler = drm_page_flip_handler,
    };
    self->pfd = (GPollFD){
        .fd = fd,
        .events = G_IO_IN | G_IO_ERR | G_IO_HUP,
    };

    g_source_add_poll(&self->base, &self->pfd);
    g_source_set_name(&self->base, "cog: drm");
    g_source_set_can_recurse(&self->base, TRUE);

    return &self->base;
}

struct buffer_object {
    struct wl_list     link;
    struct wl_listener destroy_listener;

    uint32_t            fb_id;
    struct gbm_bo      *bo;
    struct wl_resource *buffer_resource;

    struct {
        struct wl_resource                 *resource;
        struct wpe_fdo_shm_exported_buffer *shm_buffer;
    } export;
};

typedef struct {
    CogDrmRenderer base;

    GSource *drm_source;

    struct buffer_object *committed_buffer;
    struct wl_list        buffer_list; /* buffer_object::link */

    struct wpe_view_backend_exportable_fdo *exportable;

    struct gbm_device *gbm_dev;

    uint32_t        crtc_id;
    uint32_t        connector_id;
    uint32_t        plane_id;
    drmModeModeInfo mode;
    bool            mode_set;
    bool            atomic_modesetting;
    bool            addfb2_modifiers;

    struct {
        drmModeObjectProperties *props;
        drmModePropertyRes     **props_info;
    } connector_props, crtc_props, plane_props;
} CogDrmModesetRenderer;

static inline int
get_drm_fd(CogDrmModesetRenderer *self)
{
    DrmEventSource *s = wl_container_of(self->drm_source, s, base);
    return s->pfd.fd;
}

static void
destroy_buffer(CogDrmModesetRenderer *renderer, struct buffer_object *buffer)
{
    drmModeRmFB(get_drm_fd(renderer), buffer->fb_id);
    gbm_bo_destroy(buffer->bo);

    if (buffer->export.resource) {
        wpe_view_backend_exportable_fdo_dispatch_release_buffer(renderer->exportable, buffer->export.resource);
        buffer->export.resource = NULL;
    }

    if (buffer->export.shm_buffer) {
        wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(renderer->exportable,
                                                                             buffer->export.shm_buffer);
        buffer->export.shm_buffer = NULL;
    }

    g_free(buffer);
}

static void
destroy_buffer_notify(struct wl_listener *listener, void *data)
{
    struct buffer_object  *buffer = wl_container_of(listener, buffer, destroy_listener);
    CogDrmModesetRenderer *renderer = wl_resource_get_user_data(buffer->buffer_resource);

    if (renderer->committed_buffer == buffer)
        renderer->committed_buffer = NULL;

    wl_list_remove(&buffer->link);

    wl_resource_set_user_data(buffer->buffer_resource, NULL);
    destroy_buffer(renderer, buffer);
}

static struct buffer_object *
drm_buffer_for_resource(CogDrmModesetRenderer *renderer, struct wl_resource *buffer_resource)
{
    struct buffer_object *buffer;
    wl_list_for_each(buffer, &renderer->buffer_list, link) {
        if (buffer->buffer_resource == buffer_resource)
            return buffer;
    }
    return NULL;
}

static struct buffer_object *
drm_create_buffer_for_bo(CogDrmModesetRenderer *self,
                         struct gbm_bo         *bo,
                         struct wl_resource    *buffer_resource,
                         uint32_t               width,
                         uint32_t               height,
                         uint32_t               format)
{
    uint32_t in_handles[4] = {
        0,
    };
    uint32_t in_strides[4] = {
        0,
    };
    uint32_t in_offsets[4] = {
        0,
    };
    uint64_t in_modifiers[4] = {
        0,
    };

    in_modifiers[0] = gbm_bo_get_modifier(bo);

    int plane_count = MIN(gbm_bo_get_plane_count(bo), 4);
    for (int i = 0; i < plane_count; ++i) {
        in_handles[i] = gbm_bo_get_handle_for_plane(bo, i).u32;
        in_strides[i] = gbm_bo_get_stride_for_plane(bo, i);
        in_offsets[i] = gbm_bo_get_offset(bo, i);
        in_modifiers[i] = in_modifiers[0];
    }

    int      ret;
    uint32_t fb_id = 0;

    if (G_LIKELY(self->addfb2_modifiers)) {
        ret = drmModeAddFB2WithModifiers(get_drm_fd(self), width, height, format, in_handles, in_strides, in_offsets,
                                         in_modifiers, &fb_id, DRM_MODE_FB_MODIFIERS);
    } else {
        in_handles[0] = gbm_bo_get_handle(bo).u32;
        in_handles[1] = in_handles[2] = in_handles[3] = 0;
        in_strides[0] = gbm_bo_get_stride(bo);
        in_strides[1] = in_strides[2] = in_strides[3] = 0;
        in_offsets[0] = in_offsets[1] = in_offsets[2] = in_offsets[3] = 0;

        ret = drmModeAddFB2(get_drm_fd(self), width, height, format, in_handles, in_strides, in_offsets, &fb_id, 0);
    }

    if (ret) {
        g_warning("failed to create framebuffer: %s", strerror(errno));
        return NULL;
    }

    struct buffer_object *buffer = g_new0(struct buffer_object, 1);
    wl_list_insert(&self->buffer_list, &buffer->link);
    buffer->destroy_listener.notify = destroy_buffer_notify;
    wl_resource_add_destroy_listener(buffer_resource, &buffer->destroy_listener);
    wl_resource_set_user_data(buffer_resource, self);

    buffer->fb_id = fb_id;
    buffer->bo = bo;
    buffer->buffer_resource = buffer_resource;

    return buffer;
}

static struct buffer_object *
drm_create_buffer_for_shm_buffer(CogDrmModesetRenderer *self,
                                 struct wl_resource    *buffer_resource,
                                 struct wl_shm_buffer  *shm_buffer)
{
    uint32_t format = wl_shm_buffer_get_format(shm_buffer);
    if (format != WL_SHM_FORMAT_ARGB8888 && format != WL_SHM_FORMAT_XRGB8888) {
        g_warning("failed to handle non-32-bit ARGB/XRGB format");
        return NULL;
    }

    int32_t width = wl_shm_buffer_get_width(shm_buffer);
    int32_t height = wl_shm_buffer_get_height(shm_buffer);

    // TODO: don't ignore the alpha channel in case of ARGB8888 SHM data
    uint32_t       gbm_format = GBM_FORMAT_XRGB8888;
    struct gbm_bo *bo = gbm_bo_create(self->gbm_dev, width, height, gbm_format, GBM_BO_USE_SCANOUT | GBM_BO_USE_WRITE);
    if (!bo) {
        g_warning("failed to create a gbm_bo object");
        return NULL;
    }

    uint32_t in_handles[4] = {
        0,
    };
    uint32_t in_strides[4] = {
        0,
    };
    uint32_t in_offsets[4] = {
        0,
    };
    in_handles[0] = gbm_bo_get_handle(bo).u32;
    in_strides[0] = gbm_bo_get_stride(bo);

    uint32_t fb_id = 0;
    int ret = drmModeAddFB2(get_drm_fd(self), width, height, gbm_format, in_handles, in_strides, in_offsets, &fb_id, 0);
    if (ret) {
        gbm_bo_destroy(bo);
        g_warning("failed to create framebuffer: %s", g_strerror(errno));
        return NULL;
    }

    struct buffer_object *buffer = g_new0(struct buffer_object, 1);
    wl_list_insert(&self->buffer_list, &buffer->link);
    buffer->destroy_listener.notify = destroy_buffer_notify;
    wl_resource_add_destroy_listener(buffer_resource, &buffer->destroy_listener);
    wl_resource_set_user_data(buffer_resource, self);

    buffer->fb_id = fb_id;
    buffer->bo = bo;
    buffer->buffer_resource = buffer_resource;

    return buffer;
}

static void
drm_copy_shm_buffer_into_bo(struct wl_shm_buffer *shm_buffer, struct gbm_bo *bo)
{
    int32_t width = wl_shm_buffer_get_width(shm_buffer);
    int32_t height = wl_shm_buffer_get_height(shm_buffer);
    int32_t stride = wl_shm_buffer_get_stride(shm_buffer);

    uint32_t bo_stride = 0;
    void    *map_data = NULL;
    gbm_bo_map(bo, 0, 0, width, height, GBM_BO_TRANSFER_WRITE, &bo_stride, &map_data);
    if (!map_data)
        return;

    wl_shm_buffer_begin_access(shm_buffer);

    uint8_t *src = wl_shm_buffer_get_data(shm_buffer);
    uint8_t *dst = map_data;

    uint32_t bo_width = gbm_bo_get_width(bo);
    uint32_t bo_height = gbm_bo_get_height(bo);
    if (!(width == bo_width && height == bo_height && stride == bo_stride)) {
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                dst[bo_stride * y + 4 * x + 0] = src[stride * y + 4 * x + 0];
                dst[bo_stride * y + 4 * x + 1] = src[stride * y + 4 * x + 1];
                dst[bo_stride * y + 4 * x + 2] = src[stride * y + 4 * x + 2];
                dst[bo_stride * y + 4 * x + 3] = src[stride * y + 4 * x + 3];
            }
        }
    } else
        memcpy(dst, src, stride * height);

    wl_shm_buffer_end_access(shm_buffer);
    gbm_bo_unmap(bo, map_data);
}

typedef struct {
    CogDrmModesetRenderer *renderer;
    struct buffer_object  *buffer;
} FlipHandlerData;

static int
drm_commit_buffer_nonatomic(CogDrmModesetRenderer *self, struct buffer_object *buffer)
{
    if (!self->mode_set) {
        int ret =
            drmModeSetCrtc(get_drm_fd(self), self->crtc_id, buffer->fb_id, 0, 0, &self->connector_id, 1, &self->mode);
        if (ret)
            return -1;

        self->mode_set = true;
    }

    FlipHandlerData *data = g_slice_new(FlipHandlerData);
    *data = (FlipHandlerData){self, buffer};

    return drmModePageFlip(get_drm_fd(self), self->crtc_id, buffer->fb_id, DRM_MODE_PAGE_FLIP_EVENT, data);
}

static int
add_property(drmModeObjectProperties *props,
             drmModePropertyRes     **props_info,
             drmModeAtomicReq        *req,
             uint32_t                 obj_id,
             const char              *name,
             uint64_t                 value)
{
    for (int i = 0; i < props->count_props; ++i) {
        if (!g_strcmp0(props_info[i]->name, name)) {
            int ret = drmModeAtomicAddProperty(req, obj_id, props_info[i]->prop_id, value);
            return (ret > 0) ? 0 : -1;
        }
    }

    return -1;
}

static int
add_connector_property(CogDrmModesetRenderer *self,
                       drmModeAtomicReq      *req,
                       uint32_t               obj_id,
                       const char            *name,
                       uint64_t               value)
{
    return add_property(self->connector_props.props, self->connector_props.props_info, req, obj_id, name, value);
}

static int
add_crtc_property(CogDrmModesetRenderer *self, drmModeAtomicReq *req, uint32_t obj_id, const char *name, uint64_t value)
{
    return add_property(self->crtc_props.props, self->crtc_props.props_info, req, obj_id, name, value);
}

static int
add_plane_property(CogDrmModesetRenderer *self,
                   drmModeAtomicReq      *req,
                   uint32_t               obj_id,
                   const char            *name,
                   uint64_t               value)
{
    return add_property(self->plane_props.props, self->plane_props.props_info, req, obj_id, name, value);
}

static int
drm_commit_buffer_atomic(CogDrmModesetRenderer *self, struct buffer_object *buffer)
{
    int      ret = 0;
    uint32_t flags = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK;

    drmModeAtomicReq *req = drmModeAtomicAlloc();

    if (!self->mode_set) {
        flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;

        uint32_t blob_id;
        ret = drmModeCreatePropertyBlob(get_drm_fd(self), &self->mode, sizeof(drmModeModeInfo), &blob_id);
        if (ret) {
            drmModeAtomicFree(req);
            return -1;
        }

        ret |= add_connector_property(self, req, self->connector_id, "CRTC_ID", self->crtc_id);
        ret |= add_crtc_property(self, req, self->crtc_id, "MODE_ID", blob_id);
        ret |= add_crtc_property(self, req, self->crtc_id, "ACTIVE", 1);
        if (ret) {
            drmModeAtomicFree(req);
            return -1;
        }

        self->mode_set = true;
    }

    ret |= add_plane_property(self, req, self->plane_id, "FB_ID", buffer->fb_id);
    ret |= add_plane_property(self, req, self->plane_id, "CRTC_ID", self->crtc_id);
    ret |= add_plane_property(self, req, self->plane_id, "SRC_X", 0);
    ret |= add_plane_property(self, req, self->plane_id, "SRC_Y", 0);
    ret |= add_plane_property(self, req, self->plane_id, "SRC_W", ((uint64_t) self->mode.hdisplay) << 16);
    ret |= add_plane_property(self, req, self->plane_id, "SRC_H", ((uint64_t) self->mode.vdisplay) << 16);
    ret |= add_plane_property(self, req, self->plane_id, "CRTC_X", 0);
    ret |= add_plane_property(self, req, self->plane_id, "CRTC_Y", 0);
    ret |= add_plane_property(self, req, self->plane_id, "CRTC_W", self->mode.hdisplay);
    ret |= add_plane_property(self, req, self->plane_id, "CRTC_H", self->mode.vdisplay);
    if (ret) {
        drmModeAtomicFree(req);
        return -1;
    }

    FlipHandlerData *data = g_slice_new(FlipHandlerData);
    *data = (FlipHandlerData){self, buffer};

    ret = drmModeAtomicCommit(get_drm_fd(self), req, flags, data);
    if (ret) {
        drmModeAtomicFree(req);
        return -1;
    }

    drmModeAtomicFree(req);
    return 0;
}

static void
drm_commit_buffer(CogDrmModesetRenderer *self, struct buffer_object *buffer)
{
    int ret;
    if (self->atomic_modesetting)
        ret = drm_commit_buffer_atomic(self, buffer);
    else
        ret = drm_commit_buffer_nonatomic(self, buffer);

    if (ret)
        g_warning("failed to schedule a page flip: %s", g_strerror(errno));
}

static void
on_export_buffer_resource(void *data, struct wl_resource *buffer_resource)
{
    CogDrmModesetRenderer *self = data;
    struct buffer_object  *buffer = drm_buffer_for_resource(self, buffer_resource);
    if (buffer) {
        buffer->export.resource = buffer_resource;
        drm_commit_buffer(self, buffer);
        return;
    }

    struct gbm_bo *bo =
        gbm_bo_import(self->gbm_dev, GBM_BO_IMPORT_WL_BUFFER, (void *) buffer_resource, GBM_BO_USE_SCANOUT);
    if (!bo) {
        g_warning("failed to import a wl_buffer resource into gbm_bo");
        return;
    }

    uint32_t width = gbm_bo_get_width(bo);
    uint32_t height = gbm_bo_get_height(bo);
    uint32_t format = gbm_bo_get_format(bo);

    buffer = drm_create_buffer_for_bo(self, bo, buffer_resource, width, height, format);
    if (buffer) {
        buffer->export.resource = buffer_resource;
        drm_commit_buffer(self, buffer);
    }
}

static void
on_export_dmabuf_resource(void *data, struct wpe_view_backend_exportable_fdo_dmabuf_resource *dmabuf_resource)
{
    CogDrmModesetRenderer *self = data;

    struct buffer_object *buffer = drm_buffer_for_resource(self, dmabuf_resource->buffer_resource);
    if (buffer) {
        buffer->export.resource = dmabuf_resource->buffer_resource;
        drm_commit_buffer(self, buffer);
        return;
    }

    struct gbm_import_fd_modifier_data modifier_data = {
        .width = dmabuf_resource->width,
        .height = dmabuf_resource->height,
        .format = dmabuf_resource->format,
        .num_fds = dmabuf_resource->n_planes,
        .modifier = dmabuf_resource->modifiers[0],
    };
    for (uint32_t i = 0; i < modifier_data.num_fds; ++i) {
        modifier_data.fds[i] = dmabuf_resource->fds[i];
        modifier_data.strides[i] = dmabuf_resource->strides[i];
        modifier_data.offsets[i] = dmabuf_resource->offsets[i];
    }

    struct gbm_bo *bo =
        gbm_bo_import(self->gbm_dev, GBM_BO_IMPORT_FD_MODIFIER, (void *) (&modifier_data), GBM_BO_USE_SCANOUT);
    if (!bo) {
        g_warning("failed to import a dma-buf resource into gbm_bo");
        return;
    }

    buffer = drm_create_buffer_for_bo(self, bo, dmabuf_resource->buffer_resource, dmabuf_resource->width,
                                      dmabuf_resource->height, dmabuf_resource->format);
    if (buffer) {
        buffer->export.resource = dmabuf_resource->buffer_resource;
        drm_commit_buffer(self, buffer);
    }
}

static void
on_export_shm_buffer(void *data, struct wpe_fdo_shm_exported_buffer *exported_buffer)
{
    CogDrmModesetRenderer *self = data;

    struct wl_resource   *exported_resource = wpe_fdo_shm_exported_buffer_get_resource(exported_buffer);
    struct wl_shm_buffer *exported_shm_buffer = wpe_fdo_shm_exported_buffer_get_shm_buffer(exported_buffer);

    struct buffer_object *buffer = drm_buffer_for_resource(self, exported_resource);
    if (buffer) {
        drm_copy_shm_buffer_into_bo(exported_shm_buffer, buffer->bo);

        buffer->export.shm_buffer = exported_buffer;
        drm_commit_buffer(self, buffer);
        return;
    }

    buffer = drm_create_buffer_for_shm_buffer(self, exported_resource, exported_shm_buffer);
    if (buffer) {
        drm_copy_shm_buffer_into_bo(exported_shm_buffer, buffer->bo);

        buffer->export.shm_buffer = exported_buffer;
        drm_commit_buffer(self, buffer);
    }
}

static void
drm_page_flip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void *data)
{
    CogDrmModesetRenderer *self = ((FlipHandlerData *) data)->renderer;
    struct buffer_object  *buffer = ((FlipHandlerData *) data)->buffer;
    g_slice_free(FlipHandlerData, data);

    if (self->committed_buffer) {
        struct buffer_object *buffer = self->committed_buffer;

        if (buffer->export.resource) {
            wpe_view_backend_exportable_fdo_dispatch_release_buffer(self->exportable, buffer->export.resource);
            buffer->export.resource = NULL;
        }

        if (buffer->export.shm_buffer) {
            wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(self->exportable,
                                                                                 buffer->export.shm_buffer);
            buffer->export.shm_buffer = NULL;
        }
    }

    self->committed_buffer = buffer;
    wpe_view_backend_exportable_fdo_dispatch_frame_complete(self->exportable);
}

static bool
cog_drm_modeset_renderer_initialize(CogDrmRenderer *renderer, GError **error)
{
    CogDrmModesetRenderer *self = wl_container_of(renderer, self, base);

    g_source_attach(self->drm_source, g_main_context_get_thread_default());

    return true;
}

static void
cog_drm_modeset_renderer_destroy(CogDrmRenderer *renderer)
{
    CogDrmModesetRenderer *self = wl_container_of(renderer, self, base);

    struct buffer_object *buffer, *tmp;
    wl_list_for_each_safe(buffer, tmp, &self->buffer_list, link) {
        wl_list_remove(&buffer->link);
        wl_list_remove(&buffer->destroy_listener.link);
        destroy_buffer(self, buffer);
    }
    wl_list_init(&self->buffer_list);
    self->committed_buffer = NULL;

    if (self->connector_props.props_info) {
        for (uint32_t i = 0; i < self->connector_props.props->count_props; i++)
            drmModeFreeProperty(self->connector_props.props_info[i]);
    }
    g_clear_pointer(&self->connector_props.props, drmModeFreeObjectProperties);
    g_clear_pointer(&self->connector_props.props_info, g_free);

    if (self->crtc_props.props_info) {
        for (uint32_t i = 0; i < self->crtc_props.props->count_props; ++i)
            drmModeFreeProperty(self->crtc_props.props_info[i]);
    }
    g_clear_pointer(&self->crtc_props.props, drmModeFreeObjectProperties);
    g_clear_pointer(&self->crtc_props.props_info, g_free);

    if (self->plane_props.props_info) {
        for (uint32_t i = 0; i < self->plane_props.props->count_props; ++i)
            drmModeFreeProperty(self->plane_props.props_info[i]);
    }
    g_clear_pointer(&self->plane_props.props, drmModeFreeObjectProperties);
    g_clear_pointer(&self->plane_props.props_info, g_free);

    g_clear_pointer(&self->gbm_dev, gbm_device_destroy);

    g_slice_free(CogDrmModesetRenderer, self);
}

static struct wpe_view_backend_exportable_fdo *
cog_drm_modeset_renderer_create_exportable(CogDrmRenderer *renderer, uint32_t width, uint32_t height)
{
    static const struct wpe_view_backend_exportable_fdo_client client = {
        .export_buffer_resource = on_export_buffer_resource,
        .export_dmabuf_resource = on_export_dmabuf_resource,
        .export_shm_buffer = on_export_shm_buffer,
    };

    CogDrmModesetRenderer *self = wl_container_of(renderer, self, base);
    return (self->exportable = wpe_view_backend_exportable_fdo_create(&client, renderer, width, height));
}

CogDrmRenderer *
cog_drm_modeset_renderer_new(struct gbm_device     *gbm_dev,
                             uint32_t               plane_id,
                             uint32_t               crtc_id,
                             uint32_t               connector_id,
                             const drmModeModeInfo *mode,
                             bool                   atomic_modesetting)
{
    CogDrmModesetRenderer *self = g_slice_new0(CogDrmModesetRenderer);

    *self = (CogDrmModesetRenderer){
        .base.name = "modeset",
        .base.initialize = cog_drm_modeset_renderer_initialize,
        .base.destroy = cog_drm_modeset_renderer_destroy,
        .base.create_exportable = cog_drm_modeset_renderer_create_exportable,

        .drm_source = drm_event_source_new(gbm_device_get_fd(gbm_dev)),
        .gbm_dev = gbm_dev,

        .crtc_id = crtc_id,
        .connector_id = connector_id,
        .plane_id = plane_id,
        .atomic_modesetting = atomic_modesetting,
    };

    uint64_t value = 0;
    if (drmGetCap(gbm_device_get_fd(self->gbm_dev), DRM_CAP_ADDFB2_MODIFIERS, &value)) {
        g_debug("%s: Cannot get addfb2_modifiers capability: %s", G_STRFUNC, g_strerror(errno));
    } else {
        g_debug("%s: Capability addfb2_modifiers = %#" PRIx64, G_STRFUNC, value);
        self->addfb2_modifiers = !!value;
    }

    wl_list_init(&self->buffer_list);
    memcpy(&self->mode, mode, sizeof(drmModeModeInfo));

    self->connector_props.props =
        drmModeObjectGetProperties(get_drm_fd(self), self->connector_id, DRM_MODE_OBJECT_CONNECTOR);
    if (self->connector_props.props) {
        self->connector_props.props_info = g_new0(drmModePropertyRes *, self->connector_props.props->count_props);
        for (uint32_t i = 0; i < self->connector_props.props->count_props; i++)
            self->connector_props.props_info[i] =
                drmModeGetProperty(get_drm_fd(self), self->connector_props.props->props[i]);
    }

    self->crtc_props.props = drmModeObjectGetProperties(get_drm_fd(self), self->crtc_id, DRM_MODE_OBJECT_CRTC);
    if (self->crtc_props.props) {
        self->crtc_props.props_info = g_new0(drmModePropertyRes *, self->crtc_props.props->count_props);
        for (uint32_t i = 0; i < self->crtc_props.props->count_props; i++)
            self->crtc_props.props_info[i] = drmModeGetProperty(get_drm_fd(self), self->crtc_props.props->props[i]);
    }

    self->plane_props.props = drmModeObjectGetProperties(get_drm_fd(self), self->plane_id, DRM_MODE_OBJECT_PLANE);
    if (self->plane_props.props) {
        self->plane_props.props_info = g_new0(drmModePropertyRes *, self->plane_props.props->count_props);
        for (uint32_t i = 0; i < self->plane_props.props->count_props; i++)
            self->plane_props.props_info[i] = drmModeGetProperty(get_drm_fd(self), self->plane_props.props->props[i]);
    }

    g_debug("%s: Using plane #%" PRIu32 ", crtc #%" PRIu32 ", connector #%" PRIu32 " (%s).", __func__, plane_id,
            crtc_id, connector_id, atomic_modesetting ? "atomic" : "legacy");

    return &self->base;
}
