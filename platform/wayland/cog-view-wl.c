/*
 * cog-utils-wl.c
 * Copyright (C) 2023 Pablo Saavedra <psaavedra@igalia.com>
 * Copyright (C) 2023 Adrian Perez de Castro <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "../../core/cog.h"

#include <wpe/fdo-egl.h>
#include <wpe/fdo.h>

#include "../common/egl-proc-address.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "presentation-time-client.h"
#include "xdg-shell-client.h"

#include "cog-view-wl.h"
#include "cog-window-wl.h"

#if defined(WPE_FDO_CHECK_VERSION) && 0
#    define HAVE_SHM_EXPORTED_BUFFER WPE_FDO_CHECK_VERSION(1, 9, 0)
#    define HAVE_FULLSCREEN_HANDLING WPE_FDO_CHECK_VERSION(1, 11, 1)
#else
#    define HAVE_SHM_EXPORTED_BUFFER 0
#    define HAVE_FULLSCREEN_HANDLING 0
#endif

G_DEFINE_DYNAMIC_TYPE(CogWlView, cog_wl_view, COG_TYPE_VIEW)

static WebKitWebViewBackend *cog_wl_view_create_backend(CogView *);
static void                  cog_wl_view_dispose(GObject *);
#if HAVE_FULLSCREEN_HANDLING
static bool cog_wl_view_handle_dom_fullscreen_request(void *, bool);
#endif

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
static void on_show_option_menu(WebKitWebView *, WebKitOptionMenu *, WebKitRectangle *, gpointer *);
static void on_wl_surface_frame(void *, struct wl_callback *, uint32_t);

static void shell_popup_surface_on_configure(void *, struct wl_shell_surface *, uint32_t, int32_t, int32_t);
static void shell_popup_surface_on_ping(void *, struct wl_shell_surface *, uint32_t);
static void shell_popup_surface_on_popup_done(void *, struct wl_shell_surface *);

#if HAVE_SHM_EXPORTED_BUFFER
static struct shm_buffer *shm_buffer_create(CogWlDisplay *, struct wl_resource *, size_t);
static void               shm_buffer_destroy_notify(struct wl_listener *, void *);
static struct shm_buffer *shm_buffer_for_resource(CogWlDisplay *, struct wl_resource *);
#endif

static void xdg_surface_on_configure(void *, struct xdg_surface *, uint32_t);

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

    G_OBJECT_CLASS(cog_wl_view_parent_class)->dispose(object);
}

/*
 * CogWlView register type method.
 */

void
cog_wl_view_register_type_exported(GTypeModule *type_module)
{
    cog_wl_view_register_type(type_module);
}

/*
 * Method definitions.
 */

CogWlWindow *
cog_wl_view_get_window(CogWlView *view)
{
    g_assert(view);
    CogViewStack *stack = view->stack;
    if (!view->platform || !view->platform->windows)
        return NULL;
    return (CogWlWindow *) g_hash_table_lookup(view->platform->windows, stack);
}

static inline bool
cog_wl_view_background_has_alpha(CogView *view)
{
    WebKitColor bg_color;
    webkit_web_view_get_background_color((WebKitWebView *) view, &bg_color);
    return bg_color.alpha != .0f;
}

static WebKitWebViewBackend *
cog_wl_view_create_backend(CogView *view)
{
    CogWlView *self = COG_WL_VIEW(view);

    static const struct wpe_view_backend_exportable_fdo_egl_client client = {
        .export_fdo_egl_image = on_export_wl_egl_image,
#if HAVE_SHM_EXPORTED_BUFFER
        .export_shm_buffer = on_export_shm_buffer,
#endif
    };

    self->exportable =
        wpe_view_backend_exportable_fdo_egl_create(&client, self, COG_WIN_DEFAULT_WIDTH, COG_WIN_DEFAULT_HEIGHT);

    struct wpe_view_backend *view_backend = wpe_view_backend_exportable_fdo_get_view_backend(self->exportable);

    /* TODO: IM support. */
#if 0
    if (display->text_input_manager_v1 != NULL)
        cog_im_context_wl_v1_set_view_backend(wpe_view_data.backend);
#endif

    WebKitWebViewBackend *backend =
        webkit_web_view_backend_new(view_backend,
                                    (GDestroyNotify) wpe_view_backend_exportable_fdo_destroy,
                                    self->exportable);

    /* TODO: Fullscreen support. */
#if HAVE_FULLSCREEN_HANDLING
    wpe_view_backend_set_fullscreen_handler(wpe_view_data.backend, cog_wl_view_handle_dom_fullscreen_request, self);
#endif

    return backend;
}

bool
cog_wl_view_does_image_match_win_size(CogWlView *view)
{
    CogWlWindow *window = cog_wl_view_get_window(view);

    struct wpe_fdo_egl_exported_image *image = view->image;
    return image && wpe_fdo_egl_exported_image_get_width(image) == cog_window_get_width(COG_WINDOW(window)) &&
           wpe_fdo_egl_exported_image_get_height(image) == cog_window_get_height(COG_WINDOW(window));
}

void
cog_wl_view_fullscreen_image_ready(CogWlView *view)
{
    CogWlDisplay *display = view->platform->display;
    g_assert(display);
    CogWlWindow *window = cog_wl_view_get_window(view);
    g_assert(window);

    if (display->xdg_shell) {
        xdg_toplevel_set_fullscreen(window->xdg_toplevel, NULL);
    } else if (display->shell) {
        wl_shell_surface_set_fullscreen(window->shell_surface, WL_SHELL_SURFACE_FULLSCREEN_METHOD_SCALE, 0, NULL);
    } else if (display->fshell == NULL) {
        g_assert_not_reached();
    }

    cog_window_fullscreen_done(COG_WINDOW(window));
#if HAVE_FULLSCREEN_HANDLING
    if (window->was_fullscreen_requested_from_dom)
        wpe_view_backend_dispatch_did_enter_fullscreen(cog_view_get_backend(COG_VIEW(view)));
#endif
}

#if HAVE_FULLSCREEN_HANDLING
static bool
cog_wl_view_handle_dom_fullscreen_request(void *data, bool fullscreen)
{
    CogWlView   *view = data;
    CogWlWindow *window = window;

    window->was_fullscreen_requested_from_dom = true;
    if (fullscreen != window->is_fullscreen)
        return cog_wl_set_fullscreen(unused, fullscreen);

    // Handle situations where DOM fullscreen requests are mixed with system fullscreen commands (e.g F11)
    if (fullscreen)
        wpe_view_backend_dispatch_did_enter_fullscreen(wpe_view_data.backend);
    else
        wpe_view_backend_dispatch_did_exit_fullscreen(wpe_view_data.backend);

    return true;
}
#endif

static void
cog_wl_view_on_buffer_release(void *data G_GNUC_UNUSED, struct wl_buffer *buffer)
{
    wl_buffer_destroy(buffer);
}

static void
cog_wl_view_popup_create(CogWlView *view, WebKitOptionMenu *option_menu)
{
    CogWlWindow  *window = cog_wl_view_get_window(view);
    CogWlDisplay *display = view->platform->display;

    window->popup_data.configured = false;

    window->popup_data.option_menu = option_menu;

    window->popup_data.width = cog_window_get_width(COG_WINDOW(window));
    window->popup_data.height = cog_popup_menu_get_height_for_option_menu(option_menu);

    window->popup_data.popup_menu = cog_popup_menu_create(option_menu, display->shm, window->popup_data.width,
                                                          window->popup_data.height, view->scale_factor);

    window->popup_data.wl_surface = wl_compositor_create_surface(display->compositor);
    g_assert(window->popup_data.wl_surface);

#ifdef WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION
    if (wl_surface_get_version(window->popup_data.wl_surface) >= WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION)
        wl_surface_set_buffer_scale(window->popup_data.wl_surface, view->scale_factor);
#endif /* WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION */

    if (display->xdg_shell != NULL) {
        window->popup_data.xdg_positioner = xdg_wm_base_create_positioner(display->xdg_shell);
        g_assert(window->popup_data.xdg_positioner);

        xdg_positioner_set_size(window->popup_data.xdg_positioner, window->popup_data.width, window->popup_data.height);
        xdg_positioner_set_anchor_rect(window->popup_data.xdg_positioner, 0,
                                       (cog_window_get_height(COG_WINDOW(window)) - window->popup_data.height),
                                       window->popup_data.width, window->popup_data.height);

        window->popup_data.xdg_surface = xdg_wm_base_get_xdg_surface(display->xdg_shell, window->popup_data.wl_surface);
        g_assert(window->popup_data.xdg_surface);

        static const struct xdg_surface_listener xdg_surface_listener = {.configure = xdg_surface_on_configure};
        xdg_surface_add_listener(window->popup_data.xdg_surface, &xdg_surface_listener, window);
        window->popup_data.xdg_popup = xdg_surface_get_popup(window->popup_data.xdg_surface, window->xdg_surface,
                                                             window->popup_data.xdg_positioner);
        g_assert(window->popup_data.xdg_popup);

        static const struct xdg_popup_listener xdg_popup_listener = {
            .configure = cog_wl_window_xdg_popup_on_configure,
            .popup_done = cog_wl_window_xdg_popup_on_done,
        };
        xdg_popup_add_listener(window->popup_data.xdg_popup, &xdg_popup_listener, window);
        xdg_popup_grab(window->popup_data.xdg_popup, display->seat_default->seat,
                       display->seat_default->keyboard.serial);
        wl_surface_commit(window->popup_data.wl_surface);
    } else if (display->shell != NULL) {
        window->popup_data.shell_surface = wl_shell_get_shell_surface(display->shell, window->popup_data.wl_surface);
        g_assert(window->popup_data.shell_surface);

        static const struct wl_shell_surface_listener shell_popup_surface_listener = {
            .ping = shell_popup_surface_on_ping,
            .configure = shell_popup_surface_on_configure,
            .popup_done = shell_popup_surface_on_popup_done,
        };
        wl_shell_surface_add_listener(window->popup_data.shell_surface, &shell_popup_surface_listener, NULL);
        wl_shell_surface_set_popup(window->popup_data.shell_surface, display->seat_default->seat,
                                   display->seat_default->keyboard.serial, window->wl_surface, 0,
                                   (cog_window_get_height(COG_WINDOW(window)) - window->popup_data.height), 0);

        cog_wl_window_popup_display(window);
    }
}

static void
cog_wl_view_request_frame(CogWlView *view)
{
    CogWlDisplay *display = view->platform->display;
    g_assert(display);

    CogWlWindow *window = cog_wl_view_get_window(view);
    g_assert(window);

    if (!view->frame_callback) {
        static const struct wl_callback_listener listener = {.done = on_wl_surface_frame};
        view->frame_callback = wl_surface_frame(window->wl_surface);
        wl_callback_add_listener(view->frame_callback, &listener, view);
    }

    if (display->presentation != NULL) {
        static const struct wp_presentation_feedback_listener presentation_feedback_listener = {
            .sync_output = presentation_feedback_on_sync_output,
            .presented = presentation_feedback_on_presented,
            .discarded = presentation_feedback_on_discarded};
        struct wp_presentation_feedback *presentation_feedback =
            wp_presentation_feedback(display->presentation, window->wl_surface);
        wp_presentation_feedback_add_listener(presentation_feedback, &presentation_feedback_listener, NULL);
    }
}

void
cog_wl_view_resize(CogWlView *view)
{
    CogWlPlatform *platform = COG_WL_VIEW(view)->platform;
    g_assert(platform);
    CogWindow *window = COG_WINDOW(cog_wl_view_get_window(COG_WL_VIEW(view)));
    g_assert(window);

    struct wpe_view_backend *backend = cog_view_get_backend(COG_VIEW(view));

    if (platform->current_output) {
        COG_WL_VIEW(view)->scale_factor = platform->current_output->scale;

        int32_t pixel_width = cog_window_get_width(window) * platform->current_output->scale;
        int32_t pixel_height = cog_window_get_height(window) * platform->current_output->scale;

        wpe_view_backend_dispatch_set_size(backend, cog_window_get_width(window), cog_window_get_height(window));
        wpe_view_backend_dispatch_set_device_scale_factor(backend, platform->current_output->scale);

        g_debug("%s<%p>: Resized EGL buffer to: (%" PRIi32 ", %" PRIi32 ") @%" PRIi32 "x", G_STRFUNC, view, pixel_width,
                pixel_height, platform->current_output->scale);
    } else {
        g_debug("Window resize failed. No current output defined.");
    }
}

void
cog_wl_view_update_surface_contents(CogWlView *view, struct wl_surface *surface)
{
    g_assert(view);
    CogWlWindow *window = cog_wl_view_get_window(COG_WL_VIEW(view));
    g_assert(window);
    CogWlDisplay *display = view->platform->display;
    g_assert(display);

    if (view->should_update_opaque_region) {
        view->should_update_opaque_region = false;
        if (cog_window_is_fullscreen(COG_WINDOW(window)) || !cog_wl_view_background_has_alpha((CogView *) view)) {
            struct wl_region *region = wl_compositor_create_region(display->compositor);
            wl_region_add(region, 0, 0, INT32_MAX, INT32_MAX);
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
        display->egl_display, wpe_fdo_egl_exported_image_get_egl_image(view->image));
    g_assert(buffer);

    static const struct wl_buffer_listener buffer_listener = {.release = cog_wl_view_on_buffer_release};
    wl_buffer_add_listener(buffer, &buffer_listener, NULL);

    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, INT32_MAX, INT32_MAX);

    cog_wl_view_request_frame(view);

    wl_surface_commit(surface);

    if (cog_window_fullscreen_is_resizing(COG_WINDOW(window)) && cog_wl_view_does_image_match_win_size(view))
        cog_wl_view_fullscreen_image_ready(view);
}

#if HAVE_SHM_EXPORTED_BUFFER
static void
on_export_shm_buffer(void *data, struct wpe_fdo_shm_exported_buffer *exported_buffer)
{
    CogWlView    *view = data;
    CogWlWindow  *window = COG_WL_VIEW(view)->window;
    CogWlDisplay *display = window->display;

    struct wl_resource   *exported_resource = wpe_fdo_shm_exported_buffer_get_resource(exported_buffer);
    struct wl_shm_buffer *exported_shm_buffer = wpe_fdo_shm_exported_buffer_get_shm_buffer(exported_buffer);

    const uint32_t surface_pixel_width = platform->current_output->scale * window->width;
    const uint32_t surface_pixel_height = platform->current_output->scale * window->height;

    if (surface_pixel_width != wl_shm_buffer_get_width(exported_shm_buffer) ||
        surface_pixel_height != wl_shm_buffer_get_height(exported_shm_buffer)) {
        g_debug("Exported SHM buffer size %" PRIu32 "x%" PRIu32 ", does not match surface size %" PRIu32 "x%" PRIu32
                ", skipping.",
                wl_shm_buffer_get_width(exported_shm_buffer), wl_shm_buffer_get_width(exported_shm_buffer),
                surface_pixel_width, surface_pixel_width);
        wpe_view_backend_exportable_fdo_dispatch_frame_complete(wpe_host_data.exportable);
        wpe_view_backend_exportable_fdo_egl_dispatch_release_shm_exported_buffer(wpe_host_data.exportable,
                                                                                 exported_buffer);
        return;
    }

    struct shm_buffer *buffer = shm_buffer_for_resource(window->display, exported_resource);
    if (!buffer) {
        int32_t width;
        int32_t height;
        if (window->is_fullscreen) {
            width = window->width;
            height = window->height;
        } else {
            width = wl_shm_buffer_get_width(exported_shm_buffer);
            height = wl_shm_buffer_get_height(exported_shm_buffer);
        }
        int32_t  stride = wl_shm_buffer_get_stride(exported_shm_buffer);
        uint32_t format = wl_shm_buffer_get_format(exported_shm_buffer);

        size_t size = stride * height;
        buffer = shm_buffer_create(window->display, exported_resource, size);
        if (!buffer)
            return;
        wl_list_insert(&display->shm_buffer_list, &buffer->link);

        buffer->buffer = wl_shm_pool_create_buffer(buffer->shm_pool, 0, width, height, stride, format);
        wl_buffer_add_listener(buffer->buffer, &shm_buffer_listener, buffer);
    }

    buffer->exported_buffer = exported_buffer;
    shm_buffer_copy_contents(buffer, exported_shm_buffer);

    wl_surface_attach(window->wl_surface, buffer->buffer, 0, 0);
    wl_surface_damage(window->wl_surface, 0, 0, INT32_MAX, INT32_MAX);
    cog_wl_view_request_frame(view);
    wl_surface_commit(window->wl_surface);
}
#endif

static void
on_export_wl_egl_image(void *data, struct wpe_fdo_egl_exported_image *image)
{
    CogWlView   *self = data;
    CogWlWindow *window = cog_wl_view_get_window(self);
    if (!window)
        return;

    const uint32_t surface_pixel_width = self->scale_factor * cog_window_get_width(COG_WINDOW(window));
    const uint32_t surface_pixel_height = self->scale_factor * cog_window_get_height(COG_WINDOW(window));

    if (surface_pixel_width != wpe_fdo_egl_exported_image_get_width(image) ||
        surface_pixel_height != wpe_fdo_egl_exported_image_get_height(image)) {
        g_debug("Exported FDO EGL image size %" PRIu32 "x%" PRIu32 ", does not match surface size %" PRIu32 "x%" PRIu32
                ", skipping.",
                wpe_fdo_egl_exported_image_get_width(image), wpe_fdo_egl_exported_image_get_height(image),
                surface_pixel_width, surface_pixel_width);
        wpe_view_backend_exportable_fdo_dispatch_frame_complete(self->exportable);
        wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(self->exportable, image);
        return;
    }

    if (self->image)
        wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(self->exportable, self->image);

    self->image = image;

    const int32_t state = wpe_view_backend_get_activity_state(cog_view_get_backend((CogView *) self));
    if (state & wpe_view_activity_state_visible)
        cog_wl_view_update_surface_contents(self, window->wl_surface);
}

static void
on_show_option_menu(WebKitWebView *view, WebKitOptionMenu *menu, WebKitRectangle *rectangle, gpointer *data)
{
    cog_wl_view_popup_create(COG_WL_VIEW(view), g_object_ref(menu));
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

static void
shell_popup_surface_on_configure(void                    *data,
                                 struct wl_shell_surface *shell_surface,
                                 uint32_t                 edges,
                                 int32_t                  width,
                                 int32_t                  height)
{
}

static void
shell_popup_surface_on_ping(void *data, struct wl_shell_surface *shell_surface, uint32_t serial)
{
    wl_shell_surface_pong(shell_surface, serial);
}

static void
shell_popup_surface_on_popup_done(void *data, struct wl_shell_surface *shell_surface)
{
}

#if HAVE_SHM_EXPORTED_BUFFER
static struct shm_buffer *
shm_buffer_for_resource(CogWlDisplay *display, struct wl_resource *buffer_resource)
{
    struct shm_buffer *buffer;
    wl_list_for_each(buffer, &display->shm_buffer_list, link) {
        if (buffer->buffer_resource == buffer_resource)
            return buffer;
    }

    return NULL;
}

static struct shm_buffer *
shm_buffer_create(CogWlDisplay *display, struct wl_resource *buffer_resource, size_t size)
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
    buffer->destroy_listener.notify = shm_buffer_destroy_notify;
    buffer->buffer_resource = buffer_resource;
    wl_resource_add_destroy_listener(buffer_resource, &buffer->destroy_listener);

    buffer->shm_pool = wl_shm_create_pool(display->shm, fd, size);
    buffer->data = data;
    buffer->size = size;

    close(fd);
    return buffer;
}

static void
shm_buffer_destroy(struct shm_buffer *buffer)
{
    if (buffer->exported_buffer) {
        wpe_view_backend_exportable_fdo_egl_dispatch_release_shm_exported_buffer(wpe_host_data.exportable,
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
    shm_buffer_destroy(buffer);
}

static void
shm_buffer_on_release(void *data, struct wl_buffer *wl_buffer)
{
    struct shm_buffer *buffer = data;
    if (buffer->exported_buffer) {
        wpe_view_backend_exportable_fdo_egl_dispatch_release_shm_exported_buffer(wpe_host_data.exportable,
                                                                                 buffer->exported_buffer);
        buffer->exported_buffer = NULL;
    }
}

static const struct wl_buffer_listener shm_buffer_listener = {
    .release = shm_buffer_on_release,
};

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
#endif

static void
xdg_surface_on_configure(void *data, struct xdg_surface *surface, uint32_t serial)
{
    CogWlWindow *window = data;

    xdg_surface_ack_configure(surface, serial);

    if (window->popup_data.xdg_surface == surface && !window->popup_data.configured) {
        window->popup_data.configured = true;
        cog_wl_window_popup_display(window);
    }
}
