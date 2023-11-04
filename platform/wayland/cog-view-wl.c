/*
 * cog-view-wl.c
 * Copyright (C) 2023 Pablo Saavedra <psaavedra@igalia.com>
 * Copyright (C) 2023 Adrian Perez de Castro <aperez@igalia.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../core/cog.h"

#include <wpe/fdo-egl.h>
#include <wpe/fdo.h>

#include "../common/cursors.h"
#include "../common/egl-proc-address.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>

/* for mmap */
#include <sys/mman.h>

#ifdef COG_USE_WAYLAND_CURSOR
#    include <wayland-cursor.h>
#endif

#include "presentation-time-client.h"
#include "xdg-shell-client.h"

#if COG_HAVE_LIBPORTAL
#    include "../common/cog-file-chooser.h"
#endif /* COG_HAVE_LIBPORTAL */

#include "cog-im-context-wl-v1.h"
#include "cog-im-context-wl.h"
#include "cog-utils-wl.h"
#include "cog-view-wl.h"
#include "cog-viewport-wl.h"
#if COG_HAVE_LIBPORTAL
#    include "cog-xdp-parent-wl.h"
#endif /* COG_HAVE_LIBPORTAL */

#include "os-compatibility.h"

G_DEFINE_DYNAMIC_TYPE(CogWlView, cog_wl_view, COG_TYPE_VIEW)

static void                  cog_wl_view_clear_buffers(CogWlView *);
static WebKitWebViewBackend *cog_wl_view_create_backend(CogView *);
static void                  cog_wl_view_dispose(GObject *);
#if HAVE_FULLSCREEN_HANDLING
static bool cog_wl_view_handle_dom_fullscreen_request(void *, bool);
#endif
static void cog_wl_view_shm_buffer_destroy(CogWlView *, struct shm_buffer *);

static void presentation_feedback_on_discarded(void *, struct wp_presentation_feedback *);
static void presentation_feedback_on_presented(void *,
                                               struct wp_presentation_feedback *,
                                               uint32_t,
                                               uint32_t,
                                               uint32_t,
                                               uint32_t,
                                               uint32_t,
                                               uint32_t,
                                               uint32_t);
static void presentation_feedback_on_sync_output(void *, struct wp_presentation_feedback *, struct wl_output *);

#if HAVE_SHM_EXPORTED_BUFFER
static void on_export_shm_buffer(void *, struct wpe_fdo_shm_exported_buffer *);
#endif
static void on_export_wl_egl_image(void *data, struct wpe_fdo_egl_exported_image *image);
#if COG_USE_WAYLAND_CURSOR
static void on_mouse_target_changed(WebKitWebView *, WebKitHitTestResult *hitTestResult, guint mouseModifiers);
#endif
#if COG_HAVE_LIBPORTAL
static void on_run_file_chooser(WebKitWebView *, WebKitFileChooserRequest *);
#endif
static void on_show_option_menu(WebKitWebView *, WebKitOptionMenu *, WebKitRectangle *, gpointer *);
static void on_wl_surface_frame(void *, struct wl_callback *, uint32_t);

#if HAVE_SHM_EXPORTED_BUFFER
static void               shm_buffer_copy_contents(struct shm_buffer *, struct wl_shm_buffer *);
static struct shm_buffer *shm_buffer_create(CogWlView *, struct wl_resource *, size_t);
static void               shm_buffer_destroy_notify(struct wl_listener *, void *);
static struct shm_buffer *shm_buffer_for_resource(CogWlView *, struct wl_resource *);
static void               shm_buffer_on_release(void *, struct wl_buffer *);
#endif

/*
 * CogWlView instantiation.
 */

static void
cog_wl_view_class_init(CogWlViewClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = cog_wl_view_dispose;

    CogViewClass *view_class = COG_VIEW_CLASS(klass);
    view_class->create_backend = cog_wl_view_create_backend;
}

static void
cog_wl_view_init(CogWlView *self)
{
    /* Always configure the opaque region on first display after creation. */
    self->should_update_opaque_region = true;

    self->scale_factor = 1;
    self->should_update_opaque_region = true;
    self->image = NULL;
    self->frame_callback = NULL;

    wl_list_init(&self->shm_buffer_list);

    g_signal_connect(self, "mouse-target-changed", G_CALLBACK(on_mouse_target_changed), NULL);
#if COG_HAVE_LIBPORTAL
    g_signal_connect(self, "run-file-chooser", G_CALLBACK(on_run_file_chooser), NULL);
#endif /* COG_HAVE_LIBPORTAL */
    g_signal_connect(self, "show-option-menu", G_CALLBACK(on_show_option_menu), NULL);
}

/*
 * CogWlView deinstantiation.
 */

static void
cog_wl_view_class_finalize(CogWlViewClass *klass G_GNUC_UNUSED)
{
}

static void
cog_wl_view_dispose(GObject *object)
{
    CogWlView *self = COG_WL_VIEW(object);

    g_clear_pointer(&self->frame_callback, wl_callback_destroy);

    if (self->image) {
        g_assert(self->exportable);
        wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(self->exportable, self->image);
        self->image = NULL;
    }

    cog_wl_view_clear_buffers(self);

    G_OBJECT_CLASS(cog_wl_view_parent_class)->dispose(object);
}

/*
 * Method definitions.
 */

static inline bool
cog_wl_view_background_has_alpha(CogWlView *view)
{
    WebKitColor bg_color;
    webkit_web_view_get_background_color(WEBKIT_WEB_VIEW(view), &bg_color);
    return bg_color.alpha != .0f;
}

static void
cog_wl_view_clear_buffers(CogWlView *view)
{
#if HAVE_SHM_EXPORTED_BUFFER
    struct shm_buffer *buffer, *tmp;
    wl_list_for_each_safe(buffer, tmp, &view->shm_buffer_list, link) {

        wl_list_remove(&buffer->link);
        wl_list_remove(&buffer->destroy_listener.link);

        cog_wl_view_shm_buffer_destroy(view, buffer);
    }
    wl_list_init(&view->shm_buffer_list);
#endif
}

static WebKitWebViewBackend *
cog_wl_view_create_backend(CogView *view)
{
    CogWlPlatform *platform = (CogWlPlatform *) cog_platform_get();
    CogWlViewport *viewport = COG_WL_VIEWPORT(platform->viewport);
    CogWlView     *self = COG_WL_VIEW(view);

    static const struct wpe_view_backend_exportable_fdo_egl_client client = {
        .export_fdo_egl_image = on_export_wl_egl_image,
#if HAVE_SHM_EXPORTED_BUFFER
        .export_shm_buffer = on_export_shm_buffer,
#endif
    };

    self->exportable =
        wpe_view_backend_exportable_fdo_egl_create(&client, self, viewport->window.width, viewport->window.height);

    /* init WPE view backend */
    struct wpe_view_backend *view_backend = wpe_view_backend_exportable_fdo_get_view_backend(self->exportable);
    g_assert(view_backend);

    if (platform->display->text_input_manager_v1)
        cog_im_context_wl_v1_set_view_backend(view_backend);

    WebKitWebViewBackend *wk_view_backend =
        webkit_web_view_backend_new(view_backend,
                                    (GDestroyNotify) wpe_view_backend_exportable_fdo_destroy,
                                    self->exportable);
    g_assert(wk_view_backend);

#if HAVE_FULLSCREEN_HANDLING
    wpe_view_backend_set_fullscreen_handler(view_backend, cog_wl_view_handle_dom_fullscreen_request, self);
#endif

    return wk_view_backend;
}

bool
cog_wl_view_does_image_match_win_size(CogWlView *view)
{
    CogWlPlatform *platform = (CogWlPlatform *) cog_platform_get();
    CogWlViewport *viewport = COG_WL_VIEWPORT(platform->viewport);
    return view->image && wpe_fdo_egl_exported_image_get_width(view->image) == viewport->window.width &&
           wpe_fdo_egl_exported_image_get_height(view->image) == viewport->window.height;
}

void
cog_wl_view_enter_fullscreen(CogWlView *view)
{
    view->is_resizing_fullscreen = true;
    if (!cog_wl_view_does_image_match_win_size(view))
        return;

#if HAVE_FULLSCREEN_HANDLING
    CogWlPlatform *platform = (CogWlPlatform *) cog_platform_get();
    CogWlViewport *viewport = COG_WL_VIEWPORT(platform->viewport);
    if (viewport->window.was_fullscreen_requested_from_dom)
        wpe_view_backend_dispatch_did_enter_fullscreen(cog_view_get_backend(COG_VIEW(view)));
#endif

    view->is_resizing_fullscreen = false;
}

void
cog_wl_view_exit_fullscreen(CogWlView *view)
{
#if HAVE_FULLSCREEN_HANDLING
    struct wpe_view_backend *backend = cog_view_get_backend(COG_VIEW(view));
    wpe_view_backend_dispatch_did_exit_fullscreen(backend);
#endif
}

#if HAVE_FULLSCREEN_HANDLING
static bool
cog_wl_view_handle_dom_fullscreen_request(void *data, bool fullscreen)
{
    CogWlPlatform *platform = (CogWlPlatform *) cog_platform_get();
    CogWlViewport *viewport = COG_WL_VIEWPORT(platform->viewport);

    viewport->window.was_fullscreen_requested_from_dom = true;
    if (fullscreen != viewport->window.is_fullscreen)
        return cog_wl_viewport_set_fullscreen(viewport, fullscreen);

    // Handle situations where DOM fullscreen requests are mixed with system fullscreen commands (e.g F11)
    struct wpe_view_backend *backend = cog_view_get_backend(COG_VIEW(data));
    if (fullscreen)
        wpe_view_backend_dispatch_did_enter_fullscreen(backend);
    else
        wpe_view_backend_dispatch_did_exit_fullscreen(backend);

    return true;
}
#endif

static void
cog_wl_view_on_buffer_release(void *data G_GNUC_UNUSED, struct wl_buffer *buffer)
{
    wl_buffer_destroy(buffer);
}

static void
cog_wl_view_request_frame(CogWlView *view)
{
    CogWlPlatform *platform = (CogWlPlatform *) cog_platform_get();
    CogWlViewport *viewport = COG_WL_VIEWPORT(platform->viewport);

    if (!view->frame_callback) {
        static const struct wl_callback_listener listener = {.done = on_wl_surface_frame};
        view->frame_callback = wl_surface_frame(viewport->window.wl_surface);
        wl_callback_add_listener(view->frame_callback, &listener, view);
    }

    if (platform->display->presentation != NULL) {
        static const struct wp_presentation_feedback_listener presentation_feedback_listener = {
            .sync_output = presentation_feedback_on_sync_output,
            .presented = presentation_feedback_on_presented,
            .discarded = presentation_feedback_on_discarded};
        struct wp_presentation_feedback *presentation_feedback =
            wp_presentation_feedback(platform->display->presentation, viewport->window.wl_surface);
        wp_presentation_feedback_add_listener(presentation_feedback, &presentation_feedback_listener, NULL);
    }
}

void
cog_wl_view_resize(CogWlView *view)
{
    CogWlPlatform *platform = (CogWlPlatform *) cog_platform_get();
    CogWlViewport *viewport = COG_WL_VIEWPORT(platform->viewport);

    view->should_update_opaque_region = true;

    int32_t pixel_width = viewport->window.width * platform->display->current_output->scale;
    int32_t pixel_height = viewport->window.height * platform->display->current_output->scale;

    struct wpe_view_backend *backend = cog_view_get_backend(COG_VIEW(view));
    wpe_view_backend_dispatch_set_size(backend, viewport->window.width, viewport->window.height);
    wpe_view_backend_dispatch_set_device_scale_factor(backend, platform->display->current_output->scale);

    g_debug("Resized EGL buffer to: (%" PRIi32 ", %" PRIi32 ") @%" PRIi32 "x", pixel_width, pixel_height,
            platform->display->current_output->scale);
}

void
cog_wl_view_update_surface_contents(CogWlView *view)
{
    CogWlPlatform *platform = (CogWlPlatform *) cog_platform_get();
    CogWlViewport *viewport = COG_WL_VIEWPORT(platform->viewport);
    g_assert(view);

    struct wl_surface *surface = viewport->window.wl_surface;
    g_assert(surface);

    const uint32_t surface_pixel_width = platform->display->current_output->scale * viewport->window.width;
    const uint32_t surface_pixel_height = platform->display->current_output->scale * viewport->window.height;

    if (view->should_update_opaque_region) {
        view->should_update_opaque_region = false;

        WebKitColor bg_color;
        webkit_web_view_get_background_color(WEBKIT_WEB_VIEW(view), &bg_color);

        if (viewport->window.is_fullscreen || !cog_wl_view_background_has_alpha(COG_WL_VIEW(view))) {
            struct wl_region *region = wl_compositor_create_region(platform->display->compositor);
            wl_region_add(region, 0, 0, viewport->window.width, viewport->window.height);
            wl_surface_set_opaque_region(surface, region);
            wl_region_destroy(region);
        } else {
            wl_surface_set_opaque_region(surface, NULL);
        }
    }

    static PFNEGLCREATEWAYLANDBUFFERFROMIMAGEWL s_eglCreateWaylandBufferFromImageWL;
    if (G_UNLIKELY(s_eglCreateWaylandBufferFromImageWL == NULL)) {
        s_eglCreateWaylandBufferFromImageWL =
            (PFNEGLCREATEWAYLANDBUFFERFROMIMAGEWL) load_egl_proc_address("eglCreateWaylandBufferFromImageWL");
        g_assert(s_eglCreateWaylandBufferFromImageWL);
    }

    struct wl_buffer *buffer = s_eglCreateWaylandBufferFromImageWL(
        platform->display->egl_display, wpe_fdo_egl_exported_image_get_egl_image(view->image));
    g_assert(buffer);

    static const struct wl_buffer_listener buffer_listener = {.release = cog_wl_view_on_buffer_release};
    wl_buffer_add_listener(buffer, &buffer_listener, NULL);

    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, surface_pixel_width, surface_pixel_height);

    cog_wl_view_request_frame(view);

    wl_surface_commit(surface);

    if (view->is_resizing_fullscreen)
        cog_wl_view_enter_fullscreen(view);
}

static bool
validate_exported_geometry(uint32_t width, uint32_t height)
{
    CogWlPlatform *platform = (CogWlPlatform *) cog_platform_get();
    CogWlViewport *viewport = COG_WL_VIEWPORT(platform->viewport);

    const uint32_t surface_pixel_width = platform->display->current_output->scale * viewport->window.width;
    const uint32_t surface_pixel_height = platform->display->current_output->scale * viewport->window.height;

    if (surface_pixel_width != width || surface_pixel_height != height) {
        g_debug("Image geometry %" PRIu32 "x%" PRIu32 ", does not match surface geometry %" PRIu32 "x%" PRIu32
                ", skipping.",
                width, height, surface_pixel_width, surface_pixel_height);
        return false;
    }

    return true;
}

#if HAVE_SHM_EXPORTED_BUFFER
static void
on_export_shm_buffer(void *data, struct wpe_fdo_shm_exported_buffer *exported_buffer)
{
    CogWlPlatform *platform = (CogWlPlatform *) cog_platform_get();
    CogWlViewport *viewport = COG_WL_VIEWPORT(platform->viewport);
    CogWlView     *view = data;

    struct wl_resource   *exported_resource = wpe_fdo_shm_exported_buffer_get_resource(exported_buffer);
    struct wl_shm_buffer *exported_shm_buffer = wpe_fdo_shm_exported_buffer_get_shm_buffer(exported_buffer);

    uint32_t image_width = wl_shm_buffer_get_width(exported_shm_buffer);
    uint32_t image_height = wl_shm_buffer_get_height(exported_shm_buffer);
    if (!validate_exported_geometry(image_width, image_height)) {
        wpe_view_backend_exportable_fdo_dispatch_frame_complete(view->exportable);
        wpe_view_backend_exportable_fdo_egl_dispatch_release_shm_exported_buffer(view->exportable, exported_buffer);
        return;
    }

    struct shm_buffer *buffer = shm_buffer_for_resource(view, exported_resource);
    if (!buffer) {
        int32_t width;
        int32_t height;
        if (viewport->window.is_fullscreen) {
            width = viewport->window.width;
            height = viewport->window.height;
        } else {
            width = wl_shm_buffer_get_width(exported_shm_buffer);
            height = wl_shm_buffer_get_height(exported_shm_buffer);
        }
        int32_t  stride = wl_shm_buffer_get_stride(exported_shm_buffer);
        uint32_t format = wl_shm_buffer_get_format(exported_shm_buffer);

        size_t size = stride * height;
        buffer = shm_buffer_create(view, exported_resource, size);
        if (!buffer)
            return;
        wl_list_insert(&view->shm_buffer_list, &buffer->link);

        buffer->buffer = wl_shm_pool_create_buffer(buffer->shm_pool, 0, width, height, stride, format);

        static const struct wl_buffer_listener shm_buffer_listener = {
            .release = shm_buffer_on_release,
        };
        wl_buffer_add_listener(buffer->buffer, &shm_buffer_listener, buffer);
    }

    buffer->exported_buffer = exported_buffer;
    shm_buffer_copy_contents(buffer, exported_shm_buffer);

    const int32_t state = wpe_view_backend_get_activity_state(cog_view_get_backend((CogView *) view));
    if (state & wpe_view_activity_state_visible) {
        wl_surface_attach(viewport->window.wl_surface, buffer->buffer, 0, 0);
        wl_surface_damage(viewport->window.wl_surface, 0, 0, INT32_MAX, INT32_MAX);
        cog_wl_view_request_frame(view);
        wl_surface_commit(viewport->window.wl_surface);
    }
}
#endif

static void
on_export_wl_egl_image(void *data, struct wpe_fdo_egl_exported_image *image)
{
    CogWlView *self = data;

    uint32_t image_width = wpe_fdo_egl_exported_image_get_width(image);
    uint32_t image_height = wpe_fdo_egl_exported_image_get_height(image);
    if (!validate_exported_geometry(image_width, image_height)) {
        wpe_view_backend_exportable_fdo_dispatch_frame_complete(self->exportable);
        wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(self->exportable, image);
        return;
    }

    if (self->image)
        wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(self->exportable, self->image);

    self->image = image;

    const int32_t state = wpe_view_backend_get_activity_state(cog_view_get_backend((CogView *) self));
    if (state & wpe_view_activity_state_visible)
        cog_wl_view_update_surface_contents(self);
}

static void
on_mouse_target_changed(WebKitWebView *view, WebKitHitTestResult *hitTestResult, guint mouseModifiers)
{
#ifdef COG_USE_WAYLAND_CURSOR
    CogWlPlatform *platform = (CogWlPlatform *) cog_platform_get();
    if (webkit_hit_test_result_context_is_link(hitTestResult)) {
        cog_wl_seat_set_cursor(platform->display->seat_default, CURSOR_HAND);
    } else if (webkit_hit_test_result_context_is_editable(hitTestResult)) {
        cog_wl_seat_set_cursor(platform->display->seat_default, CURSOR_TEXT);
    } else if (webkit_hit_test_result_context_is_selection(hitTestResult)) {
        cog_wl_seat_set_cursor(platform->display->seat_default, CURSOR_TEXT);
    } else {
        cog_wl_seat_set_cursor(platform->display->seat_default, CURSOR_LEFT_PTR);
    }
#endif /* COG_USE_WAYLAND_CURSOR */
}

#if COG_HAVE_LIBPORTAL
static void
on_run_file_chooser(WebKitWebView *view, WebKitFileChooserRequest *request)
{
    CogWlPlatform *platform = (CogWlPlatform *) cog_platform_get();
    CogWlViewport *viewport = COG_WL_VIEWPORT(platform->viewport);

    g_autoptr(XdpParent) xdp_parent = NULL;
    if (viewport->window.xdp_parent_wl_data.zxdg_exporter && viewport->window.xdp_parent_wl_data.wl_surface) {
        xdp_parent = xdp_parent_new_wl(&viewport->window.xdp_parent_wl_data);
    }

    run_file_chooser(view, request, xdp_parent);
}
#endif /* COG_HAVE_LIBPORTAL */

static void
on_show_option_menu(WebKitWebView *view, WebKitOptionMenu *menu, WebKitRectangle *rectangle, gpointer *data)
{
    CogWlPlatform *platform = (CogWlPlatform *) cog_platform_get();
    cog_wl_platform_popup_create(platform, g_object_ref(menu));
}

static void
on_wl_surface_frame(void *data, struct wl_callback *callback, uint32_t time)
{
    CogWlView *view = data;

    if (view->frame_callback) {
        g_assert(view->frame_callback == callback);
        g_clear_pointer(&view->frame_callback, wl_callback_destroy);
    }

    wpe_view_backend_exportable_fdo_dispatch_frame_complete(view->exportable);
}

static void
presentation_feedback_on_discarded(void *data, struct wp_presentation_feedback *presentation_feedback)
{
    wp_presentation_feedback_destroy(presentation_feedback);
}

static void
presentation_feedback_on_presented(void                            *data,
                                   struct wp_presentation_feedback *presentation_feedback,
                                   uint32_t                         tv_sec_hi,
                                   uint32_t                         tv_sec_lo,
                                   uint32_t                         tv_nsec,
                                   uint32_t                         refresh,
                                   uint32_t                         seq_hi,
                                   uint32_t                         seq_lo,
                                   uint32_t                         flags)
{
    wp_presentation_feedback_destroy(presentation_feedback);
}

static void
presentation_feedback_on_sync_output(void                            *data,
                                     struct wp_presentation_feedback *presentation_feedback,
                                     struct wl_output                *output)
{
}

#if HAVE_SHM_EXPORTED_BUFFER
static void
shm_buffer_copy_contents(struct shm_buffer *buffer, struct wl_shm_buffer *exported_shm_buffer)
{
    int32_t height = wl_shm_buffer_get_height(exported_shm_buffer);
    int32_t stride = wl_shm_buffer_get_stride(exported_shm_buffer);

    size_t data_size = height * stride;

    wl_shm_buffer_begin_access(exported_shm_buffer);
    void *exported_data = wl_shm_buffer_get_data(exported_shm_buffer);

    memcpy(buffer->data, exported_data, data_size);

    wl_shm_buffer_end_access(exported_shm_buffer);
}

static struct shm_buffer *
shm_buffer_create(CogWlView *view, struct wl_resource *buffer_resource, size_t size)
{
    int fd = os_create_anonymous_file(size);
    if (fd < 0)
        return NULL;

    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return NULL;
    }

    struct shm_buffer *buffer = g_new0(struct shm_buffer, 1);
    buffer->user_data = view;
    buffer->destroy_listener.notify = shm_buffer_destroy_notify;
    buffer->buffer_resource = buffer_resource;
    wl_resource_add_destroy_listener(buffer_resource, &buffer->destroy_listener);

    CogWlPlatform *platform = (CogWlPlatform *) cog_platform_get();
    buffer->shm_pool = wl_shm_create_pool(platform->display->shm, fd, size);
    buffer->data = data;
    buffer->size = size;

    close(fd);
    return buffer;
}

void
cog_wl_view_shm_buffer_destroy(CogWlView *view, struct shm_buffer *buffer)
{
    if (buffer->exported_buffer) {
        wpe_view_backend_exportable_fdo_egl_dispatch_release_shm_exported_buffer(view->exportable,
                                                                                 buffer->exported_buffer);
    }

    wl_buffer_destroy(buffer->buffer);
    wl_shm_pool_destroy(buffer->shm_pool);
    munmap(buffer->data, buffer->size);

    g_free(buffer);
}

static void
shm_buffer_destroy_notify(struct wl_listener *listener, void *data)
{
    struct shm_buffer *buffer = wl_container_of(listener, buffer, destroy_listener);

    wl_list_remove(&buffer->link);
    CogWlView *view = COG_WL_VIEW(buffer->user_data);
    cog_wl_view_shm_buffer_destroy(view, buffer);
}

static struct shm_buffer *
shm_buffer_for_resource(CogWlView *view, struct wl_resource *buffer_resource)
{
    struct shm_buffer *buffer;
    wl_list_for_each(buffer, &view->shm_buffer_list, link) {
        if (buffer->buffer_resource == buffer_resource)
            return buffer;
    }

    return NULL;
}

static void
shm_buffer_on_release(void *data, struct wl_buffer *wl_buffer)
{
    struct shm_buffer *buffer = data;
    if (buffer->exported_buffer) {
        wpe_view_backend_exportable_fdo_egl_dispatch_release_shm_exported_buffer(
            COG_WL_VIEW(buffer->user_data)->exportable, buffer->exported_buffer);
        buffer->exported_buffer = NULL;
    }
}
#endif

/*
 * CogWlView register type method.
 */

void
cog_wl_view_register_type_exported(GTypeModule *type_module)
{
    cog_wl_view_register_type(type_module);
}
