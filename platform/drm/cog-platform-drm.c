#include "../../core/cog.h"

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
#include <drm_fourcc.h>
#include "kms.h"
#include "cursor-drm.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "../common/egl-proc-address.h"

#ifndef LIBINPUT_CHECK_VERSION
#    define LIBINPUT_CHECK_VERSION(a, b, c)                                                         \
        ((LIBINPUT_VER_MAJOR > (a)) || (LIBINPUT_VER_MAJOR == (a) && (LIBINPUT_VER_MINOR > (b))) || \
         (LIBINPUT_VER_MAJOR == (a) && (LIBINPUT_VER_MINOR == (b)) && (LIBINPUT_VER_MICRO >= (c))))
#endif /* !LIBINPUT_CHECK_VERSION */

#if !defined(EGL_EXT_platform_base)
typedef EGLDisplay (EGLAPIENTRYP PFNEGLGETPLATFORMDISPLAYEXTPROC) (EGLenum platform, void *native_display, const EGLint *attrib_list);
#endif

#if !defined(EGL_MESA_platform_gbm) || !defined(EGL_KHR_platform_gbm)
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif

#if defined(WPE_FDO_CHECK_VERSION)
# define HAVE_SHM_EXPORTED_BUFFER WPE_FDO_CHECK_VERSION(1, 9, 0)
#else
# define HAVE_SHM_EXPORTED_BUFFER 0
#endif

#define KEY_STARTUP_DELAY 500000
#define KEY_REPEAT_DELAY 100000

#ifndef g_debug_once
#    define g_debug_once(...)                                                                     \
        G_STMT_START                                                                              \
        {                                                                                         \
            static int G_PASTE(_GDebugOnceBoolean_, __LINE__) = 0; /* (atomic) */                 \
            if (g_atomic_int_compare_and_exchange(&G_PASTE(_GDebugOnceBoolean_, __LINE__), 0, 1)) \
                g_debug(__VA_ARGS__);                                                             \
        }                                                                                         \
        G_STMT_END
#endif /* !g_debug_once */

struct _CogDrmPlatformClass {
    CogPlatformClass parent_class;
};

struct _CogDrmPlatform {
    CogPlatform parent;
};

G_DECLARE_FINAL_TYPE(CogDrmPlatform, cog_drm_platform, COG, DRM_PLATFORM, CogPlatform)

G_DEFINE_DYNAMIC_TYPE_EXTENDED(
    CogDrmPlatform,
    cog_drm_platform,
    COG_TYPE_PLATFORM,
    0,
    g_io_extension_point_implement(COG_MODULES_PLATFORM_EXTENSION_POINT, g_define_type_id, "drm", 200);)

struct buffer_object {
    struct wl_list link;
    struct wl_listener destroy_listener;

    uint32_t fb_id;
    struct gbm_bo *bo;
    struct wl_resource *buffer_resource;

    struct  {
        struct wl_resource* resource;
        struct wpe_fdo_shm_exported_buffer *shm_buffer;
    } export;
};

static struct {
    int fd;
    drmModeRes *base_resources;
    drmModePlaneRes *plane_resources;

    struct {
        drmModeConnector *obj;
        uint32_t obj_id;
    } connector;
    struct {
        drmModeCrtc *obj;
        uint32_t obj_id;
        uint32_t index;
    } crtc;
    struct {
        drmModePlane *obj;
        uint32_t obj_id;
    } plane;

    struct {
        drmModeObjectProperties *props;
        drmModePropertyRes **props_info;
    } connector_props, crtc_props, plane_props;

    drmModeModeInfo *mode;
    drmModeEncoder *encoder;

    uint32_t width;
    uint32_t height;
    double device_scale;

    bool atomic_modesetting;
    bool mode_set;
    struct wl_list buffer_list;
    struct buffer_object *committed_buffer;
} drm_data = {
    .fd = -1,
    .base_resources = NULL,
    .plane_resources = NULL,
    .connector = { NULL, 0, },
    .crtc = { NULL, 0, 0, },
    .plane = { NULL, 0, },
    .connector_props = { NULL, NULL, },
    .crtc_props = { NULL, NULL, },
    .plane_props = { NULL, NULL, },
    .mode = NULL,
    .encoder = NULL,
    .width = 0,
    .height = 0,
    .device_scale = 1.0,
    .atomic_modesetting = true,
    .mode_set = false,
    .committed_buffer = NULL,
};

static struct {
    gboolean enabled;
    struct kms_device *device;
    struct kms_plane *plane;
    struct kms_framebuffer *cursor;
    unsigned int x;
    unsigned int y;
    unsigned int screen_width;
    unsigned int screen_height;
} cursor = {
    .enabled = FALSE,
    .device = NULL,
    .plane = NULL,
    .cursor = NULL,
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

typedef struct keyboard_event {
    uint32_t time;
    uint32_t key;
} keyboard_event;

static struct {
    struct udev *udev;
    struct libinput *libinput;

    uint32_t input_width;
    uint32_t input_height;

    struct keyboard_event repeating_key;

    struct wpe_input_touch_event_raw touch_points[10];
    enum wpe_input_touch_event_type last_touch_type;
    int last_touch_id;
} input_data = {
    .udev = NULL,
    .libinput = NULL,
    .input_width = 0,
    .input_height = 0,
    .repeating_key = {0, 0},
    .last_touch_type = wpe_input_touch_event_type_null,
    .last_touch_id = 0,
};

static struct {
    GSource *drm_source;
    GSource *input_source;
    GSource *key_repeat_source;
} glib_data = {
    .drm_source = NULL,
    .input_source = NULL,
    .key_repeat_source = NULL,
};

static struct {
    struct wpe_view_backend_exportable_fdo *exportable;
} wpe_host_data;

static struct {
    struct wpe_view_backend *backend;
} wpe_view_data;


static void
init_config (CogShell *shell)
{
    drm_data.device_scale = cog_shell_get_device_scale_factor (shell);
    g_debug ("init_config: overriding device_scale value, using %.2f from shell",
             drm_data.device_scale);

    GKeyFile *key_file = cog_shell_get_config_file (shell);
    if (!key_file)
        return;

    {
        g_autoptr(GError) lookup_error = NULL;
        gboolean value = g_key_file_get_boolean (key_file,
                                                 "drm", "disable-atomic-modesetting",
                                                 &lookup_error);
        if (!lookup_error) {
            drm_data.atomic_modesetting = !value;
            g_debug ("init_config: atomic modesetting reconfigured to value '%s'",
                     drm_data.atomic_modesetting ? "true" : "false");
        }
    }

    {
        g_autoptr(GError) lookup_error = NULL;
        gdouble value = g_key_file_get_double (key_file,
                                               "drm", "device-scale-factor",
                                               &lookup_error);
        if (!lookup_error) {
            drm_data.device_scale = value;
            g_debug ("init_config:overriding device_scale value, using %.2f from config",
                     drm_data.device_scale);
        }
    }
}


static void
destroy_buffer (struct buffer_object *buffer)
{
    drmModeRmFB (drm_data.fd, buffer->fb_id);
    gbm_bo_destroy (buffer->bo);

    if (buffer->export.resource) {
        wpe_view_backend_exportable_fdo_dispatch_release_buffer (wpe_host_data.exportable,
                                                                 buffer->export.resource);
        buffer->export.resource = NULL;
    }
#if HAVE_SHM_EXPORTED_BUFFER
    if (buffer->export.shm_buffer) {
        wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer (wpe_host_data.exportable,
                                                                              buffer->export.shm_buffer);
        buffer->export.shm_buffer = NULL;
    }
#endif
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
clear_buffers (void)
{
    drm_data.committed_buffer = NULL;

    struct buffer_object *buffer, *tmp;
    wl_list_for_each_safe (buffer, tmp, &drm_data.buffer_list, link) {
        wl_list_remove (&buffer->link);
        wl_list_remove (&buffer->destroy_listener.link);

        destroy_buffer (buffer);
    }
    wl_list_init (&drm_data.buffer_list);
}


static void
clear_drm (void)
{
    if (drm_data.connector_props.props_info) {
        for (int i = 0; i < drm_data.connector_props.props->count_props; ++i) {
            drmModeFreeProperty (drm_data.connector_props.props_info[i]);
        }
    }
    g_clear_pointer (&drm_data.connector_props.props, drmModeFreeObjectProperties);
    g_clear_pointer (&drm_data.connector_props.props_info, free);

    if (drm_data.crtc_props.props_info) {
        for (int i = 0; i < drm_data.crtc_props.props->count_props; ++i) {
            drmModeFreeProperty (drm_data.crtc_props.props_info[i]);
        }
    }
    g_clear_pointer (&drm_data.crtc_props.props, drmModeFreeObjectProperties);
    g_clear_pointer (&drm_data.crtc_props.props_info, free);

    if (drm_data.plane_props.props_info) {
        for (int i = 0; i < drm_data.plane_props.props->count_props; ++i) {
            drmModeFreeProperty (drm_data.plane_props.props_info[i]);
        }
    }
    g_clear_pointer (&drm_data.plane_props.props, drmModeFreeObjectProperties);
    g_clear_pointer (&drm_data.plane_props.props_info, free);

    g_clear_pointer (&drm_data.base_resources, drmModeFreeResources);
    g_clear_pointer (&drm_data.plane_resources, drmModeFreePlaneResources);

    g_clear_pointer (&drm_data.encoder, drmModeFreeEncoder);

    g_clear_pointer (&drm_data.plane.obj, drmModeFreePlane);
    g_clear_pointer (&drm_data.crtc.obj, drmModeFreeCrtc);
    g_clear_pointer (&drm_data.connector.obj, drmModeFreeConnector);

    if (drm_data.fd != -1) {
        close (drm_data.fd);
        drm_data.fd = -1;
    }
}

static gboolean
check_drm(void)
{
    drmDevice *devices[64];
    memset(devices, 0, sizeof(*devices) * 64);

    int num_devices = drmGetDevices2(0, devices, 64);
    if (num_devices < 0)
        return FALSE;

    gboolean supported = FALSE;
    for (int i = 0; !supported && i < num_devices; ++i) {
        if (devices[i]->available_nodes & (1 << DRM_NODE_PRIMARY)) {
            supported = TRUE;
            break;
        }
    }

    drmFreeDevices(devices, num_devices);
    return supported;
}

static gboolean
init_drm(void)
{
    drmDevice *devices[64];
    memset(devices, 0, sizeof(*devices) * 64);

    int num_devices = drmGetDevices2(0, devices, 64);
    if (num_devices < 0)
        return FALSE;

    for (int i = 0; i < num_devices; ++i) {
        drmDevice* device = devices[i];
        g_debug ("init_drm: enumerated device %p, available_nodes %d",
                 device, device->available_nodes);

        if (device->available_nodes & (1 << DRM_NODE_PRIMARY))
            g_debug ("init_drm:   DRM_NODE_PRIMARY: %s", device->nodes[DRM_NODE_PRIMARY]);
        if (device->available_nodes & (1 << DRM_NODE_CONTROL))
            g_debug ("init_drm:   DRM_NODE_CONTROL: %s", device->nodes[DRM_NODE_CONTROL]);
        if (device->available_nodes & (1 << DRM_NODE_RENDER))
            g_debug ("init_drm:   DRM_NODE_RENDER: %s", device->nodes[DRM_NODE_RENDER]);
    }

    for (int i = 0; i < num_devices; ++i) {
        drmDevice* device = devices[i];
        if (!(device->available_nodes & (1 << DRM_NODE_PRIMARY)))
            continue;

        drm_data.fd = open (device->nodes[DRM_NODE_PRIMARY], O_RDWR);
        if (drm_data.fd < 0)
            continue;

        drm_data.base_resources = drmModeGetResources (drm_data.fd);
        if (drm_data.base_resources) {
            g_debug ("init_drm: using device %p, DRM_NODE_PRIMARY %s",
                     device, device->nodes[DRM_NODE_PRIMARY]);
            break;
        }

        close (drm_data.fd);
        drm_data.fd = -1;
    }

    drmFreeDevices(devices, num_devices);

    if (!drm_data.base_resources)
        return FALSE;

    if (drm_data.atomic_modesetting) {
        int ret = drmSetClientCap (drm_data.fd, DRM_CLIENT_CAP_ATOMIC, 1);
        if (ret) {
            drm_data.atomic_modesetting = false;
            g_debug ("init_drm: atomic mode not usable, falling back to non-atomic mode");
        }
    }

    g_debug ("init_drm: %d connectors available", drm_data.base_resources->count_connectors);
    for (int i = 0; i < drm_data.base_resources->count_connectors; ++i) {
        drmModeConnector *connector = drmModeGetConnector (drm_data.fd,
                                                           drm_data.base_resources->connectors[i]);

        g_debug ("init_drm:  connector id %u, type %u, %sconnected, %d usable modes",
                 connector->connector_id, connector->connector_type,
                 (connector->connection == DRM_MODE_CONNECTED) ? "" : "not ",
                 connector->count_modes);

        for (int j = 0; j < connector->count_modes; ++j) {
            drmModeModeInfo *mode = &connector->modes[j];
            g_debug ("init_drm:    [%d]: '%s', %ux%u@%u, flags %u, type %u %s%s",
                     j, mode->name, mode->hdisplay, mode->vdisplay, mode->vrefresh,
                     mode->flags, mode->type,
                     (mode->type & DRM_MODE_TYPE_PREFERRED) ? "(preferred) " : "",
                     (mode->type & DRM_MODE_TYPE_DEFAULT) ? "(default) " : "");
        }

        g_clear_pointer (&connector, drmModeFreeConnector);
    }

    for (int i = 0; i < drm_data.base_resources->count_connectors; ++i) {
        drm_data.connector.obj = drmModeGetConnector (drm_data.fd, drm_data.base_resources->connectors[i]);
        if (drm_data.connector.obj->connection == DRM_MODE_CONNECTED)
            break;

        g_clear_pointer (&drm_data.connector.obj, drmModeFreeConnector);
    }
    if (!drm_data.connector.obj)
        return FALSE;

    g_debug ("init_drm: using connector id %d, type %d",
             drm_data.connector.obj->connector_id, drm_data.connector.obj->connector_type);

    const char* user_selected_mode = g_getenv("COG_PLATFORM_DRM_VIDEO_MODE");

    int user_max_width = 0;
    int user_max_height = 0;
    const char *user_mode_max = g_getenv("COG_PLATFORM_DRM_MODE_MAX");
    if (user_mode_max) {
        if (sscanf(user_mode_max, "%dx%d", &user_max_width, &user_max_height) != 2 ||
            user_max_width < 0 || user_max_height < 0) {
            fprintf(stderr, "invalid value for COG_PLATFORM_DRM_MODE_MAX\n");
            user_max_width = 0;
            user_max_height = 0;
        }
    }

    for (int i = 0, area = 0; i < drm_data.connector.obj->count_modes; ++i) {
        drmModeModeInfo *current_mode = &drm_data.connector.obj->modes[i];
        if (user_selected_mode && strcmp(user_selected_mode, current_mode->name) != 0) {
            continue;
        }

        if (user_max_width && current_mode->hdisplay > user_max_width) {
            continue;
        }
        if (user_max_height && current_mode->vdisplay > user_max_height) {
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
    if (!drm_data.mode)
        return FALSE;

    g_debug ("init_drm: using mode [%ld] '%s'",
             (drm_data.mode - drm_data.connector.obj->modes) / sizeof (drmModeModeInfo *),
             drm_data.mode->name);

    for (int i = 0; i < drm_data.base_resources->count_encoders; ++i) {
        drm_data.encoder = drmModeGetEncoder (drm_data.fd, drm_data.base_resources->encoders[i]);
        if (drm_data.encoder->encoder_id == drm_data.connector.obj->encoder_id)
            break;

        g_clear_pointer (&drm_data.encoder, drmModeFreeEncoder);
    }
    if (!drm_data.encoder)
        return FALSE;

    drm_data.connector.obj_id = drm_data.connector.obj->connector_id;

    drm_data.crtc.obj_id = drm_data.encoder->crtc_id;
    drm_data.crtc.obj = drmModeGetCrtc (drm_data.fd, drm_data.crtc.obj_id);
    for (int i = 0; i < drm_data.base_resources->count_crtcs; ++i) {
        if (drm_data.base_resources->crtcs[i] == drm_data.crtc.obj_id) {
            drm_data.crtc.index = i;
            break;
        }
    }

    drm_data.plane_resources = drmModeGetPlaneResources (drm_data.fd);
    if (!drm_data.plane_resources)
        return FALSE;

    for (int i = 0; i < drm_data.plane_resources->count_planes; ++i) {
        uint32_t plane_id = drm_data.plane_resources->planes[i];
        drmModePlane *plane = drmModeGetPlane (drm_data.fd, plane_id);
        if (!plane)
            continue;

        if (!(plane->possible_crtcs & (1 << drm_data.crtc.index))) {
            drmModeFreePlane (plane);
            continue;
        }

        g_clear_pointer (&drm_data.plane.obj, drmModeFreePlane);
        drm_data.plane.obj_id = 0;

        drm_data.plane.obj = plane;
        drm_data.plane.obj_id = plane_id;
        bool is_primary = false;

        drmModeObjectProperties *plane_props = drmModeObjectGetProperties (drm_data.fd, plane_id, DRM_MODE_OBJECT_PLANE);

        for (int j = 0; j < plane_props->count_props; ++j) {
            drmModePropertyRes *prop = drmModeGetProperty (drm_data.fd, plane_props->props[j]);
            is_primary = !g_strcmp0 (prop->name, "type")
                              && plane_props->prop_values[j] == DRM_PLANE_TYPE_PRIMARY;
            drmModeFreeProperty (prop);

            if (is_primary)
                break;
        }

        drmModeFreeObjectProperties (plane_props);

        if (is_primary)
            break;
    }

    drm_data.connector_props.props = drmModeObjectGetProperties (drm_data.fd,
                                                                 drm_data.connector.obj_id,
                                                                 DRM_MODE_OBJECT_CONNECTOR);
    if (drm_data.connector_props.props) {
        drm_data.connector_props.props_info = calloc (drm_data.connector_props.props->count_props,
                                                      sizeof (*drm_data.connector_props.props_info));
        for (int i = 0; i < drm_data.connector_props.props->count_props; ++i) {
            drm_data.connector_props.props_info[i] = drmModeGetProperty (drm_data.fd,
                                                                         drm_data.connector_props.props->props[i]);
        }
    }

    drm_data.crtc_props.props = drmModeObjectGetProperties (drm_data.fd,
                                                            drm_data.crtc.obj_id,
                                                            DRM_MODE_OBJECT_CRTC);
    if (drm_data.crtc_props.props) {
        drm_data.crtc_props.props_info = calloc (drm_data.crtc_props.props->count_props,
                                                 sizeof (*drm_data.crtc_props.props_info));
        for (int i = 0; i < drm_data.crtc_props.props->count_props; ++i) {
            drm_data.crtc_props.props_info[i] = drmModeGetProperty (drm_data.fd,
                                                                    drm_data.crtc_props.props->props[i]);
        }
    }

    drm_data.plane_props.props = drmModeObjectGetProperties (drm_data.fd,
                                                             drm_data.plane.obj_id,
                                                             DRM_MODE_OBJECT_PLANE);
    if (drm_data.plane_props.props) {
        drm_data.plane_props.props_info = calloc (drm_data.plane_props.props->count_props,
                                                  sizeof (*drm_data.plane_props.props_info));
        for (int i = 0; i < drm_data.plane_props.props->count_props; ++i) {
            drm_data.plane_props.props_info[i] = drmModeGetProperty (drm_data.fd,
                                                                     drm_data.plane_props.props->props[i]);
        }
    }

    drm_data.width = drm_data.mode->hdisplay;
    drm_data.height = drm_data.mode->vdisplay;
    wl_list_init (&drm_data.buffer_list);

    g_clear_pointer (&drm_data.base_resources, drmModeFreeResources);
    g_clear_pointer (&drm_data.plane_resources, drmModeFreePlaneResources);

    return TRUE;
}

static const uint32_t formats[] = {
    DRM_FORMAT_RGBA8888,
    DRM_FORMAT_ARGB8888,
};

static uint32_t
choose_format (struct kms_plane *plane)
{
    for (int i = 0; i < G_N_ELEMENTS(formats); i++) {
        if (kms_plane_supports_format(plane, formats[i]))
            return formats[i];
    }

    return 0;
}

static void
clear_cursor (void) {
    g_clear_pointer(&cursor.cursor, kms_framebuffer_free);
    g_clear_pointer(&cursor.device, kms_device_free);
    cursor.plane = NULL;
}

static gboolean
init_cursor (void)
{
    bool cursor_supported = drmSetClientCap(drm_data.fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) == 0;
    if (!cursor_supported) {
        g_warning("cursor not supported");
        return FALSE;
    }

    cursor.device = kms_device_open(drm_data.fd);
    if (!cursor.device)
        return FALSE;

    cursor.plane = kms_device_find_plane_by_type(cursor.device, DRM_PLANE_TYPE_CURSOR, 0);
    if (!cursor.plane) {
        g_clear_pointer(&cursor.device, kms_device_free);
        return FALSE;
    }

    uint32_t format = choose_format(cursor.plane);
    if (!format) {
        g_clear_pointer(&cursor.device, kms_device_free);
        return FALSE;
    }

    cursor.cursor = create_cursor_framebuffer(cursor.device, format);
    if (!cursor.cursor) {
        g_clear_pointer(&cursor.device, kms_device_free);
        return FALSE;
    }

    cursor.x = (cursor.device->screens[0]->width - cursor.cursor->width) / 2;
    cursor.y = (cursor.device->screens[0]->height - cursor.cursor->height) / 2;
    cursor.screen_width = cursor.device->screens[0]->width;
    cursor.screen_height = cursor.device->screens[0]->height;

    if (kms_plane_set(cursor.plane, cursor.cursor, cursor.x, cursor.y)) {
        g_clear_pointer(&cursor.device, kms_device_free);
        g_clear_pointer(&cursor.cursor, kms_framebuffer_free);
        return FALSE;
    }

    cursor.enabled = TRUE;

    return TRUE;
}

static void
drm_page_flip_handler (int fd, unsigned int frame, unsigned int sec, unsigned int usec, void *data)
{
    if (drm_data.committed_buffer) {
        struct buffer_object* buffer = drm_data.committed_buffer;

        if (buffer->export.resource) {
            wpe_view_backend_exportable_fdo_dispatch_release_buffer (wpe_host_data.exportable,
                                                                     buffer->export.resource);
            buffer->export.resource = NULL;
        }
#if HAVE_SHM_EXPORTED_BUFFER
        if (buffer->export.shm_buffer) {
            wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer (wpe_host_data.exportable,
                                                                                  buffer->export.shm_buffer);
            buffer->export.shm_buffer = NULL;
        }
#endif
    }
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

#if HAVE_SHM_EXPORTED_BUFFER
static struct buffer_object *
drm_create_buffer_for_shm_buffer (struct wl_resource *buffer_resource, struct wl_shm_buffer *shm_buffer)
{
    uint32_t format = wl_shm_buffer_get_format (shm_buffer);
    if (format != WL_SHM_FORMAT_ARGB8888 && format != WL_SHM_FORMAT_XRGB8888) {
        g_warning ("failed to handle non-32-bit ARGB/XRGB format");
        return NULL;
    }

    int32_t width = wl_shm_buffer_get_width (shm_buffer);
    int32_t height = wl_shm_buffer_get_height (shm_buffer);

    // TODO: don't ignore the alpha channel in case of ARGB8888 SHM data
    uint32_t gbm_format = GBM_FORMAT_XRGB8888;
    struct gbm_bo *bo = gbm_bo_create (gbm_data.device, width, height, gbm_format,
                                       GBM_BO_USE_SCANOUT | GBM_BO_USE_WRITE);
    if (!bo) {
        g_warning ("failed to create a gbm_bo object");
        return NULL;
    }

    uint32_t in_handles[4] = { 0, };
    uint32_t in_strides[4] = { 0, };
    uint32_t in_offsets[4] = { 0, };
    in_handles[0] = gbm_bo_get_handle (bo).u32;
    in_strides[0] = gbm_bo_get_stride (bo);

    uint32_t fb_id = 0;
    int ret = drmModeAddFB2 (drm_data.fd, width, height, gbm_format,
                             in_handles, in_strides, in_offsets,
                             &fb_id, 0);
    if (ret) {
        gbm_bo_destroy (bo);
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
drm_copy_shm_buffer_into_bo (struct wl_shm_buffer *shm_buffer, struct gbm_bo *bo)
{
    int32_t width = wl_shm_buffer_get_width (shm_buffer);
    int32_t height = wl_shm_buffer_get_height (shm_buffer);
    int32_t stride = wl_shm_buffer_get_stride (shm_buffer);

    uint32_t bo_stride = 0;
    void *map_data = NULL;
    gbm_bo_map (bo, 0, 0, width, height, GBM_BO_TRANSFER_WRITE, &bo_stride, &map_data);
    if (!map_data)
        return;

    wl_shm_buffer_begin_access (shm_buffer);

    uint8_t *src = wl_shm_buffer_get_data (shm_buffer);
    uint8_t *dst = map_data;

    uint32_t bo_width = gbm_bo_get_width (bo);
    uint32_t bo_height = gbm_bo_get_height (bo);
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
        memcpy (dst, src, stride * height);

    wl_shm_buffer_end_access (shm_buffer);
    gbm_bo_unmap (bo, map_data);
}
#endif

static int
add_property (drmModeObjectProperties *props, drmModePropertyRes **props_info, drmModeAtomicReq *req, uint32_t obj_id, const char *name, uint64_t value)
{
    for (int i = 0; i < props->count_props; ++i) {
        if (!g_strcmp0 (props_info[i]->name, name)) {
            int ret = drmModeAtomicAddProperty (req, obj_id, props_info[i]->prop_id, value);
            return (ret > 0) ? 0 : -1;
        }
    }

    return -1;
}

static int
add_connector_property (drmModeAtomicReq *req, uint32_t obj_id, const char *name, uint64_t value)
{
    return add_property (drm_data.connector_props.props,
                         drm_data.connector_props.props_info,
                         req, obj_id, name, value);
}

static int
add_crtc_property (drmModeAtomicReq *req, uint32_t obj_id, const char *name, uint64_t value)
{
    return add_property (drm_data.crtc_props.props,
                         drm_data.crtc_props.props_info,
                         req, obj_id, name, value);
}

static int
add_plane_property (drmModeAtomicReq *req, uint32_t obj_id, const char *name, uint64_t value)
{
    return add_property (drm_data.plane_props.props,
                         drm_data.plane_props.props_info,
                         req, obj_id, name, value);
}

static int
drm_commit_buffer_nonatomic (struct buffer_object *buffer)
{
    if (!drm_data.mode_set) {
        int ret = drmModeSetCrtc (drm_data.fd, drm_data.crtc.obj_id, buffer->fb_id, 0, 0,
                                  &drm_data.connector.obj_id, 1, drm_data.mode);
        if (ret)
            return -1;

        drm_data.mode_set = true;
    }

    return drmModePageFlip (drm_data.fd, drm_data.crtc.obj_id, buffer->fb_id,
                            DRM_MODE_PAGE_FLIP_EVENT, buffer);
}

static int
drm_commit_buffer_atomic (struct buffer_object *buffer)
{
    int ret = 0;
    uint32_t flags = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK;

    drmModeAtomicReq *req = drmModeAtomicAlloc ();

    if (!drm_data.mode_set) {
        flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;

        uint32_t blob_id;
        ret = drmModeCreatePropertyBlob (drm_data.fd, drm_data.mode, sizeof (*drm_data.mode), &blob_id);
        if (ret) {
            drmModeAtomicFree (req);
            return -1;
        }

        ret |= add_connector_property (req, drm_data.connector.obj_id, "CRTC_ID", drm_data.crtc.obj_id);
        ret |= add_crtc_property (req, drm_data.crtc.obj_id, "MODE_ID", blob_id);
        ret |= add_crtc_property (req, drm_data.crtc.obj_id, "ACTIVE", 1);
        if (ret) {
            drmModeAtomicFree (req);
            return -1;
        }

        drm_data.mode_set = true;
    }

    ret |= add_plane_property (req, drm_data.plane.obj_id,
                               "FB_ID", buffer->fb_id);
    ret |= add_plane_property (req, drm_data.plane.obj_id,
                               "CRTC_ID", drm_data.crtc.obj_id);
    ret |= add_plane_property (req, drm_data.plane.obj_id,
                               "SRC_X", 0);
    ret |= add_plane_property (req, drm_data.plane.obj_id,
                               "SRC_Y", 0);
    ret |= add_plane_property (req, drm_data.plane.obj_id,
                               "SRC_W", ((uint64_t) drm_data.width) << 16);
    ret |= add_plane_property (req, drm_data.plane.obj_id,
                               "SRC_H", ((uint64_t) drm_data.height) << 16);
    ret |= add_plane_property (req, drm_data.plane.obj_id,
                               "CRTC_X", 0);
    ret |= add_plane_property (req, drm_data.plane.obj_id,
                               "CRTC_Y", 0);
    ret |= add_plane_property (req, drm_data.plane.obj_id,
                               "CRTC_W", drm_data.width);
    ret |= add_plane_property (req, drm_data.plane.obj_id,
                               "CRTC_H", drm_data.height);
    if (ret) {
        drmModeAtomicFree (req);
        return -1;
    }

    ret = drmModeAtomicCommit (drm_data.fd, req, flags, buffer);
    if (ret) {
        drmModeAtomicFree (req);
        return -1;
    }

    drmModeAtomicFree (req);
    return 0;
}

static void
drm_commit_buffer (struct buffer_object *buffer)
{
    int ret;
    if (drm_data.atomic_modesetting)
        ret = drm_commit_buffer_atomic (buffer);
    else
        ret = drm_commit_buffer_nonatomic (buffer);

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
        s_eglGetPlatformDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC) load_egl_proc_address ("eglGetPlatformDisplayEXT");

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
input_dispatch_key_event (uint32_t time, uint32_t key, enum libinput_key_state state)
{
    struct wpe_input_xkb_context *default_context = wpe_input_xkb_context_get_default ();
    struct xkb_state *context_state = wpe_input_xkb_context_get_state (default_context);

    uint32_t keysym = wpe_input_xkb_context_get_key_code (default_context, key, !!state);

    xkb_state_update_key (context_state, key, !!state ? XKB_KEY_DOWN : XKB_KEY_UP);
    uint32_t modifiers = wpe_input_xkb_context_get_modifiers (default_context,
        xkb_state_serialize_mods (context_state, XKB_STATE_MODS_DEPRESSED),
        xkb_state_serialize_mods (context_state, XKB_STATE_MODS_LATCHED),
        xkb_state_serialize_mods (context_state, XKB_STATE_MODS_LOCKED),
        xkb_state_serialize_layout (context_state, XKB_STATE_LAYOUT_EFFECTIVE));

    struct wpe_input_keyboard_event event = {
            .time = time,
            .key_code = keysym,
            .hardware_key_code = key,
            .pressed = (!!state),
            .modifiers = modifiers,
    };

    wpe_view_backend_dispatch_keyboard_event (wpe_view_data.backend, &event);
}

static void
start_repeating_key (uint32_t time, uint32_t key)
{
    if (!!input_data.repeating_key.time && input_data.repeating_key.time == time
        && input_data.repeating_key.key == key) {
        return;
    }

    input_data.repeating_key = (keyboard_event) {time, key};
    g_source_set_ready_time (glib_data.key_repeat_source,
                             g_get_monotonic_time() + KEY_STARTUP_DELAY);
}

static void
stop_repeating_key (void)
{
    if (!!input_data.repeating_key.time) {
        g_source_set_ready_time (glib_data.key_repeat_source, -1);
    }
    input_data.repeating_key = (keyboard_event) {0, 0};
}

static void
input_handle_key_event (struct libinput_event_keyboard *key_event)
{
    // Explanation for the offset-by-8, copied from Weston:
    //   evdev XKB rules reflect X's  broken keycode system, which starts at 8
    uint32_t key = libinput_event_keyboard_get_key (key_event) + 8;
    uint32_t time = libinput_event_keyboard_get_time (key_event);
    enum libinput_key_state key_state = libinput_event_keyboard_get_key_state (key_event);

    input_dispatch_key_event (time, key, key_state);

    if (!!key_state) {
        start_repeating_key (time, key);
    } else {
        stop_repeating_key ();
    }
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
input_handle_pointer_motion_event(struct libinput_event_pointer *pointer_event, bool absolute)
{
    if (!cursor.enabled)
        return;

    if (absolute) {
        cursor.x = libinput_event_pointer_get_absolute_x_transformed(pointer_event, cursor.screen_width);
        cursor.y = libinput_event_pointer_get_absolute_y_transformed(pointer_event, cursor.screen_height);
    } else {
        cursor.x += libinput_event_pointer_get_dx(pointer_event);
        cursor.y += libinput_event_pointer_get_dy(pointer_event);
    }

    if (cursor.x < 0) {
        cursor.x = 0;
    } else if (cursor.x > cursor.screen_width - 1) {
        cursor.x = cursor.screen_width - 1;
    }

    if (cursor.y < 0) {
        cursor.y = 0;
    } else if (cursor.y > cursor.screen_height - 1) {
        cursor.y = cursor.screen_height - 1;
    }

    struct wpe_input_pointer_event event = {
        .type = wpe_input_pointer_event_type_motion,
        .time = libinput_event_pointer_get_time(pointer_event),
        .x = cursor.x,
        .y = cursor.y,
        .button = 0,
        .state = 0,
        .modifiers = 0,
    };

    wpe_view_backend_dispatch_pointer_event(wpe_view_data.backend, &event);
    kms_plane_set(cursor.plane, cursor.cursor, cursor.x, cursor.y);
}

static void
input_handle_pointer_button_event (struct libinput_event_pointer *pointer_event)
{
    if (!cursor.enabled)
        return;

    struct wpe_input_pointer_event event = {
        .type = wpe_input_pointer_event_type_button,
        .time = libinput_event_pointer_get_time(pointer_event),
        .x = cursor.x,
        .y = cursor.y,
        .button = libinput_event_pointer_get_button(pointer_event),
        .state = libinput_event_pointer_get_button_state(pointer_event),
        .modifiers = 0,
    };

    wpe_view_backend_dispatch_pointer_event(wpe_view_data.backend, &event);
}

#if LIBINPUT_CHECK_VERSION(1, 19, 0)
static void
input_handle_pointer_discrete_scroll_event(struct libinput_event_pointer *pointer_event)
{
    struct wpe_input_axis_2d_event event = {
        .base.type = wpe_input_axis_event_type_mask_2d | wpe_input_axis_event_type_motion,
        .base.time = libinput_event_pointer_get_time(pointer_event),
        .base.x = cursor.x,
        .base.y = cursor.y,
        .x_axis = 0.0,
        .y_axis = 0.0,
    };

    if (libinput_event_pointer_has_axis(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL))
        event.y_axis = -drm_data.device_scale * libinput_event_pointer_get_scroll_value_v120(
                                                    pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
    if (libinput_event_pointer_has_axis(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL))
        event.x_axis = drm_data.device_scale * libinput_event_pointer_get_scroll_value_v120(
                                                   pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);

    wpe_view_backend_dispatch_axis_event(wpe_view_data.backend, &event.base);
}

static void
input_handle_pointer_smooth_scroll_event(struct libinput_event_pointer *pointer_event)
{
    struct wpe_input_axis_2d_event event = {
        .base.type = wpe_input_axis_event_type_mask_2d | wpe_input_axis_event_type_motion_smooth,
        .base.time = libinput_event_pointer_get_time(pointer_event),
        .base.x = cursor.x,
        .base.y = cursor.y,
        .x_axis = 0.0,
        .y_axis = 0.0,
    };

    if (libinput_event_pointer_has_axis(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL))
        event.y_axis = drm_data.device_scale *
                       libinput_event_pointer_get_scroll_value(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
    if (libinput_event_pointer_has_axis(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL))
        event.x_axis = drm_data.device_scale *
                       libinput_event_pointer_get_scroll_value(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);

    wpe_view_backend_dispatch_axis_event(wpe_view_data.backend, &event.base);
}
#else
static void
input_handle_pointer_axis_event(struct libinput_event_pointer *pointer_event)
{
    struct wpe_input_axis_2d_event event = {
        .base.type = wpe_input_axis_event_type_mask_2d,
        .base.time = libinput_event_pointer_get_time(pointer_event),
        .base.x = cursor.x,
        .base.y = cursor.y,
        .x_axis = 0.0,
        .y_axis = 0.0,
    };

    if (libinput_event_pointer_get_axis_source(pointer_event) == LIBINPUT_POINTER_AXIS_SOURCE_WHEEL) {
        event.base.type |= wpe_input_axis_event_type_motion;
        if (libinput_event_pointer_has_axis(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL))
            event.y_axis = -120.0 * libinput_event_pointer_get_axis_value_discrete(
                                        pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
        if (libinput_event_pointer_has_axis(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL))
            event.x_axis = 120.0 * libinput_event_pointer_get_axis_value_discrete(
                                       pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);
    } else {
        event.base.type |= wpe_input_axis_event_type_motion_smooth;
        if (libinput_event_pointer_has_axis(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL))
            event.y_axis = libinput_event_pointer_get_axis_value(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
        if (libinput_event_pointer_has_axis(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL))
            event.x_axis =
                libinput_event_pointer_get_axis_value(pointer_event, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);
    }

    event.x_axis *= drm_data.device_scale;
    event.y_axis *= drm_data.device_scale;

    wpe_view_backend_dispatch_axis_event(wpe_view_data.backend, &event.base);
}
#endif /* !LIBINPUT_CHECK_VERSION(1, 19, 0) */

static void
input_handle_device_added(struct libinput_device *device)
{
    g_debug("Input device %p added: %s (%04x:%04x)",
            device,
            libinput_device_get_name(device),
            libinput_device_get_id_vendor(device),
            libinput_device_get_id_product(device));
}

static void
input_handle_device_removed(struct libinput_device *device)
{
    g_debug("Input device %p removed: %s (%04x:%04x)",
            device,
            libinput_device_get_name(device),
            libinput_device_get_id_vendor(device),
            libinput_device_get_id_product(device));
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
        case LIBINPUT_EVENT_NONE:
            return;

        case LIBINPUT_EVENT_DEVICE_ADDED:
            input_handle_device_added(libinput_event_get_device(event));
            break;
        case LIBINPUT_EVENT_DEVICE_REMOVED:
            input_handle_device_removed(libinput_event_get_device(event));
            break;

        case LIBINPUT_EVENT_KEYBOARD_KEY:
            input_handle_key_event(libinput_event_get_keyboard_event(event));
            break;

        case LIBINPUT_EVENT_TOUCH_CANCEL:
            break;
        case LIBINPUT_EVENT_TOUCH_DOWN:
        case LIBINPUT_EVENT_TOUCH_UP:
        case LIBINPUT_EVENT_TOUCH_MOTION:
        case LIBINPUT_EVENT_TOUCH_FRAME:
            input_handle_touch_event(event_type, libinput_event_get_touch_event(event));
            break;

        case LIBINPUT_EVENT_POINTER_MOTION:
            input_handle_pointer_motion_event(libinput_event_get_pointer_event(event), false);
            break;
        case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
            input_handle_pointer_motion_event(libinput_event_get_pointer_event(event), true);
            break;

        case LIBINPUT_EVENT_POINTER_BUTTON:
            input_handle_pointer_button_event(libinput_event_get_pointer_event(event));
            break;

        case LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN:
        case LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE:
        case LIBINPUT_EVENT_GESTURE_SWIPE_END:
            g_debug_once("%s: GESTURE_SWIPE_{BEGIN,UPDATE,END} unimplemented", __func__);
            break;

        case LIBINPUT_EVENT_GESTURE_PINCH_BEGIN:
        case LIBINPUT_EVENT_GESTURE_PINCH_UPDATE:
        case LIBINPUT_EVENT_GESTURE_PINCH_END:
            g_debug_once("%s: GESTURE_PINCH_{BEGIN,UPDATE,END} unimplemented", __func__);
            break;

#if LIBINPUT_CHECK_VERSION(1, 2, 0)
        case LIBINPUT_EVENT_TABLET_TOOL_AXIS:
            g_debug_once("%s: TABLET_TOOL_AXIS unimplemented", __func__);
            break;
        case LIBINPUT_EVENT_TABLET_TOOL_PROXIMITY:
            g_debug_once("%s: TABLET_TOOL_PROXIMITY unimplemented", __func__);
            break;
        case LIBINPUT_EVENT_TABLET_TOOL_TIP:
            g_debug_once("%s: TABLET_TOOL_TIP unimplemented", __func__);
            break;
        case LIBINPUT_EVENT_TABLET_TOOL_BUTTON:
            g_debug_once("%s: TABLET_TOOL_BUTTON unimplemented", __func__);
            break;
#endif /* LIBINPUT_CHECK_VERSION(1, 2, 0) */

#if LIBINPUT_CHECK_VERSION(1, 3, 0)
        case LIBINPUT_EVENT_TABLET_PAD_BUTTON:
            g_debug_once("%s: TABLET_PAD_BUTTON unimplemented", __func__);
            break;
        case LIBINPUT_EVENT_TABLET_PAD_RING:
            g_debug_once("%s: TABLET_PAD_RING unimplemented", __func__);
            break;
        case LIBINPUT_EVENT_TABLET_PAD_STRIP:
            g_debug_once("%s: TABLET_PAD_STRIP unimplemented", __func__);
            break;
#endif /* LIBINPUT_CHECK_VERSION(1, 3, 0) */

#if LIBINPUT_CHECK_VERSION(1, 7, 0)
        case LIBINPUT_EVENT_SWITCH_TOGGLE:
            g_debug_once("%s: SWITCH_TOGGLE unimplemented", __func__);
            break;
#endif /* LIBINPUT_CHECK_VERSION(1, 7, 0) */

#if LIBINPUT_CHECK_VERSION(1, 15, 0)
        case LIBINPUT_EVENT_TABLET_PAD_KEY:
            g_debug_once("%s: TABLET_PAD_KEY unimplemented", __func__);
            break;
#endif /* LIBINPUT_CHECK_VERSION(1, 15, 0) */

#if LIBINPUT_CHECK_VERSION(1, 19, 0)
        case LIBINPUT_EVENT_POINTER_AXIS:
            /* Deprecated, use _SCROLL_{WHEEL,FINGER,CONTINUOUS} below. */
            break;
        case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
            input_handle_pointer_discrete_scroll_event(libinput_event_get_pointer_event(event));
            break;
        case LIBINPUT_EVENT_POINTER_SCROLL_FINGER:
        case LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS:
            input_handle_pointer_smooth_scroll_event(libinput_event_get_pointer_event(event));
            break;

        case LIBINPUT_EVENT_GESTURE_HOLD_BEGIN:
        case LIBINPUT_EVENT_GESTURE_HOLD_END:
            g_debug_once("%s: GESTURE_HOLD_{BEGIN,END} unimplemented", __func__);
            break;
#else
        case LIBINPUT_EVENT_POINTER_AXIS:
            input_handle_pointer_axis_event(libinput_event_get_pointer_event(event));
            break;
#endif /* LIBINPUT_CHECK_VERSION(1, 19, 0) */
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

static gboolean
key_repeat_source_dispatch (GSource *base, GSourceFunc callback, gpointer user_data)
{
    if (!input_data.repeating_key.time) {
        g_source_set_ready_time (base, -1);
        return TRUE;
    }

    input_dispatch_key_event (input_data.repeating_key.time,
                              input_data.repeating_key.key,
                              LIBINPUT_KEY_STATE_PRESSED);
    g_source_set_ready_time (base, g_get_monotonic_time() + KEY_REPEAT_DELAY);
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

    if (glib_data.key_repeat_source)
        g_source_destroy (glib_data.key_repeat_source);
    g_clear_pointer (&glib_data.key_repeat_source, g_source_unref);
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

    static GSourceFuncs key_repeat_source_funcs = {
        .dispatch = key_repeat_source_dispatch,
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

    glib_data.key_repeat_source = g_source_new (&key_repeat_source_funcs, sizeof (GSource));
    g_source_set_name (glib_data.key_repeat_source, "cog: key repeat");
    g_source_set_can_recurse (glib_data.key_repeat_source, TRUE);
    g_source_set_priority (glib_data.key_repeat_source, G_PRIORITY_DEFAULT_IDLE);
    g_source_attach (glib_data.key_repeat_source, g_main_context_get_thread_default ());

    return TRUE;
}


static void
on_export_buffer_resource (void *data, struct wl_resource *buffer_resource)
{
    struct buffer_object *buffer = drm_buffer_for_resource (buffer_resource);
    if (buffer) {
        buffer->export.resource = buffer_resource;
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
    if (buffer) {
        buffer->export.resource = buffer_resource;
        drm_commit_buffer (buffer);
    }
}

static void
on_export_dmabuf_resource (void *data, struct wpe_view_backend_exportable_fdo_dmabuf_resource *dmabuf_resource)
{
    struct buffer_object *buffer = drm_buffer_for_resource (dmabuf_resource->buffer_resource);
    if (buffer) {
        buffer->export.resource = dmabuf_resource->buffer_resource;
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
    if (buffer) {
        buffer->export.resource = dmabuf_resource->buffer_resource;
        drm_commit_buffer (buffer);
    }
}

#if HAVE_SHM_EXPORTED_BUFFER
static void
on_export_shm_buffer (void* data, struct wpe_fdo_shm_exported_buffer *exported_buffer)
{
    struct wl_resource *exported_resource = wpe_fdo_shm_exported_buffer_get_resource (exported_buffer);
    struct wl_shm_buffer *exported_shm_buffer = wpe_fdo_shm_exported_buffer_get_shm_buffer (exported_buffer);

    struct buffer_object *buffer = drm_buffer_for_resource (exported_resource);
    if (buffer) {
        drm_copy_shm_buffer_into_bo (exported_shm_buffer, buffer->bo);

        buffer->export.shm_buffer = exported_buffer;
        drm_commit_buffer (buffer);
        return;
    }

    buffer = drm_create_buffer_for_shm_buffer (exported_resource, exported_shm_buffer);
    if (buffer) {
        drm_copy_shm_buffer_into_bo (exported_shm_buffer, buffer->bo);

        buffer->export.shm_buffer = exported_buffer;
        drm_commit_buffer (buffer);
    }
}
#endif

static void *
check_supported(void *data G_GNUC_UNUSED)
{
    if (check_drm())
        return GINT_TO_POINTER(TRUE);
    return GINT_TO_POINTER(FALSE);
}

static gboolean
cog_drm_platform_is_supported(void)
{
    static GOnce once = G_ONCE_INIT;
    g_once(&once, check_supported, NULL);
    return GPOINTER_TO_INT(once.retval);
}

static gboolean
cog_drm_platform_setup(CogPlatform *platform, CogShell *shell, const char *params, GError **error)
{
    g_assert (platform);
    g_return_val_if_fail (COG_IS_SHELL (shell), FALSE);

    init_config (shell);

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

    if (g_getenv ("COG_PLATFORM_DRM_CURSOR")) {
        if (!init_cursor ()) {
            g_warning ("Failed to initialize cursor");
        }
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

static void
cog_drm_platform_teardown(CogPlatform *platform)
{
    g_assert (platform);

    clear_buffers ();

    clear_glib ();
    clear_input ();
    clear_egl ();
    clear_gbm ();
    clear_cursor ();
    clear_drm ();
}

static WebKitWebViewBackend *
cog_drm_platform_get_view_backend(CogPlatform *platform, WebKitWebView *related_view, GError **error)
{
    static struct wpe_view_backend_exportable_fdo_client exportable_client = {
        .export_buffer_resource = on_export_buffer_resource,
        .export_dmabuf_resource = on_export_dmabuf_resource,
#if HAVE_SHM_EXPORTED_BUFFER
        .export_shm_buffer = on_export_shm_buffer,
#endif
    };

    wpe_host_data.exportable = wpe_view_backend_exportable_fdo_create (&exportable_client,
                                                                       NULL,
                                                                       drm_data.width / drm_data.device_scale,
                                                                       drm_data.height / drm_data.device_scale);
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

static void
cog_drm_platform_init_web_view(CogPlatform *platform, WebKitWebView *view)
{
    wpe_view_backend_dispatch_set_device_scale_factor (wpe_view_data.backend,
                                                       drm_data.device_scale);
}

static void
cog_drm_platform_class_init(CogDrmPlatformClass *klass)
{
    CogPlatformClass *platform_class = COG_PLATFORM_CLASS(klass);
    platform_class->is_supported = cog_drm_platform_is_supported;
    platform_class->setup = cog_drm_platform_setup;
    platform_class->teardown = cog_drm_platform_teardown;
    platform_class->get_view_backend = cog_drm_platform_get_view_backend;
    platform_class->init_web_view = cog_drm_platform_init_web_view;
}

static void
cog_drm_platform_class_finalize(CogDrmPlatformClass *klass)
{
}

static void
cog_drm_platform_init(CogDrmPlatform *self)
{
}

G_MODULE_EXPORT void
g_io_cogplatform_drm_load(GIOModule *module)
{
    GTypeModule *type_module = G_TYPE_MODULE(module);
    cog_drm_platform_register_type(type_module);
}

G_MODULE_EXPORT void
g_io_cogplatform_drm_unload(GIOModule *module G_GNUC_UNUSED)
{
}
