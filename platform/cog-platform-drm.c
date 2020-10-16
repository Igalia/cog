#include <cog.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <libinput.h>
#include <libudev.h>
#include <string.h>
#include <wayland-server.h>
#include <wpe/fdo.h>
#include <wpe/fdo-egl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>


#if !defined(EGL_EXT_platform_base)
typedef EGLDisplay (EGLAPIENTRYP PFNEGLGETPLATFORMDISPLAYEXTPROC) (EGLenum platform, void *native_display, const EGLint *attrib_list);
#endif

#if !defined(EGL_MESA_platform_gbm) || !defined(EGL_KHR_platform_gbm)
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif


struct buffer_object {
    struct wl_list link;
    struct wl_listener destroy_listener;

    uint32_t fb_id;
    struct gbm_bo *bo;
    struct wl_resource *buffer_resource;
};

static struct {
    int fd;

    drmModeConnector *connector;
    drmModeModeInfo *mode;
    drmModeEncoder *encoder;

    uint32_t connector_id;
    uint32_t crtc_id;
    int crtc_index;

    uint32_t width;
    uint32_t height;

    bool mode_set;
    struct wl_list buffer_list;
    struct buffer_object *committed_buffer;
} drm_data = {
    .fd = -1,
    .connector = NULL,
    .mode = NULL,
    .encoder = NULL,
    .connector_id = 0,
    .crtc_id = 0,
    .crtc_index = -1,
    .width = 0,
    .height = 0,
    .mode_set = false,
    .committed_buffer = NULL,
};

static struct {
    struct gbm_device *device;
} gbm_data = {
    .device = NULL,
};

static struct {
    EGLDisplay display;
} egl_data = {
    .display = EGL_NO_DISPLAY,
};

static struct {
    struct udev *udev;
    struct libinput *libinput;

    uint32_t input_width;
    uint32_t input_height;

    struct wpe_input_touch_event_raw touch_points[10];
    enum wpe_input_touch_event_type last_touch_type;
    int last_touch_id;
} input_data = {
    .udev = NULL,
    .libinput = NULL,
    .input_width = 0,
    .input_height = 0,
    .last_touch_type = wpe_input_touch_event_type_null,
    .last_touch_id = 0,
};

static struct {
    GSource *drm_source;
    GSource *input_source;
} glib_data = {
    .drm_source = NULL,
    .input_source = NULL,
};

static struct {
    struct wpe_view_backend_exportable_fdo *exportable;
} wpe_host_data;

static struct {
    struct wpe_view_backend *backend;
} wpe_view_data;


static void
destroy_buffer (struct buffer_object *buffer)
{
    drmModeRmFB (drm_data.fd, buffer->fb_id);
    gbm_bo_destroy (buffer->bo);

    wpe_view_backend_exportable_fdo_dispatch_release_buffer (wpe_host_data.exportable, buffer->buffer_resource);
    g_free (buffer);
}

static void
destroy_buffer_notify (struct wl_listener *listener, void *data)
{
    struct buffer_object *buffer = wl_container_of (listener, buffer, destroy_listener);

    if (drm_data.committed_buffer == buffer)
        drm_data.committed_buffer = NULL;

    wl_list_remove (&buffer->link);
    destroy_buffer (buffer);
}

static void
clear_drm (void)
{
    drm_data.committed_buffer = NULL;

    struct buffer_object *buffer, *tmp;
    wl_list_for_each_safe (buffer, tmp, &drm_data.buffer_list, link) {
        wl_list_remove (&buffer->link);
        wl_list_remove (&buffer->destroy_listener.link);

        destroy_buffer (buffer);
    }
    wl_list_init (&drm_data.buffer_list);

    g_clear_pointer (&drm_data.encoder, drmModeFreeEncoder);
    g_clear_pointer (&drm_data.connector, drmModeFreeConnector);
    if (drm_data.fd != -1) {
        close (drm_data.fd);
        drm_data.fd = -1;
    }
}

static gboolean
init_drm (void)
{
    drmDevicePtr devices[64];
    memset (devices, 0, sizeof (drmDevicePtr) * 64);

    int num_devices = drmGetDevices2 (0, devices, 64);
    if (num_devices < 0)
        return FALSE;

    drmModeRes *resources = NULL;
    for (int i = 0; i < num_devices; ++i) {
        drmDevicePtr device = devices[i];
        if (!(device->available_nodes & (1 << DRM_NODE_PRIMARY)))
            continue;

        drm_data.fd = open (device->nodes[DRM_NODE_PRIMARY], O_RDWR);
        if (drm_data.fd < 0)
            continue;

        resources = drmModeGetResources (drm_data.fd);
        if (resources)
            break;

        close (drm_data.fd);
        drm_data.fd = -1;
    }

    if (!resources)
        return FALSE;

    for (int i = 0; i < resources->count_connectors; ++i) {
        drm_data.connector = drmModeGetConnector (drm_data.fd, resources->connectors[i]);
        if (drm_data.connector->connection == DRM_MODE_CONNECTED)
            break;

        g_clear_pointer (&drm_data.connector, drmModeFreeConnector);
    }
    if (!drm_data.connector) {
        g_clear_pointer (&resources, drmModeFreeResources);
        return FALSE;
    }

    const char* user_selected_mode = g_getenv("COG_PLATFORM_DRM_VIDEO_MODE");

    const char* user_max_width_str = g_getenv("COG_PLATFORM_DRM_VIDEO_MAX_WIDTH");
    const char* user_max_height_str = g_getenv("COG_PLATFORM_DRM_VIDEO_MAX_HEIGHT");
    const int user_max_width = user_max_width_str ? atoi(user_max_width_str) : 0;
    const int user_max_height = user_max_height_str ? atoi(user_max_height_str) : 0;

    for (int i = 0, area = 0; i < drm_data.connector->count_modes; ++i) {
        drmModeModeInfo *current_mode = &drm_data.connector->modes[i];
        if (user_selected_mode && strcmp(user_selected_mode, current_mode->name) != 0) {
            continue;
        }

        if (user_max_width_str && current_mode->hdisplay > user_max_width) {
            continue;
        }
        if (user_max_height_str && current_mode->vdisplay > user_max_height) {
            continue;
        }

        if (current_mode->type & DRM_MODE_TYPE_PREFERRED) {
            drm_data.mode = current_mode;
            break;
        }

        int current_area = current_mode->hdisplay * current_mode->vdisplay;
        if (current_area > area) {
            drm_data.mode = current_mode;
            area = current_area;
        }
    }
    if (!drm_data.mode) {
        g_clear_pointer (&drm_data.connector, drmModeFreeConnector);
        g_clear_pointer (&resources, drmModeFreeResources);
        return FALSE;
    }

    for (int i = 0; i < resources->count_encoders; ++i) {
        drm_data.encoder = drmModeGetEncoder (drm_data.fd, resources->encoders[i]);
        if (drm_data.encoder->encoder_id == drm_data.connector->encoder_id)
            break;

        g_clear_pointer (&drm_data.encoder, drmModeFreeEncoder);
    }
    if (!drm_data.encoder) {
        g_clear_pointer (&drm_data.connector, drmModeFreeConnector);
        g_clear_pointer (&resources, drmModeFreeResources);
        return FALSE;
    }

    drm_data.connector_id = drm_data.connector->connector_id;
    drm_data.crtc_id = drm_data.encoder->crtc_id;
    for (int i = 0; i < resources->count_crtcs; ++i) {
        if (resources->crtcs[i] == drm_data.crtc_id) {
            drm_data.crtc_index = i;
            break;
        }
    }

    drm_data.width = drm_data.mode->hdisplay;
    drm_data.height = drm_data.mode->vdisplay;
    wl_list_init (&drm_data.buffer_list);

    g_clear_pointer (&resources, drmModeFreeResources);
    return TRUE;
}

static void
drm_page_flip_handler (int fd, unsigned int frame, unsigned int sec, unsigned int usec, void *data)
{
    if (drm_data.committed_buffer)
        wpe_view_backend_exportable_fdo_dispatch_release_buffer (wpe_host_data.exportable, drm_data.committed_buffer->buffer_resource);
    drm_data.committed_buffer = (struct buffer_object *) data;

    wpe_view_backend_exportable_fdo_dispatch_frame_complete (wpe_host_data.exportable);
}

static struct buffer_object *
drm_buffer_for_resource (struct wl_resource *buffer_resource)
{
    struct buffer_object *buffer;
    wl_list_for_each (buffer, &drm_data.buffer_list, link) {
        if (buffer->buffer_resource == buffer_resource)
            return buffer;
    }

    return NULL;
}

static struct buffer_object *
drm_create_buffer_for_bo (struct gbm_bo *bo, struct wl_resource *buffer_resource, uint32_t width, uint32_t height, uint32_t format)
{
    uint32_t in_handles[4] = { 0, };
    uint32_t in_strides[4] = { 0, };
    uint32_t in_offsets[4] = { 0, };
    uint64_t in_modifiers[4] = { 0, };

    in_modifiers[0] = gbm_bo_get_modifier (bo);

    int plane_count = MIN (gbm_bo_get_plane_count (bo), 4);
    for (int i = 0; i < plane_count; ++i) {
        in_handles[i] = gbm_bo_get_handle_for_plane (bo, i).u32;
        in_strides[i] = gbm_bo_get_stride_for_plane (bo, i);
        in_offsets[i] = gbm_bo_get_offset (bo, i);
        in_modifiers[i] = in_modifiers[0];
    }

    int flags = 0;
    if (in_modifiers[0])
        flags = DRM_MODE_FB_MODIFIERS;

    uint32_t fb_id = 0;
    int ret = drmModeAddFB2WithModifiers (drm_data.fd, width, height, format,
                                          in_handles, in_strides, in_offsets, in_modifiers,
                                          &fb_id, flags);
    if (ret) {
        in_handles[0] = gbm_bo_get_handle (bo).u32;
        in_handles[1] = in_handles[2] = in_handles[3] = 0;
        in_strides[0] = gbm_bo_get_stride (bo);
        in_strides[1] = in_strides[2] = in_strides[3] = 0;
        in_offsets[0] = in_offsets[1] = in_offsets[2] = in_offsets[3] = 0;

        ret = drmModeAddFB2 (drm_data.fd, width, height, format,
                             in_handles, in_strides, in_offsets,
                             &fb_id, 0);
    }

    if (ret) {
        g_warning ("failed to create framebuffer: %s", strerror (errno));
        return NULL;
    }

    struct buffer_object *buffer = g_new0 (struct buffer_object, 1);
    wl_list_insert (&drm_data.buffer_list, &buffer->link);
    buffer->destroy_listener.notify = destroy_buffer_notify;
    wl_resource_add_destroy_listener (buffer_resource, &buffer->destroy_listener);

    buffer->fb_id = fb_id;
    buffer->bo = bo;
    buffer->buffer_resource = buffer_resource;

    return buffer;
}

static void
drm_commit_buffer (struct buffer_object *buffer)
{
    int ret;
    if (!drm_data.mode_set) {
        ret = drmModeSetCrtc (drm_data.fd, drm_data.crtc_id, buffer->fb_id, 0, 0,
                              &drm_data.connector_id, 1, drm_data.mode);
        if (ret) {
            g_warning ("failed to set mode: %s", strerror (errno));
            return;
        }

        drm_data.mode_set = true;
    }

    ret = drmModePageFlip (drm_data.fd, drm_data.crtc_id, buffer->fb_id,
                               DRM_MODE_PAGE_FLIP_EVENT, buffer);
    if (ret)
        g_warning ("failed to schedule a page flip: %s", strerror (errno));
}


static void
clear_gbm (void)
{
    g_clear_pointer (&gbm_data.device, gbm_device_destroy);
}

static gboolean
init_gbm (void)
{
    gbm_data.device = gbm_create_device (drm_data.fd);
    if (!gbm_data.device)
        return FALSE;

    return TRUE;
}


static void
clear_egl (void)
{
    if (egl_data.display != EGL_NO_DISPLAY)
        eglTerminate (egl_data.display);
    eglReleaseThread ();
}

static gboolean
init_egl (void)
{
    static PFNEGLGETPLATFORMDISPLAYEXTPROC s_eglGetPlatformDisplay = NULL;
    if (!s_eglGetPlatformDisplay)
        s_eglGetPlatformDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC) eglGetProcAddress ("eglGetPlatformDisplayEXT");

    if (s_eglGetPlatformDisplay)
        egl_data.display = s_eglGetPlatformDisplay (EGL_PLATFORM_GBM_KHR, gbm_data.device, NULL);
    else
        egl_data.display = eglGetDisplay (gbm_data.device);

    if (!egl_data.display) {
        clear_egl ();
        return FALSE;
    }

    if (!eglInitialize (egl_data.display, NULL, NULL)) {
        clear_egl ();
        return FALSE;
    }

    return TRUE;
}


static void
input_handle_key_event (struct libinput_event_keyboard *key_event)
{
    struct wpe_input_xkb_context *default_context = wpe_input_xkb_context_get_default ();
    struct xkb_state *state = wpe_input_xkb_context_get_state (default_context);

    // Explanation for the offset-by-8, copied from Weston:
    //   evdev XKB rules reflect X's  broken keycode system, which starts at 8
    uint32_t key = libinput_event_keyboard_get_key (key_event) + 8;

    uint32_t keysym = xkb_state_key_get_one_sym (state, key);
    uint32_t unicode = xkb_state_key_get_utf32 (state, key);

    enum libinput_key_state key_state = libinput_event_keyboard_get_key_state (key_event);
    struct wpe_input_keyboard_event event = {
        .time = libinput_event_keyboard_get_time (key_event),
        .key_code = keysym,
        .hardware_key_code = unicode,
        .pressed = (key_state == LIBINPUT_KEY_STATE_PRESSED),
    };

    wpe_view_backend_dispatch_keyboard_event (wpe_view_data.backend, &event);
}

static void
input_handle_touch_event (enum libinput_event_type touch_type, struct libinput_event_touch *touch_event)
{
    uint32_t time = libinput_event_touch_get_time (touch_event);

    enum wpe_input_touch_event_type event_type = wpe_input_touch_event_type_null;
    switch (touch_type) {
        case LIBINPUT_EVENT_TOUCH_DOWN:
            event_type = wpe_input_touch_event_type_down;
            break;
        case LIBINPUT_EVENT_TOUCH_UP:
            event_type = wpe_input_touch_event_type_up;
            break;
        case LIBINPUT_EVENT_TOUCH_MOTION:
            event_type = wpe_input_touch_event_type_motion;
            break;
        case LIBINPUT_EVENT_TOUCH_FRAME: {
            struct wpe_input_touch_event event = {
                .touchpoints = input_data.touch_points,
                .touchpoints_length = G_N_ELEMENTS (input_data.touch_points),
                .type = input_data.last_touch_type,
                .id = input_data.last_touch_id,
                .time = time,
            };

            wpe_view_backend_dispatch_touch_event (wpe_view_data.backend, &event);

            for (int i = 0; i < G_N_ELEMENTS (input_data.touch_points); ++i) {
                struct wpe_input_touch_event_raw *touch_point = &input_data.touch_points[i];
                if (touch_point->type != wpe_input_touch_event_type_up)
                    continue;

                memset (touch_point, 0, sizeof (struct wpe_input_touch_event_raw));
                touch_point->type = wpe_input_touch_event_type_null;
            }

            return;
        }
        default:
            g_assert_not_reached ();
            return;
    }

    int id = libinput_event_touch_get_seat_slot (touch_event);
    if (id < 0 || id >= G_N_ELEMENTS (input_data.touch_points))
        return;

    input_data.last_touch_type = event_type;
    input_data.last_touch_id = id;

    struct wpe_input_touch_event_raw *touch_point = &input_data.touch_points[id];
    touch_point->type = event_type;
    touch_point->time = time;
    touch_point->id = id;

    if (touch_type == LIBINPUT_EVENT_TOUCH_DOWN
        || touch_type == LIBINPUT_EVENT_TOUCH_MOTION) {
        touch_point->x = libinput_event_touch_get_x_transformed (touch_event,
                                                                 input_data.input_width);
        touch_point->y = libinput_event_touch_get_y_transformed (touch_event,
                                                                 input_data.input_height);
    }
}

static void
input_process_events (void)
{
    g_assert (input_data.libinput);
    libinput_dispatch (input_data.libinput);

    while (true) {
        struct libinput_event *event = libinput_get_event (input_data.libinput);
        if (!event)
            break;

        enum libinput_event_type event_type = libinput_event_get_type (event);
        switch (event_type) {
            case LIBINPUT_EVENT_KEYBOARD_KEY:
                input_handle_key_event (libinput_event_get_keyboard_event (event));
                break;
            case LIBINPUT_EVENT_TOUCH_DOWN:
            case LIBINPUT_EVENT_TOUCH_UP:
            case LIBINPUT_EVENT_TOUCH_MOTION:
            case LIBINPUT_EVENT_TOUCH_FRAME:
                input_handle_touch_event (event_type,
                                          libinput_event_get_touch_event (event));
                break;
            default:
                break;
        }

        libinput_event_destroy (event);
    }
}

static int
input_interface_open_restricted (const char *path, int flags, void *user_data)
{
    return open (path, flags);
}

static void
input_interface_close_restricted (int fd, void *user_data)
{
    close (fd);
}

static void
clear_input (void)
{
    g_clear_pointer (&input_data.libinput, libinput_unref);
    g_clear_pointer (&input_data.udev, udev_unref);
}

static gboolean
init_input (void)
{
    static struct libinput_interface interface = {
        .open_restricted = input_interface_open_restricted,
        .close_restricted = input_interface_close_restricted,
    };

    input_data.udev = udev_new ();
    if (!input_data.udev)
        return FALSE;

    input_data.libinput = libinput_udev_create_context (&interface,
                                                        NULL,
                                                        input_data.udev);
    if (!input_data.libinput)
        return FALSE;

    int ret = libinput_udev_assign_seat (input_data.libinput, "seat0");
    if (ret)
        return FALSE;

    input_data.input_width = drm_data.mode->hdisplay;
    input_data.input_height = drm_data.mode->vdisplay;

    for (int i = 0; i < G_N_ELEMENTS (input_data.touch_points); ++i) {
        struct wpe_input_touch_event_raw *touch_point = &input_data.touch_points[i];

        memset (touch_point, 0, sizeof (struct wpe_input_touch_event_raw));
        touch_point->type = wpe_input_touch_event_type_null;
    }

    return TRUE;
}


struct drm_source {
    GSource source;
    GPollFD pfd;
    drmEventContext event_context;
};

static gboolean
drm_source_check (GSource *base)
{
    struct drm_source *source = (struct drm_source *) base;
    return !!source->pfd.revents;
}

static gboolean
drm_source_dispatch (GSource *base, GSourceFunc callback, gpointer user_data)
{
    struct drm_source *source = (struct drm_source *) base;
    if (source->pfd.revents & (G_IO_ERR | G_IO_HUP))
        return FALSE;

    if (source->pfd.revents & G_IO_IN)
        drmHandleEvent (drm_data.fd, &source->event_context);
    source->pfd.revents = 0;
    return TRUE;
}


struct input_source {
    GSource source;
    GPollFD pfd;
};

static gboolean
input_source_check (GSource *base)
{
    struct input_source *source = (struct input_source *) base;
    return !!source->pfd.revents;
}

static gboolean
input_source_dispatch (GSource *base, GSourceFunc callback, gpointer user_data)
{
    struct input_source *source = (struct input_source *) base;
    if (source->pfd.revents & (G_IO_ERR | G_IO_HUP))
        return FALSE;

    input_process_events ();
    source->pfd.revents = 0;
    return TRUE;
}


static void
clear_glib (void)
{
    if (glib_data.drm_source)
        g_source_destroy (glib_data.drm_source);
    g_clear_pointer (&glib_data.drm_source, g_source_unref);

    if (glib_data.input_source)
        g_source_destroy (glib_data.input_source);
    g_clear_pointer (&glib_data.input_source, g_source_unref);
}

static gboolean
init_glib (void)
{
    static GSourceFuncs drm_source_funcs = {
        .check = drm_source_check,
        .dispatch = drm_source_dispatch,
    };

    static GSourceFuncs input_source_funcs = {
        .check = input_source_check,
        .dispatch = input_source_dispatch,
    };

    glib_data.drm_source = g_source_new (&drm_source_funcs,
                                         sizeof (struct drm_source));
    {
        struct drm_source *source = (struct drm_source *) glib_data.drm_source;
        memset (&source->event_context, 0, sizeof (drmEventContext));
        source->event_context.version = DRM_EVENT_CONTEXT_VERSION;
        source->event_context.page_flip_handler = drm_page_flip_handler;

        source->pfd.fd = drm_data.fd;
        source->pfd.events = G_IO_IN | G_IO_ERR | G_IO_HUP;
        source->pfd.revents = 0;
        g_source_add_poll (glib_data.drm_source, &source->pfd);

        g_source_set_name (glib_data.drm_source, "cog: drm");
        g_source_set_can_recurse (glib_data.drm_source, TRUE);
        g_source_attach (glib_data.drm_source, g_main_context_get_thread_default ());
    }

    glib_data.input_source = g_source_new (&input_source_funcs,
                                           sizeof (struct input_source));
    {
        struct input_source *source = (struct input_source *) glib_data.input_source;
        source->pfd.fd = libinput_get_fd (input_data.libinput);
        source->pfd.events = G_IO_IN | G_IO_ERR | G_IO_HUP;
        source->pfd.revents = 0;
        g_source_add_poll (glib_data.input_source, &source->pfd);

        g_source_set_name (glib_data.input_source, "cog: input");
        g_source_set_can_recurse (glib_data.input_source, TRUE);
        g_source_attach (glib_data.input_source, g_main_context_get_thread_default ());
    }

    return TRUE;
}


static void
on_export_buffer_resource (void *data, struct wl_resource *buffer_resource)
{
    struct buffer_object *buffer = drm_buffer_for_resource (buffer_resource);
    if (buffer) {
        drm_commit_buffer (buffer);
        return;
    }

    struct gbm_bo* bo = gbm_bo_import (gbm_data.device, GBM_BO_IMPORT_WL_BUFFER,
                                       (void *) buffer_resource, GBM_BO_USE_SCANOUT);
    if (!bo) {
        g_warning ("failed to import a wl_buffer resource into gbm_bo");
        return;
    }

    uint32_t width = gbm_bo_get_width (bo);
    uint32_t height = gbm_bo_get_height (bo);
    uint32_t format = gbm_bo_get_format (bo);

    buffer = drm_create_buffer_for_bo (bo, buffer_resource, width, height, format);
    if (buffer)
        drm_commit_buffer (buffer);
}

static void
on_export_dmabuf_resource (void *data, struct wpe_view_backend_exportable_fdo_dmabuf_resource *dmabuf_resource)
{
    struct buffer_object *buffer = drm_buffer_for_resource (dmabuf_resource->buffer_resource);
    if (buffer) {
        drm_commit_buffer (buffer);
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

    struct gbm_bo *bo = gbm_bo_import (gbm_data.device, GBM_BO_IMPORT_FD_MODIFIER,
                                       (void *)(&modifier_data), GBM_BO_USE_SCANOUT);
    if (!bo) {
        g_warning ("failed to import a dma-buf resource into gbm_bo");
        return;
    }

    buffer = drm_create_buffer_for_bo (bo, dmabuf_resource->buffer_resource,
                                       dmabuf_resource->width, dmabuf_resource->height,
                                       dmabuf_resource->format);
    if (buffer)
        drm_commit_buffer (buffer);
}

gboolean
cog_platform_plugin_setup (CogPlatform *platform,
                           CogShell    *shell G_GNUC_UNUSED,
                           const char  *params,
                           GError     **error)
{
    g_assert (platform);
    g_return_val_if_fail (COG_IS_SHELL (shell), FALSE);

    if (!wpe_loader_init ("libWPEBackend-fdo-1.0.so")) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to set backend library name");
        return FALSE;
    }

    if (!init_drm ()) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to initialize DRM");
        return FALSE;
    }

    if (!init_gbm ()) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to initialize GBM");
        return FALSE;
    }

    if (!init_egl ()) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to initialize EGL");
        return FALSE;
    }

    if (!init_input ()) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to initialize input");
        return FALSE;
    }

    if (!init_glib ()) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to initialize GLib");
        return FALSE;
    }

    wpe_fdo_initialize_for_egl_display (egl_data.display);

    return TRUE;
}

void
cog_platform_plugin_teardown (CogPlatform *platform)
{
    g_assert (platform);

    g_clear_pointer (&drm_data.committed_buffer, destroy_buffer);

    clear_glib ();
    clear_input ();
    clear_egl ();
    clear_gbm ();
    clear_drm ();
}

WebKitWebViewBackend *
cog_platform_plugin_get_view_backend (CogPlatform   *platform,
                                      WebKitWebView *related_view,
                                      GError       **error)
{
    static struct wpe_view_backend_exportable_fdo_client exportable_client = {
        .export_buffer_resource = on_export_buffer_resource,
        .export_dmabuf_resource = on_export_dmabuf_resource,
    };

    wpe_host_data.exportable = wpe_view_backend_exportable_fdo_create (&exportable_client,
                                                                       NULL,
                                                                       drm_data.width,
                                                                       drm_data.height);
    g_assert (wpe_host_data.exportable);

    wpe_view_data.backend = wpe_view_backend_exportable_fdo_get_view_backend (wpe_host_data.exportable);
    g_assert (wpe_view_data.backend);

    WebKitWebViewBackend *wk_view_backend =
        webkit_web_view_backend_new (wpe_view_data.backend,
                                     (GDestroyNotify) wpe_view_backend_exportable_fdo_destroy,
                                     wpe_host_data.exportable);
    g_assert (wk_view_backend);

    return wk_view_backend;
}
