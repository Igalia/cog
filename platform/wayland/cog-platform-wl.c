/*
 * cog-wl-platform.c
 * Copyright (C) 2018 Eduardo Lima <elima@igalia.com>
 * Copyright (C) 2018 Adrian Perez de Castro <aperez@igalia.com>
 *
 * SPDX-License-Identifier: MIT
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

#if COG_HAVE_LIBPORTAL
#    include "../common/cog-file-chooser.h"
#endif /* COG_HAVE_LIBPORTAL */
#include "../common/cursors.h"
#include "../common/egl-proc-address.h"
#include "os-compatibility.h"

#include "cog-im-context-wl-v1.h"
#include "cog-im-context-wl.h"
#include "cog-platform-wl.h"
#include "cog-popup-menu-wl.h"
#if COG_HAVE_LIBPORTAL
#    include "cog-xdp-parent-wl.h"
#endif /* COG_HAVE_LIBPORTAL */

#include "fullscreen-shell-unstable-v1-client.h"
#include "linux-dmabuf-unstable-v1-client.h"
#include "presentation-time-client.h"
#include "text-input-unstable-v1-client.h"
#include "text-input-unstable-v3-client.h"
#include "xdg-foreign-unstable-v2-client.h"
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

G_DEFINE_DYNAMIC_TYPE_EXTENDED(
    CogWlPlatform,
    cog_wl_platform,
    COG_TYPE_PLATFORM,
    0,
    g_io_extension_point_implement(COG_MODULES_PLATFORM_EXTENSION_POINT, g_define_type_id, "wl", 500);)

#ifndef EGL_WL_create_wayland_buffer_from_image
typedef struct wl_buffer *(EGLAPIENTRYP PFNEGLCREATEWAYLANDBUFFERFROMIMAGEWL)(EGLDisplay dpy, EGLImageKHR image);
#endif

CogWlOutput output_metrics;

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

static void display_popup(void);
static void update_popup(void);
static void destroy_popup(void);

static void
cog_wl_platform_configure_geometry(CogWlPlatform *platform, int32_t width, int32_t height)
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

    if (platform->window.width != width || platform->window.height != height) {
        g_debug("Configuring new size: %" PRId32 "x%" PRId32, width, height);
        platform->window.width = width;
        platform->window.height = height;

        cog_wl_view_resize(platform->view);
    }
}

static void
shell_surface_on_ping(void *data, struct wl_shell_surface *shell_surface, uint32_t serial)
{
    wl_shell_surface_pong(shell_surface, serial);
}

static void
shell_surface_on_configure(void                    *data,
                           struct wl_shell_surface *shell_surface,
                           uint32_t                 edges,
                           int32_t                  width,
                           int32_t                  height)
{
    CogWlPlatform *platform = data;

    cog_wl_platform_configure_geometry(platform, width, height);

    g_debug("New wl_shell configuration: (%" PRIu32 ", %" PRIu32 ")", width, height);
}

static const struct wl_shell_surface_listener shell_surface_listener = {
    .ping = shell_surface_on_ping,
    .configure = shell_surface_on_configure,
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
xdg_toplevel_on_configure(void                *data,
                          struct xdg_toplevel *toplevel,
                          int32_t              width,
                          int32_t              height,
                          struct wl_array     *states)
{
    CogWlPlatform *platform = data;

    cog_wl_platform_configure_geometry(platform, width, height);

    g_debug("New XDG toplevel configuration: (%" PRIu32 ", %" PRIu32 ")", width, height);
}

static void
xdg_toplevel_on_close(void *data, struct xdg_toplevel *xdg_toplevel)
{
    g_application_quit(g_application_get_default());
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
    CogWlDisplay *display = platform->display;

    /* Find the largest output and resize the surface to match */
    int32_t width = 0;
    int32_t height = 0;
    for (int i = 0; i < G_N_ELEMENTS(display->metrics); i++) {
        if (display->metrics[i].output && display->metrics[i].width * display->metrics[i].height >= width * height) {
            width = display->metrics[i].width;
            height = display->metrics[i].height;
        }
    }
    cog_wl_platform_configure_geometry(platform, width, height);
}

static void
noop()
{
}

static CogWlOutput *
find_output(CogWlDisplay *display, struct wl_output *output)
{
    for (int i = 0; i < G_N_ELEMENTS(display->metrics); i++) {
        if (display->metrics[i].output == output) {
            return &display->metrics[i];
        }
    }
    g_warning("Unknown output %p\n", output);
    return NULL;
}

static void
output_handle_mode(void *data, struct wl_output *output, uint32_t flags, int32_t width, int32_t height, int32_t refresh)
{
    CogWlPlatform *platform = data;

    CogWlOutput *metrics = find_output(platform->display, output);
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
    CogWlDisplay  *display = platform->display;

    CogWlOutput *metrics = find_output(platform->display, output);

    if (!metrics->refresh) {
        g_warning("No refresh rate reported for output %p, using 60Hz", output);
        metrics->refresh = 60 * 1000;
    }

    if (!metrics->scale) {
        g_warning("No scale factor reported for output %p, using 1x", output);
        metrics->scale = 1;
    }

    if (!display->current_output) {
        g_debug("%s: Using %p as initial output", G_STRFUNC, output);
        display->current_output = metrics;
    }

    if (platform->window.should_resize_to_largest_output) {
        resize_to_largest_output(platform);
    }
}

#ifdef WL_OUTPUT_SCALE_SINCE_VERSION
static void
output_handle_scale(void *data, struct wl_output *output, int32_t factor)
{
    CogWlPlatform *platform = data;

    CogWlOutput *metrics = find_output(platform->display, output);
    if (!metrics) {
        return;
    }

    metrics->scale = factor;
    g_info("Got scale factor %i for output %p", factor, output);
}
#endif /* WL_OUTPUT_SCALE_SINCE_VERSION */

bool
cog_wl_platform_set_fullscreen(CogWlPlatform *platform, bool fullscreen)
{
    CogWlDisplay *display = platform->display;

    if (platform->window.is_resizing_fullscreen || platform->window.is_fullscreen == fullscreen)
        return false;

    platform->window.is_fullscreen = fullscreen;
    if (fullscreen) {
        // Resize window to the size of the screen.
        // Wait until a new exported image is reveived. See cog_wl_view_enter_fullscreen().
        platform->window.is_resizing_fullscreen = true;
        platform->window.width_before_fullscreen = platform->window.width;
        platform->window.height_before_fullscreen = platform->window.height;
        resize_to_largest_output(platform);
        cog_wl_view_enter_fullscreen(platform->view);
    } else {
        if (display->xdg_shell != NULL) {
            xdg_toplevel_unset_fullscreen(platform->window.xdg_toplevel);
        } else if (display->fshell != NULL) {
            noop();
        } else if (display->shell != NULL) {
            wl_shell_surface_set_toplevel(platform->window.shell_surface);
        } else {
            g_assert_not_reached();
        }
        cog_wl_platform_configure_geometry(platform, platform->window.width_before_fullscreen,
                                           platform->window.height_before_fullscreen);
#if HAVE_FULLSCREEN_HANDLING
        if (platform->window.was_fullscreen_requested_from_dom) {
            struct wpe_view_backend *backend = cog_view_get_backend(COG_VIEW(platform->view));
            wpe_view_backend_dispatch_did_exit_fullscreen(backend);
        }
        platform->window.was_fullscreen_requested_from_dom = false;
#endif
    }
    platform->view->should_update_opaque_region = true;
    return true;
}

static const struct wl_output_listener output_listener = {
    .geometry = noop,
    .mode = output_handle_mode,
    .done = output_handle_done,
#ifdef WL_OUTPUT_SCALE_SINCE_VERSION
    .scale = output_handle_scale,
#endif /* WL_OUTPUT_SCALE_SINCE_VERSION */
};

static void
surface_on_enter(void *data, struct wl_surface *surface, struct wl_output *output)
{
    CogWlPlatform *platform = data;
    CogWlDisplay  *display = platform->display;

    struct wpe_view_backend *backend = cog_view_get_backend(COG_VIEW(platform->view));

    if (display->current_output->output != output) {
        g_debug("%s: Surface %p output changed %p -> %p", G_STRFUNC, surface, display->current_output->output, output);
        display->current_output = find_output(platform->display, output);
        g_assert(display->current_output);
    }

#ifdef WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION
    if (wl_surface_get_version(surface) >= WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION) {
        wl_surface_set_buffer_scale(surface, display->current_output->scale);
        wpe_view_backend_dispatch_set_device_scale_factor(backend, display->current_output->scale);
    } else {
        g_debug("%s: Surface %p uses old protocol version, cannot set scale factor", G_STRFUNC, surface);
    }
#endif /* WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION */

#if HAVE_REFRESH_RATE_HANDLING
    wpe_view_backend_set_target_refresh_rate(backend, display->current_output->refresh);
#endif /* HAVE_REFRESH_RATE_HANDLING */
}

static const struct wl_surface_listener surface_listener = {
    .enter = surface_on_enter,
    .leave = noop,
};

static void
registry_on_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    CogWlPlatform *platform = data;
    CogWlDisplay  *display = platform->display;

    gboolean interface_used = TRUE;

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
        xdg_wm_base_add_listener(display->xdg_shell, &xdg_shell_listener, NULL);
    } else if (strcmp(interface, zwp_fullscreen_shell_v1_interface.name) == 0) {
        display->fshell = wl_registry_bind(registry, name, &zwp_fullscreen_shell_v1_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        display->seat.seat_name = name;
        display->seat.seat_version = MAX(3, MIN(version, 7));
        display->seat.seat = wl_registry_bind(registry, name, &wl_seat_interface, MAX(3, MIN(version, 7)));
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
        /* Version 2 introduced the wl_output_listener::scale. */
        struct wl_output *output = wl_registry_bind(registry, name, &wl_output_interface, MIN(2, version));
        wl_output_add_listener(output, &output_listener, platform);
        bool inserted = false;
        for (int i = 0; i < G_N_ELEMENTS(display->metrics); i++) {
            if (display->metrics[i].output == NULL) {
                display->metrics[i].output = output;
                display->metrics[i].name = name;
                inserted = true;
                break;
            }
        }
        if (!inserted) {
            g_warning("Exceeded %" G_GSIZE_FORMAT " connected outputs(!)", G_N_ELEMENTS(display->metrics));
        }
    } else if (strcmp(interface, zwp_text_input_manager_v3_interface.name) == 0) {
        display->text_input_manager = wl_registry_bind(registry, name, &zwp_text_input_manager_v3_interface, 1);
    } else if (strcmp(interface, zwp_text_input_manager_v1_interface.name) == 0) {
        display->text_input_manager_v1 = wl_registry_bind(registry, name, &zwp_text_input_manager_v1_interface, 1);
    } else if (strcmp(interface, zxdg_exporter_v2_interface.name) == 0) {
        display->zxdg_exporter = wl_registry_bind(registry, name, &zxdg_exporter_v2_interface, 1);
    } else if (strcmp(interface, wp_presentation_interface.name) == 0) {
        display->presentation = wl_registry_bind(registry, name, &wp_presentation_interface, 1);
    } else {
        interface_used = FALSE;
    }
    g_debug("%s '%s' interface obtained from the Wayland registry.", interface_used ? "Using" : "Ignoring", interface);
}

#ifdef COG_USE_WAYLAND_CURSOR
static void
set_cursor(CogWlPlatform *platform, enum cursor_type type)
{
    CogWlDisplay *display = platform->display;

    if (!display->cursor_theme || !display->cursor_surface)
        return;

    struct wl_cursor *cursor = NULL;
    for (int i = 0; !cursor && i < G_N_ELEMENTS(cursor_names[type]) && cursor_names[type][i]; i++) {
        cursor = wl_cursor_theme_get_cursor(display->cursor_theme, cursor_names[type][i]);
    }

    if (!cursor) {
        g_warning("Could not get %s cursor", cursor_names[type][0]);
        return;
    }

    /*
     * TODO: Take the output device scaling into account and load
     *       a cursor image of the appropriate size, if possible.
     */
    struct wl_cursor_image *image = cursor->images[0];
    struct wl_buffer       *buffer = wl_cursor_image_get_buffer(image);
    wl_pointer_set_cursor(display->seat.pointer_obj,
                          display->seat.pointer.serial,
                          display->cursor_surface,
                          image->hotspot_x,
                          image->hotspot_y);
    wl_surface_attach(display->cursor_surface, buffer, 0, 0);
    wl_surface_damage(display->cursor_surface, 0, 0, image->width, image->height);
    wl_surface_commit(display->cursor_surface);
}
#endif /* COG_USE_WAYLAND_CURSOR */

static void
pointer_on_enter(void              *data,
                 struct wl_pointer *pointer,
                 uint32_t           serial,
                 struct wl_surface *surface,
                 wl_fixed_t         fixed_x,
                 wl_fixed_t         fixed_y)
{
    CogWlPlatform *platform = data;
    CogWlDisplay  *display = platform->display;

    display->seat.keyboard.serial = serial;
    display->seat.pointer.serial = serial;
    display->seat.pointer.surface = surface;

#ifdef COG_USE_WAYLAND_CURSOR
    set_cursor(platform, CURSOR_LEFT_PTR);
#endif /* COG_USE_WAYLAND_CURSOR */
}

static void
pointer_on_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface)
{
    CogWlPlatform *platform = data;
    CogWlDisplay  *display = platform->display;

    display->seat.keyboard.serial = serial;
    display->seat.pointer.surface = NULL;
}

static void
pointer_on_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t fixed_x, wl_fixed_t fixed_y)
{
    CogWlPlatform *platform = data;
    CogWlDisplay  *display = platform->display;

    display->seat.pointer.x = wl_fixed_to_int(fixed_x);
    display->seat.pointer.y = wl_fixed_to_int(fixed_y);

    struct wpe_input_pointer_event event = {wpe_input_pointer_event_type_motion,
                                            time,
                                            display->seat.pointer.x * display->current_output->scale,
                                            display->seat.pointer.y * display->current_output->scale,
                                            display->seat.pointer.button,
                                            display->seat.pointer.state};

    struct wpe_view_backend *backend = cog_view_get_backend(COG_VIEW(platform->view));
    wpe_view_backend_dispatch_pointer_event(backend, &event);
}

static void
pointer_on_button(void              *data,
                  struct wl_pointer *pointer,
                  uint32_t           serial,
                  uint32_t           time,
                  uint32_t           button,
                  uint32_t           state)
{
    CogWlPlatform *platform = data;
    CogWlDisplay  *display = platform->display;

    display->seat.keyboard.serial = serial;

    /* @FIXME: what is this for?
    if (button >= BTN_MOUSE)
        button = button - BTN_MOUSE + 1;
    else
        button = 0;
    */

    display->seat.pointer.button = !!state ? button : 0;
    display->seat.pointer.state = state;

    struct wpe_input_pointer_event event = {
        wpe_input_pointer_event_type_button,
        time,
        display->seat.pointer.x * display->current_output->scale,
        display->seat.pointer.y * display->current_output->scale,
        display->seat.pointer.button,
        display->seat.pointer.state,
    };

    if (popup_data.wl_surface) {
        if (display->seat.pointer.surface == popup_data.wl_surface) {
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

    struct wpe_view_backend *backend = cog_view_get_backend(COG_VIEW(platform->view));
    wpe_view_backend_dispatch_pointer_event(backend, &event);
}

static void
dispatch_axis_event(CogWlPlatform *platform)
{
    CogWlDisplay *display = platform->display;

    if (!display->seat.axis.has_delta)
        return;

    struct wpe_input_axis_2d_event event = {
        0,
    };
    event.base.type = wpe_input_axis_event_type_mask_2d | wpe_input_axis_event_type_motion_smooth;
    event.base.time = display->seat.axis.time;
    event.base.x = display->seat.pointer.x * display->current_output->scale;
    event.base.y = display->seat.pointer.y * display->current_output->scale;

    event.x_axis = wl_fixed_to_double(display->seat.axis.x_delta) * display->current_output->scale;
    event.y_axis = -wl_fixed_to_double(display->seat.axis.y_delta) * display->current_output->scale;

    struct wpe_view_backend *backend = cog_view_get_backend(COG_VIEW(platform->view));
    wpe_view_backend_dispatch_axis_event(backend, &event.base);

    display->seat.axis.has_delta = false;
    display->seat.axis.time = 0;
    display->seat.axis.x_delta = display->seat.axis.y_delta = 0;
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
    CogWlDisplay  *display = platform->display;

    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        display->seat.axis.has_delta = true;
        display->seat.axis.time = time;
        display->seat.axis.y_delta += value * SCROLL_WHEEL_STEP_SCALING_FACTOR;
    }

    if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
        display->seat.axis.has_delta = true;
        display->seat.axis.time = time;
        display->seat.axis.x_delta += value * SCROLL_WHEEL_STEP_SCALING_FACTOR;
    }

    if (!pointer_uses_frame_event(pointer))
        dispatch_axis_event(platform);
}

#ifdef WL_POINTER_FRAME_SINCE_VERSION
static void
pointer_on_frame(void *data, struct wl_pointer *pointer)
{
    CogWlPlatform *platform = data;

    /* @FIXME: buffer pointer events and handle them in frame. That's the
     * recommended usage of this interface.
     */

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
keyboard_on_keymap(void *data, struct wl_keyboard *wl_keyboard, uint32_t format, int32_t fd, uint32_t size)
{
    CogWlPlatform *platform = data;
    CogWlDisplay  *display = platform->display;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }

    const int map_mode = wl_seat_get_version(display->seat.seat) > 6 ? MAP_PRIVATE : MAP_SHARED;
    void     *mapping = mmap(NULL, size, PROT_READ, map_mode, fd, 0);
    if (mapping == MAP_FAILED) {
        close(fd);
        return;
    }

    display->seat.xkb.keymap = xkb_keymap_new_from_string(display->seat.xkb.context,
                                                          (char *) mapping,
                                                          XKB_KEYMAP_FORMAT_TEXT_V1,
                                                          XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(mapping, size);
    close(fd);

    if (display->seat.xkb.keymap == NULL)
        return;

    display->seat.xkb.state = xkb_state_new(display->seat.xkb.keymap);
    if (display->seat.xkb.state == NULL)
        return;

    display->seat.xkb.indexes.control = xkb_keymap_mod_get_index(display->seat.xkb.keymap, XKB_MOD_NAME_CTRL);
    display->seat.xkb.indexes.alt = xkb_keymap_mod_get_index(display->seat.xkb.keymap, XKB_MOD_NAME_ALT);
    display->seat.xkb.indexes.shift = xkb_keymap_mod_get_index(display->seat.xkb.keymap, XKB_MOD_NAME_SHIFT);
}

static void
keyboard_on_enter(void               *data,
                  struct wl_keyboard *wl_keyboard,
                  uint32_t            serial,
                  struct wl_surface  *surface,
                  struct wl_array    *keys)
{
    CogWlPlatform *platform = data;
    CogWlDisplay  *display = platform->display;

    display->seat.keyboard.serial = serial;
}

static void
keyboard_on_leave(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, struct wl_surface *surface)
{
    CogWlPlatform *platform = data;
    CogWlDisplay  *display = platform->display;

    display->seat.keyboard.serial = serial;
}

static void
handle_key_event(CogWlPlatform *platform, uint32_t key, uint32_t state, uint32_t time)
{
    CogWlDisplay *display = platform->display;

    if (display->seat.xkb.state == NULL)
        return;

    uint32_t keysym = xkb_state_key_get_one_sym(display->seat.xkb.state, key);
    uint32_t unicode = xkb_state_key_get_utf32(display->seat.xkb.state, key);

    /* TODO: Move as much as possible from fullscreen handling to common code. */
    if (cog_view_get_use_key_bindings(COG_VIEW(platform->view)) && state == WL_KEYBOARD_KEY_STATE_PRESSED &&
        display->seat.xkb.modifiers == 0 && unicode == 0 && keysym == XKB_KEY_F11) {
#if HAVE_FULLSCREEN_HANDLING
        if (platform->window.is_fullscreen && platform->window.was_fullscreen_requested_from_dom) {
            struct wpe_view_backend *backend = cog_view_get_backend(COG_VIEW(platform->view));
            wpe_view_backend_dispatch_request_exit_fullscreen(backend);
            return;
        }
#endif
        cog_wl_platform_set_fullscreen(platform, !platform->window.is_fullscreen);
        return;
    }

    if (display->seat.xkb.compose_state != NULL && state == WL_KEYBOARD_KEY_STATE_PRESSED &&
        xkb_compose_state_feed(display->seat.xkb.compose_state, keysym) == XKB_COMPOSE_FEED_ACCEPTED &&
        xkb_compose_state_get_status(display->seat.xkb.compose_state) == XKB_COMPOSE_COMPOSED) {
        keysym = xkb_compose_state_get_one_sym(display->seat.xkb.compose_state);
    }

    struct wpe_input_keyboard_event event = {time, keysym, key, state == true, display->seat.xkb.modifiers};

    cog_view_handle_key_event(COG_VIEW(platform->view), &event);
}

static gboolean
repeat_delay_timeout(CogWlPlatform *platform)
{
    CogWlDisplay *display = platform->display;

    handle_key_event(platform,
                     display->seat.keyboard.repeat_data.key,
                     display->seat.keyboard.repeat_data.state,
                     display->seat.keyboard.repeat_data.time);

    display->seat.keyboard.repeat_data.event_source =
        g_timeout_add(display->seat.keyboard.repeat_info.rate, (GSourceFunc) repeat_delay_timeout, platform);

    return G_SOURCE_REMOVE;
}

static void
keyboard_on_key(void               *data,
                struct wl_keyboard *wl_keyboard,
                uint32_t            serial,
                uint32_t            time,
                uint32_t            key,
                uint32_t            state)
{
    CogWlPlatform *platform = data;
    CogWlDisplay  *display = platform->display;

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

    display->seat.keyboard.serial = serial;
    handle_key_event(platform, key, state, time);

    if (display->seat.keyboard.repeat_info.rate == 0)
        return;

    if (state == WL_KEYBOARD_KEY_STATE_RELEASED && display->seat.keyboard.repeat_data.key == key) {
        if (display->seat.keyboard.repeat_data.event_source)
            g_source_remove(display->seat.keyboard.repeat_data.event_source);

        memset(&display->seat.keyboard.repeat_data, 0x00, sizeof(display->seat.keyboard.repeat_data));
    } else if (display->seat.xkb.keymap != NULL && state == WL_KEYBOARD_KEY_STATE_PRESSED &&
               xkb_keymap_key_repeats(display->seat.xkb.keymap, key)) {
        if (display->seat.keyboard.repeat_data.event_source)
            g_source_remove(display->seat.keyboard.repeat_data.event_source);

        display->seat.keyboard.repeat_data.key = key;
        display->seat.keyboard.repeat_data.time = time;
        display->seat.keyboard.repeat_data.state = state;
        display->seat.keyboard.repeat_data.event_source =
            g_timeout_add(display->seat.keyboard.repeat_info.delay, (GSourceFunc) repeat_delay_timeout, platform);
    }
}

static void
keyboard_on_modifiers(void               *data,
                      struct wl_keyboard *wl_keyboard,
                      uint32_t            serial,
                      uint32_t            mods_depressed,
                      uint32_t            mods_latched,
                      uint32_t            mods_locked,
                      uint32_t            group)
{
    CogWlPlatform *platform = data;
    CogWlDisplay  *display = platform->display;

    if (display->seat.xkb.state == NULL)
        return;
    display->seat.keyboard.serial = serial;

    xkb_state_update_mask(display->seat.xkb.state, mods_depressed, mods_latched, mods_locked, 0, 0, group);

    display->seat.xkb.modifiers = 0;
    uint32_t component = (XKB_STATE_MODS_DEPRESSED | XKB_STATE_MODS_LATCHED);

    if (xkb_state_mod_index_is_active(display->seat.xkb.state, display->seat.xkb.indexes.control, component)) {
        display->seat.xkb.modifiers |= wpe_input_keyboard_modifier_control;
    }
    if (xkb_state_mod_index_is_active(display->seat.xkb.state, display->seat.xkb.indexes.alt, component)) {
        display->seat.xkb.modifiers |= wpe_input_keyboard_modifier_alt;
    }
    if (xkb_state_mod_index_is_active(display->seat.xkb.state, display->seat.xkb.indexes.shift, component)) {
        display->seat.xkb.modifiers |= wpe_input_keyboard_modifier_shift;
    }
}

static void
keyboard_on_repeat_info(void *data, struct wl_keyboard *wl_keyboard, int32_t rate, int32_t delay)
{
    CogWlPlatform *platform = data;
    CogWlDisplay  *display = platform->display;

    display->seat.keyboard.repeat_info.rate = rate;
    display->seat.keyboard.repeat_info.delay = delay;

    /* a rate of zero disables any repeating. */
    if (rate == 0 && display->seat.keyboard.repeat_data.event_source > 0) {
        g_source_remove(display->seat.keyboard.repeat_data.event_source);
        memset(&display->seat.keyboard.repeat_data, 0x00, sizeof(display->seat.keyboard.repeat_data));
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
touch_on_down(void              *data,
              struct wl_touch   *touch,
              uint32_t           serial,
              uint32_t           time,
              struct wl_surface *surface,
              int32_t            id,
              wl_fixed_t         x,
              wl_fixed_t         y)
{
    CogWlPlatform *platform = data;
    CogWlDisplay  *display = platform->display;

    display->seat.touch.surface = surface;
    display->seat.keyboard.serial = serial;

    if (id < 0 || id >= 10)
        return;

    struct wpe_input_touch_event_raw raw_event = {
        wpe_input_touch_event_type_down,
        time,
        id,
        wl_fixed_to_int(x) * display->current_output->scale,
        wl_fixed_to_int(y) * display->current_output->scale,
    };

    memcpy(&display->seat.touch.points[id], &raw_event, sizeof(struct wpe_input_touch_event_raw));

    if (popup_data.wl_surface) {
        if (display->seat.touch.surface == popup_data.wl_surface) {
            cog_popup_menu_handle_event(popup_data.popup_menu, COG_POPUP_MENU_EVENT_STATE_PRESSED, raw_event.x,
                                        raw_event.y);
            update_popup();
            return;
        } else
            destroy_popup();
    }

    struct wpe_input_touch_event event = {display->seat.touch.points, 10, raw_event.type, raw_event.id, raw_event.time};

    struct wpe_view_backend *backend = cog_view_get_backend(COG_VIEW(platform->view));
    wpe_view_backend_dispatch_touch_event(backend, &event);
}

static void
touch_on_up(void *data, struct wl_touch *touch, uint32_t serial, uint32_t time, int32_t id)
{
    CogWlPlatform *platform = data;
    CogWlDisplay  *display = platform->display;

    struct wl_surface *target_surface = display->seat.touch.surface;
    display->seat.touch.surface = NULL;
    display->seat.keyboard.serial = serial;

    if (id < 0 || id >= 10)
        return;

    struct wpe_input_touch_event_raw raw_event = {
        wpe_input_touch_event_type_up, time, id, display->seat.touch.points[id].x, display->seat.touch.points[id].y,
    };

    if (popup_data.wl_surface) {
        if (target_surface == popup_data.wl_surface) {
            cog_popup_menu_handle_event(popup_data.popup_menu, COG_POPUP_MENU_EVENT_STATE_RELEASED, raw_event.x,
                                        raw_event.y);
            update_popup();

            memset(&display->seat.touch.points[id], 0x00, sizeof(struct wpe_input_touch_event_raw));
            return;
        }
    }

    memcpy(&display->seat.touch.points[id], &raw_event, sizeof(struct wpe_input_touch_event_raw));

    struct wpe_input_touch_event event = {display->seat.touch.points, 10, raw_event.type, raw_event.id, raw_event.time};

    struct wpe_view_backend *backend = cog_view_get_backend(COG_VIEW(platform->view));
    wpe_view_backend_dispatch_touch_event(backend, &event);

    memset(&display->seat.touch.points[id], 0x00, sizeof(struct wpe_input_touch_event_raw));
}

static void
touch_on_motion(void *data, struct wl_touch *touch, uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y)
{
    CogWlPlatform *platform = data;
    CogWlDisplay  *display = platform->display;

    if (id < 0 || id >= 10)
        return;

    struct wpe_input_touch_event_raw raw_event = {
        wpe_input_touch_event_type_motion,
        time,
        id,
        wl_fixed_to_int(x) * display->current_output->scale,
        wl_fixed_to_int(y) * display->current_output->scale,
    };

    memcpy(&display->seat.touch.points[id], &raw_event, sizeof(struct wpe_input_touch_event_raw));

    struct wpe_input_touch_event event = {display->seat.touch.points, 10, raw_event.type, raw_event.id, raw_event.time};

    struct wpe_view_backend *backend = cog_view_get_backend(COG_VIEW(platform->view));
    wpe_view_backend_dispatch_touch_event(backend, &event);
}

static void
touch_on_frame(void *data, struct wl_touch *touch)
{
    /* @FIXME: buffer touch events and handle them here */
}

static void
touch_on_cancel(void *data, struct wl_touch *touch)
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
seat_on_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities)
{
    g_debug("Enumerating seat capabilities:");

    CogWlPlatform *platform = data;
    CogWlDisplay  *display = platform->display;

    /* Pointer */
    const bool has_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
    if (has_pointer && display->seat.pointer_obj == NULL) {
        display->seat.pointer_obj = wl_seat_get_pointer(display->seat.seat);
        g_assert(display->seat.pointer_obj);
        wl_pointer_add_listener(display->seat.pointer_obj, &pointer_listener, platform);
        g_debug("  - Pointer");
    } else if (!has_pointer && display->seat.pointer_obj != NULL) {
        wl_pointer_release(display->seat.pointer_obj);
        display->seat.pointer_obj = NULL;
    }

    /* Keyboard */
    const bool has_keyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;
    if (has_keyboard && display->seat.keyboard_obj == NULL) {
        display->seat.keyboard_obj = wl_seat_get_keyboard(display->seat.seat);
        g_assert(display->seat.keyboard_obj);
        wl_keyboard_add_listener(display->seat.keyboard_obj, &keyboard_listener, platform);
        g_debug("  - Keyboard");
    } else if (!has_keyboard && display->seat.keyboard_obj != NULL) {
        wl_keyboard_release(display->seat.keyboard_obj);
        display->seat.keyboard_obj = NULL;
    }

    /* Touch */
    const bool has_touch = capabilities & WL_SEAT_CAPABILITY_TOUCH;
    if (has_touch && display->seat.touch_obj == NULL) {
        display->seat.touch_obj = wl_seat_get_touch(display->seat.seat);
        g_assert(display->seat.touch_obj);
        wl_touch_add_listener(display->seat.touch_obj, &touch_listener, platform);
        g_debug("  - Touch");
    } else if (!has_touch && display->seat.touch_obj != NULL) {
        wl_touch_release(display->seat.touch_obj);
        display->seat.touch_obj = NULL;
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

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_on_capabilities,
#ifdef WL_SEAT_NAME_SINCE_VERSION
    .name = seat_on_name,
#endif /* WL_SEAT_NAME_SINCE_VERSION */
};

static void
registry_on_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    CogWlPlatform *platform = data;
    CogWlDisplay  *display = platform->display;

    for (int i = 0; i < G_N_ELEMENTS(display->metrics); i++) {
        if (display->metrics[i].name == name) {
            display->metrics[i].output = NULL;
            display->metrics[i].name = 0;
            g_debug("Removed output %i\n", name);
            break;
        }
    }
}

#if COG_ENABLE_WESTON_DIRECT_DISPLAY
static void
on_dmabuf_surface_frame(void *data, struct wl_callback *callback, uint32_t time)
{
    // for WAYLAND_DEBUG=1 purposes only
    wl_callback_destroy(callback);
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

#if COG_ENABLE_WESTON_DIRECT_DISPLAY
static void
create_succeeded(void *data, struct zwp_linux_buffer_params_v1 *params, struct wl_buffer *new_buffer)
{
    zwp_linux_buffer_params_v1_destroy(params);

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
on_video_plane_display_dmabuf_receiver_handle_dmabuf(void                                         *data,
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

    uint64_t                           modifier = DRM_FORMAT_MOD_INVALID;
    struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(display->dmabuf);
    if (display->direct_display != NULL)
        weston_direct_display_v1_enable(display->direct_display, params);

    struct video_surface *surf =
        (struct video_surface *) g_hash_table_lookup(platform->window.video_surfaces, GUINT_TO_POINTER(id));
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
        g_hash_table_insert(platform->window.video_surfaces, GUINT_TO_POINTER(id), surf);
    }

    zwp_linux_buffer_params_v1_add(params, fd, 0, 0, stride, modifier >> 32, modifier & 0xffffffff);

    if ((x + width) > platform->window.width)
        width -= x;

    if ((y + height) > platform->window.height)
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
    wl_buffer_add_listener (buffer->buffer, &dmabuf_buffer_listener, buffer);

    wl_surface_attach(surf->wl_surface, buffer->buffer, 0, 0);
    wl_surface_damage(surf->wl_surface, 0, 0, buffer->width, buffer->height);

    struct wl_callback *callback = wl_surface_frame(surf->wl_surface);
    wl_callback_add_listener(callback, &dmabuf_frame_listener, NULL);

    if (!surf->wl_subsurface) {
        surf->wl_subsurface =
            wl_subcompositor_get_subsurface(display->subcompositor, surf->wl_surface, platform->window.wl_surface);
        wl_subsurface_set_sync(surf->wl_subsurface);
    }

    wl_subsurface_set_position(surf->wl_subsurface, buffer->x, buffer->y);
    wl_surface_commit(surf->wl_surface);
}

static void
on_video_plane_display_dmabuf_receiver_end_of_stream(void *data, uint32_t id)
{
    CogWlPlatform *platform = data;
    g_hash_table_remove(platform->window.video_surfaces, GUINT_TO_POINTER(id));
}

static const struct wpe_video_plane_display_dmabuf_receiver video_plane_display_dmabuf_receiver = {
    .handle_dmabuf = on_video_plane_display_dmabuf_receiver_handle_dmabuf,
    .end_of_stream = on_video_plane_display_dmabuf_receiver_end_of_stream,
};
#endif

static gboolean
init_wayland(CogWlPlatform *platform, GError **error)
{
    g_debug("Initializing Wayland...");

    struct wl_display *wl_display = wl_display_connect(NULL);
    if (!wl_display) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Could not open Wayland display");
        return FALSE;
    }

    CogWlDisplay *display = g_slice_new0(CogWlDisplay);
    display->display = wl_display;

    if (!display->event_src) {
        display->event_src = setup_wayland_event_source(g_main_context_get_thread_default(), display->display);
    }

    platform->display = display;
    display->registry = wl_display_get_registry(display->display);
    g_assert(display->registry);

    static const struct wl_registry_listener registry_listener = {.global = registry_on_global,
                                                                  .global_remove = registry_on_global_remove};

    wl_registry_add_listener(display->registry, &registry_listener, platform);
    wl_display_roundtrip(display->display);

#if COG_USE_WAYLAND_CURSOR
    if (!(display->cursor_theme = wl_cursor_theme_load(NULL, 32, display->shm))) {
        g_warning("%s: Could not load cursor theme", G_STRFUNC);
    }
    display->cursor_surface = wl_compositor_create_surface(display->compositor);
#endif /* COG_USE_WAYLAND_CURSOR */

    g_assert(display->compositor);
    g_assert(display->xdg_shell != NULL || display->shell != NULL || display->fshell != NULL);

    return TRUE;
}

static void
clear_wayland(CogWlPlatform *platform)
{
    CogWlDisplay *display = platform->display;

    g_clear_pointer(&display->event_src, g_source_destroy);

    if (display->xdg_shell != NULL)
        xdg_wm_base_destroy(display->xdg_shell);
    if (display->fshell != NULL)
        zwp_fullscreen_shell_v1_destroy(display->fshell);
    if (display->shell != NULL)
        wl_shell_destroy(display->shell);
    if (display->zxdg_exporter != NULL)
        zxdg_exporter_v2_destroy(display->zxdg_exporter);

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
    g_clear_pointer(&display->cursor_surface, wl_surface_destroy);
    g_clear_pointer(&display->cursor_theme, wl_cursor_theme_destroy);
#endif /* COG_USE_WAYLAND_CURSOR */

    wl_registry_destroy(display->registry);
    wl_display_flush(display->display);
    wl_display_disconnect(display->display);

    g_slice_free(CogWlDisplay, display);
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
check_supported_registry_on_global(void               *data,
                                   struct wl_registry *registry,
                                   uint32_t            name,
                                   const char         *interface,
                                   uint32_t            version)
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
        struct wl_registry              *registry = wl_display_get_registry(display);
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

static gboolean
cog_wl_platform_is_supported(void)
{
    static GOnce once = G_ONCE_INIT;
    g_once(&once, check_supported, NULL);
    return GPOINTER_TO_INT(once.retval);
}

#define ERR_EGL(_err, _msg)                                                                                         \
    do {                                                                                                            \
        EGLint error_code_##__LINE__ = eglGetError();                                                               \
        g_set_error((_err), COG_PLATFORM_EGL_ERROR, error_code_##__LINE__, _msg " (%#06x)", error_code_##__LINE__); \
    } while (0)

static void clear_egl(CogWlDisplay *);
static void destroy_window(CogWlPlatform *);

static gboolean
init_egl(CogWlDisplay *display, GError **error)
{
    g_debug("Initializing EGL...");

    display->egl_display = eglGetDisplay((EGLNativeDisplayType) display->display);
    if (display->egl_display == EGL_NO_DISPLAY) {
        ERR_EGL(error, "Could not open EGL display");
        return FALSE;
    }

    EGLint major, minor;
    if (!eglInitialize(display->egl_display, &major, &minor)) {
        ERR_EGL(error, "Could not initialize  EGL");
        clear_egl(display);
        return FALSE;
    }
    g_info("EGL version %d.%d initialized.", major, minor);

    return TRUE;
}

static void
clear_egl(CogWlDisplay *display)
{
    if (display->egl_display != EGL_NO_DISPLAY) {
        eglTerminate(display->egl_display);
        display->egl_display = EGL_NO_DISPLAY;
    }
    eglReleaseThread();
}

static gboolean
create_window(CogWlPlatform *platform, GError **error)
{
    g_debug("Creating Wayland surface...");

    CogWlDisplay *display = platform->display;

    platform->window.wl_surface = wl_compositor_create_surface(display->compositor);
    g_assert(platform->window.wl_surface);

#if COG_ENABLE_WESTON_DIRECT_DISPLAY
    platform->window.video_surfaces = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, destroy_video_surface);
#endif

    wl_surface_add_listener(platform->window.wl_surface, &surface_listener, platform);

    if (display->xdg_shell != NULL) {
        platform->window.xdg_surface = xdg_wm_base_get_xdg_surface(display->xdg_shell, platform->window.wl_surface);
        g_assert(platform->window.xdg_surface);

        xdg_surface_add_listener(platform->window.xdg_surface, &xdg_surface_listener, NULL);
        platform->window.xdg_toplevel = xdg_surface_get_toplevel(platform->window.xdg_surface);
        g_assert(platform->window.xdg_toplevel);

        xdg_toplevel_add_listener(platform->window.xdg_toplevel, &xdg_toplevel_listener, platform);
        xdg_toplevel_set_title(platform->window.xdg_toplevel, COG_DEFAULT_APPNAME);

        const char   *app_id = NULL;
        GApplication *app = g_application_get_default();
        if (app) {
            app_id = g_application_get_application_id(app);
        }
        if (!app_id) {
            app_id = COG_DEFAULT_APPID;
        }
        xdg_toplevel_set_app_id(platform->window.xdg_toplevel, app_id);
        wl_surface_commit(platform->window.wl_surface);
    } else if (display->fshell != NULL) {
        zwp_fullscreen_shell_v1_present_surface(display->fshell,
                                                platform->window.wl_surface,
                                                ZWP_FULLSCREEN_SHELL_V1_PRESENT_METHOD_DEFAULT,
                                                NULL);

        /* Configure the surface so that it respects the width and height
         * environment variables */
        cog_wl_platform_configure_geometry(platform, 0, 0);
    } else if (display->shell != NULL) {
        platform->window.shell_surface = wl_shell_get_shell_surface(display->shell, platform->window.wl_surface);
        g_assert(platform->window.shell_surface);

        wl_shell_surface_add_listener(platform->window.shell_surface, &shell_surface_listener, platform);
        wl_shell_surface_set_toplevel(platform->window.shell_surface);

        /* wl_shell needs an initial surface configuration. */
        cog_wl_platform_configure_geometry(platform, 0, 0);
    }

#if COG_HAVE_LIBPORTAL
    platform->window.xdp_parent_wl_data.zxdg_exporter = display->zxdg_exporter;
    platform->window.xdp_parent_wl_data.wl_surface = platform->window.wl_surface;
#endif /* COG_HAVE_LIBPORTAL */

    const char *env_var;
    if ((env_var = g_getenv("COG_PLATFORM_WL_VIEW_FULLSCREEN")) && g_ascii_strtoll(env_var, NULL, 10) > 0) {
        platform->window.is_maximized = false;
        platform->window.is_fullscreen = true;

        if (display->xdg_shell != NULL) {
            xdg_toplevel_set_fullscreen(platform->window.xdg_toplevel, NULL);
        } else if (display->fshell != NULL) {
            platform->window.should_resize_to_largest_output = true;
            resize_to_largest_output(platform);
        } else if (display->shell != NULL) {
            wl_shell_surface_set_fullscreen(platform->window.shell_surface, WL_SHELL_SURFACE_FULLSCREEN_METHOD_SCALE, 0,
                                            NULL);
        } else {
            g_warning("No available shell capable of fullscreening.");
            platform->window.is_fullscreen = false;
        }
    } else if ((env_var = g_getenv("COG_PLATFORM_WL_VIEW_MAXIMIZE")) && g_ascii_strtoll(env_var, NULL, 10) > 0) {
        platform->window.is_maximized = true;
        platform->window.is_fullscreen = false;

        if (display->xdg_shell != NULL) {
            xdg_toplevel_set_maximized(platform->window.xdg_toplevel);
        } else if (display->shell != NULL) {
            wl_shell_surface_set_maximized(platform->window.shell_surface, NULL);
        } else {
            g_warning("No available shell capable of maximizing.");
            platform->window.is_maximized = false;
        }
    }

    return TRUE;
}

static void
destroy_window(CogWlPlatform *platform)
{
    g_clear_pointer(&platform->window.xdg_toplevel, xdg_toplevel_destroy);
    g_clear_pointer(&platform->window.xdg_surface, xdg_surface_destroy);
    g_clear_pointer(&platform->window.shell_surface, wl_shell_surface_destroy);
    g_clear_pointer(&platform->window.wl_surface, wl_surface_destroy);

#if COG_ENABLE_WESTON_DIRECT_DISPLAY
    g_clear_pointer(&platform->window.video_surfaces, g_hash_table_destroy);
#endif
}

void
cog_wl_platform_popup_create(CogWlPlatform *platform, WebKitOptionMenu *option_menu)
{
    CogWlDisplay *display = platform->display;

    popup_data.option_menu = option_menu;

    popup_data.width = platform->window.width;
    popup_data.height = cog_popup_menu_get_height_for_option_menu(option_menu);

    popup_data.popup_menu = cog_popup_menu_create(option_menu, display->shm, popup_data.width, popup_data.height,
                                                  display->current_output->scale);

    popup_data.wl_surface = wl_compositor_create_surface(display->compositor);
    g_assert(popup_data.wl_surface);

#ifdef WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION
    if (wl_surface_get_version(popup_data.wl_surface) >= WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION)
        wl_surface_set_buffer_scale(popup_data.wl_surface, display->current_output->scale);
#endif /* WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION */

    if (display->xdg_shell != NULL) {
        popup_data.xdg_positioner = xdg_wm_base_create_positioner(display->xdg_shell);
        g_assert(popup_data.xdg_positioner);

        xdg_positioner_set_size(popup_data.xdg_positioner, popup_data.width, popup_data.height);
        xdg_positioner_set_anchor_rect(popup_data.xdg_positioner, 0, (platform->window.height - popup_data.height),
                                       popup_data.width, popup_data.height);

        popup_data.xdg_surface = xdg_wm_base_get_xdg_surface(display->xdg_shell, popup_data.wl_surface);
        g_assert(popup_data.xdg_surface);

        xdg_surface_add_listener(popup_data.xdg_surface, &xdg_surface_listener, NULL);
        popup_data.xdg_popup =
            xdg_surface_get_popup(popup_data.xdg_surface, platform->window.xdg_surface, popup_data.xdg_positioner);
        g_assert(popup_data.xdg_popup);

        xdg_popup_add_listener(popup_data.xdg_popup, &xdg_popup_listener, NULL);
        xdg_popup_grab(popup_data.xdg_popup, display->seat.seat, display->seat.keyboard.serial);
        wl_surface_commit(popup_data.wl_surface);
    } else if (display->shell != NULL) {
        popup_data.shell_surface = wl_shell_get_shell_surface(display->shell, popup_data.wl_surface);
        g_assert(popup_data.shell_surface);

        wl_shell_surface_add_listener(popup_data.shell_surface, &shell_popup_surface_listener, NULL);
        wl_shell_surface_set_popup(popup_data.shell_surface, display->seat.seat, display->seat.keyboard.serial,
                                   platform->window.wl_surface, 0, (platform->window.height - popup_data.height), 0);

        display_popup();
    }
}

static void
destroy_popup(void)
{
    if (popup_data.option_menu == NULL)
        return;

    webkit_option_menu_close(popup_data.option_menu);
    g_clear_pointer(&popup_data.popup_menu, cog_popup_menu_destroy);
    g_clear_object(&popup_data.option_menu);

    g_clear_pointer(&popup_data.xdg_popup, xdg_popup_destroy);
    g_clear_pointer(&popup_data.xdg_surface, xdg_surface_destroy);
    g_clear_pointer(&popup_data.xdg_positioner, xdg_positioner_destroy);
    g_clear_pointer(&popup_data.shell_surface, wl_shell_surface_destroy);
    g_clear_pointer(&popup_data.wl_surface, wl_surface_destroy);

    popup_data.configured = false;
}

static void
display_popup(void)
{
    struct wl_buffer *buffer = cog_popup_menu_get_buffer(popup_data.popup_menu);
    wl_surface_attach(popup_data.wl_surface, buffer, 0, 0);
    wl_surface_damage(popup_data.wl_surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(popup_data.wl_surface);
}

static void
update_popup(void)
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
init_input(CogWlPlatform *platform, GError **error)
{
    CogWlDisplay *display = platform->display;

    if (display->seat.seat != NULL) {
        wl_seat_add_listener(display->seat.seat, &seat_listener, platform);

        display->seat.xkb.context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        g_assert(display->seat.xkb.context);
        display->seat.xkb.compose_table = xkb_compose_table_new_from_locale(display->seat.xkb.context,
                                                                            setlocale(LC_CTYPE, NULL),
                                                                            XKB_COMPOSE_COMPILE_NO_FLAGS);
        if (display->seat.xkb.compose_table != NULL) {
            display->seat.xkb.compose_state =
                xkb_compose_state_new(display->seat.xkb.compose_table, XKB_COMPOSE_STATE_NO_FLAGS);
        }

        if (display->text_input_manager != NULL) {
            struct zwp_text_input_v3 *text_input =
                zwp_text_input_manager_v3_get_text_input(display->text_input_manager, display->seat.seat);
            cog_im_context_wl_set_text_input(text_input);
        } else if (display->text_input_manager_v1 != NULL) {
            struct zwp_text_input_v1 *text_input =
                zwp_text_input_manager_v1_create_text_input(display->text_input_manager_v1);
            cog_im_context_wl_v1_set_text_input(text_input, display->seat.seat, platform->window.wl_surface);
        }
    }

    return TRUE;
}

static void
clear_input(CogWlDisplay *display)
{
    g_clear_pointer(&display->seat.pointer_obj, wl_pointer_destroy);
    g_clear_pointer(&display->seat.keyboard_obj, wl_keyboard_destroy);
    g_clear_pointer(&display->seat.seat, wl_seat_destroy);

    cog_im_context_wl_set_text_input(NULL);
    g_clear_pointer(&display->text_input_manager, zwp_text_input_manager_v3_destroy);
    cog_im_context_wl_v1_set_text_input(NULL, NULL, NULL);
    g_clear_pointer(&display->text_input_manager_v1, zwp_text_input_manager_v1_destroy);

    g_clear_pointer(&display->seat.xkb.state, xkb_state_unref);
    g_clear_pointer(&display->seat.xkb.compose_state, xkb_compose_state_unref);
    g_clear_pointer(&display->seat.xkb.compose_table, xkb_compose_table_unref);
    g_clear_pointer(&display->seat.xkb.keymap, xkb_keymap_unref);
    g_clear_pointer(&display->seat.xkb.context, xkb_context_unref);
}

static struct wpe_view_backend *
gamepad_provider_get_view_backend_for_gamepad(void *provider G_GNUC_UNUSED, void *gamepad G_GNUC_UNUSED)
{
    CogWlPlatform *platform = COG_WL_PLATFORM(cog_platform_get_default());
    CogWlView     *view = platform->view;
    g_assert(view);
    return wpe_view_backend_exportable_fdo_get_view_backend(view->exportable);
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

    if (!init_wayland(COG_WL_PLATFORM(platform), error))
        return FALSE;

    CogWlDisplay *display = COG_WL_PLATFORM(platform)->display;
    if (!init_egl(display, error)) {
        clear_wayland(COG_WL_PLATFORM(platform));
        return FALSE;
    }

    if (!create_window(COG_WL_PLATFORM(platform), error)) {
        clear_egl(display);
        clear_wayland(COG_WL_PLATFORM(platform));
        return FALSE;
    }

    if (!init_input(COG_WL_PLATFORM(platform), error)) {
        destroy_window(COG_WL_PLATFORM(platform));
        clear_egl(display);
        clear_wayland(COG_WL_PLATFORM(platform));
        return FALSE;
    }

    /* init WPE host data */
    wpe_fdo_initialize_for_egl_display(display->egl_display);

#if COG_ENABLE_WESTON_DIRECT_DISPLAY
    wpe_video_plane_display_dmabuf_register_receiver(&video_plane_display_dmabuf_receiver, platform);
#endif

    cog_gamepad_setup(gamepad_provider_get_view_backend_for_gamepad);

    return TRUE;
}

static void
cog_wl_platform_finalize(GObject *object)
{
    CogWlPlatform *platform = COG_WL_PLATFORM(object);

    /* free WPE view data */
    if (platform->view->frame_callback != NULL)
        wl_callback_destroy(platform->view->frame_callback);
    if (platform->view->image != NULL) {
        wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(platform->view->exportable,
                                                                            platform->view->image);
    }

    /* @FIXME: check why this segfaults
    wpe_view_backend_destroy (wpe_view_data.backend);
    */

    /* free WPE host data */
    /* @FIXME: check why this segfaults
    wpe_view_backend_exportable_wl_destroy (wpe_host_data.exportable);
    */

    clear_input(platform->display);
    destroy_popup();
    destroy_window(platform);
    clear_egl(platform->display);
    clear_wayland(platform);

    G_OBJECT_CLASS(cog_wl_platform_parent_class)->finalize(object);
}

#if COG_HAVE_LIBPORTAL
static void
on_run_file_chooser(WebKitWebView *view, WebKitFileChooserRequest *request)
{
    g_autoptr(XdpParent) xdp_parent = NULL;

    if (COG_WL_VIEW(view)->platform->window.xdp_parent_wl_data.zxdg_exporter &&
        platform->window.xdp_parent_wl_data.wl_surface) {
        xdp_parent = xdp_parent_new_wl(&COG_WL_VIEW(view)->platform->window.xdp_parent_wl_data);
    }

    run_file_chooser(view, request, xdp_parent);
}
#endif /* COG_HAVE_LIBPORTAL */

static void
on_mouse_target_changed(WebKitWebView *view, WebKitHitTestResult *hitTestResult, guint mouseModifiers)
{
#ifdef COG_USE_WAYLAND_CURSOR
    CogWlPlatform *platform = COG_WL_VIEW(view)->platform;
    if (webkit_hit_test_result_context_is_link(hitTestResult)) {
        set_cursor(platform, CURSOR_HAND);
    } else if (webkit_hit_test_result_context_is_editable(hitTestResult)) {
        set_cursor(platform, CURSOR_TEXT);
    } else if (webkit_hit_test_result_context_is_selection(hitTestResult)) {
        set_cursor(platform, CURSOR_TEXT);
    } else {
        set_cursor(platform, CURSOR_LEFT_PTR);
    }
#endif /* COG_USE_WAYLAND_CURSOR */
}

static void
cog_wl_platform_init_web_view(CogPlatform *platform, WebKitWebView *view)
{
    CogWlPlatform *cog_wl_platform = COG_WL_PLATFORM(platform);
    CogWlView     *cog_wl_view = COG_WL_VIEW(view);

    cog_wl_platform->view = cog_wl_view;
    cog_wl_view->should_update_opaque_region = true;
    cog_wl_view->platform = cog_wl_platform;

#if COG_HAVE_LIBPORTAL
    g_signal_connect(view, "run-file-chooser", G_CALLBACK(on_run_file_chooser), NULL);
#endif /* COG_HAVE_LIBPORTAL */
    g_signal_connect(view, "mouse-target-changed", G_CALLBACK(on_mouse_target_changed), NULL);
}

static WebKitInputMethodContext *
cog_wl_platform_create_im_context(CogPlatform *platform)
{
    CogWlDisplay *display = COG_WL_PLATFORM(platform)->display;

    if (display->text_input_manager)
        return cog_im_context_wl_new();
    if (display->text_input_manager_v1)
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
    platform_class->init_web_view = cog_wl_platform_init_web_view;
    platform_class->create_im_context = cog_wl_platform_create_im_context;
}

static void
cog_wl_platform_class_finalize(CogWlPlatformClass *klass)
{
}

static void
cog_wl_platform_init(CogWlPlatform *platform)
{
    platform->window.width = DEFAULT_WIDTH;
    platform->window.height = DEFAULT_HEIGHT;
    platform->window.width_before_fullscreen = DEFAULT_WIDTH;
    platform->window.height_before_fullscreen = DEFAULT_HEIGHT;
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
