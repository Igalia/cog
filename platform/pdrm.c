/*
 * pdrm.c
 * Copyright (C) 2020 Adrian Perez de Castro <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "pdrm.h"
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <glib/gstdio.h>
#include <inttypes.h>
#include <stdbool.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <wpe/fdo.h>


#if 0
# define TRACE(fmt, ...) g_debug ("%s: " fmt, G_STRFUNC, __VA_ARGS__)
#else
# define TRACE(fmt, ...) ((void) 0)
#endif


#if !defined(EGL_EXT_platform_base)
typedef EGLDisplay (EGLAPIENTRYP PFNEGLGETPLATFORMDISPLAYEXTPROC) (EGLenum platform, void *native_display, const EGLint *attrib_list);
#endif

#if !defined(EGL_MESA_platform_gbm) || !defined(EGL_KHR_platform_gbm)
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif

G_DEFINE_QUARK (com_igalia_Cog_PdrmError, pdrm_error)

G_DEFINE_AUTOPTR_CLEANUP_FUNC (drmModeConnector, drmModeFreeConnector)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (drmModeEncoder, drmModeFreeEncoder)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (drmModeRes, drmModeFreeResources)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (drmVersion, drmFreeVersion)


typedef struct _PdrmSource PdrmSource;


struct _PdrmDisplay {
    int                             fd;
    char                           *path;
    uint32_t                        crtc_id;
    uint32_t                        connector_id;
    uint32_t                        phys_width;
    uint32_t                        phys_height;
    drmModeModeInfo                 mode;
    bool                            mode_set;
    PdrmSource                     *drm_source;
    struct gbm_device              *gbm_device;
    EGLDisplay                      egl_display;
    PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT;
};


struct _PdrmSource {
    GSource         base;
    int             fd;
    void           *fd_tag;
    drmEventContext event_context;
};


struct _PdrmBuffer {
    PdrmDisplay        *display;
    uint32_t            fb_id;
    struct gbm_bo      *bo;
    struct wl_resource *resource;
    void              (*commit_callback) (PdrmBuffer*, void*);
    void               *commit_userdata;
};


void
pdrm_buffer_destroy (PdrmBuffer *self)
{
    g_assert (self);

    if (self->bo) {
        drmModeRmFB (self->display->fd, self->fb_id);
        self->fb_id = 0;
        g_clear_pointer (&self->bo, gbm_bo_destroy);
    }

    g_slice_free (PdrmBuffer, self);
}


PdrmBuffer*
pdrm_buffer_new_for_bo (PdrmDisplay   *display,
                        struct gbm_bo *bo)
{
    g_return_val_if_fail (display, NULL);
    g_return_val_if_fail (bo, NULL);

    uint32_t in_handles[4] = { 0, };
    uint32_t in_strides[4] = { 0, };
    uint32_t in_offsets[4] = { 0, };
    uint64_t in_modifiers[4] = { gbm_bo_get_modifier (bo), };

    const unsigned n_planes = MIN (gbm_bo_get_plane_count (bo), 4);
    for (unsigned i = 0; i < n_planes; i++) {
        in_handles[i] = gbm_bo_get_handle_for_plane (bo, i).u32;
        in_strides[i] = gbm_bo_get_stride_for_plane (bo, i);
        in_offsets[i] = gbm_bo_get_offset (bo, i);
        in_modifiers[i] = in_modifiers[0];
    }

    int flags = 0;
    if (in_modifiers[0])
        flags = DRM_MODE_FB_MODIFIERS;

    uint32_t fb_id = 0;
    int ret = drmModeAddFB2WithModifiers (display->fd,
                                          gbm_bo_get_width (bo),
                                          gbm_bo_get_height (bo),
                                          gbm_bo_get_format (bo),
                                          in_handles,
                                          in_strides,
                                          in_offsets,
                                          in_modifiers,
                                          &fb_id,
                                          flags);
    if (ret) {
        in_handles[0] = gbm_bo_get_handle (bo).u32;
        in_handles[1] = in_handles[2] = in_handles[3] = 0;
        in_strides[0] = gbm_bo_get_stride (bo);
        in_strides[1] = in_strides[2] = in_strides[3] = 0;
        in_offsets[0] = in_offsets[1] = in_offsets[2] = in_offsets[3] = 0;

        ret = drmModeAddFB2 (display->fd,
                             gbm_bo_get_width (bo),
                             gbm_bo_get_height (bo),
                             gbm_bo_get_format (bo),
                             in_handles,
                             in_strides,
                             in_offsets,
                             &fb_id,
                             0);
    }

    if (ret) {
        g_critical ("Cannot create framebuffer: %s", strerror (errno));
        return NULL;
    }

    PdrmBuffer *self = g_slice_new0 (PdrmBuffer);
    self->display = display;
    self->fb_id = fb_id;
    self->bo = bo;
    return self;
}


void
pdrm_buffer_commit (PdrmBuffer *self,
                    void      (*callback) (PdrmBuffer*, void*),
                    void       *userdata)
{
    g_return_if_fail (self);

    if (G_UNLIKELY (!self->display->mode_set)) {
        TRACE ("Setting mode %" PRIu32 "x%" PRIu32 ", CRTC %" PRIu32
               ", FB %" PRIu32 ", connector %" PRIu32 ".",
               self->display->mode.hdisplay,
               self->display->mode.vdisplay,
               self->display->crtc_id,
               self->fb_id,
               self->display->connector_id);
        if (drmModeSetCrtc (self->display->fd,
                            self->display->crtc_id,
                            self->fb_id,
                            0,
                            0,
                            &self->display->connector_id,
                            1,
                            &self->display->mode)) {
            g_critical ("Failed to set mode: %s", strerror (errno));
            return;
        }
        self->display->mode_set = true;
    }

    self->commit_callback = callback;
    self->commit_userdata = userdata;

    TRACE ("Scheduling page flip for buffer @ %p", self);
    if (drmModePageFlip (self->display->fd,
                         self->display->crtc_id,
                         self->fb_id,
                         DRM_MODE_PAGE_FLIP_EVENT,
                         self)) {
        g_critical ("Failed to schedule page flip: %s", strerror (errno));
    }
}


static gboolean
pdrm_source_check (GSource *source)
{
    PdrmSource *self = (PdrmSource*) source;
    return !!g_source_query_unix_fd (source, self->fd_tag);
}


static gboolean
pdrm_source_dispatch (GSource    *source,
                      GSourceFunc callback,
                      void       *userdata)
{
    PdrmSource *self = (PdrmSource*) source;

    const GIOCondition events = g_source_query_unix_fd (source, self->fd_tag);

    if (events & (G_IO_ERR | G_IO_HUP))
        return G_SOURCE_REMOVE;

    if (events & G_IO_IN)
        drmHandleEvent (self->fd, &self->event_context);

    return G_SOURCE_CONTINUE;
}


static void
pdrm_buffer_on_page_flip (int      fd,
                          unsigned frame G_GNUC_UNUSED,
                          unsigned sec G_GNUC_UNUSED,
                          unsigned usec G_GNUC_UNUSED,
                          void    *data)
{
    TRACE ("Page flipped for buffer @ %p.", data);

    PdrmBuffer *self = data;

    g_assert (self->display->fd == fd);

    if (G_LIKELY(self->commit_callback))
        (*self->commit_callback) (self, self->commit_userdata);
}


static PdrmSource*
pdrm_source_new (PdrmDisplay *display)
{
    g_assert (display);
    g_assert (display->fd >= 0);

    static GSourceFuncs funcs = {
        .check = pdrm_source_check,
        .dispatch = pdrm_source_dispatch,
    };
    PdrmSource *self = (PdrmSource*) g_source_new (&funcs, sizeof (PdrmSource));
    self->event_context = (drmEventContext) {
        .version = DRM_EVENT_CONTEXT_VERSION,
        .page_flip_handler = pdrm_buffer_on_page_flip,
    };
    self->fd = display->fd;
    self->fd_tag = g_source_add_unix_fd (&self->base,
                                         self->fd,
                                         G_IO_IN | G_IO_ERR | G_IO_HUP);

    g_source_set_name (&self->base, "Cog: DRM");
    g_source_set_can_recurse (&self->base, TRUE);

    return self;
}


static void
close_or_warn (int fd, const char *path)
{
    g_autoptr(GError) error = NULL;
    if (!g_close (fd, &error))
        g_warning ("%s: Cannot close '%s', %s.", G_STRFUNC, path, error->message);
}


static int
pdrm_open_devnode (const char *path,
                   GError    **error)
{
    g_assert (path);

    if (!drmAvailable ()) {
        g_set_error_literal (error,
                             PDRM_ERROR,
                             PDRM_ERROR_UNAVAILABLE,
                             "DRM unavailable");
        return -1;
    }

    TRACE ("Opening %s.", path);

    int fd = g_open (path, O_RDWR, 0);
    if (fd < 0) {
        g_set_error (error,
                     G_FILE_ERROR,
                     g_file_error_from_errno (errno),
                     "Cannot open '%s'",
                     path);
        return fd;
    }

    g_autoptr(drmModeRes) resources = drmModeGetResources (fd);
    if (resources && resources->count_crtcs) {
        /* Check whether the device has a connected output. */
        for (int j = 0; j < resources->count_connectors; j++) {
            g_autoptr(drmModeConnector) connector =
                drmModeGetConnector (fd, resources->connectors[j]);
            if (connector->connection == DRM_MODE_CONNECTED) {
                /* Success! */
                return fd;
            }
        }
    }

    g_set_error (error,
                 PDRM_ERROR,
                 PDRM_ERROR_UNAVAILABLE,
                 ((resources && resources->count_crtcs)
                  ? "Device '%s' does not have any output connected"
                  : "Device '%s' does not have any CRTC (render only node?)"),
                 path);
    close_or_warn (fd, path);
    return -1;
}


char*
pdrm_find_primary_devnode (int *out_fd)
{
    if (!drmAvailable ())
        return NULL;

    drmDevicePtr devices[64] = { NULL, };

    int n_devices = drmGetDevices2 (0, devices, G_N_ELEMENTS (devices));
    if (n_devices < 0)
        return NULL;

    for (int i = 0; i < n_devices; i++) {
        drmDevicePtr device = devices[i];

        if (!(device->available_nodes & (1 << DRM_NODE_PRIMARY)))
            continue;

        int fd = pdrm_open_devnode (device->nodes[DRM_NODE_PRIMARY], NULL);
        if (fd >= 0) {
            if (out_fd)
                *out_fd = fd;
            else
                close_or_warn (fd, device->nodes[DRM_NODE_PRIMARY]);

            char *device_node = g_strdup (device->nodes[DRM_NODE_PRIMARY]);
            drmFreeDevices(devices, n_devices);
            return device_node;
        }
 
        close_or_warn (fd, device->nodes[DRM_NODE_PRIMARY]);
    }

    drmFreeDevices(devices, n_devices);
    return NULL;
}


static void
set_egl_error (GError    **error,
               EGLint      code,
               const char *message)
{
    g_assert (message);

    const char *code_string = NULL;
    switch (code) {
#define ERRCASE(v) case EGL_ ## v: code_string = #v; break
        ERRCASE (SUCCESS);
        ERRCASE (NOT_INITIALIZED);
        ERRCASE (BAD_ACCESS);
        ERRCASE (BAD_ALLOC);
        ERRCASE (BAD_ATTRIBUTE);
        ERRCASE (BAD_CONTEXT);
        ERRCASE (BAD_CONFIG);
        ERRCASE (BAD_CURRENT_SURFACE);
        ERRCASE (BAD_DISPLAY);
        ERRCASE (BAD_SURFACE);
        ERRCASE (BAD_MATCH);
        ERRCASE (BAD_PARAMETER);
        ERRCASE (BAD_NATIVE_PIXMAP);
        ERRCASE (BAD_NATIVE_WINDOW);
        ERRCASE (CONTEXT_LOST);
#undef ERRCASE
        default:
            code_string = "UNKNOWN";
    }

    g_set_error (error, PDRM_ERROR, PDRM_ERROR_EGL,
                 "EGL: %s - #%06x %s", message, code, code_string);
}


static gboolean
pdrm_display_initialize_egl (PdrmDisplay *self,
                             GError     **error)
{
    g_return_val_if_fail (self, FALSE);

    if (self->egl_display != EGL_NO_DISPLAY)
        return TRUE;

    self->eglGetPlatformDisplayEXT = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
        eglGetProcAddress ("eglGetPlatformDisplayEXT");

    if (self->eglGetPlatformDisplayEXT) {
        self->egl_display = self->eglGetPlatformDisplayEXT (EGL_PLATFORM_GBM_KHR,
                                                            self->gbm_device,
                                                            NULL);
    } else {
        self->egl_display = eglGetDisplay ((void*) self->gbm_device);
    }

    if (self->egl_display == EGL_NO_DISPLAY) {
        set_egl_error (error, eglGetError (), "Could not open EGL display");
        return FALSE;
    }

    EGLint major, minor;
    if (!eglInitialize (self->egl_display, &major, &minor)) {
        set_egl_error (error, eglGetError (), "Initialization failed");
        eglTerminate (self->egl_display);
        self->egl_display = EGL_NO_DISPLAY;
        return FALSE;
    }

    g_info ("EGL version %d.%d initialized.", major, minor);
    return TRUE;
}


static const char*
connector_type_string (uint32_t type)
{
    switch (type) {
#ifdef DRM_MODE_CONNECTOR_DPI
        case DRM_MODE_CONNECTOR_DPI: return "DPI";
#endif /* DRM_MODE_CONNECTOR_DPI */
#ifdef DRM_MODE_CONNECTOR_DSI
        case DRM_MODE_CONNECTOR_DSI: return "DSI";
#endif /* DRM_MODE_CONNECTOR_DSI */
#ifdef DRM_MODE_CONNECTOR_VIRTUAL
        case DRM_MODE_CONNECTOR_VIRTUAL: return "Virtual";
#endif /* DRM_MODE_CONNECTOR_VIRTUAL */
#ifdef DRM_MODE_CONNECTOR_eDP
        case DRM_MODE_CONNECTOR_eDP: return "eDP";
#endif /* DRM_MODE_CONNECTOR_eDP */
#ifdef DRM_MODE_CONNECTOR_TV
        case DRM_MODE_CONNECTOR_TV: return "TV";
#endif /* DRM_MODE_CONNECTOR_TV */
#ifdef DRM_MODE_CONNECTOR_HDMIB
        case DRM_MODE_CONNECTOR_HDMIB: return "HDMI-B";
#endif /* DRM_MODE_CONNECTOR_HDMIB */
#ifdef DRM_MODE_CONNECTOR_HDMIA
        case DRM_MODE_CONNECTOR_HDMIA: return "HDMI-A";
#endif /* DRM_MODE_CONNECTOR_HDMIA */
#ifdef DRM_MODE_CONNECTOR_DisplayPort
        case DRM_MODE_CONNECTOR_DisplayPort: return "DisplayPort";
#endif /* DRM_MODE_CONNECTOR_DisplayPort */
#ifdef DRM_MODE_CONNECTOR_9PinDIN
        case DRM_MODE_CONNECTOR_9PinDIN: return "9-pin DIN";
#endif /* DRM_MODE_CONNECTOR_9PinDIN */
#ifdef DRM_MODE_CONNECTOR_Component
        case DRM_MODE_CONNECTOR_Component: return "Component";
#endif /* DRM_MODE_CONNECTOR_Component */
#ifdef DRM_MODE_CONNECTOR_LVDS
        case DRM_MODE_CONNECTOR_LVDS: return "LVDS";
#endif /* DRM_MODE_CONNECTOR_LVDS */
#ifdef DRM_MODE_CONNECTOR_SVIDEO
        case DRM_MODE_CONNECTOR_SVIDEO: return "S-Video";
#endif /* DRM_MODE_CONNECTOR_SVIDEO */
#ifdef DRM_MODE_CONNECTOR_Composite
        case DRM_MODE_CONNECTOR_Composite: return "Composite";
#endif /* DRM_MODE_CONNECTOR_Composite */
#ifdef DRM_MODE_CONNECTOR_DVIA
        case DRM_MODE_CONNECTOR_DVIA: return "DVI-A";
#endif /* DRM_MODE_CONNECTOR_DVIA */
#ifdef DRM_MODE_CONNECTOR_DVID
        case DRM_MODE_CONNECTOR_DVID: return "DVI-D";
#endif /* DRM_MODE_CONNECTOR_DVID */
#ifdef DRM_MODE_CONNECTOR_DVII
        case DRM_MODE_CONNECTOR_DVII: return "DVI-I";
#endif /* DRM_MODE_CONNECTOR_DVII */
#ifdef DRM_MODE_CONNECTOR_VGA
        case DRM_MODE_CONNECTOR_VGA: return "VGA";
#endif /* DRM_MODE_CONNECTOR_VGA */
#ifdef DRM_MODE_CONNECTOR_Unknown
        case DRM_MODE_CONNECTOR_Unknown:
#endif /* DRM_MODE_CONNECTOR_Unknown */
        default: return "Unknown";
    }
}


PdrmDisplay*
pdrm_display_open (const char *device_path,
                   GError    **error)
{
    if (!drmAvailable ()) {
        g_set_error_literal (error,
                             PDRM_ERROR,
                             PDRM_ERROR_UNAVAILABLE,
                             "DRM unavailable");
        return NULL;
    }

    g_autoptr(PdrmDisplay) self = g_slice_new0 (PdrmDisplay);
    self->fd = -1;
    self->egl_display = EGL_NO_DISPLAY;

    if (device_path) {
        self->path = g_strdup (device_path);
        if ((self->fd = pdrm_open_devnode (self->path, error)) < 0)
            return NULL;
    } else if (!(self->path = pdrm_find_primary_devnode (&self->fd))) {
        g_set_error_literal (error,
                             PDRM_ERROR,
                             PDRM_ERROR_UNAVAILABLE,
                             "Cannot find a DRM device with a connected output");
        return NULL;
    }

    g_debug ("%s: Using %s, fd=%d.", G_STRFUNC, self->path, self->fd);
    {
        g_autoptr(drmVersion) version = drmGetVersion(self->fd);
        g_message ("DRM version %d.%d.%d (%*s), %*s (driver %*s)",
                   version->version_major,
                   version->version_minor,
                   version->version_patchlevel,
                   version->date_len, version->date,
                   version->desc_len, version->desc,
                   version->name_len, version->name);
    }

    g_autoptr(drmModeRes) resources = drmModeGetResources (self->fd);
    if (!resources) {
        g_set_error (error,
                     PDRM_ERROR,
                     PDRM_ERROR_UNAVAILABLE,
                     "Cannot obtain resources for '%s'",
                     self->path);
        return NULL;
    }

    g_autoptr(drmModeConnector) connector = NULL;
    for (int i = 0; i < resources->count_connectors; i++) {
        g_autoptr(drmModeConnector) cur_connector =
            drmModeGetConnector (self->fd, resources->connectors[i]);

        /* Skip connectors for which there is no usable modes. */
        if (cur_connector->count_modes == 0) {
            g_debug ("%s: Skipping %s connector, no modes available.",
                     G_STRFUNC,
                     connector_type_string (cur_connector->connector_type));
            continue;
        }

        /* Once we find one that is actually connected, stop searching. */
        if (cur_connector->connection == DRM_MODE_CONNECTED) {
            connector = g_steal_pointer (&cur_connector);
            break;
        }
    }
    if (!connector) {
        g_set_error (error,
                     PDRM_ERROR,
                     PDRM_ERROR_CONFIGURATION,
                     "Cannot find an active connector for '%s'",
                     self->path);
        return NULL;
    }
    self->connector_id = connector->connector_id;
    self->phys_width = connector->mmWidth;
    self->phys_height = connector->mmHeight;
    g_debug ("%s: Using connector %" PRIu32 "(%s, %" PRIu32 "x%" PRIu32 "mm).",
             G_STRFUNC, self->connector_id,
             connector_type_string (connector->connector_type),
             self->phys_width, self->phys_height);

    drmModeModeInfo *mode = NULL;
    for (int i = 0, max_area = 0; i < connector->count_modes; i++) {
        drmModeModeInfo *cur_mode = &connector->modes[i];
        if (cur_mode->type & DRM_MODE_TYPE_PREFERRED) {
            mode = cur_mode;
            break;
        }
        int area = cur_mode->hdisplay * cur_mode->vdisplay;
        if (area > max_area) {
            mode = cur_mode;
            max_area = area;
        }
    }
    if (!mode) {
        g_set_error (error,
                     PDRM_ERROR,
                     PDRM_ERROR_CONFIGURATION,
                     "Cannot find preferred mode for '%s'",
                     self->path);
        return NULL;
    }

    memcpy (&self->mode, mode, sizeof (drmModeModeInfo));
    g_debug ("%s: Current mode %" PRIu32 "x%" PRIu32 ", %" PRIu32 " Hz",
             G_STRFUNC, mode->hdisplay, mode->vdisplay, mode->vrefresh);

    g_autoptr(drmModeEncoder) encoder = NULL;
    for (int i = 0; i < resources->count_encoders; i++) {
        g_autoptr(drmModeEncoder) cur_encoder =
            drmModeGetEncoder (self->fd, resources->encoders[i]);
        if (cur_encoder->encoder_id == connector->encoder_id) {
            encoder = g_steal_pointer (&cur_encoder);
            break;
        }
    }
    if (!encoder) {
        g_set_error (error,
                     PDRM_ERROR,
                     PDRM_ERROR_CONFIGURATION,
                     "Cannot find active connection encoder for '%s'",
                     self->path);
        return NULL;
    }
    self->crtc_id = encoder->crtc_id;
    TRACE ("Using CRTC id=%" PRIu32 ".", self->crtc_id);

    if (!(self->gbm_device = gbm_create_device (self->fd))) {
        g_set_error (error,
                     PDRM_ERROR,
                     PDRM_ERROR_UNAVAILABLE,
                     "Could not initialize GBM for '%s'",
                     self->path);
        return NULL;
    }

    if (!pdrm_display_initialize_egl (self, error))
        return NULL;

    TRACE ("Created @ %p.", self);
    return g_steal_pointer (&self);
}


void
pdrm_display_destroy (PdrmDisplay *self)
{
    g_return_if_fail (self != NULL);

    if (self->drm_source) {
        g_source_destroy (&self->drm_source->base);
        g_source_unref (&self->drm_source->base);
        self->drm_source = NULL;
    }

    if (self->egl_display != EGL_NO_DISPLAY) {
        eglTerminate (self->egl_display);
        self->egl_display = EGL_NO_DISPLAY;
    }
    eglReleaseThread ();

    memset (&self->mode, 0x00, sizeof (drmModeModeInfo));
    self->mode_set = false;

    g_clear_pointer (&self->gbm_device, gbm_device_destroy);
    if (self->fd >= 0) {
        close_or_warn (self->fd, self->path);
        self->fd = -1;
    }

    g_clear_pointer (&self->path, g_free);
    g_slice_free (PdrmDisplay, self);

    TRACE ("Destroyed @ %p.", self);
}


void
pdrm_display_attach_sources (PdrmDisplay  *self,
                             GMainContext *context)
{
    g_return_if_fail (self);

    if (!self->drm_source) {
        self->drm_source = pdrm_source_new (self);
        g_source_attach (&self->drm_source->base, context);
    }
}


void
pdrm_display_get_size (const PdrmDisplay *self,
                       uint32_t          *width,
                       uint32_t          *height)
{
    g_return_if_fail (self);
    g_return_if_fail (width || height);

    if (width) *width = self->mode.hdisplay;
    if (height) *height = self->mode.vdisplay;
}


void
pdrm_display_get_phys_size (const PdrmDisplay *self,
                            uint32_t          *width,
                            uint32_t          *height)
{
    g_return_if_fail (self);
    g_return_if_fail (width || height);

    if (width) *width = self->phys_width;
    if (height) *height = self->phys_height;
}


EGLDisplay
pdrm_display_get_egl_display (const PdrmDisplay *self)
{
    g_return_val_if_fail (self, EGL_NO_DISPLAY);
    return self->egl_display;
}


PdrmBuffer*
pdrm_display_import_resource (PdrmDisplay        *self,
                              struct wl_resource *resource)
{
    g_return_val_if_fail (self, NULL);
    g_return_val_if_fail (resource, NULL);

    struct gbm_bo *bo = gbm_bo_import (self->gbm_device,
                                       GBM_BO_IMPORT_WL_BUFFER,
                                       resource,
                                       GBM_BO_USE_SCANOUT);

    return bo ? pdrm_buffer_new_for_bo (self, bo) : NULL;
}


PdrmBuffer*
pdrm_display_import_dmabuf (PdrmDisplay                                            *self,
                            struct wpe_view_backend_exportable_fdo_dmabuf_resource *resource)
{
    g_return_val_if_fail (self, NULL);
    g_return_val_if_fail (resource, NULL);

    struct gbm_import_fd_modifier_data modifier_data = {
        .width = resource->width,
        .height = resource->height,
        .format = resource->format,
        .num_fds = resource->n_planes,
        .modifier = resource->modifiers[0],
    };

    for (uint32_t i = 0; i < modifier_data.num_fds; i++) {
        modifier_data.fds[i] = resource->fds[i];
        modifier_data.strides[i] = resource->strides[i];
        modifier_data.offsets[i] = resource->offsets[i];
    }

    struct gbm_bo *bo = gbm_bo_import (self->gbm_device,
                                       GBM_BO_IMPORT_FD_MODIFIER,
                                       &modifier_data,
                                       GBM_BO_USE_SCANOUT);

    return bo ? pdrm_buffer_new_for_bo (self, bo) : NULL;
}
