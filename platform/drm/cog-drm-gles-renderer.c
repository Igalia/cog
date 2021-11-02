/*
 * cog-drm-gles-renderer.c
 * Copyright (C) 2021 Igalia S.L.
 *
 * Distributed under terms of the MIT license.
 */

#include "../../core/cog.h"
#include "../common/cog-gl-utils.h"
#include "cog-drm-renderer.h"
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <epoxy/gl.h>
#include <errno.h>
#include <gbm.h>
#include <glib-unix.h>
#include <wayland-util.h>
#include <wpe/fdo-egl.h>
#include <wpe/fdo.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

G_DEFINE_AUTOPTR_CLEANUP_FUNC(drmModePlane, drmModeFreePlane)

typedef struct {
    CogDrmRenderer base;

    struct gbm_device  *gbm_device;
    struct gbm_surface *gbm_surface;
    struct gbm_bo      *current_bo;
    struct gbm_bo      *next_bo;
    uint32_t            gbm_format;

    /*
     * Logical view size without transformations applied, which is needed to
     * change the transformed size (i.e. rotated) of the view after the view
     * backend has been instantiated. Note that physical output size is
     * always determinted from the chosen DRM/KMS mode.
     */
    uint32_t width, height;

    CogDrmRendererRotation rotation;

    EGLDisplay egl_display;
    EGLConfig  egl_config;
    EGLContext egl_context;
    EGLSurface egl_surface;

    /*
     * The renderer wraps frames provided by wpebackend-fdo into a texture,
     * then paints a quad which samples said texture. A simple GLSL shader
     * program uses the "position" attribute to fetch the coordinates of
     * the quad, and the "texture" attribute the UV mapping coordinates for
     * the texture. Rotation is achieved by changing the UV mapping. The
     * "texture" uniform is used to reference the texture unit where the
     * frame textures get loaded.
     */
    GLuint gl_program;
    GLuint gl_texture;
    GLint  gl_attrib_position;
    GLint  gl_attrib_texture;
    GLint  gl_uniform_texture;

    struct wpe_view_backend_exportable_fdo *exportable;

    drmEventContext drm_context;
    unsigned        drm_fd_source;
    uint32_t        crtc_id;
    uint32_t        connector_id;
    uint32_t        plane_id;
    drmModeModeInfo mode;
    bool            mode_set;
    bool            atomic_modesetting;

    struct {
        drmModeObjectProperties *props;
        drmModePropertyRes     **props_info;
    } connector_props, crtc_props, plane_props;
} CogDrmGlesRenderer;

static void
cog_drm_gles_renderer_handle_egl_image(void *data, struct wpe_fdo_egl_exported_image *image)
{
    CogDrmGlesRenderer *self = data;

    if (!eglMakeCurrent(self->egl_display, self->egl_surface, self->egl_surface, self->egl_context)) {
        g_critical("%s: Cannot activate EGL context for rendering (%#04x)", __func__, eglGetError());
        return;
    }

    glViewport(0, 0, self->mode.hdisplay, self->mode.vdisplay);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(self->gl_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, self->gl_texture);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, wpe_fdo_egl_exported_image_get_egl_image(image));
    glUniform1i(self->gl_uniform_texture, 0);

    /* clang-format off */
    static const float position_coords[4][2] = {
        { -1,  1 }, { 1,  1 },
        { -1, -1 }, { 1, -1 },
    };
    static const float texture_coords[4][4][2] = {
        [COG_DRM_RENDERER_ROTATION_0] = {
            { 0, 0 }, { 1, 0 },
            { 0, 1 }, { 1, 1 },
        },
        [COG_DRM_RENDERER_ROTATION_90] = {
            { 1, 0 }, { 1, 1 },
            { 0, 0 }, { 0, 1 },
        },
        [COG_DRM_RENDERER_ROTATION_180] = {
            { 1, 1 }, { 0, 1 },
            { 1, 0 }, { 0, 0 },
        },
        [COG_DRM_RENDERER_ROTATION_270] = {
            { 0, 1 }, { 0, 0 },
            { 1, 1 }, { 1, 0 },
        },
    };
    /* clang-format on */

    glVertexAttribPointer(self->gl_attrib_position, 2, GL_FLOAT, GL_FALSE, 0, position_coords);
    glVertexAttribPointer(self->gl_attrib_texture, 2, GL_FLOAT, GL_FALSE, 0, texture_coords[self->rotation]);

    glEnableVertexAttribArray(self->gl_attrib_position);
    glEnableVertexAttribArray(self->gl_attrib_texture);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisableVertexAttribArray(self->gl_attrib_position);
    glDisableVertexAttribArray(self->gl_attrib_texture);

    if (G_UNLIKELY(!eglSwapBuffers(self->egl_display, self->egl_surface))) {
        g_critical("%s: eglSwapBuffers failed (%#04x)", __func__, eglGetError());
        return;
    }

    wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(self->exportable, image);

    int drm_fd = gbm_device_get_fd(self->gbm_device);

    struct gbm_bo *bo = gbm_surface_lock_front_buffer(self->gbm_surface);

    uint32_t handles[4], strides[4], offsets[4];
    uint64_t modifiers[4];

    for (unsigned i = 0; i < gbm_bo_get_plane_count(bo); i++) {
        handles[i] = gbm_bo_get_handle_for_plane(bo, i).u32;
        strides[i] = gbm_bo_get_stride_for_plane(bo, i);
        offsets[i] = gbm_bo_get_offset(bo, i);
        modifiers[i] = gbm_bo_get_modifier(bo);
    }

    /*
     * Allocating a BO with most drivers typically does NOT query the DRM/KMS
     * subsystem to know which modifiers are appropriate for an output, which
     * means the modifiers reported by libgbm might result in a failed frame
     * buffer allocation. Retrying without modifiers can work in many cases.
     *
     * For more on this topic, see https://lkml.org/lkml/2020/7/1/1118
     */
    uint32_t fb_id = 0;
    uint32_t flags = (modifiers[0] && modifiers[0] != DRM_FORMAT_MOD_INVALID) ? DRM_MODE_FB_MODIFIERS : 0;
    int ret = drmModeAddFB2WithModifiers(drm_fd, self->mode.hdisplay, self->mode.vdisplay, self->gbm_format, handles,
                                         strides, offsets, modifiers, &fb_id, flags);
    if (ret) {
        handles[0] = gbm_bo_get_handle(bo).u32;
        handles[1] = handles[2] = handles[3] = 0;
        strides[0] = gbm_bo_get_stride(bo);
        strides[1] = strides[2] = strides[3] = 0;
        offsets[0] = offsets[1] = offsets[2] = offsets[3] = 0;
        ret = drmModeAddFB2(drm_fd, self->mode.hdisplay, self->mode.vdisplay, self->gbm_format, handles, strides,
                            offsets, &fb_id, 0);
    }
    if (ret) {
        g_warning("%s: Cannot create framebuffer (%s)", __func__, g_strerror(errno));
        gbm_surface_release_buffer(self->gbm_surface, bo);
        return;
    }
    gbm_bo_set_user_data(bo, GINT_TO_POINTER(fb_id), NULL);

    if (G_UNLIKELY(!self->mode_set)) {
        int ret = drmModeSetCrtc(drm_fd, self->crtc_id, fb_id, 0, 0, &self->connector_id, 1, &self->mode);
        if (ret) {
            g_warning("%s: Cannot set mode (%s)", __func__, g_strerror(errno));
            return;
        }
        self->mode_set = true;
    }

    self->next_bo = bo;

    if (drmModePageFlip(drm_fd, self->crtc_id, fb_id, DRM_MODE_PAGE_FLIP_EVENT, self)) {
        g_warning("%s: Cannot schedule page flip (%s)", __func__, g_strerror(errno));
        return;
    }
}

static void
cog_drm_gles_renderer_handle_page_flip(int fd, unsigned frame, unsigned sec, unsigned usec, void *data)
{
    CogDrmGlesRenderer *self = data;

    if (self->current_bo) {
        uint32_t fb_id = GPOINTER_TO_INT(gbm_bo_get_user_data(self->current_bo));
        drmModeRmFB(gbm_device_get_fd(self->gbm_device), fb_id);
        gbm_surface_release_buffer(self->gbm_surface, self->current_bo);
    }
    self->current_bo = g_steal_pointer(&self->next_bo);

    wpe_view_backend_exportable_fdo_dispatch_frame_complete(self->exportable);
}

static bool
cog_drm_gles_renderer_initialize_shaders(CogDrmGlesRenderer *self, GError **error)
{
    static const char vertex_shader_source[] = "attribute vec2 position;\n"
                                               "attribute vec2 texture;\n"
                                               "varying vec2 v_texture;\n"
                                               "void main() {\n"
                                               "  v_texture = texture;\n"
                                               "  gl_Position = vec4(position, 0, 1);\n"
                                               "}\n";
    static const char fragment_shader_source[] = "precision mediump float;\n"
                                                 "uniform sampler2D u_texture;\n"
                                                 "varying vec2 v_texture;\n"
                                                 "void main() {\n"
                                                 "  gl_FragColor = texture2D(u_texture, v_texture);\n"
                                                 "}\n";

    g_auto(CogGLShaderId) vertex_shader = cog_gl_load_shader(vertex_shader_source, GL_VERTEX_SHADER, error);
    if (!vertex_shader)
        return false;

    g_auto(CogGLShaderId) fragment_shader = cog_gl_load_shader(fragment_shader_source, GL_FRAGMENT_SHADER, error);
    if (!fragment_shader)
        return false;

    if (!(self->gl_program = glCreateProgram())) {
        g_set_error_literal(error, COG_PLATFORM_EGL_ERROR, glGetError(), "Cannot create shader program");
        return false;
    }

    glAttachShader(self->gl_program, vertex_shader);
    glAttachShader(self->gl_program, fragment_shader);
    glBindAttribLocation(self->gl_program, 0, "position");
    glBindAttribLocation(self->gl_program, 1, "texture");

    if (!cog_gl_link_program(self->gl_program, error)) {
        glDeleteProgram(self->gl_program);
        self->gl_program = 0;
        return false;
    }

    self->gl_attrib_position = glGetAttribLocation(self->gl_program, "position");
    self->gl_attrib_texture = glGetAttribLocation(self->gl_program, "texture");
    g_assert(self->gl_attrib_position >= 0 && self->gl_attrib_texture >= 0 && self->gl_uniform_texture >= 0);

    return true;
}

static bool
cog_drm_gles_renderer_initialize_texture(CogDrmGlesRenderer *self, GError **error)
{
    glGenTextures(1, &self->gl_texture);
    glBindTexture(GL_TEXTURE_2D, self->gl_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);

    return true;
}

static gboolean
cog_drm_gles_renderer_dispatch_drm_events(int fd, GIOCondition condition, void *data)
{
    if (condition & (G_IO_ERR | G_IO_HUP)) {
        g_debug("%s: hangup/error, removing source.", __func__);
        return G_SOURCE_REMOVE;
    }
    if (condition & G_IO_IN) {
        CogDrmGlesRenderer *self = data;
        drmHandleEvent(fd, &self->drm_context);
    }
    return G_SOURCE_CONTINUE;
}

static bool
cog_drm_gles_renderer_initialize(CogDrmRenderer *renderer, GError **error)
{
    CogDrmGlesRenderer *self = wl_container_of(renderer, self, base);

    static const char *required_egl_extensions[] = {
        "EGL_KHR_image_base",
        "EGL_KHR_image",
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(required_egl_extensions); i++) {
        if (!epoxy_has_egl_extension(self->egl_display, required_egl_extensions[i])) {
            g_set_error(error, COG_PLATFORM_WPE_ERROR, COG_PLATFORM_WPE_ERROR_INIT, "EGL extension %s missing",
                        required_egl_extensions[i]);
            return false;
        }
    }

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        g_set_error_literal(error, COG_PLATFORM_EGL_ERROR, eglGetError(), "eglBindAPI");
        return false;
    }

    /* clang-format off */
    static const EGLint context_attr[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE,
    };
    static const EGLint config_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 1,
        EGL_GREEN_SIZE, 1,
        EGL_BLUE_SIZE, 1,
        EGL_ALPHA_SIZE, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SAMPLES, 0,
        EGL_NONE,
    };
    /* clang-format on */

    EGLint count = 0, matched = 0;
    if (!eglGetConfigs(self->egl_display, NULL, 0, &count) || count < 1) {
        g_set_error_literal(error, COG_PLATFORM_EGL_ERROR, eglGetError(), "eglGetConfigs");
        return false;
    }

    g_autofree EGLConfig *configs = g_new0(EGLConfig, count);
    if (!eglChooseConfig(self->egl_display, config_attr, configs, count, &matched)) {
        g_set_error_literal(error, COG_PLATFORM_EGL_ERROR, eglGetError(), "eglChooseConfig");
        return false;
    }
    if (!matched) {
        g_set_error_literal(error, COG_PLATFORM_WPE_ERROR, COG_PLATFORM_WPE_ERROR_INIT, "No suitable EGLConfig found");
        return false;
    }

    /*
     * Make sure to pick an EGL configuration with a pixel format compatible
     * with the chosen output plane, otherwise page-flipping to the BO backing
     * the surface will fail.
     */
    g_autoptr(drmModePlane) plane = drmModeGetPlane(gbm_device_get_fd(self->gbm_device), self->plane_id);
    if (!plane) {
        g_set_error(error, COG_PLATFORM_WPE_ERROR, COG_PLATFORM_WPE_ERROR_INIT,
                    "Cannot get information for DRM/KMS plane #%" PRIu32, self->plane_id);
        return false;
    }

    bool config_found = false;
    for (EGLint i = 0; !config_found && i < matched; i++) {
        EGLint gbm_format;
        if (!eglGetConfigAttrib(self->egl_display, configs[i], EGL_NATIVE_VISUAL_ID, &gbm_format)) {
            g_set_error(error, COG_PLATFORM_EGL_ERROR, eglGetError(), "Cannot get GBM format for config #%d", i);
            return false;
        }

        for (uint32_t j = 0; j < plane->count_formats; j++) {
            if (plane->formats[j] == gbm_format) {
                self->egl_config = configs[i];
                self->gbm_format = gbm_format;
                config_found = true;
                g_debug("%s: Using config #%d with format '%c%c%c%c'", __func__, i, (gbm_format >> 0) & 0xFF,
                        (gbm_format >> 8) & 0xFF, (gbm_format >> 16) & 0xFF, (gbm_format >> 24) & 0xFF);
                break;
            }
        }
    }
    if (!config_found) {
        g_set_error(error, COG_PLATFORM_WPE_ERROR, COG_PLATFORM_WPE_ERROR_INIT,
                    "Cannot find an EGL configuration with a pixel format compatible with plane #%" PRIu32,
                    self->plane_id);
        return false;
    }

    self->egl_context = eglCreateContext(self->egl_display, self->egl_config, EGL_NO_CONTEXT, context_attr);
    if (self->egl_context == EGL_NO_CONTEXT) {
        g_set_error_literal(error, COG_PLATFORM_EGL_ERROR, eglGetError(), "eglCreateContext");
        return false;
    }

    if (!(self->gbm_surface = gbm_surface_create(self->gbm_device, self->mode.hdisplay, self->mode.vdisplay,
                                                 self->gbm_format, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING))) {
        g_set_error(error, COG_PLATFORM_WPE_ERROR, COG_PLATFORM_WPE_ERROR_INIT,
                    "Cannot create GBM surface for output rendering (%s)", g_strerror(errno));
        return false;
    }

    if (epoxy_has_egl_extension(self->egl_display, "EGL_MESA_platform_gbm")) {
        self->egl_surface =
            eglCreatePlatformWindowSurfaceEXT(self->egl_display, self->egl_config, self->gbm_surface, NULL);
    } else {
        self->egl_surface =
            eglCreateWindowSurface(self->egl_display, self->egl_config, (EGLNativeWindowType) self->gbm_surface, NULL);
    }
    if (!self->egl_surface) {
        g_set_error(error, COG_PLATFORM_EGL_ERROR, eglGetError(), "Cannot create EGL window surface");
        return false;
    }

    /* An active context is needed in order to check for GL extensions. */
    if (!eglMakeCurrent(self->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, self->egl_context)) {
        g_set_error_literal(error, COG_PLATFORM_EGL_ERROR, eglGetError(),
                            "Could not activate EGL context for shader compilation");
        return false;
    }

    static const char *required_gl_extensions[] = {
        "GL_OES_EGL_image",
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(required_gl_extensions); i++) {
        if (!epoxy_has_gl_extension(required_gl_extensions[i])) {
            g_set_error(error, COG_PLATFORM_WPE_ERROR, COG_PLATFORM_WPE_ERROR_INIT, "GL extension %s missing",
                        required_gl_extensions[i]);
            return false;
        }
    }

    bool ok =
        cog_drm_gles_renderer_initialize_shaders(self, error) && cog_drm_gles_renderer_initialize_texture(self, error);

    eglMakeCurrent(self->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    self->drm_fd_source =
        g_unix_fd_add(gbm_device_get_fd(self->gbm_device), G_IO_IN, cog_drm_gles_renderer_dispatch_drm_events, self);

    return ok;
}

static void
cog_drm_gles_renderer_destroy(CogDrmRenderer *renderer)
{
    CogDrmGlesRenderer *self = wl_container_of(renderer, self, base);

    g_clear_handle_id(&self->drm_fd_source, g_source_remove);

    if (self->egl_surface != EGL_NO_SURFACE) {
        eglDestroySurface(self->egl_display, self->egl_surface);
        self->egl_surface = EGL_NO_SURFACE;
    }

    g_clear_pointer(&self->gbm_surface, gbm_surface_destroy);

    if (self->egl_context) {
        eglDestroyContext(self->egl_display, self->egl_context);
        self->egl_context = EGL_NO_CONTEXT;
    }
}

static void
cog_drm_gles_renderer_transformed_logical_size(const CogDrmGlesRenderer *self, uint32_t *width, uint32_t *height)
{
    switch (self->rotation) {
    case COG_DRM_RENDERER_ROTATION_0:
    case COG_DRM_RENDERER_ROTATION_180:
        *width = self->width;
        *height = self->height;
        break;
    case COG_DRM_RENDERER_ROTATION_90:
    case COG_DRM_RENDERER_ROTATION_270:
        *width = self->height;
        *height = self->width;
        break;
    default:
        g_assert_not_reached();
    }
}

static bool
cog_drm_gles_renderer_set_rotation(CogDrmRenderer *renderer, CogDrmRendererRotation rotation, bool apply)
{
    const bool supported = (rotation == COG_DRM_RENDERER_ROTATION_0 || rotation == COG_DRM_RENDERER_ROTATION_90 ||
                            rotation == COG_DRM_RENDERER_ROTATION_180 || rotation == COG_DRM_RENDERER_ROTATION_270);

    if (!apply)
        return supported;

    CogDrmGlesRenderer *self = wl_container_of(renderer, self, base);
    if (self->rotation == rotation)
        return true;

    self->rotation = rotation;

    if (self->exportable) {
        uint32_t width, height;
        cog_drm_gles_renderer_transformed_logical_size(self, &width, &height);
        wpe_view_backend_dispatch_set_size(wpe_view_backend_exportable_fdo_get_view_backend(self->exportable), width,
                                           height);
    }
    return true;
}

static struct wpe_view_backend_exportable_fdo *
cog_drm_gles_renderer_create_exportable(CogDrmRenderer *renderer, uint32_t width, uint32_t height)
{
    CogDrmGlesRenderer *self = wl_container_of(renderer, self, base);

    self->width = width;
    self->height = height;
    cog_drm_gles_renderer_transformed_logical_size(self, &width, &height);

    static const struct wpe_view_backend_exportable_fdo_egl_client client = {
        .export_fdo_egl_image = cog_drm_gles_renderer_handle_egl_image,
    };
    return (self->exportable = wpe_view_backend_exportable_fdo_egl_create(&client, renderer, width, height));
}

CogDrmRenderer *
cog_drm_gles_renderer_new(struct gbm_device     *gbm_device,
                          EGLDisplay             egl_display,
                          uint32_t               plane_id,
                          uint32_t               crtc_id,
                          uint32_t               connector_id,
                          const drmModeModeInfo *mode,
                          bool                   atomic_modesetting)
{
    g_assert(gbm_device);
    g_assert(egl_display != EGL_NO_DISPLAY);

    CogDrmGlesRenderer *self = g_slice_new(CogDrmGlesRenderer);
    *self = (CogDrmGlesRenderer){
        .base.name = "gles",
        .base.initialize = cog_drm_gles_renderer_initialize,
        .base.destroy = cog_drm_gles_renderer_destroy,
        .base.set_rotation = cog_drm_gles_renderer_set_rotation,
        .base.create_exportable = cog_drm_gles_renderer_create_exportable,

        .rotation = COG_DRM_RENDERER_ROTATION_0,

        .gbm_device = gbm_device,
        .egl_display = egl_display,
        .egl_context = EGL_NO_CONTEXT,
        .egl_surface = EGL_NO_SURFACE,

        .drm_context.version = DRM_EVENT_CONTEXT_VERSION,
        .drm_context.page_flip_handler = cog_drm_gles_renderer_handle_page_flip,

        .crtc_id = crtc_id,
        .connector_id = connector_id,
        .plane_id = plane_id,
        .atomic_modesetting = atomic_modesetting,
    };

    memcpy(&self->mode, mode, sizeof(drmModeModeInfo));

    int drm_fd = gbm_device_get_fd(gbm_device);

    self->connector_props.props = drmModeObjectGetProperties(drm_fd, self->connector_id, DRM_MODE_OBJECT_CONNECTOR);
    if (self->connector_props.props) {
        self->connector_props.props_info = g_new0(drmModePropertyRes *, self->connector_props.props->count_props);
        for (uint32_t i = 0; i < self->connector_props.props->count_props; i++)
            self->connector_props.props_info[i] = drmModeGetProperty(drm_fd, self->connector_props.props->props[i]);
    }

    self->crtc_props.props = drmModeObjectGetProperties(drm_fd, self->crtc_id, DRM_MODE_OBJECT_CRTC);
    if (self->crtc_props.props) {
        self->crtc_props.props_info = g_new0(drmModePropertyRes *, self->crtc_props.props->count_props);
        for (uint32_t i = 0; i < self->crtc_props.props->count_props; i++)
            self->crtc_props.props_info[i] = drmModeGetProperty(drm_fd, self->crtc_props.props->props[i]);
    }

    self->plane_props.props = drmModeObjectGetProperties(drm_fd, self->plane_id, DRM_MODE_OBJECT_PLANE);
    if (self->plane_props.props) {
        self->plane_props.props_info = g_new0(drmModePropertyRes *, self->plane_props.props->count_props);
        for (uint32_t i = 0; i < self->plane_props.props->count_props; i++)
            self->plane_props.props_info[i] = drmModeGetProperty(drm_fd, self->plane_props.props->props[i]);
    }

    g_debug("%s: Using plane #%" PRIu32 ", crtc #%" PRIu32 ", connector #%" PRIu32 " (%s).", __func__, plane_id,
            crtc_id, connector_id, atomic_modesetting ? "atomic" : "legacy");

    return &self->base;
}
