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
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>


#ifndef EGL_EXT_platform_base
#define EGL_EXT_platform_base 1
typedef EGLDisplay (EGLAPIENTRYP PFNEGLGETPLATFORMDISPLAYEXTPROC) (EGLenum platform, void *native_display, const EGLint *attrib_list);
#endif

#ifndef EGL_KHR_platform_gbm
#define EGL_KHR_platform_gbm 1
#define EGL_PLATFORM_GBM_KHR              0x31D7
#endif


struct buffer_object {
    EGLImageKHR image;
    struct wl_resource *buffer_resource;

    struct gbm_bo *bo;
};

struct cached_bo {
    struct wl_list link;
    struct wl_listener destroyListener;

    uint32_t fb_id;
    struct gbm_bo *bo;
    struct buffer_object *current_buffer;
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
    uint32_t width;
    uint32_t height;

    float input_coords_transform[6];

    float position_coords[4][2];
    float texture_coords[4][2];
} geometry_data = {
    .width = 0,
    .height = 0,
};

static struct {
    struct gbm_device *device;
    struct gbm_surface *surface;
} gbm_data = {
    .device = NULL,
    .surface = NULL,
};

static struct {
    EGLDisplay display;

    PFNEGLCREATEIMAGEKHRPROC create_image;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture;

    EGLConfig config;
    EGLContext context;
    EGLSurface surface;
} egl_data = {
    .display = EGL_NO_DISPLAY,
    .config = 0,
};

static struct {
    GLint vertex_shader;
    GLint fragment_shader;
    GLint program;
    GLuint texture;

    GLint attr_pos;
    GLint attr_texture;
    GLint uniform_texture;
} gl_data = {
    .vertex_shader = 0,
    .fragment_shader = 0,
    .program = 0,
    .texture = 0,
};

static struct {
    bool use_raw_touch_event_coordinates;
} config_data = {
    .use_raw_touch_event_coordinates = 0,
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
    gbm_surface_release_buffer (gbm_data.surface, buffer->bo);
    egl_data.destroy_image (egl_data.display, buffer->image);

    wpe_view_backend_exportable_fdo_dispatch_release_buffer (wpe_host_data.exportable, buffer->buffer_resource);
    g_free (buffer);
}

static void
destroy_cached_bo (struct gbm_bo *bo, void *data)
{
    struct cached_bo *cached = data;
    drmModeRmFB (drm_data.fd, cached->fb_id);

    if (cached->current_buffer && cached->current_buffer->bo == bo)
        cached->current_buffer->bo = NULL;
}

static void
clear_drm (void)
{
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

    for (int i = 0; i < num_devices; ++i) {
        drmDevicePtr device = devices[i];
        fprintf(stderr, "device %p, available_nodes %u\n  ", device, device->available_nodes);
        for (int j = 0; j < DRM_NODE_MAX; ++j)
            fprintf(stderr, " [%d] %s,", j, device->nodes[j]);
        fprintf(stderr, "\n");
    }

    drmModeRes *resources = NULL;
    for (int i = 0; i < num_devices; ++i) {
        drmDevicePtr device = devices[i];
        if (!(device->available_nodes & (1 << DRM_NODE_PRIMARY)))
            continue;

        drm_data.fd = open (device->nodes[DRM_NODE_PRIMARY], O_RDWR | O_CLOEXEC);
        if (drm_data.fd < 0)
            continue;

        resources = drmModeGetResources (drm_data.fd);
        if (resources) {
            fprintf(stderr, "retrieved resources for device %p, primary node %s\n",
                device, device->nodes[DRM_NODE_PRIMARY]);
            break;
        }

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

    fprintf(stderr, "Available  modes:\n");
    for (int i = 0; i < drm_data.connector->count_modes; ++i) {
        drmModeModeInfo *mode = &drm_data.connector->modes[i];
        fprintf(stderr, "  [%d] mode '%s', %ux%u@%u\n", i,
            mode->name, mode->hdisplay, mode->vdisplay, mode->vrefresh);
    }

    const char* selected_mode_index = getenv("COG_DRM_MODE_INDEX");
    if (selected_mode_index) {
        char* end_char;
        unsigned selected_mode_index_value = strtoul(selected_mode_index, &end_char, 10);
        if (selected_mode_index_value >= drm_data.connector->count_modes) {
            g_warning ("COG_DRM_MODE_INDEX value out of bounds");
            return FALSE;
        }

        drm_data.mode = &drm_data.connector->modes[selected_mode_index_value];
    }

    for (int i = 0, area = 0; drm_data.mode == NULL && i < drm_data.connector->count_modes; ++i) {
        drmModeModeInfo *current_mode = &drm_data.connector->modes[i];
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

    fprintf(stderr, "Selected mode '%s', %ux%u@%u\n", drm_data.mode->name,
        drm_data.mode->hdisplay, drm_data.mode->vdisplay, drm_data.mode->vrefresh);

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

    g_clear_pointer (&resources, drmModeFreeResources);
    return TRUE;
}

static void
drm_page_flip_handler (int fd, unsigned int frame, unsigned int sec, unsigned int usec, void *data)
{
    g_clear_pointer (&drm_data.committed_buffer, destroy_buffer);
    drm_data.committed_buffer = (struct buffer_object *) data;

    wpe_view_backend_exportable_fdo_dispatch_frame_complete (wpe_host_data.exportable);
}


static gboolean
init_geometry (void)
{
    geometry_data.width = drm_data.width;
    geometry_data.height = drm_data.height;

    float input_coords_transform[6] = {
        1, 0,
        0, 1,
        0, 0,
    };

    float position_coords[4][2] = {
        { -1,  1 }, {  1,  1 },
        { -1, -1 }, {  1, -1 },
    };
    float texture_coords[4][2] = {
        { 0, 0 }, { 1, 0 },
        { 0, 1 }, { 1, 1 },
    };

    const char* rotation_mode = getenv("COG_DRM_ROTATION");
    if (rotation_mode) {
        if (!strcmp(rotation_mode, "0")) {
            // Everything is already default-initialized for 0-degrees rotation.
        } else if (!strcmp(rotation_mode, "90")) {
            geometry_data.width = drm_data.height;
            geometry_data.height = drm_data.width;

            float rot_input[6] = {
                0, 1,
                -1, 0,
                drm_data.height, 0,
            };
            memcpy (input_coords_transform, rot_input, 6 * sizeof(float));

            float rot_coords[4][2] = {
                { 1, 0 }, { 1, 1 },
                { 0, 0 }, { 0, 1 },
            };
            memcpy (texture_coords, rot_coords, 8 * sizeof(float));
        } else if (!strcmp(rotation_mode, "180")) {
            float rot_input[6] = {
                -1, 0,
                0, -1,
                drm_data.width, drm_data.height,
            };
            memcpy (input_coords_transform, rot_input, 6 * sizeof(float));

            float rot_coords[4][2] = {
                { 1, 1 }, { 0, 1 },
                { 1, 0 }, { 0, 0 },
            };
            memcpy (texture_coords, rot_coords, 8 * sizeof(float));
        } else if (!strcmp(rotation_mode, "270")) {
            geometry_data.width = drm_data.height;
            geometry_data.height = drm_data.width;

            float rot_input[6] = {
                0, -1,
                1, 0,
                0, drm_data.width,
            };
            memcpy (input_coords_transform, rot_input, 6 * sizeof(float));

            float rot_coords[4][2] = {
                { 0, 1 }, { 0, 0 },
                { 1, 1 }, { 1, 0 },
            };
            memcpy (texture_coords, rot_coords, 8 * sizeof(float));
        } else {
            fprintf(stderr, "unknown COG_DRM_ROTATION value '%s' (supporting 0, 90, 180, 270\n",
                rotation_mode);
        }
    }

    memcpy(geometry_data.input_coords_transform, input_coords_transform, 6 * sizeof(float));
    memcpy(geometry_data.position_coords, position_coords, 8 * sizeof(float));
    memcpy(geometry_data.texture_coords, texture_coords, 8 * sizeof(float));

    return TRUE;
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

    gbm_data.surface = gbm_surface_create (gbm_data.device, drm_data.width, drm_data.height,
                                           GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!gbm_data.surface)
        return FALSE;
    return TRUE;
}


static void
clear_egl (void)
{
    if (egl_data.surface != EGL_NO_SURFACE)
        eglDestroySurface (egl_data.display, egl_data.surface);

    if (egl_data.context != EGL_NO_CONTEXT)
        eglDestroyContext (egl_data.display, egl_data.context);
}

static gboolean
init_egl (void)
{
    static const EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE,
    };

    static const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 1,
        EGL_GREEN_SIZE, 1,
        EGL_BLUE_SIZE, 1,
        EGL_ALPHA_SIZE, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SAMPLES, 0,
        EGL_NONE,
    };

    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC) eglGetProcAddress ("eglGetPlatformDisplayEXT");
    if (get_platform_display)
        egl_data.display = get_platform_display (EGL_PLATFORM_GBM_KHR, gbm_data.device, NULL);
    else
        egl_data.display = eglGetDisplay((EGLNativeDisplayType) gbm_data.device);
    if (egl_data.display == EGL_NO_DISPLAY)
        return FALSE;

    if (!eglInitialize(egl_data.display, NULL, NULL))
        return FALSE;

    egl_data.create_image = (PFNEGLCREATEIMAGEKHRPROC) eglGetProcAddress ("eglCreateImageKHR");
    egl_data.destroy_image = (PFNEGLDESTROYIMAGEKHRPROC) eglGetProcAddress ("eglDestroyImageKHR");
    egl_data.image_target_texture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) eglGetProcAddress ("glEGLImageTargetTexture2DOES");
    if (!egl_data.create_image || !egl_data.destroy_image || !egl_data.image_target_texture)
        return FALSE;

    if (!eglBindAPI (EGL_OPENGL_ES_API))
        return FALSE;

    {

        EGLint count = 0;
        EGLint matched = 0;
        EGLConfig *configs;

        if (!eglGetConfigs (egl_data.display, NULL, 0, &count) || count < 1)
            return FALSE;

        configs = g_new0 (EGLConfig, count);
        if (!eglChooseConfig (egl_data.display, config_attribs, configs, count, &matched) || !matched) {
            g_free (configs);
            return FALSE;
        }

        for (int i = 0; i < matched; ++i) {
            EGLint id = 0;
            if (!eglGetConfigAttrib (egl_data.display, configs[i], EGL_NATIVE_VISUAL_ID, &id))
                continue;

            if (id == GBM_FORMAT_XRGB8888) {
                egl_data.config = configs[i];
                break;
            }
        }

        g_free (configs);
        if (!egl_data.config)
            return FALSE;
    }

    egl_data.context = eglCreateContext (egl_data.display, egl_data.config, EGL_NO_CONTEXT, context_attribs);
    if (egl_data.context == EGL_NO_CONTEXT)
        return FALSE;

    egl_data.surface = eglCreateWindowSurface (egl_data.display, egl_data.config, gbm_data.surface, NULL);
    if (egl_data.surface == EGL_NO_SURFACE)
        return FALSE;

    eglMakeCurrent (egl_data.display, egl_data.surface, egl_data.surface, egl_data.context);
    return TRUE;
}


static void
clear_gl (void)
{
    if (gl_data.texture)
        glDeleteTextures (1, &gl_data.texture);

    if (gl_data.vertex_shader)
        glDeleteShader (gl_data.vertex_shader);
    if (gl_data.fragment_shader)
        glDeleteShader (gl_data.fragment_shader);
    if (gl_data.program)
        glDeleteProgram (gl_data.program);
}

static gboolean
init_gl (void)
{
    static const char* vertex_shader_source =
        "attribute vec2 pos;\n"
        "attribute vec2 texture;\n"
        "varying vec2 v_texture;\n"
        "void main() {\n"
        "  v_texture = texture;\n"
        "  gl_Position = vec4(pos, 0, 1);\n"
        "}\n";
    static const char* fragment_shader_source =
        "precision mediump float;\n"
        "uniform sampler2D u_texture;\n"
        "varying vec2 v_texture;\n"
        "void main() {\n"
        "  gl_FragColor = texture2D(u_texture, v_texture);\n"
        "}\n";

    gl_data.vertex_shader = glCreateShader (GL_VERTEX_SHADER);
    glShaderSource (gl_data.vertex_shader, 1, &vertex_shader_source, NULL);
    glCompileShader (gl_data.vertex_shader);

    GLint vertex_shader_compile_status = 0;
    glGetShaderiv (gl_data.vertex_shader, GL_COMPILE_STATUS, &vertex_shader_compile_status);
    if (!vertex_shader_compile_status) {
        GLsizei vertex_shader_info_log_length = 0;
        char vertex_shader_info_log[1024];
        glGetShaderInfoLog (gl_data.vertex_shader, 1023,
            &vertex_shader_info_log_length, vertex_shader_info_log);
        vertex_shader_info_log[vertex_shader_info_log_length] = 0;
        g_warning ("Unable to compile vertex shader:\n%s", vertex_shader_info_log);
    }

    gl_data.fragment_shader = glCreateShader (GL_FRAGMENT_SHADER);
    glShaderSource (gl_data.fragment_shader, 1, &fragment_shader_source, NULL);
    glCompileShader (gl_data.fragment_shader);

    GLint fragment_shader_compile_status = 0;
    glGetShaderiv (gl_data.fragment_shader, GL_COMPILE_STATUS, &fragment_shader_compile_status);
    if (!fragment_shader_compile_status) {
        GLsizei fragment_shader_info_log_length = 0;
        char fragment_shader_info_log[1024];
        glGetShaderInfoLog (gl_data.fragment_shader, 1023,
            &fragment_shader_info_log_length, fragment_shader_info_log);
        fragment_shader_info_log[fragment_shader_info_log_length] = 0;
        g_warning ("Unable to compile fragment shader:\n%s", fragment_shader_info_log);
    }

    gl_data.program = glCreateProgram ();
    glAttachShader (gl_data.program, gl_data.vertex_shader);
    glAttachShader (gl_data.program, gl_data.fragment_shader);
    glLinkProgram (gl_data.program);

    GLint link_status = 0;
    glGetProgramiv (gl_data.program, GL_LINK_STATUS, &link_status);
    if (!link_status) {
        GLsizei program_info_log_length = 0;
        char program_info_log[1024];
        glGetProgramInfoLog (gl_data.program, 1023,
            &program_info_log_length, program_info_log);
        program_info_log[program_info_log_length] = 0;
        g_warning ("Unable to link program:\n%s", program_info_log);
        return FALSE;
    }

    gl_data.attr_pos = glGetAttribLocation (gl_data.program, "pos");
    gl_data.attr_texture = glGetAttribLocation (gl_data.program, "texture");
    gl_data.uniform_texture = glGetUniformLocation (gl_data.program, "u_texture");

    glGenTextures (1, &gl_data.texture);
    glBindTexture (GL_TEXTURE_2D, gl_data.texture);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, geometry_data.width, geometry_data.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture (GL_TEXTURE_2D, 0);

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
input_transform_event_coords (float x, float y, float *out_x, float *out_y)
{
    float *t = geometry_data.input_coords_transform;

    *out_x = x * t[0] + y * t[2] + t[4];
    *out_y = x * t[1] + y * t[3] + t[5];
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
        double event_x, event_y;

        if (config_data.use_raw_touch_event_coordinates) {
            event_x = libinput_event_touch_get_x (touch_event);
            event_y = libinput_event_touch_get_y (touch_event);
        } else {
            event_x = libinput_event_touch_get_x_transformed (touch_event, input_data.input_width);
            event_y = libinput_event_touch_get_y_transformed (touch_event, input_data.input_height);
        }

        float x, y;
        input_transform_event_coords (event_x, event_y, &x, &y);
        touch_point->x = x;
        touch_point->y = y;
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


static gboolean
init_config (CogShell *shell)
{
    GKeyFile* key_file = cog_shell_get_config_file (shell);
    if (!key_file)
        return TRUE;

    config_data.use_raw_touch_event_coordinates = g_key_file_get_boolean (key_file,
                                                                          "input",
                                                                          "raw-touch-event-coordinates",
                                                                          NULL);
    return TRUE;
}


static void
render_resource (struct wl_resource *buffer_resource, EGLImageKHR image)
{
    eglMakeCurrent (egl_data.display, egl_data.surface, egl_data.surface, egl_data.context);

    glViewport (0, 0, drm_data.width, drm_data.height);
    glClearColor (1.0, 0, 0, 1.0);
    glClear (GL_COLOR_BUFFER_BIT);

    glUseProgram (gl_data.program);

    glActiveTexture (GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, gl_data.texture);
    egl_data.image_target_texture (GL_TEXTURE_2D, image);
    glUniform1i (gl_data.uniform_texture, 0);

    glVertexAttribPointer (gl_data.attr_pos, 2, GL_FLOAT, GL_FALSE, 0, geometry_data.position_coords);
    glVertexAttribPointer (gl_data.attr_texture, 2, GL_FLOAT, GL_FALSE, 0, geometry_data.texture_coords);

    glEnableVertexAttribArray (gl_data.attr_pos);
    glEnableVertexAttribArray (gl_data.attr_texture);

    glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);

    glDisableVertexAttribArray (gl_data.attr_pos);
    glDisableVertexAttribArray (gl_data.attr_texture);

    eglSwapBuffers (egl_data.display, egl_data.surface);

    struct gbm_bo* bo = gbm_surface_lock_front_buffer (gbm_data.surface);
    if (!bo) {
        g_warning ("failed to lock GBM front buffer");
        return;
    }

    int ret;
    struct cached_bo *cached = gbm_bo_get_user_data (bo);
    if (!cached) {
        uint32_t fb_id = 0;
        uint32_t handles[] = { gbm_bo_get_handle(bo).u32, 0, 0, 0 };
        uint32_t pitches[] = { gbm_bo_get_stride(bo), 0, 0, 0 };
        uint32_t offsets[] = { 0, 0, 0, 0 };
        ret = drmModeAddFB2(drm_data.fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo),
            gbm_bo_get_format(bo), handles, pitches, offsets, &fb_id, 0);
        if (ret) {
            g_warning ("failed to create framebuffer: %s", strerror (errno));
            return;
        }

        cached = g_new0 (struct cached_bo, 1);
        cached->fb_id = fb_id;
        cached->bo = bo;
        gbm_bo_set_user_data (bo, cached, destroy_cached_bo);
    }

    if (!drm_data.mode_set) {
        ret = drmModeSetCrtc (drm_data.fd, drm_data.crtc_id, cached->fb_id, 0, 0,
                              &drm_data.connector_id, 1, drm_data.mode);
        if (ret) {
            g_warning ("failed to set mode: %s", strerror (errno));
            return;
        }

        drm_data.mode_set = true;
    }

    struct buffer_object *buffer = g_new0 (struct buffer_object, 1);
    buffer->image = image;
    buffer->buffer_resource = buffer_resource;
    buffer->bo = bo;

    cached->current_buffer = buffer;

    ret = drmModePageFlip (drm_data.fd, drm_data.crtc_id, cached->fb_id,
                           DRM_MODE_PAGE_FLIP_EVENT, buffer);
    if (ret)
        g_warning ("failed to schedule a page flip: %s", strerror (errno));
}

static void
on_export_buffer_resource(void *data, struct wl_resource *buffer_resource)
{
    EGLImageKHR image = egl_data.create_image (egl_data.display, EGL_NO_CONTEXT, EGL_WAYLAND_BUFFER_WL,
                                               (EGLClientBuffer) buffer_resource, NULL);

    render_resource (buffer_resource, image);
}

static void
on_export_dmabuf_resource (void *data, struct wpe_view_backend_exportable_fdo_dmabuf_resource *dmabuf_resource)
{
    EGLint image_attributes[] = {
        EGL_WIDTH, dmabuf_resource->width,
        EGL_HEIGHT, dmabuf_resource->height,
        EGL_LINUX_DRM_FOURCC_EXT, dmabuf_resource->format,
        EGL_DMA_BUF_PLANE0_FD_EXT, dmabuf_resource->fds[0],
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, dmabuf_resource->offsets[0],
        EGL_DMA_BUF_PLANE0_PITCH_EXT, dmabuf_resource->strides[0],
        EGL_NONE,
    };
    EGLImageKHR image = egl_data.create_image (egl_data.display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                                               (EGLClientBuffer) NULL, image_attributes);

    render_resource (dmabuf_resource->buffer_resource, image);
}

gboolean
cog_platform_plugin_setup (CogPlatform *platform,
                    CogShell    *shell,
                    const char  *params,
                    GError     **error)
{
    g_assert (platform);
    g_return_val_if_fail (COG_IS_SHELL (shell), FALSE);

    if (!init_config (shell)) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to initialize configuration");
        return FALSE;
    }

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

    if (!init_geometry()) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to initialize geometry");
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

    if (!init_gl ()) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to initialize GL");
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
    clear_gl ();
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
                                                                       geometry_data.width,
                                                                       geometry_data.height);
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
