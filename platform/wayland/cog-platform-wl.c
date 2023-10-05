/*
 * cog-wl-platform.c
 * Copyright (C) 2018 Eduardo Lima <elima@igalia.com>
 * Copyright (C) 2018 Adrian Perez de Castro <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "../../core/cog.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <wpe/webkit.h>
#include <wpe/fdo.h>
#include <wpe/fdo-egl.h>
#include <stdlib.h>
#include <stdio.h>
#include <glib.h>
#include <glib-object.h>
#include <string.h>

#include <wayland-client.h>
#include <wayland-server.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

/* for mmap */
#include <sys/mman.h>

#include <locale.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <xkbcommon/xkbcommon.h>

#include "os-compatibility.h"

#include "cog-im-context-wl-v1.h"
#include "cog-im-context-wl.h"
#include "cog-platform-wl.h"
#include "cog-utils-wl.h"
#include "cog-view-wl.h"
#include "cog-window-wl.h"

#include "fullscreen-shell-unstable-v1-client.h"
#include "linux-dmabuf-unstable-v1-client.h"
#include "presentation-time-client.h"
#include "text-input-unstable-v1-client.h"
#include "text-input-unstable-v3-client.h"
#include "xdg-shell-client.h"

#if COG_ENABLE_WESTON_DIRECT_DISPLAY
#    include "weston-direct-display-client.h"
#    include <drm_fourcc.h>
#    include <wpe/extensions/video-plane-display-dmabuf.h>
#endif

#if COG_ENABLE_WESTON_CONTENT_PROTECTION
#    include "weston-content-protection-client.h"
#endif

#ifdef COG_USE_WAYLAND_CURSOR
#    include <wayland-cursor.h>
#endif

#if defined(WPE_CHECK_VERSION)
#    define HAVE_REFRESH_RATE_HANDLING WPE_CHECK_VERSION(1, 13, 2)
#else
#    define HAVE_REFRESH_RATE_HANDLING 0
#endif

#if defined(WPE_FDO_CHECK_VERSION) && 0
#    define HAVE_SHM_EXPORTED_BUFFER WPE_FDO_CHECK_VERSION(1, 9, 0)
#    define HAVE_FULLSCREEN_HANDLING WPE_FDO_CHECK_VERSION(1, 11, 1)
#else
#    define HAVE_SHM_EXPORTED_BUFFER 0
#    define HAVE_FULLSCREEN_HANDLING 0
#endif

#ifndef EGL_WL_create_wayland_buffer_from_image
typedef struct wl_buffer *(EGLAPIENTRYP PFNEGLCREATEWAYLANDBUFFERFROMIMAGEWL)(EGLDisplay dpy, EGLImageKHR image);
#endif

G_DEFINE_DYNAMIC_TYPE_EXTENDED(
    CogWlPlatform,
    cog_wl_platform,
    COG_TYPE_PLATFORM,
    0,
    g_io_extension_point_implement(COG_MODULES_PLATFORM_EXTENSION_POINT, g_define_type_id, "wl", 500);)

static void cog_egl_terminate(CogWlPlatform *);

static void                      cog_wl_platform_finalize(GObject *);
static gboolean                  cog_wl_platform_is_supported(void);
static void                      cog_wl_platform_resize_to_largest_output(CogWlPlatform *);
static gboolean                  cog_wl_platform_setup(CogPlatform *, CogShell *, const char *, GError **);
static WebKitInputMethodContext *cog_wl_platform_create_im_context(CogPlatform *);

static void cog_wl_seat_destroy(CogWlSeat *);

static bool     cog_wl_set_fullscreen(CogWlPlatform *, bool);
static GSource *cog_wl_src_setup(GMainContext *, struct wl_display *);

static void cog_wl_terminate(CogWlPlatform *);

static void noop();

static void check_supported_registry_on_global(void *, struct wl_registry *, uint32_t, const char *, uint32_t);

static inline bool pointer_uses_frame_event(struct wl_pointer *);

static void registry_on_global(void *, struct wl_registry *, uint32_t, const char *, uint32_t);
static void registry_on_global_remove(void *, struct wl_registry *, uint32_t);

static void seat_on_capabilities(void *, struct wl_seat *, uint32_t);
static void seat_on_name(void *, struct wl_seat *, const char *);

static void shell_surface_on_configure(void *, struct wl_shell_surface *, uint32_t, int32_t, int32_t);
static void shell_surface_on_ping(void *, struct wl_shell_surface *, uint32_t);

static void surface_on_enter(void *, struct wl_surface *, struct wl_output *);

static void touch_on_cancel(void *, struct wl_touch *);
static void
touch_on_down(void *, struct wl_touch *, uint32_t, uint32_t, struct wl_surface *, int32_t, wl_fixed_t, wl_fixed_t);
static void touch_on_frame(void *, struct wl_touch *);
static void touch_on_motion(void *, struct wl_touch *, uint32_t, int32_t, wl_fixed_t, wl_fixed_t);
static void touch_on_up(void *, struct wl_touch *, uint32_t, uint32_t, int32_t);

#if COG_ENABLE_WESTON_DIRECT_DISPLAY
static void video_plane_display_dmabuf_receiver_on_handle_dmabuf(void *,
                                                                 struct wpe_video_plane_display_dmabuf_export *,
                                                                 uint32_t,
                                                                 int,
                                                                 int32_t,
                                                                 int32_t,
                                                                 int32_t,
                                                                 int32_t,
                                                                 uint32_t);
static void video_plane_display_dmabuf_receiver_on_end_of_stream(void *, uint32_t);

static void video_surface_create_succeeded(void *, struct zwp_linux_buffer_params_v1 *, struct wl_buffer *);
static void video_surface_create_failed(void *, struct zwp_linux_buffer_params_v1 *);
static void video_surface_destroy(gpointer);
#endif

static void xdg_shell_on_ping(void *, struct xdg_wm_base *, uint32_t);

static void xdg_surface_on_configure(void *, struct xdg_surface *, uint32_t);

static void xdg_toplevel_on_configure(void *, struct xdg_toplevel *, int32_t, int32_t, struct wl_array *);
static void xdg_toplevel_on_close(void *, struct xdg_toplevel *);

static CogWlPlatform *s_platform = NULL;

// clang-format off
#define SHELL_PROTOCOLS(m) \
    m(xdg_wm_base)         \
    m(wl_shell)            \
    m(zwp_fullscreen_shell_v1)
// clang-format on

#define DECLARE_PROTOCOL_ENTRY(proto) gboolean found_##proto;
struct cog_wl_check_supported_protocols {
    SHELL_PROTOCOLS(DECLARE_PROTOCOL_ENTRY)
};
#undef DECLARE_PROTOCOL_ENTRY

#define ERR_EGL(_err, _msg)                                                                                         \
    do {                                                                                                            \
        EGLint error_code_##__LINE__ = eglGetError();                                                               \
        g_set_error((_err), COG_PLATFORM_EGL_ERROR, error_code_##__LINE__, _msg " (%#06x)", error_code_##__LINE__); \
    } while (0)

static gboolean
cog_egl_init(CogWlPlatform *platform, GError **error)
{
    g_debug("Initializing EGL...");

    platform->display->egl_display = eglGetDisplay((EGLNativeDisplayType) platform->display->display);
    if (platform->display->egl_display == EGL_NO_DISPLAY) {
        ERR_EGL(error, "Could not open EGL display");
        return FALSE;
    }

    EGLint major, minor;
    if (!eglInitialize(platform->display->egl_display, &major, &minor)) {
        ERR_EGL(error, "Could not initialize  EGL");
        cog_egl_terminate(platform);
        return FALSE;
    }
    g_info("EGL version %d.%d initialized.", major, minor);

    return TRUE;
}

static void
cog_egl_terminate(CogWlPlatform *platform)
{
    if (platform->display->egl_display != EGL_NO_DISPLAY) {
        eglTerminate(platform->display->egl_display);
        platform->display->egl_display = EGL_NO_DISPLAY;
    }
    eglReleaseThread();
}

#if HAVE_FULLSCREEN_HANDLING
/**
 * cog_view_get_backend:
 * @view: A #CogView
 *
 * Returns: The WPE view backend for the view.
 */
struct wpe_view_backend *
cog_view_get_backend(CogView *self)
{
    g_return_val_if_fail(COG_IS_VIEW(self), NULL);
    return webkit_web_view_backend_get_wpe_backend(webkit_web_view_get_backend((WebKitWebView *) self));
}
#endif

#if 0
static struct wpe_view_backend *
gamepad_provider_get_view_backend_for_gamepad(void *provider G_GNUC_UNUSED, void *gamepad G_GNUC_UNUSED)
{
    /* get_view_backend() might not been called yet */
    g_assert(wpe_view_data.backend);
    return wpe_view_data.backend;
}
#endif

static void *
cog_wl_check_supported(void *data G_GNUC_UNUSED)
{
    /*
     * XXX: It would be neat to have some way of determining whether EGL is
     *      usable without doing EGL initialization. Maybe an option is
     *      checking whether the wl_drm protocol is present, but some GPU
     *      drivers might expose other protocols (w.g. wl_viv for Vivante)
     *      so that could result in needing to maintain the list updated.
     *
     *      For now the check assumes that EGL will work if the compositor
     *      handles at least one of the shell protocols supported by Cog.
     */
    struct wl_display *display = wl_display_connect(NULL);
    if (display) {
        struct cog_wl_check_supported_protocols protocols = {};
        struct wl_registry                     *registry = wl_display_get_registry(display);
        wl_registry_add_listener(registry,
                                 &((const struct wl_registry_listener){
                                     .global = check_supported_registry_on_global,
                                 }),
                                 &protocols);
        wl_display_roundtrip(display);

        gboolean ok = FALSE;
#define CHECK_SHELL_PROTOCOL(proto) ok = ok || protocols.found_##proto;
        SHELL_PROTOCOLS(CHECK_SHELL_PROTOCOL)
#undef CHECK_SHELL_PROTOCOL

        wl_registry_destroy(registry);
        wl_display_disconnect(display);
        return GINT_TO_POINTER(ok);
    } else {
        return GINT_TO_POINTER(FALSE);
    }
}
static void
cog_wl_display_add_seat(CogWlDisplay *display, struct wl_seat *wl_seat, uint32_t name, uint32_t version)
{
    g_assert(display);

    CogWlSeat *seat = g_slice_new0(CogWlSeat);
    seat->seat = wl_seat;
    seat->seat_name = name;
    seat->seat_version = version;
    if (!display->seat_default)
        display->seat_default = seat;
    wl_list_init(&seat->link);
    wl_list_insert(&display->seats, &seat->link);

    static const struct wl_seat_listener seat_listener = {
        .capabilities = seat_on_capabilities,
#ifdef WL_SEAT_NAME_SINCE_VERSION
        .name = seat_on_name,
#endif /* WL_SEAT_NAME_SINCE_VERSION */
    };
    wl_seat_add_listener(wl_seat, &seat_listener, seat);

    if (!(seat->xkb.context = xkb_context_new(XKB_CONTEXT_NO_FLAGS))) {
        g_error("Could not initialize XKB context");
        return;
    }

    seat->xkb.compose_table =
        xkb_compose_table_new_from_locale(seat->xkb.context, setlocale(LC_CTYPE, NULL), XKB_COMPOSE_COMPILE_NO_FLAGS);

    if (seat->xkb.compose_table != NULL) {
        seat->xkb.compose_state = xkb_compose_state_new(seat->xkb.compose_table, XKB_COMPOSE_STATE_NO_FLAGS);
        g_clear_pointer(&seat->xkb.compose_table, xkb_compose_table_unref);
    }

    if (display->text_input_manager != NULL) {
        struct zwp_text_input_v3 *text_input =
            zwp_text_input_manager_v3_get_text_input(display->text_input_manager, seat->seat);
        cog_im_context_wl_set_text_input(text_input);
    } else if (display->text_input_manager_v1 != NULL) {
        struct zwp_text_input_v1 *text_input =
            zwp_text_input_manager_v1_create_text_input(display->text_input_manager_v1);
        // TODO: Check how this change affects to the wayland cog IM context v1.
        // cog_im_context_wl_v1_set_text_input(text_input, seat->seat, s_platform->window->wl_surface);
        cog_im_context_wl_v1_set_text_input(text_input, seat->seat, NULL);
    }
}

CogWlDisplay *
cog_wl_display_connect(const char *name, GError **error)
{
    CogWlDisplay *self = g_slice_new0(CogWlDisplay);

    wl_list_init(&self->seats);

    if (!(self->display = wl_display_connect(name))) {
        g_set_error_literal(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Could not open Wayland display");
        return NULL;
    }

    if (!self->event_src) {
        self->event_src = cog_wl_src_setup(g_main_context_get_thread_default(), self->display);
    }

    g_debug("%s: Created @ %p", G_STRFUNC, self);
    return g_steal_pointer(&self);
}

void
cog_wl_display_destroy(CogWlDisplay *self)
{
    g_assert(self != NULL);

    g_debug("%s: Destroying @ %p", G_STRFUNC, self);

    if (self->display) {
        wl_display_flush(self->display);
        g_clear_pointer(&self->display, wl_display_disconnect);
    }

#if COG_ENABLE_WESTON_DIRECT_DISPLAY
    g_clear_pointer(&self->video_surfaces, g_hash_table_destroy);
#endif

    self->seat_default = NULL;

    CogWlSeat *item, *tmp;
    wl_list_for_each_safe(item, tmp, &self->seats, link) {
        cog_wl_seat_destroy(item);
    }

    g_slice_free(CogWlDisplay, self);
}

static gboolean
cog_wl_init(CogWlPlatform *platform, GError **error)
{
    g_debug("Initializing Wayland...");

    CogWlDisplay *display = cog_wl_display_connect(NULL, error);
    if (!display) {
        g_debug("%s: %s", G_STRFUNC, (*error)->message);
        return FALSE;
    }

    platform->display = display;
    display->registry = wl_display_get_registry(display->display);
    g_assert(display->registry);

    static const struct wl_registry_listener registry_listener = {.global = registry_on_global,
                                                                  .global_remove = registry_on_global_remove};

    wl_registry_add_listener(display->registry, &registry_listener, platform);
    wl_display_roundtrip(display->display);

#if COG_USE_WAYLAND_CURSOR
    if (display->shm) {
        if (!(display->cursor_theme = wl_cursor_theme_load(NULL, 32, display->shm))) {
            g_warning("%s: Could not load cursor theme.", G_STRFUNC);
        } else if (!(display->cursor_left_ptr = wl_cursor_theme_get_cursor(display->cursor_theme, "left_ptr"))) {
            g_warning("%s: Could not load left_ptr cursor.", G_STRFUNC);
        }
    }
#endif /* COG_USE_WAYLAND_CURSOR */

    g_assert(display->compositor);
    g_assert(display->xdg_shell != NULL || display->shell != NULL || display->fshell != NULL);

    wl_list_init(&display->shm_buffer_list);
    return TRUE;
}

static void
cog_wl_platform_class_init(CogWlPlatformClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = cog_wl_platform_finalize;

    CogPlatformClass *platform_class = COG_PLATFORM_CLASS(klass);
    platform_class->is_supported = cog_wl_platform_is_supported;
    platform_class->get_view_type = cog_wl_view_get_type;
    platform_class->setup = cog_wl_platform_setup;
    platform_class->create_im_context = cog_wl_platform_create_im_context;
}

static void
cog_wl_platform_class_finalize(CogWlPlatformClass *klass)
{
    g_slice_free(CogWlPlatform, s_platform);
}

static void
cog_wl_platform_clear_buffers(CogWlPlatform *platform)
{
#if HAVE_SHM_EXPORTED_BUFFER
    struct shm_buffer *buffer, *tmp;
    wl_list_for_each_safe(buffer, tmp, &display->shm_buffer_list, link) {

        wl_list_remove(&buffer->link);
        wl_list_remove(&buffer->destroy_listener.link);

        shm_buffer_destroy(buffer);
    }
    wl_list_init(&s_display->shm_buffer_list);
#endif
}

static void
cog_wl_platform_clear_input(CogWlPlatform *platform)
{
    CogWlDisplay *display = platform->display;

    cog_im_context_wl_set_text_input(NULL);
    g_clear_pointer(&display->text_input_manager, zwp_text_input_manager_v3_destroy);
    cog_im_context_wl_v1_set_text_input(NULL, NULL, NULL);
    g_clear_pointer(&display->text_input_manager_v1, zwp_text_input_manager_v1_destroy);
}

static void
cog_wl_platform_configure_surface_geometry(CogWlPlatform *platform, int32_t width, int32_t height)
{
    CogView *view = cog_view_stack_get_visible_view(platform->views);

    const char *env_var;
    if (width == 0) {
        env_var = g_getenv("COG_PLATFORM_WL_VIEW_WIDTH");
        if (env_var != NULL)
            width = (int32_t) g_ascii_strtod(env_var, NULL);
        else
            width = COG_WL_WIN_DEFAULT_WIDTH;
    }
    if (height == 0) {
        env_var = g_getenv("COG_PLATFORM_WL_VIEW_HEIGHT");
        if (env_var != NULL)
            height = (int32_t) g_ascii_strtod(env_var, NULL);
        else
            height = COG_WL_WIN_DEFAULT_HEIGHT;
    }

    CogWlWindow *window = cog_wl_view_get_window(COG_WL_VIEW(view));
    if (view || window->width != width || window->height != height) {
        g_debug("Configuring new size: %" PRId32 "x%" PRId32, width, height);
        window->width = width;
        window->height = height;

        for (gsize i = 0; i < cog_view_group_get_n_views(COG_VIEW_GROUP(platform->views)); i++) {
            CogView *view = cog_view_group_get_nth_view(COG_VIEW_GROUP(platform->views), i);
            COG_WL_VIEW(view)->should_update_opaque_region = true;
        }
    }
}

static WebKitInputMethodContext *
cog_wl_platform_create_im_context(CogPlatform *platform)
{
    CogWlPlatform *wl_platform = (CogWlPlatform *) platform;

    if (wl_platform->display->text_input_manager)
        return cog_im_context_wl_new();
    if (wl_platform->display->text_input_manager_v1)
        return cog_im_context_wl_v1_new();
    return NULL;
}

static CogWlWindow *
cog_wl_platform_create_window(CogWlPlatform *self)
{
    CogWlDisplay *display = self->display;

    g_debug("Creating Wayland surface...");

    CogWlWindow *window = cog_wl_window_new();
    g_assert(window != NULL);

    window->wl_surface = wl_compositor_create_surface(display->compositor);
    wl_surface_set_user_data(window->wl_surface, window);
    g_assert(window->wl_surface);

#if COG_ENABLE_WESTON_DIRECT_DISPLAY
    display->video_surfaces = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, video_surface_destroy);
#endif

    static const struct wl_surface_listener surface_listener = {
        .enter = surface_on_enter,
        .leave = noop,
    };
    wl_surface_add_listener(window->wl_surface, &surface_listener, window);

    if (display->xdg_shell != NULL) {
        window->xdg_surface = xdg_wm_base_get_xdg_surface(display->xdg_shell, window->wl_surface);
        g_assert(window->xdg_surface);

        static const struct xdg_surface_listener xdg_surface_listener = {.configure = xdg_surface_on_configure};
        xdg_surface_add_listener(window->xdg_surface, &xdg_surface_listener, window);
        window->xdg_toplevel = xdg_surface_get_toplevel(window->xdg_surface);
        g_assert(window->xdg_toplevel);

        static const struct xdg_toplevel_listener xdg_toplevel_listener = {
            .configure = xdg_toplevel_on_configure,
            .close = xdg_toplevel_on_close,
        };
        xdg_toplevel_add_listener(window->xdg_toplevel, &xdg_toplevel_listener, self);
        xdg_toplevel_set_title(window->xdg_toplevel, COG_DEFAULT_APPNAME);

        const char   *app_id = NULL;
        GApplication *app = g_application_get_default();
        if (app) {
            app_id = g_application_get_application_id(app);
        }
        if (!app_id) {
            app_id = COG_DEFAULT_APPID;
        }
        xdg_toplevel_set_app_id(window->xdg_toplevel, app_id);
        wl_surface_commit(window->wl_surface);
    } else if (display->fshell != NULL) {
        zwp_fullscreen_shell_v1_present_surface(display->fshell,
                                                window->wl_surface,
                                                ZWP_FULLSCREEN_SHELL_V1_PRESENT_METHOD_DEFAULT,
                                                NULL);

        /* Configure the surface so that it respects the width and height
         * environment variables */
        cog_wl_platform_configure_surface_geometry(self, 0, 0);
    } else if (display->shell != NULL) {
        window->shell_surface = wl_shell_get_shell_surface(display->shell, window->wl_surface);
        g_assert(window->shell_surface);

        static const struct wl_shell_surface_listener shell_surface_listener = {
            .ping = shell_surface_on_ping,
            .configure = shell_surface_on_configure,
        };
        wl_shell_surface_add_listener(window->shell_surface, &shell_surface_listener, self);
        wl_shell_surface_set_toplevel(window->shell_surface);

        /* wl_shell needs an initial surface configuration. */
        cog_wl_platform_configure_surface_geometry(self, 0, 0);
    }

    const char *env_var;
    if ((env_var = g_getenv("COG_PLATFORM_WL_VIEW_FULLSCREEN")) && g_ascii_strtoll(env_var, NULL, 10) > 0) {
        window->is_maximized = false;
        window->is_fullscreen = true;

        if (display->xdg_shell != NULL) {
            xdg_toplevel_set_fullscreen(window->xdg_toplevel, NULL);
        } else if (display->fshell != NULL) {
            window->should_cog_wl_platform_resize_to_largest_output = true;
            cog_wl_platform_resize_to_largest_output(self);
        } else if (display->shell != NULL) {
            wl_shell_surface_set_fullscreen(window->shell_surface, WL_SHELL_SURFACE_FULLSCREEN_METHOD_SCALE, 0, NULL);
        } else {
            g_warning("No available shell capable of fullscreening.");
            window->is_fullscreen = false;
        }
    } else if ((env_var = g_getenv("COG_PLATFORM_WL_VIEW_MAXIMIZE")) && g_ascii_strtoll(env_var, NULL, 10) > 0) {
        window->is_maximized = true;
        window->is_fullscreen = false;

        if (display->xdg_shell != NULL) {
            xdg_toplevel_set_maximized(window->xdg_toplevel);
        } else if (display->shell != NULL) {
            wl_shell_surface_set_maximized(window->shell_surface, NULL);
        } else {
            g_warning("No available shell capable of maximizing.");
            window->is_maximized = false;
        }
    }

    return window;
}

static void
cog_wl_platform_destroy_window(void *data)
{
    CogWlWindow *window = data;

    g_return_if_fail(window);

    CogWlDisplay *display = s_platform->display;
    CogWlSeat    *seat, *tmp_seat;
    wl_list_for_each_safe(seat, tmp_seat, &display->seats, link) {
        if (seat->keyboard_target == window)
            seat->keyboard_target = NULL;

        if (seat->pointer_target == window)
            seat->pointer_target = NULL;
    }

    cog_wl_window_popup_destroy(window);

    g_clear_pointer(&window->xdg_toplevel, xdg_toplevel_destroy);
    g_clear_pointer(&window->xdg_surface, xdg_surface_destroy);
    g_clear_pointer(&window->shell_surface, wl_shell_surface_destroy);
    g_clear_pointer(&window->wl_surface, wl_surface_destroy);

    g_slice_free(CogWlWindow, window);
}

static void
cog_wl_platform_finalize(GObject *object)
{
    CogWlPlatform *self = (CogWlPlatform *) object;

    /* @FIXME: check why this segfaults
    wpe_view_backend_destroy (wpe_view_data.backend);
    */

    /* free WPE host data */
    /* @FIXME: check why this segfaults
    wpe_view_backend_exportable_wl_destroy (wpe_host_data.exportable);
    */

    g_clear_pointer(&self->windows, g_hash_table_destroy);

    cog_wl_platform_clear_buffers(self);

    cog_wl_platform_clear_input(self);
    cog_egl_terminate(self);
    cog_wl_terminate(self);

    G_OBJECT_CLASS(cog_wl_platform_parent_class)->finalize(object);
}

static inline CogWlOutput *
cog_wl_platform_get_output(struct wl_output *output)
{
    CogWlOutput *item;
    wl_list_for_each(item, &s_platform->outputs, link) {
        if (item->output == output)
            return item;
    }
    g_assert_not_reached();
}

static void
cog_wl_platform_init(CogWlPlatform *self)
{
    s_platform = self;

    wl_list_init(&self->outputs);
}

static gboolean
cog_wl_platform_is_supported(void)
{
    static GOnce once = G_ONCE_INIT;
    g_once(&once, cog_wl_check_supported, NULL);
    return GPOINTER_TO_INT(once.retval);
}

static void
cog_wl_platform_on_notify_visible_view(CogWlPlatform *self)
{
    CogWlView   *view = COG_WL_VIEW(cog_view_stack_get_visible_view(self->views));
    CogWlWindow *window = cog_wl_view_get_window(view);

    wpe_view_backend_exportable_fdo_dispatch_frame_complete(view->exportable);

    if (window) {
        g_debug("%s: visible view %p", G_STRFUNC, view);

        if (!view->image) {
            g_debug("%s: No last image to show, skipping update.", G_STRFUNC);
            return;
        }

        cog_wl_view_update_surface_contents(view, window->wl_surface);
    }
}

static void
cog_wl_view_resize(CogView *view, CogWlPlatform *platform)
{
    g_assert(platform);

    CogWlWindow *window = cog_wl_view_get_window(COG_WL_VIEW(view));

    struct wpe_view_backend *backend = cog_view_get_backend(view);

    if (platform->current_output) {
        COG_WL_VIEW(view)->scale_factor = platform->current_output->scale;

        int32_t pixel_width = window->width * platform->current_output->scale;
        int32_t pixel_height = window->height * platform->current_output->scale;

        wpe_view_backend_dispatch_set_size(backend, window->width, window->height);
        wpe_view_backend_dispatch_set_device_scale_factor(backend, platform->current_output->scale);

        g_debug("%s<%p>: Resized EGL buffer to: (%" PRIi32 ", %" PRIi32 ") @%" PRIi32 "x", G_STRFUNC, view, pixel_width,
                pixel_height, platform->current_output->scale);
    } else {
        g_debug("Window resize failed. No current output defined.");
    }
}

static void
cog_wl_platform_resize_to_largest_output(CogWlPlatform *platform)
{
    /* Find the largest output and resize the surface to match */
    int32_t width = 0;
    int32_t height = 0;

    CogWlOutput *output;
    wl_list_for_each(output, &platform->outputs, link) {
        if (output->width * output->height >= width * height) {
            width = output->width;
            height = output->height;
        }
    }
    cog_wl_platform_configure_surface_geometry(platform, width, height);
    cog_view_group_foreach(COG_VIEW_GROUP(platform->views), (GFunc) cog_wl_view_resize, platform);
}

static void
cog_wl_view_stack_handle_add(CogViewStack *self, CogView *view)
{
    COG_WL_VIEW(view)->stack = COG_VIEW_STACK(self);
    COG_WL_VIEW(view)->platform = s_platform;
    CogWlWindow *window = (CogWlWindow *) g_hash_table_lookup(s_platform->windows, self);
    if (!window) {
        window = cog_wl_platform_create_window(s_platform);
        g_hash_table_insert(s_platform->windows, self, window);
    }
    cog_wl_view_resize(COG_VIEW(view), s_platform);
}

static gboolean
cog_wl_platform_setup(CogPlatform *platform, CogShell *shell G_GNUC_UNUSED, const char *params, GError **error)
{
    g_return_val_if_fail(COG_IS_SHELL(shell), FALSE);

    if (!wpe_loader_init("libWPEBackend-fdo-1.0.so")) {
        g_set_error_literal(error,
                            COG_PLATFORM_WPE_ERROR,
                            COG_PLATFORM_WPE_ERROR_INIT,
                            "Failed to set backend library name");
        return FALSE;
    }

    CogWlPlatform *self = COG_WL_PLATFORM(platform);
    self->views = cog_shell_get_view_stack(shell);

    g_signal_connect(self->views, "add", G_CALLBACK(cog_wl_view_stack_handle_add), NULL);

    self->windows = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, cog_wl_platform_destroy_window);

    if (!cog_wl_init(self, error))
        return FALSE;

    if (!cog_egl_init(self, error)) {
        cog_wl_terminate(self);
        return FALSE;
    }

    /* init WPE host data */
    wpe_fdo_initialize_for_egl_display(self->display->egl_display);

#if COG_ENABLE_WESTON_DIRECT_DISPLAY
    static const struct wpe_video_plane_display_dmabuf_receiver video_plane_display_dmabuf_receiver = {
        .handle_dmabuf = video_plane_display_dmabuf_receiver_on_handle_dmabuf,
        .end_of_stream = video_plane_display_dmabuf_receiver_on_end_of_stream,
    };
    wpe_video_plane_display_dmabuf_register_receiver(&video_plane_display_dmabuf_receiver, self);
#endif

#if 0
    cog_gamepad_setup(gamepad_provider_get_view_backend_for_gamepad);
#endif

    g_signal_connect_object(self->views, "notify::visible-view", G_CALLBACK(cog_wl_platform_on_notify_visible_view),
                            self, G_CONNECT_AFTER | G_CONNECT_SWAPPED);

    return TRUE;
}

static void
cog_wl_seat_destroy(CogWlSeat *self)
{
    g_assert(self != NULL);

    g_debug("%s: Destroying @ %p", G_STRFUNC, self);
    g_clear_pointer(&self->keyboard_obj, wl_keyboard_destroy);
    g_clear_pointer(&self->pointer_obj, wl_pointer_destroy);
    g_clear_pointer(&self->touch_obj, wl_touch_destroy);
    g_clear_pointer(&self->seat, wl_seat_destroy);

    g_clear_pointer(&self->xkb.state, xkb_state_unref);
    g_clear_pointer(&self->xkb.compose_state, xkb_compose_state_unref);
    g_clear_pointer(&self->xkb.keymap, xkb_keymap_unref);
    g_clear_pointer(&self->xkb.context, xkb_context_unref);

    wl_list_remove(&self->link);
    g_slice_free(CogWlSeat, self);
}

static void
cog_wl_seat_keyboard_handle_key_event(CogWlSeat *seat, uint32_t key, uint32_t state, uint32_t time)
{
    CogView *view = cog_view_stack_get_visible_view(s_platform->views);
    if (!view || seat->xkb.state == NULL)
        return;

    uint32_t keysym = xkb_state_key_get_one_sym(seat->xkb.state, key);
    uint32_t unicode = xkb_state_key_get_utf32(seat->xkb.state, key);

    /* TODO: Move as much as possible from fullscreen handling to common code. */
    if (cog_view_get_use_key_bindings(view) && state == WL_KEYBOARD_KEY_STATE_PRESSED && seat->xkb.modifiers == 0 &&
        unicode == 0 && keysym == XKB_KEY_F11) {
#if HAVE_FULLSCREEN_HANDLING
        if (window->is_fullscreen && window->was_fullscreen_requested_from_dom) {
            wpe_view_backend_dispatch_request_exit_fullscreen(wpe_view_data.backend);
            return;
        }
#endif
        CogWlWindow *window = cog_wl_view_get_window(COG_WL_VIEW(view));
        cog_wl_set_fullscreen(0, !window->is_fullscreen);
        return;
    }

    if (seat->xkb.compose_state != NULL && state == WL_KEYBOARD_KEY_STATE_PRESSED &&
        xkb_compose_state_feed(seat->xkb.compose_state, keysym) == XKB_COMPOSE_FEED_ACCEPTED &&
        xkb_compose_state_get_status(seat->xkb.compose_state) == XKB_COMPOSE_COMPOSED) {
        keysym = xkb_compose_state_get_one_sym(seat->xkb.compose_state);
    }

    struct wpe_input_keyboard_event event = {time, keysym, key, state == true, seat->xkb.modifiers};

    cog_view_handle_key_event(view, &event);
}

static void
cog_wl_seat_keyboard_on_keymap(void *data, struct wl_keyboard *wl_keyboard, uint32_t format, int32_t fd, uint32_t size)
{
    CogWlSeat *seat = data;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }

    const int map_mode = wl_seat_get_version(seat->seat) > 6 ? MAP_PRIVATE : MAP_SHARED;
    void     *mapping = mmap(NULL, size, PROT_READ, map_mode, fd, 0);
    if (mapping == MAP_FAILED) {
        close(fd);
        return;
    }

    seat->xkb.keymap = xkb_keymap_new_from_string(seat->xkb.context,
                                                  (char *) mapping,
                                                  XKB_KEYMAP_FORMAT_TEXT_V1,
                                                  XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(mapping, size);
    close(fd);

    if (!seat->xkb.keymap) {
        g_error("Could not initialize XKB keymap");
        return;
    }

    if (!(seat->xkb.state = xkb_state_new(seat->xkb.keymap))) {
        g_error("Could not initialize XKB state");
        return;
    }

    seat->xkb.indexes.control = xkb_keymap_mod_get_index(seat->xkb.keymap, XKB_MOD_NAME_CTRL);
    seat->xkb.indexes.alt = xkb_keymap_mod_get_index(seat->xkb.keymap, XKB_MOD_NAME_ALT);
    seat->xkb.indexes.shift = xkb_keymap_mod_get_index(seat->xkb.keymap, XKB_MOD_NAME_SHIFT);
}

static void
cog_wl_seat_keyboard_on_enter(void               *data,
                              struct wl_keyboard *wl_keyboard,
                              uint32_t            serial,
                              struct wl_surface  *surface,
                              struct wl_array    *keys)
{
    CogWlSeat *seat = data;

    if (wl_keyboard != seat->keyboard_obj) {
        g_critical("%s: Got keyboard %p, expected %p.", G_STRFUNC, wl_keyboard, seat->keyboard_obj);
        return;
    }

    CogWlWindow *window = wl_surface_get_user_data(surface);
    seat->keyboard_target = window;

    seat->keyboard.serial = serial;
}

static void
cog_wl_seat_keyboard_on_leave(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, struct wl_surface *surface)
{
    CogWlSeat *seat = data;
    seat->keyboard.serial = serial;
}

static gboolean
cog_wl_seat_keyboard_repeat_delay_timeout(CogWlSeat *seat)
{
    cog_wl_seat_keyboard_handle_key_event(seat,
                                          seat->keyboard.repeat_data.key,
                                          seat->keyboard.repeat_data.state,
                                          seat->keyboard.repeat_data.time);

    seat->keyboard.repeat_data.event_source =
        g_timeout_add(seat->keyboard.repeat_info.rate, (GSourceFunc) cog_wl_seat_keyboard_repeat_delay_timeout, seat);

    return G_SOURCE_REMOVE;
}

static void
cog_wl_seat_keyboard_on_key(void               *data,
                            struct wl_keyboard *wl_keyboard,
                            uint32_t            serial,
                            uint32_t            time,
                            uint32_t            key,
                            uint32_t            state)
{
    CogWlSeat *seat = data;

    // wl_keyboard protocol sends key events as a physical key signals on
    // the keyboard (limited to 256 - 8 bits).  The XKB protocol doesn't share
    // this limitation and uses extended keycodes and it restricts these
    // names to at most 4 (ASCII) characters:
    //
    //   xkb_keycode_t keycode_A = KEY_A + 8;
    //
    // Ref: xkb_keycode_t section in
    // https://xkbcommon.org/doc/current/xkbcommon_8h.html
    key += 8;

    seat->keyboard.serial = serial;
    cog_wl_seat_keyboard_handle_key_event(seat, key, state, time);

    if (seat->keyboard.repeat_info.rate == 0)
        return;

    if (state == WL_KEYBOARD_KEY_STATE_RELEASED && seat->keyboard.repeat_data.key == key) {
        if (seat->keyboard.repeat_data.event_source)
            g_source_remove(seat->keyboard.repeat_data.event_source);

        memset(&seat->keyboard.repeat_data, 0x00, sizeof(seat->keyboard.repeat_data));
    } else if (seat->xkb.keymap != NULL && state == WL_KEYBOARD_KEY_STATE_PRESSED &&
               xkb_keymap_key_repeats(seat->xkb.keymap, key)) {
        if (seat->keyboard.repeat_data.event_source)
            g_source_remove(seat->keyboard.repeat_data.event_source);

        seat->keyboard.repeat_data.key = key;
        seat->keyboard.repeat_data.time = time;
        seat->keyboard.repeat_data.state = state;
        seat->keyboard.repeat_data.event_source = g_timeout_add(
            seat->keyboard.repeat_info.delay, (GSourceFunc) cog_wl_seat_keyboard_repeat_delay_timeout, seat);
    }
}

static void
cog_wl_seat_keyboard_on_modifiers(void               *data,
                                  struct wl_keyboard *wl_keyboard,
                                  uint32_t            serial,
                                  uint32_t            mods_depressed,
                                  uint32_t            mods_latched,
                                  uint32_t            mods_locked,
                                  uint32_t            group)
{
    CogWlSeat *seat = data;

    if (seat->xkb.state == NULL)
        return;
    seat->keyboard.serial = serial;

    xkb_state_update_mask(seat->xkb.state, mods_depressed, mods_latched, mods_locked, 0, 0, group);

    seat->xkb.modifiers = 0;
    uint32_t component = (XKB_STATE_MODS_DEPRESSED | XKB_STATE_MODS_LATCHED);

    if (xkb_state_mod_index_is_active(seat->xkb.state, seat->xkb.indexes.control, component)) {
        seat->xkb.modifiers |= wpe_input_keyboard_modifier_control;
    }
    if (xkb_state_mod_index_is_active(seat->xkb.state, seat->xkb.indexes.alt, component)) {
        seat->xkb.modifiers |= wpe_input_keyboard_modifier_alt;
    }
    if (xkb_state_mod_index_is_active(seat->xkb.state, seat->xkb.indexes.shift, component)) {
        seat->xkb.modifiers |= wpe_input_keyboard_modifier_shift;
    }
}

static void
cog_wl_seat_keyboard_on_repeat_info(void *data, struct wl_keyboard *wl_keyboard, int32_t rate, int32_t delay)
{
    CogWlSeat *seat = data;

    seat->keyboard.repeat_info.rate = rate;
    seat->keyboard.repeat_info.delay = delay;

    /* a rate of zero disables any repeating. */
    if (rate == 0 && seat->keyboard.repeat_data.event_source > 0) {
        g_source_remove(seat->keyboard.repeat_data.event_source);
        memset(&seat->keyboard.repeat_data, 0x00, sizeof(seat->keyboard.repeat_data));
    }
}
static bool
cog_wl_set_fullscreen(CogWlPlatform *self, bool fullscreen)
{
    CogWlDisplay *display = self->display;
    g_assert(display);
    CogView *view = cog_view_stack_get_visible_view(self->views);
    g_assert(view);
    CogWlWindow *window = cog_wl_view_get_window(COG_WL_VIEW(view));
    g_assert(window);

    if (window->is_resizing_fullscreen || window->is_fullscreen == fullscreen)
        return false;

    window->is_fullscreen = fullscreen;

    if (fullscreen) {
        // Resize the view_backend to the size of the screen.
        // Wait until a new exported image is reveived. See cog_wl_view_fullscreen_image_ready().
        window->is_resizing_fullscreen = true;
        window->width_before_fullscreen = window->width;
        window->height_before_fullscreen = window->height;
        cog_wl_platform_resize_to_largest_output(self);
        if (view && cog_wl_view_does_image_match_win_size(COG_WL_VIEW(view)))
            cog_wl_view_fullscreen_image_ready(COG_WL_VIEW(view));
    } else {
        if (display->xdg_shell != NULL) {
            xdg_toplevel_unset_fullscreen(window->xdg_toplevel);
        } else if (display->fshell != NULL) {
            cog_wl_platform_configure_surface_geometry(self, window->width_before_fullscreen,
                                                       window->height_before_fullscreen);
            cog_view_group_foreach(COG_VIEW_GROUP(self->views), (GFunc) cog_wl_view_resize, self);
        } else if (display->shell != NULL) {
            wl_shell_surface_set_toplevel(window->shell_surface);
            cog_wl_platform_configure_surface_geometry(self, window->width_before_fullscreen,
                                                       window->height_before_fullscreen);
            cog_view_group_foreach(COG_VIEW_GROUP(self->views), (GFunc) cog_wl_view_resize, self);
        } else {
            g_assert_not_reached();
        }
#if HAVE_FULLSCREEN_HANDLING
        if (window->was_fullscreen_requested_from_dom) {
            CogView *view = cog_view_stack_get_visible_view(self->views);
            if (view)
                wpe_view_backend_dispatch_did_exit_fullscreen(cog_view_get_backend(view));
        }
        window->was_fullscreen_requested_from_dom = false;
#endif
    }

    for (gsize i = 0; i < cog_view_group_get_n_views(COG_VIEW_GROUP(self->views)); i++) {
        CogView *view = cog_view_group_get_nth_view(COG_VIEW_GROUP(self->views), i);
        COG_WL_VIEW(view)->should_update_opaque_region = true;
    }
    return true;
}
static gboolean
cog_wl_src_check(GSource *base)
{
    struct wl_event_source *src = (struct wl_event_source *) base;

    if (src->pfd.revents & G_IO_IN) {
        if (wl_display_read_events(src->display) < 0)
            return false;
        return true;
    } else {
        wl_display_cancel_read(src->display);
        return false;
    }
}

static gboolean
cog_wl_src_dispatch(GSource *base, GSourceFunc callback, gpointer user_data)
{
    struct wl_event_source *src = (struct wl_event_source *) base;

    if (src->pfd.revents & G_IO_IN) {
        if (wl_display_dispatch_pending(src->display) < 0)
            return false;
    }

    if (src->pfd.revents & (G_IO_ERR | G_IO_HUP))
        return false;

    src->pfd.revents = 0;

    return true;
}

static void
cog_wl_src_finalize(GSource *base)
{
}

static gboolean
cog_wl_src_prepare(GSource *base, gint *timeout)
{
    struct wl_event_source *src = (struct wl_event_source *) base;

    *timeout = -1;

    while (wl_display_prepare_read(src->display) != 0) {
        if (wl_display_dispatch_pending(src->display) < 0)
            return false;
    }
    wl_display_flush(src->display);

    return false;
}

static GSource *
cog_wl_src_setup(GMainContext *main_context, struct wl_display *display)
{
    static GSourceFuncs wl_src_funcs = {
        .prepare = cog_wl_src_prepare,
        .check = cog_wl_src_check,
        .dispatch = cog_wl_src_dispatch,
        .finalize = cog_wl_src_finalize,
    };

    struct wl_event_source *wl_source =
        (struct wl_event_source *) g_source_new(&wl_src_funcs, sizeof(struct wl_event_source));
    wl_source->display = display;
    wl_source->pfd.fd = wl_display_get_fd(display);
    wl_source->pfd.events = G_IO_IN | G_IO_ERR | G_IO_HUP;
    wl_source->pfd.revents = 0;
    g_source_add_poll(&wl_source->source, &wl_source->pfd);

    g_source_set_can_recurse(&wl_source->source, TRUE);
    g_source_attach(&wl_source->source, main_context);

    g_source_unref(&wl_source->source);

    return &wl_source->source;
}

static void
cog_wl_terminate(CogWlPlatform *platform)
{
    CogWlDisplay *display = platform->display;

    g_clear_pointer(&display->event_src, g_source_destroy);

    if (display->xdg_shell != NULL)
        xdg_wm_base_destroy(display->xdg_shell);
    if (display->fshell != NULL)
        zwp_fullscreen_shell_v1_destroy(display->fshell);
    if (display->shell != NULL)
        wl_shell_destroy(display->shell);

    g_clear_pointer(&display->shm, wl_shm_destroy);
    g_clear_pointer(&display->subcompositor, wl_subcompositor_destroy);
    g_clear_pointer(&display->compositor, wl_compositor_destroy);

#if COG_ENABLE_WESTON_CONTENT_PROTECTION
    g_clear_pointer(&display->protection, weston_content_protection_destroy);
#endif

#if COG_ENABLE_WESTON_DIRECT_DISPLAY
    g_clear_pointer(&display->direct_display, weston_direct_display_v1_destroy);
#endif

#ifdef COG_USE_WAYLAND_CURSOR
    g_clear_pointer(&display->cursor_left_ptr_surface, wl_surface_destroy);
    g_clear_pointer(&display->cursor_theme, wl_cursor_theme_destroy);
#endif /* COG_USE_WAYLAND_CURSOR */

    wl_registry_destroy(display->registry);
    cog_wl_display_destroy(display);
}

static void
dispatch_axis_event(CogWlSeat *seat)
{
    CogWlWindow *window = seat->pointer_target;
    if (!window) {
        g_critical("%s: No current pointer event target!", G_STRFUNC);
        return;
    }

    if (!window->axis.has_delta)
        return;

    struct wpe_input_axis_2d_event event = {
        0,
    };
    event.base.type = wpe_input_axis_event_type_mask_2d | wpe_input_axis_event_type_motion_smooth;
    event.base.time = window->axis.time;
    event.base.x = window->pointer.x * s_platform->current_output->scale;
    event.base.y = window->pointer.y * s_platform->current_output->scale;

    event.x_axis = wl_fixed_to_double(window->axis.x_delta) * s_platform->current_output->scale;
    event.y_axis = -wl_fixed_to_double(window->axis.y_delta) * s_platform->current_output->scale;

    if (s_platform->views) {
        CogView *view = cog_view_stack_get_visible_view(s_platform->views);
        if (view)
            wpe_view_backend_dispatch_axis_event(cog_view_get_backend(view), &event.base);
    }

    window->axis.has_delta = false;
    window->axis.time = 0;
    window->axis.x_delta = window->axis.y_delta = 0;
}

#if COG_ENABLE_WESTON_DIRECT_DISPLAY
static void
dmabuf_on_surface_frame(void *data, struct wl_callback *callback, uint32_t time)
{
    // for WAYLAND_DEBUG=1 purposes only
    wl_callback_destroy(callback);
}

static void
dmabuf_on_buffer_release(void *data, struct wl_buffer *buffer)
{
    struct video_buffer *data_buffer = data;
    if (data_buffer->fd >= 0)
        close(data_buffer->fd);

    if (data_buffer->dmabuf_export)
        wpe_video_plane_display_dmabuf_export_release(data_buffer->dmabuf_export);

    g_slice_free(struct video_buffer, data_buffer);
    g_clear_pointer(&buffer, wl_buffer_destroy);
}
#endif /* COG_ENABLE_WESTON_DIRECT_DISPLAY */

static void
noop()
{
}

static void
check_supported_registry_on_global(void               *data,
                                   struct wl_registry *registry,
                                   uint32_t            name,
                                   const char         *interface,
                                   uint32_t            version)
{
    struct cog_wl_check_supported_protocols *protocols = data;

#define TRY_MATCH_PROTOCOL_ENTRY(proto)                   \
    if (strcmp(interface, proto##_interface.name) == 0) { \
        protocols->found_##proto = TRUE;                  \
        return;                                           \
    }

    SHELL_PROTOCOLS(TRY_MATCH_PROTOCOL_ENTRY)

#undef TRY_MATCH_PROTOCOL_ENTRY
}

static void
output_on_done(void *data, struct wl_output *output)
{
    CogWlPlatform *platform = data;
    CogWlView     *view = COG_WL_VIEW(cog_view_stack_get_visible_view(platform->views));
    CogWlWindow   *window = cog_wl_view_get_window(view);

    CogWlOutput *metrics = cog_wl_platform_get_output(output);

    if (!metrics->refresh) {
        g_warning("No refresh rate reported for output %p, using 60Hz", output);
        metrics->refresh = 60 * 1000;
    }

    if (!metrics->scale) {
        g_warning("No scale factor reported for output %p, using 1x", output);
        metrics->scale = 1;
    }

    g_info("Output %p is %" PRId32 "x%" PRId32 "-%" PRIi32 "x @ %.2fHz", output, metrics->width, metrics->height,
           metrics->scale, metrics->refresh / 1000.f);

    if (!platform->current_output) {
        g_debug("%s: Using %p as initial output", G_STRFUNC, output);
        platform->current_output = metrics;
    }

    if (view && window->should_cog_wl_platform_resize_to_largest_output) {
        cog_wl_platform_resize_to_largest_output(platform);
    }
}

static void
output_on_mode(void *data, struct wl_output *output, uint32_t flags, int32_t width, int32_t height, int32_t refresh)
{
    CogWlOutput *metrics = cog_wl_platform_get_output(output);

    if (flags & WL_OUTPUT_MODE_CURRENT) {
        metrics->width = width;
        metrics->height = height;
        metrics->refresh = refresh;
    }
}
#ifdef WL_OUTPUT_SCALE_SINCE_VERSION

static void
output_on_scale(void *data, struct wl_output *output, int32_t scale)
{
    cog_wl_platform_get_output(output)->scale = scale;
}
#endif /* WL_OUTPUT_SCALE_SINCE_VERSION */

/*
 * Wayland reports axis events as being 15 units per scroll wheel step. Scale
 * values in order to match the 120 value used in the X11 plug-in and by the
 * libinput_event_pointer_get_scroll_value_v120() function.
 */
enum {
    SCROLL_WHEEL_STEP_SCALING_FACTOR = 8,
};

static void
pointer_on_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    CogWlSeat *seat = data;

    if (pointer != seat->pointer_obj) {
        g_critical("%s: Got pointer %p, expected %p.", G_STRFUNC, pointer, seat->pointer_obj);
        return;
    }

    CogWlWindow *window = seat->pointer_target;
    if (!window) {
        g_critical("%s: No current pointer event target!", G_STRFUNC);
        return;
    }

    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        window->axis.has_delta = true;
        window->axis.time = time;
        window->axis.y_delta += value * SCROLL_WHEEL_STEP_SCALING_FACTOR;
    }

    if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
        window->axis.has_delta = true;
        window->axis.time = time;
        window->axis.x_delta += value * SCROLL_WHEEL_STEP_SCALING_FACTOR;
    }

    if (!pointer_uses_frame_event(pointer))
        dispatch_axis_event(seat);
}

static void
pointer_on_button(void              *data,
                  struct wl_pointer *pointer,
                  uint32_t           serial,
                  uint32_t           time,
                  uint32_t           button,
                  uint32_t           state)
{
    CogWlSeat *seat = data;
    if (pointer != seat->pointer_obj) {
        g_critical("%s: Got pointer %p, expected %p.", G_STRFUNC, pointer, seat->pointer_obj);
        return;
    }

    CogWlWindow *window = seat->pointer_target;
    if (!window) {
        g_critical("%s: No current pointer event target!", G_STRFUNC);
        return;
    }

    seat->keyboard.serial = serial;

    /* @FIXME: what is this for?
    if (button >= BTN_MOUSE)
        button = button - BTN_MOUSE + 1;
    else
        button = 0;
    */

    window->pointer.button = !!state ? button : 0;
    window->pointer.state = state;

    struct wpe_input_pointer_event event = {
        wpe_input_pointer_event_type_button,
        time,
        window->pointer.x * s_platform->current_output->scale,
        window->pointer.y * s_platform->current_output->scale,
        window->pointer.button,
        window->pointer.state,
    };

    if (window->popup_data.wl_surface) {
        if (window->pointer.surface == window->popup_data.wl_surface) {
            cog_popup_menu_handle_event(
                window->popup_data.popup_menu,
                !!state ? COG_POPUP_MENU_EVENT_STATE_PRESSED : COG_POPUP_MENU_EVENT_STATE_RELEASED, event.x, event.y);
            cog_wl_window_popup_update(window);
            return;
        } else {
            if (!!state)
                cog_wl_window_popup_destroy(window);
        }
    }

    CogView *view = cog_view_stack_get_visible_view(s_platform->views);
    if (!view)
        return;

    wpe_view_backend_dispatch_pointer_event(cog_view_get_backend(view), &event);
}

static void
pointer_on_enter(void              *data,
                 struct wl_pointer *pointer,
                 uint32_t           serial,
                 struct wl_surface *surface,
                 wl_fixed_t         fixed_x,
                 wl_fixed_t         fixed_y)
{
    CogWlSeat *seat = data;
    if (pointer != seat->pointer_obj) {
        g_critical("%s: Got pointer %p, expected %p.", G_STRFUNC, pointer, seat->pointer_obj);
        return;
    }

    CogWlWindow *window = wl_surface_get_user_data(surface);
    g_assert(window);
    CogWlDisplay *display = s_platform->display;
    g_assert(display);

    seat->pointer_target = window;
    seat->keyboard.serial = serial;
    window->pointer.surface = NULL;

#ifdef COG_USE_WAYLAND_CURSOR
    if (display->cursor_left_ptr) {
        /*
         * TODO: Take the output device scaling into account and load
         *       a cursor image of the appropriate size, if possible.
         */
        if (!display->cursor_left_ptr_surface) {
            struct wl_buffer *buffer = wl_cursor_image_get_buffer(display->cursor_left_ptr->images[0]);
            if (buffer) {
                struct wl_surface *surface = wl_compositor_create_surface(display->compositor);
                wl_surface_attach(surface, buffer, 0, 0);
                wl_surface_damage(surface, 0, 0, display->cursor_left_ptr->images[0]->width,
                                  display->cursor_left_ptr->images[0]->height);
                wl_surface_commit(surface);
                display->cursor_left_ptr_surface = surface;
            }
        }
        wl_pointer_set_cursor(seat->pointer_obj,
                              serial,
                              display->cursor_left_ptr_surface,
                              display->cursor_left_ptr->images[0]->hotspot_x,
                              display->cursor_left_ptr->images[0]->hotspot_y);
    }
#endif /* COG_USE_WAYLAND_CURSOR */
}

#ifdef WL_POINTER_FRAME_SINCE_VERSION
static void
pointer_on_frame(void *data, struct wl_pointer *pointer)
{
    /* @FIXME: buffer pointer events and handle them in frame. That's the
     * recommended usage of this interface.
     */

    CogWlSeat *seat = data;
    dispatch_axis_event(seat);
}
#endif /* WL_POINTER_FRAME_SINCE_VERSION */

static void
pointer_on_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface)
{
    CogWlSeat *seat = data;

    if (pointer != seat->pointer_obj) {
        g_critical("%s: Got pointer %p, expected %p.", G_STRFUNC, pointer, seat->pointer_obj);
        return;
    }

    CogWlWindow *window = seat->pointer_target;
    if (!window) {
        g_critical("%s: No current pointer event target!", G_STRFUNC);
        return;
    }

    if (surface != seat->pointer_target->wl_surface) {
        g_critical("%s: Got leave for surface %p, but current window %p "
                   "has surface %p.",
                   G_STRFUNC, surface, seat->pointer_target, seat->pointer_target->wl_surface);
        return;
    }

    seat->keyboard.serial = serial;
    window->pointer.surface = NULL;
}

static void
pointer_on_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t fixed_x, wl_fixed_t fixed_y)
{
    CogWlSeat *seat = data;

    if (pointer != seat->pointer_obj) {
        g_critical("%s: Got pointer %p, expected %p.", G_STRFUNC, pointer, seat->pointer_obj);
        return;
    }

    CogWlWindow *window = seat->pointer_target;
    if (!window) {
        g_critical("%s: No current pointer event target!", G_STRFUNC);
        return;
    }

    window->pointer.x = wl_fixed_to_int(fixed_x);
    window->pointer.y = wl_fixed_to_int(fixed_y);

    CogView *view = cog_view_stack_get_visible_view(s_platform->views);
    if (!view)
        return;

    struct wpe_input_pointer_event event = {wpe_input_pointer_event_type_motion,
                                            time,
                                            window->pointer.x * s_platform->current_output->scale,
                                            window->pointer.y * s_platform->current_output->scale,
                                            window->pointer.button,
                                            window->pointer.state};
    wpe_view_backend_dispatch_pointer_event(cog_view_get_backend(view), &event);
}

static inline bool
pointer_uses_frame_event(struct wl_pointer *pointer)
{
#ifdef WL_POINTER_FRAME_SINCE_VERSION
    return wl_pointer_get_version(pointer) >= WL_POINTER_FRAME_SINCE_VERSION;
#else
    return false;
#endif
}

static void
registry_on_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    CogWlPlatform *platform = data;
    CogWlDisplay  *display = platform->display;
    gboolean       interface_used = TRUE;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        /* Version 3 introduced wl_surface_set_buffer_scale() */
        display->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, MIN(3, version));
    } else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
        display->subcompositor = wl_registry_bind(registry, name, &wl_subcompositor_interface, 1);
    } else if (strcmp(interface, wl_shell_interface.name) == 0) {
        display->shell = wl_registry_bind(registry, name, &wl_shell_interface, 1);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        display->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        display->xdg_shell = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        g_assert(display->xdg_shell);
        static const struct xdg_wm_base_listener xdg_shell_listener = {
            .ping = xdg_shell_on_ping,
        };
        xdg_wm_base_add_listener(display->xdg_shell, &xdg_shell_listener, NULL);
    } else if (strcmp(interface, zwp_fullscreen_shell_v1_interface.name) == 0) {
        display->fshell = wl_registry_bind(registry, name, &zwp_fullscreen_shell_v1_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        uint32_t        seat_version = MIN(version, WL_POINTER_FRAME_SINCE_VERSION);
        struct wl_seat *wl_seat = wl_registry_bind(registry, name, &wl_seat_interface, seat_version);
        cog_wl_display_add_seat(display, wl_seat, name, seat_version);
#if COG_ENABLE_WESTON_DIRECT_DISPLAY
    } else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
        if (version < 3) {
            g_warning("Version %d of the zwp_linux_dmabuf_v1 protocol is not supported", version);
            return;
        }
        display->dmabuf = wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, 3);
    } else if (strcmp(interface, weston_direct_display_v1_interface.name) == 0) {
        display->direct_display = wl_registry_bind(registry, name, &weston_direct_display_v1_interface, 1);
#endif /* COG_ENABLE_WESTON_DIRECT_DISPLAY */
#if COG_ENABLE_WESTON_CONTENT_PROTECTION
    } else if (strcmp(interface, weston_content_protection_interface.name) == 0) {
        display->protection = wl_registry_bind(registry, name, &weston_content_protection_interface, 1);
#endif /* COG_ENABLE_WESTON_CONTENT_PROTECTION */
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        static const struct wl_output_listener output_listener = {
            .geometry = noop,
            .mode = output_on_mode,
            .done = output_on_done,
#ifdef WL_OUTPUT_SCALE_SINCE_VERSION
            .scale = output_on_scale,
#endif /* WL_OUTPUT_SCALE_SINCE_VERSION */
        };
        /* Version 2 introduced the wl_output_listener::scale. */
        CogWlOutput *item = g_new0(CogWlOutput, 1);
        item->output = wl_registry_bind(registry, name, &wl_output_interface, MIN(2, version));
        item->name = name;
        item->scale = 1;
        wl_list_init(&item->link);
        wl_list_insert(&platform->outputs, &item->link);
        wl_output_add_listener(item->output, &output_listener, platform);
    } else if (strcmp(interface, zwp_text_input_manager_v3_interface.name) == 0) {
        display->text_input_manager = wl_registry_bind(registry, name, &zwp_text_input_manager_v3_interface, 1);
    } else if (strcmp(interface, zwp_text_input_manager_v1_interface.name) == 0) {
        display->text_input_manager_v1 = wl_registry_bind(registry, name, &zwp_text_input_manager_v1_interface, 1);
    } else if (strcmp(interface, wp_presentation_interface.name) == 0) {
        display->presentation = wl_registry_bind(registry, name, &wp_presentation_interface, 1);
    } else {
        interface_used = FALSE;
    }
    g_debug("%s '%s' interface obtained from the Wayland registry.", interface_used ? "Using" : "Ignoring", interface);
}

static void
registry_on_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    CogWlPlatform *platform = data;
    CogWlOutput   *output, *tmp_output;
    wl_list_for_each_safe(output, tmp_output, &platform->outputs, link) {
        if (output->name == name) {
            g_debug("%s: output #%" PRIi32 " @ %p removed.", G_STRFUNC, output->name, output->output);
            g_clear_pointer(&output->output, wl_output_release);
            wl_list_remove(&output->link);
            if (platform->current_output == output) {
                platform->current_output =
                    wl_list_empty(&platform->outputs) ? NULL : wl_container_of(platform->outputs.next, output, link);
            }
            g_clear_pointer(&output, g_free);
            return;
        }
    }

    CogWlSeat *seat, *tmp_seat;
    wl_list_for_each_safe(seat, tmp_seat, &platform->display->seats, link) {
        if (seat->seat_name == name) {
            cog_wl_seat_destroy(seat);
            break;
        }
    }
}

static void
seat_on_capabilities(void *data, struct wl_seat *wl_seat, uint32_t capabilities)
{
    CogWlSeat *cog_wl_seat = data;

    g_debug("Enumerating seat capabilities:");

    /* Pointer */
    const bool has_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
    if (has_pointer && cog_wl_seat->pointer_obj == NULL) {
        static const struct wl_pointer_listener pointer_listener = {
            .enter = pointer_on_enter,
            .leave = pointer_on_leave,
            .motion = pointer_on_motion,
            .button = pointer_on_button,
            .axis = pointer_on_axis,
#ifdef WL_POINTER_FRAME_SINCE_VERSION
            .frame = pointer_on_frame,
            .axis_source = noop,
            .axis_stop = noop,
            .axis_discrete = noop,
#endif /* WL_POINTER_FRAME_SINCE_VERSION */
        };
        cog_wl_seat->pointer_obj = wl_seat_get_pointer(cog_wl_seat->seat);
        g_assert(cog_wl_seat->pointer_obj);
        wl_pointer_add_listener(cog_wl_seat->pointer_obj, &pointer_listener, cog_wl_seat);
        g_debug("  - Pointer");
    } else if (!has_pointer && cog_wl_seat->pointer_obj != NULL) {
        wl_pointer_release(cog_wl_seat->pointer_obj);
        cog_wl_seat->pointer_obj = NULL;
    }

    /* Keyboard */
    const bool has_keyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;
    if (has_keyboard && cog_wl_seat->keyboard_obj == NULL) {
        cog_wl_seat->keyboard_obj = wl_seat_get_keyboard(cog_wl_seat->seat);
        g_assert(cog_wl_seat->keyboard_obj);
        static const struct wl_keyboard_listener keyboard_listener = {
            .keymap = cog_wl_seat_keyboard_on_keymap,
            .enter = cog_wl_seat_keyboard_on_enter,
            .leave = cog_wl_seat_keyboard_on_leave,
            .key = cog_wl_seat_keyboard_on_key,
            .modifiers = cog_wl_seat_keyboard_on_modifiers,
            .repeat_info = cog_wl_seat_keyboard_on_repeat_info,
        };
        wl_keyboard_add_listener(cog_wl_seat->keyboard_obj, &keyboard_listener, cog_wl_seat);
        g_debug("  - Keyboard");
    } else if (!has_keyboard && cog_wl_seat->keyboard_obj != NULL) {
        wl_keyboard_release(cog_wl_seat->keyboard_obj);
        cog_wl_seat->keyboard_obj = NULL;
    }

    /* Touch */
    const bool has_touch = capabilities & WL_SEAT_CAPABILITY_TOUCH;
    if (has_touch && cog_wl_seat->touch_obj == NULL) {
        static const struct wl_touch_listener touch_listener = {
            .down = touch_on_down,
            .up = touch_on_up,
            .motion = touch_on_motion,
            .frame = touch_on_frame,
            .cancel = touch_on_cancel,
        };
        cog_wl_seat->touch_obj = wl_seat_get_touch(cog_wl_seat->seat);
        g_assert(cog_wl_seat->touch_obj);
        wl_touch_add_listener(cog_wl_seat->touch_obj, &touch_listener, cog_wl_seat);
        g_debug("  - Touch");
    } else if (!has_touch && cog_wl_seat->touch_obj != NULL) {
        wl_touch_release(cog_wl_seat->touch_obj);
        cog_wl_seat->touch_obj = NULL;
    }

    g_debug("Done enumerating seat capabilities.");
}

#ifdef WL_SEAT_NAME_SINCE_VERSION
static void
seat_on_name(void *data, struct wl_seat *seat, const char *name)
{
    g_debug("Seat name: '%s'", name);
}
#endif /* WL_SEAT_NAME_SINCE_VERSION */

static void
shell_surface_on_configure(void                    *data,
                           struct wl_shell_surface *shell_surface,
                           uint32_t                 edges,
                           int32_t                  width,
                           int32_t                  height)
{
    CogWlPlatform *platform = data;

    g_debug("New wl_shell configuration: (%" PRIu32 ", %" PRIu32 ")", width, height);

    cog_wl_platform_configure_surface_geometry(platform, width, height);
    cog_view_group_foreach(COG_VIEW_GROUP(platform->views), (GFunc) cog_wl_view_resize, platform);
}

static void
shell_surface_on_ping(void *data, struct wl_shell_surface *shell_surface, uint32_t serial)
{
    wl_shell_surface_pong(shell_surface, serial);
}

static void
surface_on_enter(void *data, struct wl_surface *surface, struct wl_output *output)
{
    if (s_platform->current_output->output != output) {
        g_debug("%s: Surface %p output changed %p -> %p", G_STRFUNC, surface, s_platform->current_output->output,
                output);
        s_platform->current_output = cog_wl_platform_get_output(output);
    }

#ifdef WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION
    const bool can_set_surface_scale = wl_surface_get_version(surface) >= WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION;
    if (can_set_surface_scale)
        wl_surface_set_buffer_scale(surface, s_platform->current_output->scale);
    else
        g_debug("%s: Surface %p uses old protocol version, cannot set scale factor", G_STRFUNC, surface);
#endif /* WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION */

    for (gsize i = 0; i < cog_view_group_get_n_views(COG_VIEW_GROUP(s_platform->views)); i++) {
        struct wpe_view_backend *backend =
            cog_view_get_backend(cog_view_group_get_nth_view(COG_VIEW_GROUP(s_platform->views), i));

#ifdef WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION
        if (can_set_surface_scale)
            wpe_view_backend_dispatch_set_device_scale_factor(backend, s_platform->current_output->scale);
#endif /* WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION */

#if HAVE_REFRESH_RATE_HANDLING
        wpe_view_backend_set_target_refresh_rate(backend, s_platform->current_output->refresh);
#endif /* HAVE_REFRESH_RATE_HANDLING */
    }
}

static void
touch_on_cancel(void *data, struct wl_touch *touch)
{
}

static void
touch_on_down(void              *data,
              struct wl_touch   *touch,
              uint32_t           serial,
              uint32_t           time,
              struct wl_surface *surface,
              int32_t            id,
              wl_fixed_t         x,
              wl_fixed_t         y)
{
    CogWlSeat *seat = data;

    if (touch != seat->touch_obj) {
        g_critical("%s: Got touch %p, expected %p.", G_STRFUNC, touch, seat->touch_obj);
        return;
    }

    CogWlWindow *window = wl_surface_get_user_data(surface);
    if (!window) {
        g_critical("%s: No current touch event target!", G_STRFUNC);
        return;
    }

    window->touch.surface = surface;
    seat->keyboard.serial = serial;

    if (id < 0 || id >= 10)
        return;

    struct wpe_input_touch_event_raw raw_event = {
        wpe_input_touch_event_type_down,
        time,
        id,
        wl_fixed_to_int(x) * s_platform->current_output->scale,
        wl_fixed_to_int(y) * s_platform->current_output->scale,
    };

    memcpy(&window->touch.points[id], &raw_event, sizeof(struct wpe_input_touch_event_raw));

    if (window->popup_data.wl_surface) {
        if (window->touch.surface == window->popup_data.wl_surface) {
            cog_popup_menu_handle_event(window->popup_data.popup_menu, COG_POPUP_MENU_EVENT_STATE_PRESSED, raw_event.x,
                                        raw_event.y);
            cog_wl_window_popup_update(window);
            return;
        } else
            cog_wl_window_popup_destroy(window);
    }

    CogView *view = cog_view_stack_get_visible_view(s_platform->views);
    if (!view)
        return;

    struct wpe_input_touch_event event = {window->touch.points, 10, raw_event.type, raw_event.id, raw_event.time};

    wpe_view_backend_dispatch_touch_event(cog_view_get_backend(view), &event);
}

static void
touch_on_frame(void *data, struct wl_touch *touch)
{
    /* @FIXME: buffer touch events and handle them here */
}

static void
touch_on_motion(void *data, struct wl_touch *touch, uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y)
{
    CogWlSeat *seat = data;

    if (touch != seat->touch_obj) {
        g_critical("%s: Got touch %p, expected %p.", G_STRFUNC, touch, seat->touch_obj);
        return;
    }

    CogWlWindow *window = seat->pointer_target;
    if (!window) {
        g_critical("%s: No current touch event target!", G_STRFUNC);
        return;
    }

    if (id < 0 || id >= 10)
        return;

    struct wpe_input_touch_event_raw raw_event = {
        wpe_input_touch_event_type_motion,
        time,
        id,
        wl_fixed_to_int(x) * s_platform->current_output->scale,
        wl_fixed_to_int(y) * s_platform->current_output->scale,
    };

    memcpy(&window->touch.points[id], &raw_event, sizeof(struct wpe_input_touch_event_raw));

    CogView *view = cog_view_stack_get_visible_view(s_platform->views);
    if (!view)
        return;

    struct wpe_input_touch_event event = {window->touch.points, 10, raw_event.type, raw_event.id, raw_event.time};

    wpe_view_backend_dispatch_touch_event(cog_view_get_backend(view), &event);
}

static void
touch_on_up(void *data, struct wl_touch *touch, uint32_t serial, uint32_t time, int32_t id)
{
    CogWlSeat *seat = data;

    if (touch != seat->touch_obj) {
        g_critical("%s: Got touch %p, expected %p.", G_STRFUNC, touch, seat->touch_obj);
        return;
    }

    CogWlWindow *window = seat->touch_target;
    if (!window) {
        g_critical("%s: No current touch event target!", G_STRFUNC);
        return;
    }

    struct wl_surface *target_surface = window->touch.surface;

    window->touch.surface = NULL;
    seat->keyboard.serial = serial;

    if (id < 0 || id >= 10)
        return;

    struct wpe_input_touch_event_raw raw_event = {
        wpe_input_touch_event_type_up, time, id, window->touch.points[id].x, window->touch.points[id].y,
    };

    if (window->popup_data.wl_surface) {
        if (target_surface == window->popup_data.wl_surface) {
            cog_popup_menu_handle_event(window->popup_data.popup_menu, COG_POPUP_MENU_EVENT_STATE_RELEASED, raw_event.x,
                                        raw_event.y);
            cog_wl_window_popup_update(window);

            memset(&window->touch.points[id], 0x00, sizeof(struct wpe_input_touch_event_raw));
            return;
        }
    }

    memcpy(&window->touch.points[id], &raw_event, sizeof(struct wpe_input_touch_event_raw));

    CogView *view = cog_view_stack_get_visible_view(s_platform->views);
    if (view) {
        struct wpe_input_touch_event event = {window->touch.points, 10, raw_event.type, raw_event.id, raw_event.time};
        wpe_view_backend_dispatch_touch_event(cog_view_get_backend(view), &event);
    }

    memset(&window->touch.points[id], 0x00, sizeof(struct wpe_input_touch_event_raw));
}

#if COG_ENABLE_WESTON_DIRECT_DISPLAY
static void
video_plane_display_dmabuf_receiver_on_handle_dmabuf(void                                         *data,
                                                     struct wpe_video_plane_display_dmabuf_export *dmabuf_export,
                                                     uint32_t                                      id,
                                                     int                                           fd,
                                                     int32_t                                       x,
                                                     int32_t                                       y,
                                                     int32_t                                       width,
                                                     int32_t                                       height,
                                                     uint32_t                                      stride)
{
    CogWlPlatform *platform = data;
    CogWlDisplay  *display = platform->display;
    CogView       *view = cog_view_stack_get_visible_view(platform->views);
    CogWlWindow   *window = cog_wl_view_get_window(COG_WL_VIEW(view));

    if (fd < 0)
        return;

    if (!display->dmabuf) {
        // TODO: Replace with g_warning_once() after bumping our GLib requirement.
        static bool warning_emitted = false;
        if (!warning_emitted) {
            g_warning("DMABuf not supported by the compositor. Video won't be rendered");
            warning_emitted = true;
        }
        return;
    }

    static const struct zwp_linux_buffer_params_v1_listener params_listener = {.created =
                                                                                   video_surface_create_succeeded,
                                                                               .failed = video_surface_create_failed};

    uint64_t                           modifier = DRM_FORMAT_MOD_INVALID;
    struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(display->dmabuf);
    if (display->direct_display != NULL)
        weston_direct_display_v1_enable(display->direct_display, params);

    struct video_surface *surf =
        (struct video_surface *) g_hash_table_lookup(display->video_surfaces, GUINT_TO_POINTER(id));
    if (!surf) {
        surf = g_slice_new0(struct video_surface);
        surf->wl_subsurface = NULL;
        surf->wl_surface = wl_compositor_create_surface(display->compositor);

#    if COG_ENABLE_WESTON_CONTENT_PROTECTION
        if (display->protection) {
            surf->protected_surface = weston_content_protection_get_protection(display->protection, surf->wl_surface);
            //weston_protected_surface_set_type(surf->protected_surface, WESTON_PROTECTED_SURFACE_TYPE_DC_ONLY);

            weston_protected_surface_enforce(surf->protected_surface);
        }
#    endif
        g_hash_table_insert(display->video_surfaces, GUINT_TO_POINTER(id), surf);
    }

    zwp_linux_buffer_params_v1_add(params, fd, 0, 0, stride, modifier >> 32, modifier & 0xffffffff);

    if ((x + width) > window->width)
        width -= x;

    if ((y + height) > window->height)
        height -= y;

    struct video_buffer *buffer = g_slice_new0(struct video_buffer);
    buffer->fd = fd;
    buffer->x = x;
    buffer->y = y;
    buffer->width = width;
    buffer->height = height;
    zwp_linux_buffer_params_v1_add_listener(params, &params_listener, buffer);

    buffer->buffer =
        zwp_linux_buffer_params_v1_create_immed(params, buffer->width, buffer->height, VIDEO_BUFFER_FORMAT, 0);
    zwp_linux_buffer_params_v1_destroy(params);

    buffer->dmabuf_export = dmabuf_export;

    static const struct wl_buffer_listener dmabuf_buffer_listener = {
        .release = dmabuf_on_buffer_release,
    };

    wl_buffer_add_listener(buffer->buffer, &dmabuf_buffer_listener, buffer);

    wl_surface_attach(surf->wl_surface, buffer->buffer, 0, 0);
    wl_surface_damage(surf->wl_surface, 0, 0, buffer->width, buffer->height);

    static const struct wl_callback_listener dmabuf_frame_listener = {
        .done = dmabuf_on_surface_frame,
    };

    struct wl_callback *callback = wl_surface_frame(surf->wl_surface);
    wl_callback_add_listener(callback, &dmabuf_frame_listener, NULL);

    if (!surf->wl_subsurface) {
        surf->wl_subsurface =
            wl_subcompositor_get_subsurface(display->subcompositor, surf->wl_surface, window->wl_surface);
        wl_subsurface_set_sync(surf->wl_subsurface);
    }

    wl_subsurface_set_position(surf->wl_subsurface, buffer->x, buffer->y);
    wl_surface_commit(surf->wl_surface);
}

static void
video_plane_display_dmabuf_receiver_on_end_of_stream(void *data, uint32_t id)
{
    CogWlPlatform *platform = data;

    g_hash_table_remove(platform->display->video_surfaces, GUINT_TO_POINTER(id));
}

static void
video_surface_create_succeeded(void *data, struct zwp_linux_buffer_params_v1 *params, struct wl_buffer *new_buffer)
{
    zwp_linux_buffer_params_v1_destroy(params);
    struct video_buffer *buffer = data;
    buffer->buffer = new_buffer;
}

static void
video_surface_create_failed(void *data, struct zwp_linux_buffer_params_v1 *params)
{
    zwp_linux_buffer_params_v1_destroy(params);
    struct video_buffer *buffer = data;
    buffer->buffer = NULL;
}

static void
video_surface_destroy(gpointer data)
{
    struct video_surface *surface = (struct video_surface *) data;

#    if COG_ENABLE_WESTON_CONTENT_PROTECTION
    g_clear_pointer(&surface->protected_surface, weston_protected_surface_destroy);
#    endif
    g_clear_pointer(&surface->wl_subsurface, wl_subsurface_destroy);
    g_clear_pointer(&surface->wl_surface, wl_surface_destroy);
    g_slice_free(struct video_surface, surface);
}
#endif

static void
xdg_shell_on_ping(void *data, struct xdg_wm_base *shell, uint32_t serial)
{
    xdg_wm_base_pong(shell, serial);
}

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

static void
xdg_toplevel_on_configure(void                *data,
                          struct xdg_toplevel *toplevel,
                          int32_t              width,
                          int32_t              height,
                          struct wl_array     *states)
{
    CogWlPlatform *platform = data;

    g_debug("New XDG toplevel configuration: (%" PRIu32 ", %" PRIu32 ")", width, height);

    cog_wl_platform_configure_surface_geometry(platform, width, height);
    cog_view_group_foreach(COG_VIEW_GROUP(platform->views), (GFunc) cog_wl_view_resize, platform);
}

static void
xdg_toplevel_on_close(void *data, struct xdg_toplevel *xdg_toplevel)
{
    GApplication *app = g_application_get_default();
    if (app)
        g_application_quit(app);
}

G_MODULE_EXPORT void
g_io_cogplatform_wl_load(GIOModule *module)
{
    GTypeModule *type_module = G_TYPE_MODULE(module);
    cog_wl_platform_register_type(type_module);
    cog_wl_view_register_type_exported(type_module);
}

G_MODULE_EXPORT void
g_io_cogplatform_wl_unload(GIOModule *module G_GNUC_UNUSED)
{
}
