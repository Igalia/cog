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

#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <locale.h>

#include "../common/egl-proc-address.h"
#include "os-compatibility.h"

#include "cog-im-context-wl-v1.h"
#include "cog-im-context-wl.h"
#include "cog-popup-menu-wl.h"

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
#include <wayland-cursor.h>
#endif

#define DEFAULT_WIDTH  1024
#define DEFAULT_HEIGHT  768

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

struct _CogWlView {
    CogView parent;

    struct wpe_view_backend_exportable_fdo *exportable;
    struct wpe_fdo_egl_exported_image      *image;

    struct wl_callback *frame_callback;

    bool should_update_opaque_region;
};

G_DECLARE_FINAL_TYPE(CogWlView, cog_wl_view, COG, WL_VIEW, CogView)
G_DEFINE_DYNAMIC_TYPE(CogWlView, cog_wl_view, COG_TYPE_VIEW)

typedef struct output_metrics {
    struct wl_output *output;
    int32_t           name;
    int32_t           scale;
    int32_t           width;
    int32_t           height;
    int32_t           refresh;
} output_metrics;

static struct {
    struct wl_display       *display;
    struct wl_registry      *registry;
    struct wl_compositor    *compositor;
    struct wl_subcompositor *subcompositor;
    struct wl_shm           *shm;

    struct xdg_wm_base             *xdg_shell;
    struct zwp_fullscreen_shell_v1 *fshell;
    struct wl_shell                *shell;

    struct wl_seat *seat;
    uint32_t        event_serial;

#if COG_ENABLE_WESTON_DIRECT_DISPLAY
    struct zwp_linux_dmabuf_v1      *dmabuf;
    struct weston_direct_display_v1 *direct_display;
#endif

#if COG_ENABLE_WESTON_CONTENT_PROTECTION
    struct weston_content_protection *protection;
#endif

#ifdef COG_USE_WAYLAND_CURSOR
    struct wl_cursor_theme *cursor_theme;
    struct wl_cursor       *cursor_left_ptr;
    struct wl_surface      *cursor_left_ptr_surface;
#endif /* COG_USE_WAYLAND_CURSOR */

    struct output_metrics  metrics[16];
    struct output_metrics *current_output;

    struct zwp_text_input_manager_v3 *text_input_manager;
    struct zwp_text_input_manager_v1 *text_input_manager_v1;

    struct wp_presentation *presentation;

    struct {
        struct wl_pointer *obj;
        struct wl_surface *surface;
        int32_t            x;
        int32_t            y;
        uint32_t           button;
        uint32_t           state;
    } pointer;

    struct {
        bool       has_delta;
        uint32_t   time;
        wl_fixed_t x_delta;
        wl_fixed_t y_delta;
    } axis;

    struct {
        struct wl_keyboard *obj;

        struct {
            int32_t rate;
            int32_t delay;
        } repeat_info;

        struct {
            uint32_t key;
            uint32_t time;
            uint32_t state;
            uint32_t event_source;
        } repeat_data;
    } keyboard;

    struct {
        struct wl_touch                 *obj;
        struct wl_surface               *surface;
        struct wpe_input_touch_event_raw points[10];
    } touch;

    GSource *event_src;

    struct wl_list shm_buffer_list;
} wl_data = {};

static struct {
    struct wl_surface *wl_surface;

#if COG_ENABLE_WESTON_DIRECT_DISPLAY
    GHashTable *video_surfaces;
#endif

    struct xdg_surface      *xdg_surface;
    struct xdg_toplevel     *xdg_toplevel;
    struct wl_shell_surface *shell_surface;

    uint32_t width;
    uint32_t height;
    uint32_t width_before_fullscreen;
    uint32_t height_before_fullscreen;

    bool is_fullscreen;
#if HAVE_FULLSCREEN_HANDLING
    bool was_fullscreen_requested_from_dom;
#endif
    bool is_resizing_fullscreen;
    bool is_maximized;
    bool should_resize_to_largest_output;
} win_data = {
    .width = DEFAULT_WIDTH,
    .height = DEFAULT_HEIGHT,
    .width_before_fullscreen = DEFAULT_WIDTH,
    .height_before_fullscreen = DEFAULT_HEIGHT,
};

static void on_export_wl_egl_image(void *data, struct wpe_fdo_egl_exported_image *image);

#if HAVE_SHM_EXPORTED_BUFFER
static void on_export_shm_buffer(void *data, struct wpe_fdo_shm_exported_buffer *buffer);
#endif

static GSource *setup_wayland_event_source(GMainContext *main_context, struct wl_display *display);

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

    self->exportable = wpe_view_backend_exportable_fdo_egl_create(&client, self, win_data.width, win_data.height);

    struct wpe_view_backend *view_backend = wpe_view_backend_exportable_fdo_get_view_backend(self->exportable);

    /* TODO: IM support. */
#if 0
    if (wl_data.text_input_manager_v1 != NULL)
        cog_im_context_wl_v1_set_view_backend(wpe_view_data.backend);
#endif

    WebKitWebViewBackend *backend =
        webkit_web_view_backend_new(view_backend,
                                    (GDestroyNotify) wpe_view_backend_exportable_fdo_destroy,
                                    self->exportable);

    /* TODO: Fullscreen support. */
#if HAVE_FULLSCREEN_HANDLING
    wpe_view_backend_set_fullscreen_handler(wpe_view_data.backend, cog_wl_handle_dom_fullscreen_request, NULL);
#endif

    if (!wl_data.event_src) {
        wl_data.event_src = setup_wayland_event_source(g_main_context_get_thread_default(), wl_data.display);
    }

    return backend;
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

static void
cog_wl_view_class_init(CogWlViewClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = cog_wl_view_dispose;

    CogViewClass *view_class = COG_VIEW_CLASS(klass);
    view_class->create_backend = cog_wl_view_create_backend;
}

static void
cog_wl_view_class_finalize(CogWlViewClass *klass G_GNUC_UNUSED)
{
}

static void on_show_option_menu(WebKitWebView *, WebKitOptionMenu *, WebKitRectangle *, gpointer *);

static void
cog_wl_view_init(CogWlView *self)
{
    /* Always configure the opaque region on first display after creation. */
    self->should_update_opaque_region = true;

    g_signal_connect(self, "show-option-menu", G_CALLBACK(on_show_option_menu), NULL);
}

struct _CogWlPlatformClass {
    CogPlatformClass parent_class;
};

struct _CogWlPlatform {
    CogPlatform   parent;
    CogViewStack *views;
};

G_DECLARE_FINAL_TYPE(CogWlPlatform, cog_wl_platform, COG, WL_PLATFORM, CogPlatform)

G_DEFINE_DYNAMIC_TYPE_EXTENDED(
    CogWlPlatform,
    cog_wl_platform,
    COG_TYPE_PLATFORM,
    0,
    g_io_extension_point_implement(COG_MODULES_PLATFORM_EXTENSION_POINT, g_define_type_id, "wl", 500);)

#if COG_ENABLE_WESTON_DIRECT_DISPLAY
#    define VIDEO_BUFFER_FORMAT DRM_FORMAT_YUYV
struct video_buffer {
    struct wl_buffer *buffer;

    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    int fd;

    struct wpe_video_plane_display_dmabuf_export* dmabuf_export;
};

struct video_surface {
#    if COG_ENABLE_WESTON_CONTENT_PROTECTION
    struct weston_protected_surface *protected_surface;
#    endif
    struct wl_surface *wl_surface;
    struct wl_subsurface *wl_subsurface;
};
#endif

#if HAVE_SHM_EXPORTED_BUFFER
struct shm_buffer {
    struct wl_list link;
    struct wl_listener destroy_listener;

    struct wl_resource *buffer_resource;
    struct wpe_fdo_shm_exported_buffer *exported_buffer;

    struct wl_shm_pool *shm_pool;
    void *data;
    size_t size;
    struct wl_buffer *buffer;
};
#endif

#ifndef EGL_WL_create_wayland_buffer_from_image
typedef struct wl_buffer *(EGLAPIENTRYP PFNEGLCREATEWAYLANDBUFFERFROMIMAGEWL)(EGLDisplay dpy, EGLImageKHR image);
#endif

static struct {
    struct egl_display *display;
} egl_data;

static struct {
    struct wl_surface *wl_surface;

    struct xdg_positioner *xdg_positioner;
    struct xdg_surface *xdg_surface;
    struct xdg_popup *xdg_popup;

    struct wl_shell_surface *shell_surface;

    uint32_t width;
    uint32_t height;

    CogPopupMenu *popup_menu;
    WebKitOptionMenu *option_menu;

    bool configured;
} popup_data = {
    .configured = false,
};


static struct {
    struct xkb_context* context;
    struct xkb_keymap* keymap;
    struct xkb_state* state;

    struct xkb_compose_table* compose_table;
    struct xkb_compose_state* compose_state;

    struct {
        xkb_mod_index_t control;
        xkb_mod_index_t alt;
        xkb_mod_index_t shift;
    } indexes;
    uint8_t modifiers;
} xkb_data = {NULL, };

struct wl_event_source {
    GSource source;
    GPollFD pfd;
    struct wl_display *display;
};

static gboolean
wl_src_prepare(GSource *base, gint *timeout)
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

static gboolean
wl_src_check(GSource *base)
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
wl_src_dispatch(GSource *base, GSourceFunc callback, gpointer user_data)
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
wl_src_finalize(GSource *base)
{
}

static GSource *
setup_wayland_event_source(GMainContext *main_context, struct wl_display *display)
{
    static GSourceFuncs wl_src_funcs = {
        .prepare = wl_src_prepare,
        .check = wl_src_check,
        .dispatch = wl_src_dispatch,
        .finalize = wl_src_finalize,
    };

    struct wl_event_source *wl_source =
        (struct wl_event_source *) g_source_new(&wl_src_funcs, sizeof(struct wl_event_source));
    wl_source->display = display;
    wl_source->pfd.fd = wl_display_get_fd(display);
    wl_source->pfd.events = G_IO_IN | G_IO_ERR | G_IO_HUP;
    wl_source->pfd.revents = 0;
    g_source_add_poll(&wl_source->source, &wl_source->pfd);

    g_source_set_can_recurse(&wl_source->source, TRUE);
    g_source_attach(&wl_source->source, g_main_context_get_thread_default());

    g_source_unref(&wl_source->source);

    return &wl_source->source;
}

static void create_popup(WebKitOptionMenu *option_menu);
static void display_popup(void);
static void update_popup(void);
static void destroy_popup(void);

static void
configure_surface_geometry(CogWlPlatform *platform, int32_t width, int32_t height)
{
    const char *env_var;
    if (width == 0) {
        env_var = g_getenv("COG_PLATFORM_WL_VIEW_WIDTH");
        if (env_var != NULL)
            width = (int32_t) g_ascii_strtod(env_var, NULL);
        else
            width = DEFAULT_WIDTH;
    }
    if (height == 0) {
        env_var = g_getenv("COG_PLATFORM_WL_VIEW_HEIGHT");
        if (env_var != NULL)
            height = (int32_t) g_ascii_strtod(env_var, NULL);
        else
            height = DEFAULT_HEIGHT;
    }

    if (win_data.width != width || win_data.height != height) {
        g_debug("Configuring new size: %" PRId32 "x%" PRId32, width, height);
        win_data.width = width;
        win_data.height = height;

        for (gsize i = 0; i < cog_view_group_get_n_views(COG_VIEW_GROUP(platform->views)); i++) {
            CogView *view = cog_view_group_get_nth_view(COG_VIEW_GROUP(platform->views), i);
            COG_WL_VIEW(view)->should_update_opaque_region = true;
        }
    }
}

static void
view_resize(CogView *view)
{
    struct wpe_view_backend *backend = cog_view_get_backend(view);

    int32_t pixel_width = win_data.width * wl_data.current_output->scale;
    int32_t pixel_height = win_data.height * wl_data.current_output->scale;

    wpe_view_backend_dispatch_set_size(backend, win_data.width, win_data.height);
    wpe_view_backend_dispatch_set_device_scale_factor(backend, wl_data.current_output->scale);

    g_debug("%s<%p>: Resized EGL buffer to: (%" PRIi32 ", %" PRIi32 ") @%" PRIi32 "x", G_STRFUNC, view, pixel_width,
            pixel_height, wl_data.current_output->scale);
}

static void
shell_surface_ping(void *data, struct wl_shell_surface *shell_surface, uint32_t serial)
{
    wl_shell_surface_pong(shell_surface, serial);
}

static void
shell_surface_configure(void *data,
                        struct wl_shell_surface *shell_surface,
                        uint32_t edges,
                        int32_t width,
                        int32_t height)
{
    CogWlPlatform *platform = data;

    g_debug("New wl_shell configuration: (%" PRIu32 ", %" PRIu32 ")", width, height);

    configure_surface_geometry(platform, width, height);
    cog_view_group_foreach(COG_VIEW_GROUP(platform->views), (GFunc) view_resize, NULL);
}

static const struct wl_shell_surface_listener shell_surface_listener = {
    .ping = shell_surface_ping,
    .configure = shell_surface_configure,
};

static void
shell_popup_surface_ping(void *data, struct wl_shell_surface *shell_surface, uint32_t serial)
{
    wl_shell_surface_pong(shell_surface, serial);
}

static void
shell_popup_surface_configure(void *data,
                              struct wl_shell_surface *shell_surface,
                              uint32_t edges,
                              int32_t width,
                              int32_t height)
{
}

static void
shell_popup_surface_popup_done (void *data,
                                struct wl_shell_surface *shell_surface)
{
}

static const struct wl_shell_surface_listener shell_popup_surface_listener = {
    .ping = shell_popup_surface_ping,
    .configure = shell_popup_surface_configure,
    .popup_done = shell_popup_surface_popup_done,
};

static void
xdg_shell_ping(void *data, struct xdg_wm_base *shell, uint32_t serial)
{
    xdg_wm_base_pong(shell, serial);
}

static const struct xdg_wm_base_listener xdg_shell_listener = {
    .ping = xdg_shell_ping,
};

static void
xdg_surface_on_configure(void *data, struct xdg_surface *surface, uint32_t serial)
{
    xdg_surface_ack_configure(surface, serial);

    if (popup_data.xdg_surface == surface && !popup_data.configured) {
        popup_data.configured = true;
        display_popup();
    }
}

static const struct xdg_surface_listener xdg_surface_listener = {.configure = xdg_surface_on_configure};

static void
xdg_toplevel_on_configure(void *data,
                          struct xdg_toplevel *toplevel,
                          int32_t width,
                          int32_t height,
                          struct wl_array *states)
{
    CogWlPlatform *platform = data;

    g_debug("New XDG toplevel configuration: (%" PRIu32 ", %" PRIu32 ")", width, height);

    configure_surface_geometry(platform, width, height);
    cog_view_group_foreach(COG_VIEW_GROUP(platform->views), (GFunc) view_resize, NULL);
}

static void
xdg_toplevel_on_close(void *data, struct xdg_toplevel *xdg_toplevel)
{
    GApplication *app = g_application_get_default();
    if (app)
        g_application_quit(app);
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_on_configure,
    .close = xdg_toplevel_on_close,
};

static void
xdg_popup_on_configure(void *data, struct xdg_popup *xdg_popup, int32_t x, int32_t y, int32_t width, int32_t height)
{
}

static void
xdg_popup_on_popup_done(void *data, struct xdg_popup *xdg_popup)
{
    destroy_popup();
}

static const struct xdg_popup_listener xdg_popup_listener = {
    .configure = xdg_popup_on_configure,
    .popup_done = xdg_popup_on_popup_done,
};

static void
resize_to_largest_output(CogWlPlatform *platform)
{
    /* Find the largest output and resize the surface to match */
    int32_t width = 0;
    int32_t height = 0;
    for (int i = 0; i < G_N_ELEMENTS(wl_data.metrics); i++) {
        if (wl_data.metrics[i].output && wl_data.metrics[i].width * wl_data.metrics[i].height >= width * height) {
            width = wl_data.metrics[i].width;
            height = wl_data.metrics[i].height;
        }
    }
    configure_surface_geometry(platform, width, height);
    cog_view_group_foreach(COG_VIEW_GROUP(platform->views), (GFunc) view_resize, NULL);
}

static void
noop()
{
}

static struct output_metrics *
find_output(struct wl_output *output)
{
    for (int i = 0; i < G_N_ELEMENTS(wl_data.metrics); i++) {
        if (wl_data.metrics[i].output == output) {
            return &wl_data.metrics[i];
        }
    }
    g_warning("Unknown output %p\n", output);
    return NULL;
}

static void
output_handle_mode(void *data, struct wl_output *output, uint32_t flags, int32_t width, int32_t height, int32_t refresh)
{
    struct output_metrics *metrics = find_output(output);
    if (!metrics) {
        return;
    }

    if (flags & WL_OUTPUT_MODE_CURRENT) {
        metrics->width = width;
        metrics->height = height;
        metrics->refresh = refresh;
        g_info("Output %p is %" PRId32 "x%" PRId32 " @ %.2fHz", output, width, height, refresh / 1000.f);
    }
}

static void
output_handle_done(void *data, struct wl_output *output)
{
    CogWlPlatform *platform = data;

    struct output_metrics *metrics = find_output(output);

    if (!metrics->refresh) {
        g_warning("No refresh rate reported for output %p, using 60Hz", output);
        metrics->refresh = 60 * 1000;
    }

    if (!metrics->scale) {
        g_warning("No scale factor reported for output %p, using 1x", output);
        metrics->scale = 1;
    }

    if (!wl_data.current_output) {
        g_debug("%s: Using %p as initial output", G_STRFUNC, output);
        wl_data.current_output = metrics;
    }

    if (win_data.should_resize_to_largest_output) {
        resize_to_largest_output(platform);
    }
}

#ifdef WL_OUTPUT_SCALE_SINCE_VERSION
static void
output_handle_scale(void *data, struct wl_output *output, int32_t factor)
{
    struct output_metrics *metrics = find_output(output);
    if (!metrics) {
        return;
    }

    metrics->scale = factor;
    g_info("Got scale factor %i for output %p", factor, output);
}
#endif /* WL_OUTPUT_SCALE_SINCE_VERSION */

static bool
cog_wl_does_image_match_win_size(struct wpe_fdo_egl_exported_image *image)
{
    return image && wpe_fdo_egl_exported_image_get_width(image) == win_data.width &&
           wpe_fdo_egl_exported_image_get_height(image) == win_data.height;
}

static void
cog_wl_fullscreen_image_ready(CogWlView *view)
{
    if (wl_data.xdg_shell) {
        xdg_toplevel_set_fullscreen(win_data.xdg_toplevel, NULL);
    } else if (wl_data.shell) {
        wl_shell_surface_set_fullscreen(win_data.shell_surface, WL_SHELL_SURFACE_FULLSCREEN_METHOD_SCALE, 0, NULL);
    } else if (wl_data.fshell == NULL) {
        g_assert_not_reached();
    }

    win_data.is_resizing_fullscreen = false;
#if HAVE_FULLSCREEN_HANDLING
    if (win_data.was_fullscreen_requested_from_dom)
        wpe_view_backend_dispatch_did_enter_fullscreen(cog_view_get_backend(COG_VIEW(view)));
#endif
}

static bool
cog_wl_set_fullscreen(CogWlPlatform *self, bool fullscreen)
{
    if (win_data.is_resizing_fullscreen || win_data.is_fullscreen == fullscreen)
        return false;

    win_data.is_fullscreen = fullscreen;

    if (fullscreen) {
        // Resize the view_backend to the size of the screen.
        // Wait until a new exported image is reveived. See cog_wl_fullscreen_image_ready().
        win_data.is_resizing_fullscreen = true;
        win_data.width_before_fullscreen = win_data.width;
        win_data.height_before_fullscreen = win_data.height;
        resize_to_largest_output(self);
        CogView *view = cog_view_stack_get_visible_view(self->views);
        if (view && cog_wl_does_image_match_win_size(COG_WL_VIEW(view)->image))
            cog_wl_fullscreen_image_ready(COG_WL_VIEW(view));
    } else {
        if (wl_data.xdg_shell != NULL) {
            xdg_toplevel_unset_fullscreen(win_data.xdg_toplevel);
        } else if (wl_data.fshell != NULL) {
            configure_surface_geometry(self, win_data.width_before_fullscreen, win_data.height_before_fullscreen);
            cog_view_group_foreach(COG_VIEW_GROUP(self->views), (GFunc) view_resize, NULL);
        } else if (wl_data.shell != NULL) {
            wl_shell_surface_set_toplevel(win_data.shell_surface);
            configure_surface_geometry(self, win_data.width_before_fullscreen, win_data.height_before_fullscreen);
            cog_view_group_foreach(COG_VIEW_GROUP(self->views), (GFunc) view_resize, NULL);
        } else {
            g_assert_not_reached();
        }
#if HAVE_FULLSCREEN_HANDLING
        if (win_data.was_fullscreen_requested_from_dom) {
            CogView *view = cog_view_stack_get_visible_view(self->views);
            if (view)
                wpe_view_backend_dispatch_did_exit_fullscreen(cog_view_get_backend(view));
        }
        win_data.was_fullscreen_requested_from_dom = false;
#endif
    }

    for (gsize i = 0; i < cog_view_group_get_n_views(COG_VIEW_GROUP(self->views)); i++) {
        CogView *view = cog_view_group_get_nth_view(COG_VIEW_GROUP(self->views), i);
        COG_WL_VIEW(view)->should_update_opaque_region = true;
    }
    return true;
}

#if HAVE_FULLSCREEN_HANDLING
static bool
cog_wl_handle_dom_fullscreen_request(void *unused, bool fullscreen)
{
    win_data.was_fullscreen_requested_from_dom = true;
    if (fullscreen != win_data.is_fullscreen)
        return cog_wl_set_fullscreen(unused, fullscreen);

    // Handle situations where DOM fullscreen requests are mixed with system fullscreen commands (e.g F11)
    if (fullscreen)
        wpe_view_backend_dispatch_did_enter_fullscreen(wpe_view_data.backend);
    else
        wpe_view_backend_dispatch_did_exit_fullscreen(wpe_view_data.backend);

    return true;
}
#endif

static const struct wl_output_listener output_listener = {
    .geometry = noop,
    .mode = output_handle_mode,
    .done = output_handle_done,
#ifdef WL_OUTPUT_SCALE_SINCE_VERSION
    .scale = output_handle_scale,
#endif /* WL_OUTPUT_SCALE_SINCE_VERSION */
};

static void
surface_handle_enter(void *data, struct wl_surface *surface, struct wl_output *output)
{
    CogWlPlatform *platform = data;

    if (wl_data.current_output->output != output) {
        g_debug("%s: Surface %p output changed %p -> %p", G_STRFUNC, surface, wl_data.current_output->output, output);
        wl_data.current_output = find_output(output);
        g_assert(wl_data.current_output);
    }

#ifdef WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION
    const bool can_set_surface_scale = wl_surface_get_version(surface) >= WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION;
    if (can_set_surface_scale)
        wl_surface_set_buffer_scale(surface, wl_data.current_output->scale);
    else
        g_debug("%s: Surface %p uses old protocol version, cannot set scale factor", G_STRFUNC, surface);
#endif /* WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION */

    for (gsize i = 0; i < cog_view_group_get_n_views(COG_VIEW_GROUP(platform->views)); i++) {
        struct wpe_view_backend *backend =
            cog_view_get_backend(cog_view_group_get_nth_view(COG_VIEW_GROUP(platform->views), i));

#ifdef WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION
        if (can_set_surface_scale)
            wpe_view_backend_dispatch_set_device_scale_factor(backend, wl_data.current_output->scale);
#endif /* WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION */

#if HAVE_REFRESH_RATE_HANDLING
        wpe_view_backend_set_target_refresh_rate(backend, wl_data.current_output->refresh);
#endif /* HAVE_REFRESH_RATE_HANDLING */
    }
}

static const struct wl_surface_listener surface_listener = {
    .enter = surface_handle_enter,
    .leave = noop,
};

static void
registry_global (void               *data,
                 struct wl_registry *registry,
                 uint32_t            name,
                 const char         *interface,
                 uint32_t            version)
{
    CogWlPlatform *platform = data;
    gboolean interface_used = TRUE;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        /* Version 3 introduced wl_surface_set_buffer_scale() */
        wl_data.compositor = wl_registry_bind(registry, name, &wl_compositor_interface, MIN(3, version));
    } else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
        wl_data.subcompositor = wl_registry_bind(registry, name, &wl_subcompositor_interface, 1);
    } else if (strcmp(interface, wl_shell_interface.name) == 0) {
        wl_data.shell = wl_registry_bind(registry, name, &wl_shell_interface, 1);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        wl_data.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        wl_data.xdg_shell = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        g_assert(wl_data.xdg_shell);
        xdg_wm_base_add_listener(wl_data.xdg_shell, &xdg_shell_listener, NULL);
    } else if (strcmp(interface, zwp_fullscreen_shell_v1_interface.name) == 0) {
        wl_data.fshell = wl_registry_bind(registry, name, &zwp_fullscreen_shell_v1_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        wl_data.seat = wl_registry_bind(registry, name, &wl_seat_interface, MAX(3, MIN(version, 7)));
#if COG_ENABLE_WESTON_DIRECT_DISPLAY
    } else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
        if (version < 3) {
            g_warning("Version %d of the zwp_linux_dmabuf_v1 protocol is not supported", version);
            return;
        }
        wl_data.dmabuf = wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, 3);
    } else if (strcmp(interface, weston_direct_display_v1_interface.name) == 0) {
        wl_data.direct_display = wl_registry_bind(registry, name, &weston_direct_display_v1_interface, 1);
#endif /* COG_ENABLE_WESTON_DIRECT_DISPLAY */
#if COG_ENABLE_WESTON_CONTENT_PROTECTION
    } else if (strcmp(interface, weston_content_protection_interface.name) == 0) {
        wl_data.protection = wl_registry_bind(registry, name, &weston_content_protection_interface, 1);
#endif /* COG_ENABLE_WESTON_CONTENT_PROTECTION */
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        /* Version 2 introduced the wl_output_listener::scale. */
        struct wl_output *output = wl_registry_bind(registry, name, &wl_output_interface, MIN(2, version));
        wl_output_add_listener(output, &output_listener, platform);
        bool inserted = false;
        for (int i = 0; i < G_N_ELEMENTS(wl_data.metrics); i++) {
            if (wl_data.metrics[i].output == NULL) {
                wl_data.metrics[i].output = output;
                wl_data.metrics[i].name = name;
                inserted = true;
                break;
            }
        }
        if (!inserted) {
            g_warning("Exceeded %" G_GSIZE_FORMAT " connected outputs(!)", G_N_ELEMENTS(wl_data.metrics));
        }
    } else if (strcmp(interface, zwp_text_input_manager_v3_interface.name) == 0) {
        wl_data.text_input_manager = wl_registry_bind(registry, name, &zwp_text_input_manager_v3_interface, 1);
    } else if (strcmp(interface, zwp_text_input_manager_v1_interface.name) == 0) {
        wl_data.text_input_manager_v1 = wl_registry_bind(registry, name, &zwp_text_input_manager_v1_interface, 1);
    } else if (strcmp(interface, wp_presentation_interface.name) == 0) {
        wl_data.presentation = wl_registry_bind(registry, name, &wp_presentation_interface, 1);
    } else {
        interface_used = FALSE;
    }
    g_debug("%s '%s' interface obtained from the Wayland registry.", interface_used ? "Using" : "Ignoring", interface);
}

static void
pointer_on_enter(void *data,
                 struct wl_pointer *pointer,
                 uint32_t serial,
                 struct wl_surface *surface,
                 wl_fixed_t fixed_x,
                 wl_fixed_t fixed_y)
{
    wl_data.event_serial = serial;
    wl_data.pointer.surface = surface;

#ifdef COG_USE_WAYLAND_CURSOR
    if (wl_data.cursor_left_ptr) {
        /*
         * TODO: Take the output device scaling into account and load
         *       a cursor image of the appropriate size, if possible.
         */
        if (!wl_data.cursor_left_ptr_surface) {
            struct wl_buffer *buffer = wl_cursor_image_get_buffer(wl_data.cursor_left_ptr->images[0]);
            if (buffer) {
                struct wl_surface *surface = wl_compositor_create_surface(wl_data.compositor);
                wl_surface_attach(surface, buffer, 0, 0);
                wl_surface_damage(surface, 0, 0, wl_data.cursor_left_ptr->images[0]->width,
                                  wl_data.cursor_left_ptr->images[0]->height);
                wl_surface_commit(surface);
                wl_data.cursor_left_ptr_surface = surface;
            }
        }
        wl_pointer_set_cursor(wl_data.pointer.obj,
                              serial,
                              wl_data.cursor_left_ptr_surface,
                              wl_data.cursor_left_ptr->images[0]->hotspot_x,
                              wl_data.cursor_left_ptr->images[0]->hotspot_y);
    }
#endif /* COG_USE_WAYLAND_CURSOR */
}

static void
pointer_on_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface)
{
    wl_data.event_serial = serial;
    wl_data.pointer.surface = NULL;
}

static void
pointer_on_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t fixed_x, wl_fixed_t fixed_y)
{
    CogWlPlatform *platform = data;

    wl_data.pointer.x = wl_fixed_to_int(fixed_x);
    wl_data.pointer.y = wl_fixed_to_int(fixed_y);

    CogView *view = cog_view_stack_get_visible_view(platform->views);
    if (!view)
        return;

    struct wpe_input_pointer_event event = {wpe_input_pointer_event_type_motion,
                                            time,
                                            wl_data.pointer.x * wl_data.current_output->scale,
                                            wl_data.pointer.y * wl_data.current_output->scale,
                                            wl_data.pointer.button,
                                            wl_data.pointer.state};
    wpe_view_backend_dispatch_pointer_event(cog_view_get_backend(view), &event);
}

static void
pointer_on_button(void *data,
                  struct wl_pointer *pointer,
                  uint32_t serial,
                  uint32_t time,
                  uint32_t button,
                  uint32_t state)
{
    CogWlPlatform *platform = data;

    wl_data.event_serial = serial;

    /* @FIXME: what is this for?
    if (button >= BTN_MOUSE)
        button = button - BTN_MOUSE + 1;
    else
        button = 0;
    */

    wl_data.pointer.button = !!state ? button : 0;
    wl_data.pointer.state = state;

    struct wpe_input_pointer_event event = {
        wpe_input_pointer_event_type_button,
        time,
        wl_data.pointer.x * wl_data.current_output->scale,
        wl_data.pointer.y * wl_data.current_output->scale,
        wl_data.pointer.button,
        wl_data.pointer.state,
    };

    if (popup_data.wl_surface) {
        if (wl_data.pointer.surface == popup_data.wl_surface) {
            cog_popup_menu_handle_event(
                popup_data.popup_menu,
                !!state ? COG_POPUP_MENU_EVENT_STATE_PRESSED : COG_POPUP_MENU_EVENT_STATE_RELEASED, event.x, event.y);
            update_popup();
            return;
        } else {
            if (!!state)
                destroy_popup();
        }
    }

    CogView *view = cog_view_stack_get_visible_view(platform->views);
    if (!view)
        return;

    wpe_view_backend_dispatch_pointer_event(cog_view_get_backend(view), &event);
}

static void
dispatch_axis_event(CogWlPlatform *platform)
{
    if (!wl_data.axis.has_delta)
        return;

    struct wpe_input_axis_2d_event event = { 0, };
    event.base.type = wpe_input_axis_event_type_mask_2d | wpe_input_axis_event_type_motion_smooth;
    event.base.time = wl_data.axis.time;
    event.base.x = wl_data.pointer.x * wl_data.current_output->scale;
    event.base.y = wl_data.pointer.y * wl_data.current_output->scale;

    event.x_axis = wl_fixed_to_double(wl_data.axis.x_delta) * wl_data.current_output->scale;
    event.y_axis = -wl_fixed_to_double(wl_data.axis.y_delta) * wl_data.current_output->scale;

    CogView *view = cog_view_stack_get_visible_view(platform->views);
    if (view)
        wpe_view_backend_dispatch_axis_event(cog_view_get_backend(view), &event.base);

    wl_data.axis.has_delta = false;
    wl_data.axis.time = 0;
    wl_data.axis.x_delta = wl_data.axis.y_delta = 0;
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
    CogWlPlatform *platform = data;

    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        wl_data.axis.has_delta = true;
        wl_data.axis.time = time;
        wl_data.axis.y_delta += value * SCROLL_WHEEL_STEP_SCALING_FACTOR;
    }

    if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
        wl_data.axis.has_delta = true;
        wl_data.axis.time = time;
        wl_data.axis.x_delta += value * SCROLL_WHEEL_STEP_SCALING_FACTOR;
    }

    if (!pointer_uses_frame_event(pointer))
        dispatch_axis_event(platform);
}

#ifdef WL_POINTER_FRAME_SINCE_VERSION
static void
pointer_on_frame (void* data,
                  struct wl_pointer *pointer)
{
    /* @FIXME: buffer pointer events and handle them in frame. That's the
     * recommended usage of this interface.
     */

    CogWlPlatform *platform = data;
    dispatch_axis_event(platform);
}
#endif /* WL_POINTER_FRAME_SINCE_VERSION */

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

static void
keyboard_on_keymap (void *data,
                    struct wl_keyboard *wl_keyboard,
                    uint32_t format,
                    int32_t fd,
                    uint32_t size)
{
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close (fd);
        return;
    }

    const int map_mode = wl_seat_get_version(wl_data.seat) > 6 ? MAP_PRIVATE : MAP_SHARED;
    void* mapping = mmap (NULL, size, PROT_READ, map_mode, fd, 0);
    if (mapping == MAP_FAILED) {
        close (fd);
        return;
    }

    xkb_data.keymap =
        xkb_keymap_new_from_string (xkb_data.context,
                                    (char *) mapping,
                                    XKB_KEYMAP_FORMAT_TEXT_V1,
                                    XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap (mapping, size);
    close (fd);

    if (xkb_data.keymap == NULL)
        return;

    xkb_data.state = xkb_state_new (xkb_data.keymap);
    if (xkb_data.state == NULL)
        return;

    xkb_data.indexes.control = xkb_keymap_mod_get_index (xkb_data.keymap,
                                                         XKB_MOD_NAME_CTRL);
    xkb_data.indexes.alt = xkb_keymap_mod_get_index (xkb_data.keymap,
                                                     XKB_MOD_NAME_ALT);
    xkb_data.indexes.shift = xkb_keymap_mod_get_index (xkb_data.keymap,
                                                       XKB_MOD_NAME_SHIFT);
}

static void
keyboard_on_enter (void *data,
                   struct wl_keyboard *wl_keyboard,
                   uint32_t serial,
                   struct wl_surface *surface,
                   struct wl_array *keys)
{
    wl_data.event_serial = serial;
}

static void
keyboard_on_leave (void *data,
                   struct wl_keyboard *wl_keyboard,
                   uint32_t serial,
                   struct wl_surface *surface)
{
    wl_data.event_serial = serial;
}

static void
handle_key_event(CogWlPlatform *self, uint32_t key, uint32_t state, uint32_t time)
{
    CogView *view = cog_view_stack_get_visible_view(self->views);
    if (!view || xkb_data.state == NULL)
        return;

    uint32_t keysym = xkb_state_key_get_one_sym (xkb_data.state, key);
    uint32_t unicode = xkb_state_key_get_utf32 (xkb_data.state, key);

    /* TODO: Move as much as possible from fullscreen handling to common code. */
    if (cog_view_get_use_key_bindings(view) && state == WL_KEYBOARD_KEY_STATE_PRESSED && xkb_data.modifiers == 0 &&
        unicode == 0 && keysym == XKB_KEY_F11) {
#if HAVE_FULLSCREEN_HANDLING
        if (win_data.is_fullscreen && win_data.was_fullscreen_requested_from_dom) {
            wpe_view_backend_dispatch_request_exit_fullscreen(wpe_view_data.backend);
            return;
        }
#endif
        cog_wl_set_fullscreen(0, !win_data.is_fullscreen);
        return;
    }

    if (xkb_data.compose_state != NULL
            && state == WL_KEYBOARD_KEY_STATE_PRESSED
            && xkb_compose_state_feed (xkb_data.compose_state, keysym)
            == XKB_COMPOSE_FEED_ACCEPTED
            && xkb_compose_state_get_status (xkb_data.compose_state)
            == XKB_COMPOSE_COMPOSED) {
        keysym = xkb_compose_state_get_one_sym (xkb_data.compose_state);
    }

    struct wpe_input_keyboard_event event = {time, keysym, key, state == true, xkb_data.modifiers};

    cog_view_handle_key_event(view, &event);
}

static gboolean
repeat_delay_timeout(CogWlPlatform *self)
{
    handle_key_event(self,
                     wl_data.keyboard.repeat_data.key,
                     wl_data.keyboard.repeat_data.state,
                     wl_data.keyboard.repeat_data.time);

    wl_data.keyboard.repeat_data.event_source =
        g_timeout_add(wl_data.keyboard.repeat_info.rate, (GSourceFunc) repeat_delay_timeout, self);

    return G_SOURCE_REMOVE;
}

static void
keyboard_on_key (void *data,
                 struct wl_keyboard *wl_keyboard,
                 uint32_t serial,
                 uint32_t time,
                 uint32_t key,
                 uint32_t state)
{
    CogWlPlatform *self = data;

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

    wl_data.event_serial = serial;
    handle_key_event(self, key, state, time);

    if (wl_data.keyboard.repeat_info.rate == 0)
        return;

    if (state == WL_KEYBOARD_KEY_STATE_RELEASED
        && wl_data.keyboard.repeat_data.key == key) {
        if (wl_data.keyboard.repeat_data.event_source)
            g_source_remove (wl_data.keyboard.repeat_data.event_source);

        memset (&wl_data.keyboard.repeat_data,
                0x00,
                sizeof (wl_data.keyboard.repeat_data));
    } else if (xkb_data.keymap != NULL
               && state == WL_KEYBOARD_KEY_STATE_PRESSED
               && xkb_keymap_key_repeats (xkb_data.keymap, key)) {
        if (wl_data.keyboard.repeat_data.event_source)
            g_source_remove (wl_data.keyboard.repeat_data.event_source);

        wl_data.keyboard.repeat_data.key = key;
        wl_data.keyboard.repeat_data.time = time;
        wl_data.keyboard.repeat_data.state = state;
        wl_data.keyboard.repeat_data.event_source =
            g_timeout_add(wl_data.keyboard.repeat_info.delay, (GSourceFunc) repeat_delay_timeout, self);
    }
}

static void
keyboard_on_modifiers (void *data,
                       struct wl_keyboard *wl_keyboard,
                       uint32_t serial,
                       uint32_t mods_depressed,
                       uint32_t mods_latched,
                       uint32_t mods_locked,
                       uint32_t group)
{
    if (xkb_data.state == NULL)
        return;
    wl_data.event_serial = serial;

    xkb_state_update_mask (xkb_data.state,
                           mods_depressed,
                           mods_latched,
                           mods_locked,
                           0,
                           0,
                           group);

    xkb_data.modifiers = 0;
    uint32_t component
        = (XKB_STATE_MODS_DEPRESSED | XKB_STATE_MODS_LATCHED);

    if (xkb_state_mod_index_is_active (xkb_data.state,
                                       xkb_data.indexes.control,
                                       component)) {
        xkb_data.modifiers |= wpe_input_keyboard_modifier_control;
    }
    if (xkb_state_mod_index_is_active (xkb_data.state,
                                       xkb_data.indexes.alt,
                                       component)) {
        xkb_data.modifiers |= wpe_input_keyboard_modifier_alt;
    }
    if (xkb_state_mod_index_is_active (xkb_data.state,
                                       xkb_data.indexes.shift,
                                       component)) {
        xkb_data.modifiers |= wpe_input_keyboard_modifier_shift;
    }
}

static void
keyboard_on_repeat_info (void *data,
                         struct wl_keyboard *wl_keyboard,
                         int32_t rate,
                         int32_t delay)
{
    wl_data.keyboard.repeat_info.rate = rate;
    wl_data.keyboard.repeat_info.delay = delay;

    /* a rate of zero disables any repeating. */
    if (rate == 0 && wl_data.keyboard.repeat_data.event_source > 0) {
        g_source_remove(wl_data.keyboard.repeat_data.event_source);
        memset (&wl_data.keyboard.repeat_data,
                0x00,
                sizeof (wl_data.keyboard.repeat_data));
    }
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_on_keymap,
    .enter = keyboard_on_enter,
    .leave = keyboard_on_leave,
    .key = keyboard_on_key,
    .modifiers = keyboard_on_modifiers,
    .repeat_info = keyboard_on_repeat_info,
};

static void
touch_on_down (void *data,
               struct wl_touch *touch,
               uint32_t serial,
               uint32_t time,
               struct wl_surface *surface,
               int32_t id,
               wl_fixed_t x,
               wl_fixed_t y)
{
    CogWlPlatform *platform = data;

    wl_data.touch.surface = surface;
    wl_data.event_serial = serial;

    if (id < 0 || id >= 10)
        return;

    struct wpe_input_touch_event_raw raw_event = {
        wpe_input_touch_event_type_down,
        time,
        id,
        wl_fixed_to_int(x) * wl_data.current_output->scale,
        wl_fixed_to_int(y) * wl_data.current_output->scale,
    };

    memcpy (&wl_data.touch.points[id],
            &raw_event,
            sizeof (struct wpe_input_touch_event_raw));

    if (popup_data.wl_surface) {
        if (wl_data.touch.surface == popup_data.wl_surface) {
            cog_popup_menu_handle_event (popup_data.popup_menu,
                                         COG_POPUP_MENU_EVENT_STATE_PRESSED,
                                         raw_event.x, raw_event.y);
            update_popup ();
            return;
        } else
            destroy_popup ();
    }

    CogView *view = cog_view_stack_get_visible_view(platform->views);
    if (!view)
        return;

    struct wpe_input_touch_event event = {
        wl_data.touch.points,
        10,
        raw_event.type,
        raw_event.id,
        raw_event.time
    };

    wpe_view_backend_dispatch_touch_event(cog_view_get_backend(view), &event);
}

static void
touch_on_up (void *data,
             struct wl_touch *touch,
             uint32_t serial,
             uint32_t time,
             int32_t id)
{
    CogWlPlatform *platform = data;

    struct wl_surface *target_surface = wl_data.touch.surface;
    wl_data.touch.surface = NULL;
    wl_data.event_serial = serial;

    if (id < 0 || id >= 10)
        return;

    struct wpe_input_touch_event_raw raw_event = {
        wpe_input_touch_event_type_up,
        time,
        id,
        wl_data.touch.points[id].x,
        wl_data.touch.points[id].y,
    };

    if (popup_data.wl_surface) {
        if (target_surface == popup_data.wl_surface) {
            cog_popup_menu_handle_event (popup_data.popup_menu,
                                         COG_POPUP_MENU_EVENT_STATE_RELEASED,
                                         raw_event.x, raw_event.y);
            update_popup ();

            memset (&wl_data.touch.points[id],
                    0x00,
                    sizeof (struct wpe_input_touch_event_raw));
            return;
        }
    }

    memcpy (&wl_data.touch.points[id],
            &raw_event,
            sizeof (struct wpe_input_touch_event_raw));

    CogView *view = cog_view_stack_get_visible_view(platform->views);
    if (view) {
        struct wpe_input_touch_event event = {wl_data.touch.points, 10, raw_event.type, raw_event.id, raw_event.time};
        wpe_view_backend_dispatch_touch_event(cog_view_get_backend(view), &event);
    }

    memset (&wl_data.touch.points[id],
            0x00,
            sizeof (struct wpe_input_touch_event_raw));
}

static void
touch_on_motion (void *data,
                 struct wl_touch *touch,
                 uint32_t time,
                 int32_t id,
                 wl_fixed_t x,
                 wl_fixed_t y)
{
    CogWlPlatform *platform = data;

    if (id < 0 || id >= 10)
        return;

    struct wpe_input_touch_event_raw raw_event = {
        wpe_input_touch_event_type_motion,
        time,
        id,
        wl_fixed_to_int(x) * wl_data.current_output->scale,
        wl_fixed_to_int(y) * wl_data.current_output->scale,
    };

    memcpy (&wl_data.touch.points[id],
            &raw_event,
            sizeof (struct wpe_input_touch_event_raw));

    CogView *view = cog_view_stack_get_visible_view(platform->views);
    if (!view)
        return;

    struct wpe_input_touch_event event = {
        wl_data.touch.points,
        10,
        raw_event.type,
        raw_event.id,
        raw_event.time
    };

    wpe_view_backend_dispatch_touch_event(cog_view_get_backend(view), &event);
}

static void
touch_on_frame (void *data, struct wl_touch *touch)
{
    /* @FIXME: buffer touch events and handle them here */
}

static void
touch_on_cancel (void *data, struct wl_touch *touch)
{
}

static const struct wl_touch_listener touch_listener = {
    .down = touch_on_down,
    .up = touch_on_up,
    .motion = touch_on_motion,
    .frame = touch_on_frame,
    .cancel = touch_on_cancel,
};

static void
seat_on_capabilities (void* data, struct wl_seat* seat, uint32_t capabilities)
{
    CogWlPlatform *platform = data;

    g_debug ("Enumerating seat capabilities:");

    /* Pointer */
    const bool has_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
    if (has_pointer && wl_data.pointer.obj == NULL) {
        wl_data.pointer.obj = wl_seat_get_pointer (wl_data.seat);
        g_assert (wl_data.pointer.obj);
        wl_pointer_add_listener(wl_data.pointer.obj, &pointer_listener, platform);
        g_debug ("  - Pointer");
    } else if (! has_pointer && wl_data.pointer.obj != NULL) {
        wl_pointer_release (wl_data.pointer.obj);
        wl_data.pointer.obj = NULL;
    }

    /* Keyboard */
    const bool has_keyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;
    if (has_keyboard && wl_data.keyboard.obj == NULL) {
        wl_data.keyboard.obj = wl_seat_get_keyboard (wl_data.seat);
        g_assert (wl_data.keyboard.obj);
        wl_keyboard_add_listener(wl_data.keyboard.obj, &keyboard_listener, platform);
        g_debug ("  - Keyboard");
    } else if (! has_keyboard && wl_data.keyboard.obj != NULL) {
        wl_keyboard_release (wl_data.keyboard.obj);
        wl_data.keyboard.obj = NULL;
    }

    /* Touch */
    const bool has_touch = capabilities & WL_SEAT_CAPABILITY_TOUCH;
    if (has_touch && wl_data.touch.obj == NULL) {
        wl_data.touch.obj = wl_seat_get_touch (wl_data.seat);
        g_assert (wl_data.touch.obj);
        wl_touch_add_listener(wl_data.touch.obj, &touch_listener, platform);
        g_debug ("  - Touch");
    } else if (! has_touch && wl_data.touch.obj != NULL) {
        wl_touch_release (wl_data.touch.obj);
        wl_data.touch.obj = NULL;
    }

    g_debug ("Done enumerating seat capabilities.");
}

#ifdef WL_SEAT_NAME_SINCE_VERSION
static void
seat_on_name (void *data, struct wl_seat *seat, const char *name)
{
    g_debug ("Seat name: '%s'", name);
}
#endif /* WL_SEAT_NAME_SINCE_VERSION */

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_on_capabilities,
#ifdef WL_SEAT_NAME_SINCE_VERSION
    .name = seat_on_name,
#endif /* WL_SEAT_NAME_SINCE_VERSION */
};

static void
registry_global_remove (void *data, struct wl_registry *registry, uint32_t name)
{
    for (int i = 0; i < G_N_ELEMENTS (wl_data.metrics); i++)
    {
        if (wl_data.metrics[i].name == name) {
            wl_data.metrics[i].output = NULL;
            wl_data.metrics[i].name = 0;
            g_debug ("Removed output %i\n", name);
            break;
        }
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove
};

static void
on_surface_frame(void *data, struct wl_callback *callback, uint32_t time)
{
    CogWlView *view = data;

    if (view->frame_callback) {
        g_assert(view->frame_callback == callback);
        g_clear_pointer(&view->frame_callback, wl_callback_destroy);
    }

    wpe_view_backend_exportable_fdo_dispatch_frame_complete(view->exportable);
}

static void
on_presentation_feedback_sync_output (void *data, struct wp_presentation_feedback *presentation_feedback, struct wl_output *output)
{
}

static void
on_presentation_feedback_presented (void *data, struct wp_presentation_feedback *presentation_feedback,
    uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec, uint32_t refresh,
    uint32_t seq_hi, uint32_t seq_lo, uint32_t flags)
{
    wp_presentation_feedback_destroy (presentation_feedback);
}

static void
on_presentation_feedback_discarded (void *data, struct wp_presentation_feedback *presentation_feedback)
{
    wp_presentation_feedback_destroy (presentation_feedback);
}

static const struct wp_presentation_feedback_listener presentation_feedback_listener = {
    .sync_output = on_presentation_feedback_sync_output,
    .presented = on_presentation_feedback_presented,
    .discarded = on_presentation_feedback_discarded
};

static void
cog_wl_request_frame(CogWlView *view)
{
    if (!view->frame_callback) {
        static const struct wl_callback_listener listener = {.done = on_surface_frame};
        view->frame_callback = wl_surface_frame(win_data.wl_surface);
        wl_callback_add_listener(view->frame_callback, &listener, view);
    }

    if (wl_data.presentation != NULL) {
        struct wp_presentation_feedback *presentation_feedback = wp_presentation_feedback (wl_data.presentation,
                                                                        win_data.wl_surface);
        wp_presentation_feedback_add_listener (presentation_feedback,
                                               &presentation_feedback_listener,
                                               NULL);
    }
}

#if COG_ENABLE_WESTON_DIRECT_DISPLAY
static void
on_dmabuf_surface_frame (void *data, struct wl_callback *callback, uint32_t time)
{
    // for WAYLAND_DEBUG=1 purposes only
    wl_callback_destroy (callback);
}

static const struct wl_callback_listener dmabuf_frame_listener = {
    .done = on_dmabuf_surface_frame,
};

static void
on_dmabuf_buffer_release (void* data, struct wl_buffer* buffer)
{
    struct video_buffer *data_buffer = data;
    if (data_buffer->fd >= 0)
        close(data_buffer->fd);

    if (data_buffer->dmabuf_export)
        wpe_video_plane_display_dmabuf_export_release(data_buffer->dmabuf_export);

    g_slice_free (struct video_buffer, data_buffer);
    g_clear_pointer (&buffer, wl_buffer_destroy);
}

static const struct wl_buffer_listener dmabuf_buffer_listener = {
    .release = on_dmabuf_buffer_release,
};
#endif /* COG_ENABLE_WESTON_DIRECT_DISPLAY */

static inline bool
view_background_has_alpha(CogView *view)
{
    WebKitColor bg_color;
    webkit_web_view_get_background_color((WebKitWebView *) view, &bg_color);
    return bg_color.alpha != .0f;
}

static void
cog_wl_on_buffer_release(void *data G_GNUC_UNUSED, struct wl_buffer *buffer)
{
    wl_buffer_destroy(buffer);
}

static void
cog_wl_view_update_surface_contents(CogWlView *self, struct wl_surface *surface)
{
    if (self->should_update_opaque_region) {
        self->should_update_opaque_region = false;
        if (win_data.is_fullscreen || !view_background_has_alpha((CogView *) self)) {
            struct wl_region *region = wl_compositor_create_region(wl_data.compositor);
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

    struct wl_buffer *buffer =
        s_eglCreateWaylandBufferFromImageWL(egl_data.display, wpe_fdo_egl_exported_image_get_egl_image(self->image));
    g_assert(buffer);

    static const struct wl_buffer_listener buffer_listener = {.release = cog_wl_on_buffer_release};
    wl_buffer_add_listener(buffer, &buffer_listener, NULL);

    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, INT32_MAX, INT32_MAX);

    cog_wl_request_frame(self);

    wl_surface_commit(surface);

    if (win_data.is_resizing_fullscreen && cog_wl_does_image_match_win_size(self->image))
        cog_wl_fullscreen_image_ready(self);
}

static void
on_export_wl_egl_image(void *data, struct wpe_fdo_egl_exported_image *image)
{
    CogWlView *self = data;

    const uint32_t surface_pixel_width = wl_data.current_output->scale * win_data.width;
    const uint32_t surface_pixel_height = wl_data.current_output->scale * win_data.height;

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
        cog_wl_view_update_surface_contents(self, win_data.wl_surface);
}

#if HAVE_SHM_EXPORTED_BUFFER
static struct shm_buffer *
shm_buffer_for_resource (struct wl_resource *buffer_resource)
{
    struct shm_buffer *buffer;
    wl_list_for_each (buffer, &wl_data.shm_buffer_list, link) {
        if (buffer->buffer_resource == buffer_resource)
            return buffer;
    }

    return NULL;
}

static void
shm_buffer_destroy_notify (struct wl_listener *listener, void *data);

static struct shm_buffer *
shm_buffer_create (struct wl_resource *buffer_resource, size_t size)
{
    int fd = os_create_anonymous_file (size);
    if (fd < 0)
        return NULL;

    void* data = mmap (NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return NULL;
    }

    struct shm_buffer *buffer = g_new0(struct shm_buffer, 1);
    buffer->destroy_listener.notify = shm_buffer_destroy_notify;
    buffer->buffer_resource = buffer_resource;
    wl_resource_add_destroy_listener (buffer_resource, &buffer->destroy_listener);

    buffer->shm_pool = wl_shm_create_pool (wl_data.shm, fd, size);
    buffer->data = data;
    buffer->size = size;

    close (fd);
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
on_shm_buffer_release(void *data, struct wl_buffer *wl_buffer)
{
    struct shm_buffer *buffer = data;
    if (buffer->exported_buffer) {
        wpe_view_backend_exportable_fdo_egl_dispatch_release_shm_exported_buffer (wpe_host_data.exportable,
                                                                                  buffer->exported_buffer);
        buffer->exported_buffer = NULL;
    }
}

static const struct wl_buffer_listener shm_buffer_listener = {
    .release = on_shm_buffer_release,
};

static void
shm_buffer_copy_contents (struct shm_buffer *buffer, struct wl_shm_buffer *exported_shm_buffer)
{
    int32_t height = wl_shm_buffer_get_height (exported_shm_buffer);
    int32_t stride = wl_shm_buffer_get_stride (exported_shm_buffer);

    size_t data_size = height * stride;

    wl_shm_buffer_begin_access (exported_shm_buffer);
    void* exported_data = wl_shm_buffer_get_data (exported_shm_buffer);

    memcpy (buffer->data, exported_data, data_size);

    wl_shm_buffer_end_access (exported_shm_buffer);
}

static void
on_export_shm_buffer(void *data, struct wpe_fdo_shm_exported_buffer *exported_buffer)
{
    CogWlView *view = data;

    struct wl_resource *exported_resource = wpe_fdo_shm_exported_buffer_get_resource (exported_buffer);
    struct wl_shm_buffer *exported_shm_buffer = wpe_fdo_shm_exported_buffer_get_shm_buffer (exported_buffer);

    const uint32_t surface_pixel_width = wl_data.current_output->scale * win_data.width;
    const uint32_t surface_pixel_height = wl_data.current_output->scale * win_data.height;

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

    struct shm_buffer *buffer = shm_buffer_for_resource (exported_resource);
    if (!buffer) {
        int32_t width;
        int32_t height;
        if (win_data.is_fullscreen) {
            width = win_data.width;
            height = win_data.height;
        } else {
            width = wl_shm_buffer_get_width(exported_shm_buffer);
            height = wl_shm_buffer_get_height(exported_shm_buffer);
        }
        int32_t stride = wl_shm_buffer_get_stride (exported_shm_buffer);
        uint32_t format = wl_shm_buffer_get_format (exported_shm_buffer);

        size_t size = stride * height;
        buffer = shm_buffer_create (exported_resource, size);
        if (!buffer)
            return;
        wl_list_insert (&wl_data.shm_buffer_list, &buffer->link);

        buffer->buffer = wl_shm_pool_create_buffer (buffer->shm_pool, 0, width, height, stride, format);
        wl_buffer_add_listener (buffer->buffer, &shm_buffer_listener, buffer);
    }

    buffer->exported_buffer = exported_buffer;
    shm_buffer_copy_contents (buffer, exported_shm_buffer);

    wl_surface_attach (win_data.wl_surface, buffer->buffer, 0, 0);
    wl_surface_damage(win_data.wl_surface, 0, 0, INT32_MAX, INT32_MAX);
    cog_wl_request_frame(view);
    wl_surface_commit (win_data.wl_surface);
}
#endif

#if COG_ENABLE_WESTON_DIRECT_DISPLAY
static void
create_succeeded(void *data, struct zwp_linux_buffer_params_v1 *params, struct wl_buffer *new_buffer)
{
	zwp_linux_buffer_params_v1_destroy (params);

	struct video_buffer *buffer = data;
	buffer->buffer = new_buffer;
}

static void
create_failed(void *data, struct zwp_linux_buffer_params_v1 *params)
{
	zwp_linux_buffer_params_v1_destroy (params);

    struct video_buffer *buffer = data;
    buffer->buffer = NULL;
}

static const struct zwp_linux_buffer_params_v1_listener params_listener = {
	.created = create_succeeded,
	.failed = create_failed
};

static void
destroy_video_surface (gpointer data)
{
    struct video_surface *surface = (struct video_surface*) data;

#    if COG_ENABLE_WESTON_CONTENT_PROTECTION
    g_clear_pointer (&surface->protected_surface, weston_protected_surface_destroy);
#    endif
    g_clear_pointer (&surface->wl_subsurface, wl_subsurface_destroy);
    g_clear_pointer (&surface->wl_surface, wl_surface_destroy);
    g_slice_free (struct video_surface, surface);
}

static void
on_video_plane_display_dmabuf_receiver_handle_dmabuf (void* data, struct wpe_video_plane_display_dmabuf_export* dmabuf_export, uint32_t id, int fd, int32_t x, int32_t y, int32_t width, int32_t height, uint32_t stride)
{
    if (fd < 0)
        return;

    if (!wl_data.dmabuf) {
        // TODO: Replace with g_warning_once() after bumping our GLib requirement.
        static bool warning_emitted = false;
        if (!warning_emitted) {
            g_warning ("DMABuf not supported by the compositor. Video won't be rendered");
            warning_emitted = true;
        }
        return;
    }

    uint64_t modifier = DRM_FORMAT_MOD_INVALID;
    struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params (wl_data.dmabuf);
    if (wl_data.direct_display != NULL)
        weston_direct_display_v1_enable (wl_data.direct_display, params);

    struct video_surface *surf = (struct video_surface*) g_hash_table_lookup (win_data.video_surfaces, GUINT_TO_POINTER(id));
    if (!surf) {
        surf = g_slice_new0 (struct video_surface);
        surf->wl_subsurface = NULL;
        surf->wl_surface = wl_compositor_create_surface (wl_data.compositor);

#    if COG_ENABLE_WESTON_CONTENT_PROTECTION
        if (wl_data.protection) {
            surf->protected_surface = weston_content_protection_get_protection (wl_data.protection, surf->wl_surface);
            //weston_protected_surface_set_type(surf->protected_surface, WESTON_PROTECTED_SURFACE_TYPE_DC_ONLY);

            weston_protected_surface_enforce (surf->protected_surface);
        }
#    endif
        g_hash_table_insert (win_data.video_surfaces, GUINT_TO_POINTER (id), surf);
    }

    zwp_linux_buffer_params_v1_add (params, fd, 0, 0, stride, modifier >> 32, modifier & 0xffffffff);

    if ((x + width) > win_data.width)
        width -= x;

    if ((y + height) > win_data.height)
        height -= y;

    struct video_buffer *buffer = g_slice_new0 (struct video_buffer);
    buffer->fd = fd;
    buffer->x = x;
    buffer->y = y;
    buffer->width = width;
    buffer->height = height;
	zwp_linux_buffer_params_v1_add_listener (params, &params_listener, buffer);

    buffer->buffer = zwp_linux_buffer_params_v1_create_immed (params, buffer->width, buffer->height, VIDEO_BUFFER_FORMAT, 0);
    zwp_linux_buffer_params_v1_destroy (params);

    buffer->dmabuf_export = dmabuf_export;
    wl_buffer_add_listener (buffer->buffer, &dmabuf_buffer_listener, buffer);

    wl_surface_attach (surf->wl_surface, buffer->buffer, 0, 0);
    wl_surface_damage (surf->wl_surface, 0, 0, buffer->width, buffer->height);

    struct wl_callback *callback = wl_surface_frame (surf->wl_surface);
    wl_callback_add_listener (callback, &dmabuf_frame_listener, NULL);

    if (!surf->wl_subsurface) {
        surf->wl_subsurface = wl_subcompositor_get_subsurface (wl_data.subcompositor,
                                                               surf->wl_surface,
                                                               win_data.wl_surface);
        wl_subsurface_set_sync (surf->wl_subsurface);
    }

    wl_subsurface_set_position (surf->wl_subsurface, buffer->x, buffer->y);
    wl_surface_commit (surf->wl_surface);
}

static void
on_video_plane_display_dmabuf_receiver_end_of_stream (void* data, uint32_t id)
{
    g_hash_table_remove (win_data.video_surfaces, GUINT_TO_POINTER (id));
}

static const struct wpe_video_plane_display_dmabuf_receiver video_plane_display_dmabuf_receiver = {
    .handle_dmabuf = on_video_plane_display_dmabuf_receiver_handle_dmabuf,
    .end_of_stream = on_video_plane_display_dmabuf_receiver_end_of_stream,
};
#endif

static gboolean
init_wayland(CogWlPlatform *platform, GError **error)
{
    g_debug ("Initializing Wayland...");

    if (!(wl_data.display = wl_display_connect (NULL))) {
        g_set_error (error,
                     G_FILE_ERROR,
                     g_file_error_from_errno (errno),
                     "Could not open Wayland display");
        return FALSE;
    }

    wl_data.registry = wl_display_get_registry (wl_data.display);
    g_assert (wl_data.registry);
    wl_registry_add_listener(wl_data.registry, &registry_listener, platform);
    wl_display_roundtrip (wl_data.display);

#if COG_USE_WAYLAND_CURSOR
    if (wl_data.shm) {
        if (!(wl_data.cursor_theme = wl_cursor_theme_load(NULL, 32, wl_data.shm))) {
            g_warning("%s: Could not load cursor theme.", G_STRFUNC);
        } else if (!(wl_data.cursor_left_ptr = wl_cursor_theme_get_cursor(wl_data.cursor_theme, "left_ptr"))) {
            g_warning("%s: Could not load left_ptr cursor.", G_STRFUNC);
        }
    }
#endif /* COG_USE_WAYLAND_CURSOR */

    g_assert (wl_data.compositor);
    g_assert (wl_data.xdg_shell != NULL ||
              wl_data.shell != NULL ||
              wl_data.fshell != NULL);

    wl_list_init (&wl_data.shm_buffer_list);
    return TRUE;
}

static void
clear_wayland (void)
{
    g_clear_pointer(&wl_data.event_src, g_source_destroy);

    if (wl_data.xdg_shell != NULL)
        xdg_wm_base_destroy (wl_data.xdg_shell);
    if (wl_data.fshell != NULL)
        zwp_fullscreen_shell_v1_destroy (wl_data.fshell);
    if (wl_data.shell != NULL)
        wl_shell_destroy (wl_data.shell);

    g_clear_pointer (&wl_data.shm, wl_shm_destroy);
    g_clear_pointer (&wl_data.subcompositor, wl_subcompositor_destroy);
    g_clear_pointer (&wl_data.compositor, wl_compositor_destroy);

#if COG_ENABLE_WESTON_CONTENT_PROTECTION
    g_clear_pointer (&wl_data.protection, weston_content_protection_destroy);
#endif

#if COG_ENABLE_WESTON_DIRECT_DISPLAY
    g_clear_pointer (&wl_data.direct_display, weston_direct_display_v1_destroy);
#endif

#ifdef COG_USE_WAYLAND_CURSOR
    g_clear_pointer (&wl_data.cursor_left_ptr_surface, wl_surface_destroy);
    g_clear_pointer (&wl_data.cursor_theme, wl_cursor_theme_destroy);
#endif /* COG_USE_WAYLAND_CURSOR */

    wl_registry_destroy (wl_data.registry);
    wl_display_flush (wl_data.display);
    wl_display_disconnect (wl_data.display);
}

// clang-format off
#define SHELL_PROTOCOLS(m) \
    m(xdg_wm_base)         \
    m(wl_shell)            \
    m(zwp_fullscreen_shell_v1)
// clang-format on

#define DECLARE_PROTOCOL_ENTRY(proto) gboolean found_##proto;
struct check_supported_protocols {
    SHELL_PROTOCOLS(DECLARE_PROTOCOL_ENTRY)
};
#undef DECLARE_PROTOCOL_ENTRY

static void
on_registry_global_is_supported_check(void *data,
                                      struct wl_registry *registry,
                                      uint32_t name,
                                      const char *interface,
                                      uint32_t version)
{
    struct check_supported_protocols *protocols = data;

#define TRY_MATCH_PROTOCOL_ENTRY(proto)                   \
    if (strcmp(interface, proto##_interface.name) == 0) { \
        protocols->found_##proto = TRUE;                  \
        return;                                           \
    }

    SHELL_PROTOCOLS(TRY_MATCH_PROTOCOL_ENTRY)

#undef TRY_MATCH_PROTOCOL_ENTRY
}

static void *
check_supported(void *data G_GNUC_UNUSED)
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
        struct check_supported_protocols protocols = {};
        struct wl_registry *registry = wl_display_get_registry(display);
        wl_registry_add_listener(registry,
                                 &((const struct wl_registry_listener){
                                     .global = on_registry_global_is_supported_check,
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

static gboolean
cog_wl_platform_is_supported(void)
{
    static GOnce once = G_ONCE_INIT;
    g_once(&once, check_supported, NULL);
    return GPOINTER_TO_INT(once.retval);
}

#define ERR_EGL(_err, _msg)                              \
    do {                                                 \
        EGLint error_code_ ## __LINE__ = eglGetError (); \
        g_set_error ((_err),                             \
                     COG_PLATFORM_EGL_ERROR,             \
                     error_code_ ## __LINE__,            \
                     _msg " (%#06x)",                    \
                     error_code_ ## __LINE__);           \
    } while (0)


static void clear_egl (void);
static void destroy_window (void);


static gboolean
init_egl (GError **error)
{
    g_debug ("Initializing EGL...");

    egl_data.display = eglGetDisplay ((EGLNativeDisplayType) wl_data.display);
    if (egl_data.display == EGL_NO_DISPLAY) {
        ERR_EGL (error, "Could not open EGL display");
        return FALSE;
    }

    EGLint major, minor;
    if (!eglInitialize (egl_data.display, &major, &minor)) {
        ERR_EGL (error, "Could not initialize  EGL");
        clear_egl ();
        return FALSE;
    }
    g_info ("EGL version %d.%d initialized.", major, minor);

    return TRUE;
}

static void
clear_egl (void)
{
    if (egl_data.display != EGL_NO_DISPLAY) {
        eglTerminate (egl_data.display);
        egl_data.display = EGL_NO_DISPLAY;
    }
    eglReleaseThread ();
}

static gboolean
cog_wl_platform_create_window(CogWlPlatform *self, GError **error)
{
    g_debug ("Creating Wayland surface...");

    win_data.wl_surface = wl_compositor_create_surface (wl_data.compositor);
    g_assert (win_data.wl_surface);

#if COG_ENABLE_WESTON_DIRECT_DISPLAY
    win_data.video_surfaces = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, destroy_video_surface);
#endif

    wl_surface_add_listener(win_data.wl_surface, &surface_listener, self);

    if (wl_data.xdg_shell != NULL) {
        win_data.xdg_surface =
            xdg_wm_base_get_xdg_surface (wl_data.xdg_shell,
                                         win_data.wl_surface);
        g_assert(win_data.xdg_surface);

        xdg_surface_add_listener (win_data.xdg_surface, &xdg_surface_listener,
                                  NULL);
        win_data.xdg_toplevel =
            xdg_surface_get_toplevel (win_data.xdg_surface);
        g_assert(win_data.xdg_toplevel);

        xdg_toplevel_add_listener(win_data.xdg_toplevel, &xdg_toplevel_listener, self);
        xdg_toplevel_set_title (win_data.xdg_toplevel, COG_DEFAULT_APPNAME);

        const char *app_id = NULL;
        GApplication *app = g_application_get_default ();
        if (app) {
            app_id = g_application_get_application_id (app);
        }
        if (!app_id) {
            app_id = COG_DEFAULT_APPID;
        }
        xdg_toplevel_set_app_id (win_data.xdg_toplevel, app_id);
        wl_surface_commit(win_data.wl_surface);
    } else if (wl_data.fshell != NULL) {
        zwp_fullscreen_shell_v1_present_surface (wl_data.fshell,
                                                 win_data.wl_surface,
                                                 ZWP_FULLSCREEN_SHELL_V1_PRESENT_METHOD_DEFAULT,
                                                 NULL);

        /* Configure the surface so that it respects the width and height
         * environment variables */
        configure_surface_geometry(self, 0, 0);
    } else if (wl_data.shell != NULL) {
        win_data.shell_surface = wl_shell_get_shell_surface(wl_data.shell, win_data.wl_surface);
        g_assert(win_data.shell_surface);

        wl_shell_surface_add_listener(win_data.shell_surface, &shell_surface_listener, self);
        wl_shell_surface_set_toplevel(win_data.shell_surface);

        /* wl_shell needs an initial surface configuration. */
        configure_surface_geometry(self, 0, 0);
    }

    const char *env_var;
    if ((env_var = g_getenv("COG_PLATFORM_WL_VIEW_FULLSCREEN")) && g_ascii_strtoll(env_var, NULL, 10) > 0) {
        win_data.is_maximized = false;
        win_data.is_fullscreen = true;

        if (wl_data.xdg_shell != NULL) {
            xdg_toplevel_set_fullscreen(win_data.xdg_toplevel, NULL);
        } else if (wl_data.fshell != NULL) {
            win_data.should_resize_to_largest_output = true;
            resize_to_largest_output(self);
        } else if (wl_data.shell != NULL) {
            wl_shell_surface_set_fullscreen(win_data.shell_surface, WL_SHELL_SURFACE_FULLSCREEN_METHOD_SCALE, 0, NULL);
        } else {
            g_warning("No available shell capable of fullscreening.");
            win_data.is_fullscreen = false;
        }
    } else if ((env_var = g_getenv("COG_PLATFORM_WL_VIEW_MAXIMIZE")) && g_ascii_strtoll(env_var, NULL, 10) > 0) {
        win_data.is_maximized = true;
        win_data.is_fullscreen = false;

        if (wl_data.xdg_shell != NULL) {
            xdg_toplevel_set_maximized (win_data.xdg_toplevel);
        } else if (wl_data.shell != NULL) {
            wl_shell_surface_set_maximized (win_data.shell_surface, NULL);
        } else {
            g_warning ("No available shell capable of maximizing.");
            win_data.is_maximized = false;
        }
    }

    return TRUE;
}

static void
destroy_window (void)
{
    g_clear_pointer (&win_data.xdg_toplevel, xdg_toplevel_destroy);
    g_clear_pointer (&win_data.xdg_surface, xdg_surface_destroy);
    g_clear_pointer (&win_data.shell_surface, wl_shell_surface_destroy);
    g_clear_pointer (&win_data.wl_surface, wl_surface_destroy);

#if COG_ENABLE_WESTON_DIRECT_DISPLAY
    g_clear_pointer (&win_data.video_surfaces, g_hash_table_destroy);
#endif
}

static void
create_popup (WebKitOptionMenu *option_menu)
{
    popup_data.option_menu = option_menu;

    popup_data.width = win_data.width;
    popup_data.height = cog_popup_menu_get_height_for_option_menu (option_menu);

    popup_data.popup_menu = cog_popup_menu_create(option_menu, wl_data.shm, popup_data.width, popup_data.height,
                                                  wl_data.current_output->scale);

    popup_data.wl_surface = wl_compositor_create_surface (wl_data.compositor);
    g_assert (popup_data.wl_surface);

#ifdef WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION
    if (wl_surface_get_version(popup_data.wl_surface) >= WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION)
        wl_surface_set_buffer_scale(popup_data.wl_surface, wl_data.current_output->scale);
#endif /* WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION */

    if (wl_data.xdg_shell != NULL) {
        popup_data.xdg_positioner = xdg_wm_base_create_positioner (wl_data.xdg_shell);
        g_assert(popup_data.xdg_positioner);

        xdg_positioner_set_size (popup_data.xdg_positioner,
                                 popup_data.width,
                                 popup_data.height);
        xdg_positioner_set_anchor_rect(popup_data.xdg_positioner, 0, (win_data.height - popup_data.height),
                                       popup_data.width, popup_data.height);

        popup_data.xdg_surface = xdg_wm_base_get_xdg_surface (wl_data.xdg_shell,
                                                              popup_data.wl_surface);
        g_assert (popup_data.xdg_surface);

        xdg_surface_add_listener (popup_data.xdg_surface,
                                  &xdg_surface_listener,
                                  NULL);
        popup_data.xdg_popup = xdg_surface_get_popup (popup_data.xdg_surface,
                                                      win_data.xdg_surface,
                                                      popup_data.xdg_positioner);
        g_assert (popup_data.xdg_popup);

        xdg_popup_add_listener (popup_data.xdg_popup,
                                &xdg_popup_listener,
                                NULL);
        xdg_popup_grab (popup_data.xdg_popup, wl_data.seat, wl_data.event_serial);
        wl_surface_commit (popup_data.wl_surface);
    } else if (wl_data.shell != NULL) {
        popup_data.shell_surface = wl_shell_get_shell_surface (wl_data.shell,
                                                               popup_data.wl_surface);
        g_assert(popup_data.shell_surface);

        wl_shell_surface_add_listener (popup_data.shell_surface,
                                       &shell_popup_surface_listener,
                                       NULL);
        wl_shell_surface_set_popup (popup_data.shell_surface,
                                    wl_data.seat, wl_data.event_serial,
                                    win_data.wl_surface,
                                    0, (win_data.height - popup_data.height), 0);

        display_popup();
    }
}

static void
destroy_popup (void)
{
    if (popup_data.option_menu == NULL)
        return;

    webkit_option_menu_close (popup_data.option_menu);
    g_clear_pointer (&popup_data.popup_menu, cog_popup_menu_destroy);
    g_clear_object (&popup_data.option_menu);

    g_clear_pointer (&popup_data.xdg_popup, xdg_popup_destroy);
    g_clear_pointer (&popup_data.xdg_surface, xdg_surface_destroy);
    g_clear_pointer (&popup_data.xdg_positioner, xdg_positioner_destroy);
    g_clear_pointer (&popup_data.shell_surface, wl_shell_surface_destroy);
    g_clear_pointer (&popup_data.wl_surface, wl_surface_destroy);

    popup_data.configured = false;
}

static void
display_popup (void)
{
    struct wl_buffer *buffer = cog_popup_menu_get_buffer (popup_data.popup_menu);
    wl_surface_attach (popup_data.wl_surface, buffer, 0, 0);
    wl_surface_damage (popup_data.wl_surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit (popup_data.wl_surface);
}

static void
update_popup (void)
{
    int selected_index;
    bool has_final_selection = cog_popup_menu_has_final_selection (popup_data.popup_menu,
                                                                   &selected_index);
    if (has_final_selection) {
        if (selected_index != -1)
            webkit_option_menu_activate_item (popup_data.option_menu, selected_index);
        destroy_popup ();
        return;
    }

    struct wl_buffer *buffer = cog_popup_menu_get_buffer (popup_data.popup_menu);
    wl_surface_attach (popup_data.wl_surface, buffer, 0, 0);
    wl_surface_damage (popup_data.wl_surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit (popup_data.wl_surface);
}

static gboolean
init_input(CogWlPlatform *self, GError **error)
{
    if (wl_data.seat != NULL) {
        wl_seat_add_listener(wl_data.seat, &seat_listener, self);

        xkb_data.context = xkb_context_new (XKB_CONTEXT_NO_FLAGS);
        g_assert (xkb_data.context);
        xkb_data.compose_table =
            xkb_compose_table_new_from_locale (xkb_data.context,
                                               setlocale (LC_CTYPE, NULL),
                                               XKB_COMPOSE_COMPILE_NO_FLAGS);
        if (xkb_data.compose_table != NULL) {
            xkb_data.compose_state =
                xkb_compose_state_new (xkb_data.compose_table,
                                       XKB_COMPOSE_STATE_NO_FLAGS);
        }

        if (wl_data.text_input_manager != NULL) {
            struct zwp_text_input_v3 *text_input =
                zwp_text_input_manager_v3_get_text_input(wl_data.text_input_manager, wl_data.seat);
            cog_im_context_wl_set_text_input(text_input);
        } else if (wl_data.text_input_manager_v1 != NULL) {
            struct zwp_text_input_v1 *text_input =
                zwp_text_input_manager_v1_create_text_input(wl_data.text_input_manager_v1);
            cog_im_context_wl_v1_set_text_input(text_input, wl_data.seat, win_data.wl_surface);
        }
    }

    return TRUE;
}

static void
clear_input(void)
{
    g_clear_pointer(&wl_data.pointer.obj, wl_pointer_destroy);
    g_clear_pointer(&wl_data.keyboard.obj, wl_keyboard_destroy);
    g_clear_pointer(&wl_data.seat, wl_seat_destroy);

    cog_im_context_wl_set_text_input(NULL);
    g_clear_pointer(&wl_data.text_input_manager, zwp_text_input_manager_v3_destroy);
    cog_im_context_wl_v1_set_text_input(NULL, NULL, NULL);
    g_clear_pointer(&wl_data.text_input_manager_v1, zwp_text_input_manager_v1_destroy);

    g_clear_pointer(&xkb_data.state, xkb_state_unref);
    g_clear_pointer(&xkb_data.compose_state, xkb_compose_state_unref);
    g_clear_pointer(&xkb_data.compose_table, xkb_compose_table_unref);
    g_clear_pointer(&xkb_data.keymap, xkb_keymap_unref);
    g_clear_pointer(&xkb_data.context, xkb_context_unref);
}

static void
clear_buffers(void)
{
#if HAVE_SHM_EXPORTED_BUFFER
    struct shm_buffer *buffer, *tmp;
    wl_list_for_each_safe (buffer, tmp, &wl_data.shm_buffer_list, link) {

        wl_list_remove (&buffer->link);
        wl_list_remove (&buffer->destroy_listener.link);

        shm_buffer_destroy (buffer);
    }
    wl_list_init (&wl_data.shm_buffer_list);
#endif
}

#if 0
static struct wpe_view_backend *
gamepad_provider_get_view_backend_for_gamepad(void *provider G_GNUC_UNUSED, void *gamepad G_GNUC_UNUSED)
{
    /* get_view_backend() might not been called yet */
    g_assert(wpe_view_data.backend);
    return wpe_view_data.backend;
}
#endif

static void
cog_wl_platform_on_notify_visible_view(CogWlPlatform *self)
{
    CogWlView *view = COG_WL_VIEW(cog_view_stack_get_visible_view(self->views));
    g_debug("%s: visible view %p", G_STRFUNC, view);

    if (!view->image) {
        g_debug("%s: No last image to show, skipping update.", G_STRFUNC);
        return;
    }

    cog_wl_view_update_surface_contents(view, win_data.wl_surface);
}

static gboolean
cog_wl_platform_setup(CogPlatform *platform, CogShell *shell G_GNUC_UNUSED, const char *params, GError **error)
{
    g_return_val_if_fail (COG_IS_SHELL (shell), FALSE);

    CogWlPlatform *self = COG_WL_PLATFORM(platform);
    self->views = cog_shell_get_view_stack(shell);

    if (!wpe_loader_init ("libWPEBackend-fdo-1.0.so")) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to set backend library name");
        return FALSE;
    }

    if (!init_wayland(self, error))
        return FALSE;

    if (!init_egl (error)) {
        clear_wayland ();
        return FALSE;
    }

    if (!cog_wl_platform_create_window(self, error)) {
        clear_egl ();
        clear_wayland();
        return FALSE;
    }

    if (!init_input(self, error)) {
        destroy_window ();
        clear_egl();
        clear_wayland ();
        return FALSE;
    }

    /* init WPE host data */
    wpe_fdo_initialize_for_egl_display (egl_data.display);

#if COG_ENABLE_WESTON_DIRECT_DISPLAY
    wpe_video_plane_display_dmabuf_register_receiver (&video_plane_display_dmabuf_receiver, NULL);
#endif

#if 0
    cog_gamepad_setup(gamepad_provider_get_view_backend_for_gamepad);
#endif

    g_signal_connect_object(self->views, "notify::visible-view", G_CALLBACK(cog_wl_platform_on_notify_visible_view),
                            self, G_CONNECT_AFTER | G_CONNECT_SWAPPED);

    return TRUE;
}

static void
cog_wl_platform_finalize(GObject *object)
{
    /* @FIXME: check why this segfaults
    wpe_view_backend_destroy (wpe_view_data.backend);
    */

    /* free WPE host data */
    /* @FIXME: check why this segfaults
    wpe_view_backend_exportable_wl_destroy (wpe_host_data.exportable);
    */

    clear_buffers();

    clear_input ();
    destroy_popup ();
    destroy_window ();
    clear_egl();
    clear_wayland ();

    G_OBJECT_CLASS(cog_wl_platform_parent_class)->finalize(object);
}

static void
on_show_option_menu(WebKitWebView *view, WebKitOptionMenu *menu, WebKitRectangle *rectangle, gpointer *data)
{
    create_popup (g_object_ref (menu));
}

static WebKitInputMethodContext *
cog_wl_platform_create_im_context(CogPlatform *platform)
{
    if (wl_data.text_input_manager)
        return cog_im_context_wl_new();
    if (wl_data.text_input_manager_v1)
        return cog_im_context_wl_v1_new();
    return NULL;
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
}

static void
cog_wl_platform_init(CogWlPlatform *self)
{
}

G_MODULE_EXPORT void
g_io_cogplatform_wl_load(GIOModule *module)
{
    GTypeModule *type_module = G_TYPE_MODULE(module);
    cog_wl_platform_register_type(type_module);
    cog_wl_view_register_type(type_module);
}

G_MODULE_EXPORT void
g_io_cogplatform_wl_unload(GIOModule *module G_GNUC_UNUSED)
{
}
