/*
 * cog-wl-platform.c
 * Copyright (C) 2019-2023 Igalia S.L.
 * Copyright (C) 2018 Eduardo Lima <elima@igalia.com>
 * Copyright (C) 2018 Adrian Perez de Castro <aperez@igalia.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../core/cog.h"

#include <glib-object.h>
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wpe/fdo-egl.h>
#include <wpe/fdo.h>
#include <wpe/webkit.h>

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

#include "../common/cog-cursors.h"
#include "../common/egl-proc-address.h"
#include "os-compatibility.h"

#include "cog-im-context-wl-v1.h"
#include "cog-im-context-wl.h"
#include "cog-platform-wl.h"
#include "cog-viewport-wl.h"
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

static void
noop()
{
}

static void seat_on_capabilities(void *, struct wl_seat *, uint32_t);
static void seat_on_name(void *, struct wl_seat *, const char *);

static void
xdg_shell_ping(void *data, struct xdg_wm_base *shell, uint32_t serial)
{
    xdg_wm_base_pong(shell, serial);
}

static const struct xdg_wm_base_listener xdg_shell_listener = {
    .ping = xdg_shell_ping,
};

static void
output_handle_mode(void *data, struct wl_output *output, uint32_t flags, int32_t width, int32_t height, int32_t refresh)
{
    CogWlOutput *metrics = cog_wl_display_find_output(((CogWlPlatform *) data)->display, output);
    metrics->width = width;
    metrics->height = height;
    metrics->refresh = refresh;
}

static void
output_handle_done(void *data, struct wl_output *output)
{
    CogWlPlatform *platform = data;
    CogWlDisplay  *display = platform->display;

    CogWlOutput *metrics = cog_wl_display_find_output(platform->display, output);

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

    if (!display->current_output) {
        g_debug("%s: Using %p as initial output", G_STRFUNC, output);
        display->current_output = metrics;
    }

    for (gsize i = 0; i < platform->viewports->len; i++) {
        CogViewport *viewport = g_ptr_array_index(platform->viewports, i);

        // Forces a View resize since the output changed so the device
        // scale factor could be different and the scale of the exported
        // image should be also updated.
        cog_viewport_foreach(viewport, (GFunc) cog_wl_view_resize, NULL);

        if (COG_WL_VIEWPORT(viewport)->window.should_resize_to_largest_output)
            cog_wl_viewport_resize_to_largest_output(COG_WL_VIEWPORT(viewport));
    }
}

#ifdef WL_OUTPUT_SCALE_SINCE_VERSION
static void
output_handle_scale(void *data, struct wl_output *output, int32_t scale)
{
    cog_wl_display_find_output(((CogWlPlatform *) data)->display, output)->scale = scale;
}
#endif /* WL_OUTPUT_SCALE_SINCE_VERSION */

static const struct wl_output_listener output_listener = {
    .geometry = noop,
    .mode = output_handle_mode,
    .done = output_handle_done,
#ifdef WL_OUTPUT_SCALE_SINCE_VERSION
    .scale = output_handle_scale,
#endif /* WL_OUTPUT_SCALE_SINCE_VERSION */
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
        uint32_t        seat_version = MAX(3, MIN(version, 7));
        struct wl_seat *wl_seat = wl_registry_bind(registry, name, &wl_seat_interface, seat_version);
        CogWlSeat      *seat = cog_wl_seat_create(wl_seat, name);
        cog_wl_display_add_seat(display, seat);
        static const struct wl_seat_listener seat_listener = {
            .capabilities = seat_on_capabilities,
#ifdef WL_SEAT_NAME_SINCE_VERSION
            .name = seat_on_name,
#endif // WL_SEAT_NAME_SINCE_VERSION
        };
        wl_seat_add_listener(wl_seat, &seat_listener, seat);
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
        CogWlOutput *item = g_new0(CogWlOutput, 1);
        item->output = wl_registry_bind(registry, name, &wl_output_interface, MIN(2, version));
        item->name = name;
        item->scale = 1;
        wl_list_init(&item->link);
        wl_list_insert(&platform->display->outputs, &item->link);
        wl_output_add_listener(item->output, &output_listener, platform);
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

static void
pointer_on_enter(void              *data,
                 struct wl_pointer *pointer,
                 uint32_t           serial,
                 struct wl_surface *surface,
                 wl_fixed_t         fixed_x,
                 wl_fixed_t         fixed_y)
{
    // We might be reading pending events after destroying the surface.
    if (!surface)
        return;

    CogWlSeat *seat = data;

    if (pointer != seat->pointer_obj) {
        g_critical("%s: Got pointer %p, expected %p.", G_STRFUNC, pointer, seat->pointer_obj);
        return;
    }

    seat->display->seat_default = seat;

    CogWlViewport *viewport = wl_surface_get_user_data(
        surface); // Viewport is attached the context of the wl_surface. Check cog_wl_viewport_create_window();

    seat->pointer_target = viewport;
    seat->pointer.serial = serial;
    seat->pointer.surface = surface;

#ifdef COG_USE_WAYLAND_CURSOR
    cog_wl_seat_set_cursor(seat, NULL);
#endif /* COG_USE_WAYLAND_CURSOR */
}

static void
pointer_on_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface)
{
    CogWlSeat *seat = data;

    if (pointer != seat->pointer_obj) {
        g_critical("%s: Got pointer %p, expected %p.", G_STRFUNC, pointer, seat->pointer_obj);
        return;
    }

    seat->pointer_target = NULL;
    seat->pointer.serial = serial;
    seat->pointer.surface = NULL;
}

static void
pointer_on_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t fixed_x, wl_fixed_t fixed_y)
{
    CogWlSeat    *seat = data;
    CogWlDisplay *display = seat->display;

    if (pointer != seat->pointer_obj) {
        g_critical("%s: Got pointer %p, expected %p.", G_STRFUNC, pointer, seat->pointer_obj);
        return;
    }

    seat->pointer.x = wl_fixed_to_int(fixed_x);
    seat->pointer.y = wl_fixed_to_int(fixed_y);

    struct wpe_input_pointer_event event = {wpe_input_pointer_event_type_motion,
                                            time,
                                            seat->pointer.x * display->current_output->scale,
                                            seat->pointer.y * display->current_output->scale,
                                            seat->pointer.button,
                                            seat->pointer.state};

    g_assert(seat->pointer_target);
    CogWlViewport *viewport = COG_WL_VIEWPORT(seat->pointer_target);
    CogView       *view = cog_viewport_get_visible_view((CogViewport *) viewport);

    if (view)
        wpe_view_backend_dispatch_pointer_event(cog_view_get_backend(view), &event);
}

static void
pointer_on_button(void              *data,
                  struct wl_pointer *pointer,
                  uint32_t           serial,
                  uint32_t           time,
                  uint32_t           button,
                  uint32_t           state)
{
    CogWlSeat    *seat = data;
    CogWlDisplay *display = seat->display;

    if (pointer != seat->pointer_obj) {
        g_critical("%s: Got pointer %p, expected %p.", G_STRFUNC, pointer, seat->pointer_obj);
        return;
    }

    CogWlPlatform *platform = (CogWlPlatform *) cog_platform_get();

    seat->pointer.serial = serial;

    /* @FIXME: what is this for?
    if (button >= BTN_MOUSE)
        button = button - BTN_MOUSE + 1;
    else
        button = 0;
    */

    seat->pointer.button = !!state ? button : 0;
    seat->pointer.state = state;

    struct wpe_input_pointer_event event = {
        wpe_input_pointer_event_type_button,
        time,
        seat->pointer.x * display->current_output->scale,
        seat->pointer.y * display->current_output->scale,
        seat->pointer.button,
        seat->pointer.state,
    };

    CogWlPopup *popup = platform->popup;
    if (popup && popup->wl_surface) {
        if (seat->pointer.surface == popup->wl_surface) {
            cog_popup_menu_handle_event(
                popup->popup_menu, !!state ? COG_POPUP_MENU_EVENT_STATE_PRESSED : COG_POPUP_MENU_EVENT_STATE_RELEASED,
                event.x, event.y);
            cog_wl_platform_popup_update();
            return;
        } else {
            if (!!state)
                cog_wl_platform_popup_destroy();
        }
    }

    CogWlViewport *viewport = COG_WL_VIEWPORT(seat->pointer_target);
    CogView       *view = cog_viewport_get_visible_view((CogViewport *) viewport);

    if (view)
        wpe_view_backend_dispatch_pointer_event(cog_view_get_backend(view), &event);
}

static void
dispatch_axis_event(CogWlSeat *seat)
{
    CogWlDisplay *display = seat->display;

    if (!seat->axis.has_delta)
        return;

    struct wpe_input_axis_2d_event event = {
        0,
    };
    event.base.type = wpe_input_axis_event_type_mask_2d | wpe_input_axis_event_type_motion_smooth;
    event.base.time = seat->axis.time;
    event.base.x = seat->pointer.x * display->current_output->scale;
    event.base.y = seat->pointer.y * display->current_output->scale;

    event.x_axis = wl_fixed_to_double(seat->axis.x_delta) * display->current_output->scale;
    event.y_axis = -wl_fixed_to_double(seat->axis.y_delta) * display->current_output->scale;

    g_assert(seat->pointer_target);

    CogWlViewport *viewport = COG_WL_VIEWPORT(seat->pointer_target);
    CogView       *view = cog_viewport_get_visible_view((CogViewport *) viewport);

    if (view)
        wpe_view_backend_dispatch_axis_event(cog_view_get_backend(view), &event.base);

    seat->axis.has_delta = false;
    seat->axis.time = 0;
    seat->axis.x_delta = seat->axis.y_delta = 0;
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
    CogWlSeat *seat = data;

    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        seat->axis.has_delta = true;
        seat->axis.time = time;
        seat->axis.y_delta += value * SCROLL_WHEEL_STEP_SCALING_FACTOR;
    }

    if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
        seat->axis.has_delta = true;
        seat->axis.time = time;
        seat->axis.x_delta += value * SCROLL_WHEEL_STEP_SCALING_FACTOR;
    }

    if (!pointer_uses_frame_event(pointer))
        dispatch_axis_event(seat);
}

#ifdef WL_POINTER_FRAME_SINCE_VERSION
static void
pointer_on_frame(void *data, struct wl_pointer *pointer)
{
    CogWlSeat *seat = data;

    /* @FIXME: buffer pointer events and handle them in frame. That's the
     * recommended usage of this interface.
     */

    dispatch_axis_event(seat);
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

    if (seat->xkb.keymap == NULL)
        return;

    seat->xkb.state = xkb_state_new(seat->xkb.keymap);
    if (seat->xkb.state == NULL)
        return;

    seat->xkb.indexes.control = xkb_keymap_mod_get_index(seat->xkb.keymap, XKB_MOD_NAME_CTRL);
    seat->xkb.indexes.alt = xkb_keymap_mod_get_index(seat->xkb.keymap, XKB_MOD_NAME_ALT);
    seat->xkb.indexes.shift = xkb_keymap_mod_get_index(seat->xkb.keymap, XKB_MOD_NAME_SHIFT);
}

static void
keyboard_on_enter(void               *data,
                  struct wl_keyboard *wl_keyboard,
                  uint32_t            serial,
                  struct wl_surface  *surface,
                  struct wl_array    *keys)
{
    // We might be reading pending events after destroying the surface.
    if (!surface)
        return;

    CogWlSeat *seat = data;

    if (wl_keyboard != seat->keyboard_obj) {
        g_critical("%s: Got keyboard %p, expected %p.", G_STRFUNC, wl_keyboard, seat->keyboard_obj);
        return;
    }

    seat->display->seat_default = seat;

    CogWlViewport *viewport = wl_surface_get_user_data(
        surface); // Viewport is attached the context of the wl_surface. Check cog_wl_viewport_create_window();

    seat->keyboard_target = viewport;
    seat->keyboard.serial = serial;
}

static void
keyboard_on_leave(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, struct wl_surface *surface)
{
    CogWlSeat *seat = data;

    if (wl_keyboard != seat->keyboard_obj) {
        g_critical("%s: Got keyboard %p, expected %p.", G_STRFUNC, wl_keyboard, seat->keyboard_obj);
        return;
    }

    seat->keyboard_target = NULL;
    seat->keyboard.serial = serial;
}

static void
handle_key_event(CogWlSeat *seat, uint32_t key, uint32_t state, uint32_t time)
{
    g_assert(seat->keyboard_target);

    CogWlViewport *viewport = COG_WL_VIEWPORT(seat->keyboard_target);
    CogView       *view = cog_viewport_get_visible_view(COG_VIEWPORT(viewport));

    if (!view || seat->xkb.state == NULL)
        return;

    uint32_t keysym = xkb_state_key_get_one_sym(seat->xkb.state, key);
    uint32_t unicode = xkb_state_key_get_utf32(seat->xkb.state, key);

    /* TODO: Move as much as possible from fullscreen handling to common code. */
    if (cog_view_get_use_key_bindings(view) && state == WL_KEYBOARD_KEY_STATE_PRESSED && seat->xkb.modifiers == 0 &&
        unicode == 0 && keysym == XKB_KEY_F11) {
#if HAVE_FULLSCREEN_HANDLING
        if (viewport->window.is_fullscreen && viewport->window.was_fullscreen_requested_from_dom) {
            struct wpe_view_backend *backend = cog_view_get_backend(view);
            wpe_view_backend_dispatch_request_exit_fullscreen(backend);
            return;
        }
#endif
        cog_wl_viewport_set_fullscreen(viewport, !viewport->window.is_fullscreen);
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

static gboolean
repeat_delay_timeout(CogWlSeat *seat)
{
    handle_key_event(seat,
                     seat->keyboard.repeat_data.key,
                     seat->keyboard.repeat_data.state,
                     seat->keyboard.repeat_data.time);

    seat->keyboard.repeat_data.event_source =
        g_timeout_add(seat->keyboard.repeat_info.rate, (GSourceFunc) repeat_delay_timeout, seat);

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
    CogWlSeat *seat = data;

    if (wl_keyboard != seat->keyboard_obj) {
        g_critical("%s: Got keyboard %p, expected %p.", G_STRFUNC, wl_keyboard, seat->keyboard_obj);
        return;
    }

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
    handle_key_event(seat, key, state, time);

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
        seat->keyboard.repeat_data.event_source =
            g_timeout_add(seat->keyboard.repeat_info.delay, (GSourceFunc) repeat_delay_timeout, seat);
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
    CogWlSeat *seat = data;

    if (wl_keyboard != seat->keyboard_obj) {
        g_critical("%s: Got keyboard %p, expected %p.", G_STRFUNC, wl_keyboard, seat->keyboard_obj);
        return;
    }

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
keyboard_on_repeat_info(void *data, struct wl_keyboard *wl_keyboard, int32_t rate, int32_t delay)
{
    CogWlSeat *seat = data;

    if (wl_keyboard != seat->keyboard_obj) {
        g_critical("%s: Got keyboard %p, expected %p.", G_STRFUNC, wl_keyboard, seat->keyboard_obj);
        return;
    }

    seat->keyboard.repeat_info.rate = rate;
    seat->keyboard.repeat_info.delay = delay;

    /* a rate of zero disables any repeating. */
    if (rate == 0 && seat->keyboard.repeat_data.event_source > 0) {
        g_source_remove(seat->keyboard.repeat_data.event_source);
        memset(&seat->keyboard.repeat_data, 0x00, sizeof(seat->keyboard.repeat_data));
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
    // We might be reading pending events after destroying the surface.
    if (!surface)
        return;

    CogWlSeat    *seat = data;
    CogWlDisplay *display = seat->display;

    if (touch != seat->touch_obj) {
        g_critical("%s: Got touch %p, expected %p.", G_STRFUNC, touch, seat->touch_obj);
        return;
    }

    seat->display->seat_default = seat;

    CogWlViewport *viewport = wl_surface_get_user_data(
        surface); // Viewport is attached the context of the wl_surface. Check cog_wl_viewport_create_window();
    seat->touch_target = viewport;
    seat->touch.serial = serial;
    seat->touch.surface = surface;

    if (id < 0 || id >= 10)
        return;

    struct wpe_input_touch_event_raw raw_event = {
        wpe_input_touch_event_type_down,
        time,
        id,
        wl_fixed_to_int(x) * display->current_output->scale,
        wl_fixed_to_int(y) * display->current_output->scale,
    };

    memcpy(&seat->touch.points[id], &raw_event, sizeof(struct wpe_input_touch_event_raw));

    CogWlPlatform *platform = (CogWlPlatform *) cog_platform_get();
    CogWlPopup    *popup = platform->popup;
    if (popup && popup->wl_surface) {
        if (seat->touch.surface == popup->wl_surface) {
            cog_popup_menu_handle_event(popup->popup_menu, COG_POPUP_MENU_EVENT_STATE_PRESSED, raw_event.x,
                                        raw_event.y);
            cog_wl_platform_popup_update();
            return;
        } else
            cog_wl_platform_popup_destroy();
    }

    struct wpe_input_touch_event event = {seat->touch.points, 10, raw_event.type, raw_event.id, raw_event.time};

    CogView *view = cog_viewport_get_visible_view((CogViewport *) viewport);
    if (view)
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

    g_assert(seat->touch_target);
    CogWlViewport *viewport = COG_WL_VIEWPORT(seat->touch_target);

    struct wl_surface *target_surface = seat->touch.surface;
    seat->touch_target = NULL;
    seat->touch.serial = serial;
    seat->touch.surface = NULL;

    if (id < 0 || id >= 10)
        return;

    struct wpe_input_touch_event_raw raw_event = {
        wpe_input_touch_event_type_up, time, id, seat->touch.points[id].x, seat->touch.points[id].y,
    };

    CogWlPlatform *platform = (CogWlPlatform *) cog_platform_get();
    CogWlPopup    *popup = platform->popup;

    if (popup && popup->wl_surface) {
        if (target_surface == popup->wl_surface) {
            cog_popup_menu_handle_event(popup->popup_menu, COG_POPUP_MENU_EVENT_STATE_RELEASED, raw_event.x,
                                        raw_event.y);
            cog_wl_platform_popup_update();

            memset(&seat->touch.points[id], 0x00, sizeof(struct wpe_input_touch_event_raw));
            return;
        }
    }

    memcpy(&seat->touch.points[id], &raw_event, sizeof(struct wpe_input_touch_event_raw));

    struct wpe_input_touch_event event = {seat->touch.points, 10, raw_event.type, raw_event.id, raw_event.time};

    CogView *view = cog_viewport_get_visible_view((CogViewport *) viewport);
    if (view)
        wpe_view_backend_dispatch_touch_event(cog_view_get_backend(view), &event);

    memset(&seat->touch.points[id], 0x00, sizeof(struct wpe_input_touch_event_raw));
}

static void
touch_on_motion(void *data, struct wl_touch *touch, uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y)
{
    CogWlSeat    *seat = data;
    CogWlDisplay *display = seat->display;

    if (touch != seat->touch_obj) {
        g_critical("%s: Got touch %p, expected %p.", G_STRFUNC, touch, seat->touch_obj);
        return;
    }

    if (id < 0 || id >= 10)
        return;

    struct wpe_input_touch_event_raw raw_event = {
        wpe_input_touch_event_type_motion,
        time,
        id,
        wl_fixed_to_int(x) * display->current_output->scale,
        wl_fixed_to_int(y) * display->current_output->scale,
    };

    memcpy(&seat->touch.points[id], &raw_event, sizeof(struct wpe_input_touch_event_raw));

    struct wpe_input_touch_event event = {seat->touch.points, 10, raw_event.type, raw_event.id, raw_event.time};

    CogWlViewport *viewport = COG_WL_VIEWPORT(seat->touch_target);
    CogView       *view = cog_viewport_get_visible_view((CogViewport *) viewport);
    if (view)
        wpe_view_backend_dispatch_touch_event(cog_view_get_backend(view), &event);
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
seat_on_capabilities(void *data, struct wl_seat *wl_seat, uint32_t capabilities)
{
    g_debug("Enumerating seat capabilities:");

    CogWlSeat *seat = data;

    /* Pointer */
    const bool has_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
    if (has_pointer && seat->pointer_obj == NULL) {
        seat->pointer_obj = wl_seat_get_pointer(seat->seat);
        g_assert(seat->pointer_obj);
        wl_pointer_add_listener(seat->pointer_obj, &pointer_listener, seat);
        g_debug("  - Pointer");
    } else if (!has_pointer && seat->pointer_obj != NULL) {
        wl_pointer_release(seat->pointer_obj);
        seat->pointer_obj = NULL;
    }

    /* Keyboard */
    const bool has_keyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;
    if (has_keyboard && seat->keyboard_obj == NULL) {
        seat->keyboard_obj = wl_seat_get_keyboard(seat->seat);
        g_assert(seat->keyboard_obj);
        wl_keyboard_add_listener(seat->keyboard_obj, &keyboard_listener, seat);
        g_debug("  - Keyboard");
    } else if (!has_keyboard && seat->keyboard_obj != NULL) {
        wl_keyboard_release(seat->keyboard_obj);
        seat->keyboard_obj = NULL;
    }

    /* Touch */
    const bool has_touch = capabilities & WL_SEAT_CAPABILITY_TOUCH;
    if (has_touch && seat->touch_obj == NULL) {
        seat->touch_obj = wl_seat_get_touch(seat->seat);
        g_assert(seat->touch_obj);
        wl_touch_add_listener(seat->touch_obj, &touch_listener, seat);
        g_debug("  - Touch");
    } else if (!has_touch && seat->touch_obj != NULL) {
        wl_touch_release(seat->touch_obj);
        seat->touch_obj = NULL;
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
registry_on_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    CogWlPlatform *platform = data;

    CogWlOutput *output, *tmp_output;
    wl_list_for_each_safe(output, tmp_output, &platform->display->outputs, link) {
        if (output->name == name) {
            g_debug("%s: output #%" PRIi32 " @ %p removed.", G_STRFUNC, output->name, output->output);
            g_clear_pointer(&output->output, wl_output_release);
            wl_list_remove(&output->link);
            if (platform->display->current_output == output) {
                platform->display->current_output =
                    wl_list_empty(&platform->display->outputs)
                        ? NULL
                        : wl_container_of(platform->display->outputs.next, output, link);
            }
            g_clear_pointer(&output, g_free);
            return;
        }
    }

    CogWlSeat *seat, *tmp_seat;
    wl_list_for_each_safe(seat, tmp_seat, &platform->display->seats, link) {
        if (seat->seat_name == name) {
            if (platform->display->seat_default == seat)
                platform->display->seat_default = NULL;
            cog_wl_seat_destroy(seat);
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
on_dmabuf_buffer_release(void *data, struct wl_buffer *buffer)
{
    struct video_buffer *data_buffer = data;
    if (data_buffer->fd >= 0)
        close(data_buffer->fd);

    if (data_buffer->dmabuf_export)
        wpe_video_plane_display_dmabuf_export_release(data_buffer->dmabuf_export);

    g_slice_free (struct video_buffer, data_buffer);
    g_clear_pointer(&buffer, wl_buffer_destroy);
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

static const struct zwp_linux_buffer_params_v1_listener params_listener = {.created = create_succeeded,
                                                                           .failed = create_failed};

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
    CogWlViewport *viewport = COG_WL_VIEWPORT(platform->viewport);
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
        (struct video_surface *) g_hash_table_lookup(viewport->window.video_surfaces, GUINT_TO_POINTER(id));
    if (!surf) {
        surf = g_slice_new0(struct video_surface);
        surf->wl_subsurface = NULL;
        surf->wl_surface = cog_wl_compositor_create_surface(display->compositor, viewport);

#    if COG_ENABLE_WESTON_CONTENT_PROTECTION
        if (display->protection) {
            surf->protected_surface = weston_content_protection_get_protection(display->protection, surf->wl_surface);
            //weston_protected_surface_set_type(surf->protected_surface, WESTON_PROTECTED_SURFACE_TYPE_DC_ONLY);

            weston_protected_surface_enforce(surf->protected_surface);
        }
#    endif
        g_hash_table_insert(viewport->window.video_surfaces, GUINT_TO_POINTER(id), surf);
    }

    zwp_linux_buffer_params_v1_add(params, fd, 0, 0, stride, modifier >> 32, modifier & 0xffffffff);

    if ((x + width) > viewport->window.width)
        width -= x;

    if ((y + height) > viewport->window.height)
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
    wl_buffer_add_listener(buffer->buffer, &dmabuf_buffer_listener, buffer);

    wl_surface_attach(surf->wl_surface, buffer->buffer, 0, 0);
    wl_surface_damage(surf->wl_surface, 0, 0, buffer->width, buffer->height);

    struct wl_callback *callback = wl_surface_frame(surf->wl_surface);
    wl_callback_add_listener(callback, &dmabuf_frame_listener, NULL);

    if (!surf->wl_subsurface) {
        surf->wl_subsurface =
            wl_subcompositor_get_subsurface(display->subcompositor, surf->wl_surface, viewport->window.wl_surface);
        wl_subsurface_set_sync(surf->wl_subsurface);
    }

    wl_subsurface_set_position(surf->wl_subsurface, buffer->x, buffer->y);
    wl_surface_commit(surf->wl_surface);
}

static void
on_video_plane_display_dmabuf_receiver_end_of_stream(void *data, uint32_t id)
{
    CogWlPlatform *platform = data;
    CogWlViewport *viewport = COG_WL_VIEWPORT(platform->viewport);
    g_hash_table_remove(viewport->window.video_surfaces, GUINT_TO_POINTER(id));
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

    CogWlDisplay *display = cog_wl_display_create(NULL, error);
    platform->display = display;
    display->registry = wl_display_get_registry(display->display);
    g_assert(display->registry);

    static const struct wl_registry_listener registry_listener = {.global = registry_on_global,
                                                                  .global_remove = registry_on_global_remove};

    wl_registry_add_listener(display->registry, &registry_listener, platform);
    wl_display_roundtrip(display->display);

    g_assert(display->compositor);

#if COG_USE_WAYLAND_CURSOR
    if (!(display->cursor_theme = wl_cursor_theme_load(NULL, 32, display->shm))) {
        g_warning("%s: Could not load cursor theme", G_STRFUNC);
    }
    display->cursor_surface = wl_compositor_create_surface(display->compositor);
#endif /* COG_USE_WAYLAND_CURSOR */

    g_assert(display->xdg_shell != NULL || display->shell != NULL || display->fshell != NULL);

    return TRUE;
}

static void
clear_wayland(CogWlPlatform *platform)
{
    cog_wl_display_destroy(platform->display);
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

static struct wpe_view_backend *
gamepad_provider_get_view_backend_for_gamepad(void *provider G_GNUC_UNUSED, void *gamepad G_GNUC_UNUSED)
{
    CogWlPlatform *platform = COG_WL_PLATFORM(cog_platform_get());

    g_assert(platform->viewports);
    g_assert(platform->viewports->len > 0);
    CogViewport *viewport = g_ptr_array_index(platform->viewports, COG_SHELL_DEFAULT_VIEWPORT_INDEX);
    g_assert(viewport);
    CogWlView *view = (CogWlView *) cog_viewport_get_visible_view(viewport);
    g_assert(view);
    return wpe_view_backend_exportable_fdo_get_view_backend(view->exportable);
}

static void
cog_wl_platform_on_notify_visible_view(CogWlPlatform *self, GParamSpec *pspec G_GNUC_UNUSED, CogViewport *viewport)
{
    g_assert(viewport);

    g_assert(COG_IS_PLATFORM(self));
    g_assert(COG_IS_VIEWPORT(viewport));

    CogWlView *view = (CogWlView *) cog_viewport_get_visible_view(viewport);
    g_debug("%s: Visible view %p.", G_STRFUNC, view);

    if (!view)
        return;

    /*
     * TODO: Once we support multiple viewports, a view may be visible
     *       without having focus. At that point the input events will
     *       need to go to the *focused* view instead.
     *
     *       For now add the flag and assume that visible == focused.
     */
    wpe_view_backend_add_activity_state(cog_view_get_backend((CogView *) view), wpe_view_activity_state_focused);

    if (!view->image)
        return g_debug("%s: No image to show, skipping update.", G_STRFUNC);

    cog_wl_view_update_surface_contents(view);
}

static gboolean
cog_wl_platform_setup(CogPlatform *platform, CogShell *shell G_GNUC_UNUSED, const char *params, GError **error)
{
    g_return_val_if_fail(COG_IS_SHELL(shell), FALSE);

    CogWlPlatform *self = COG_WL_PLATFORM(platform);

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

    /* init WPE host data */
    wpe_fdo_initialize_for_egl_display(self->display->egl_display);

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

    cog_wl_text_input_clear();
    if (platform->popup)
        cog_wl_platform_popup_destroy();
    clear_egl(platform->display);
    clear_wayland(platform);

    g_clear_pointer(&platform->viewports, g_ptr_array_unref);

    G_OBJECT_CLASS(cog_wl_platform_parent_class)->finalize(object);
}

static WebKitInputMethodContext *
cog_wl_platform_create_im_context(CogViewport *viewport)
{
    CogWlPlatform *platform = (CogWlPlatform *) cog_platform_get();
    g_assert(COG_WL_PLATFORM(platform)->display->seat_default);

    cog_wl_text_input_set(COG_WL_VIEWPORT(viewport), COG_WL_PLATFORM(platform)->display->seat_default);

    CogWlDisplay *display = COG_WL_PLATFORM(platform)->display;
    if (display->text_input_manager)
        return cog_im_context_wl_new();
    if (display->text_input_manager_v1)
        return cog_im_context_wl_v1_new();
    return NULL;
}

void
cog_wl_platform_popup_create(CogWlViewport *viewport, WebKitOptionMenu *option_menu)
{
    CogWlPlatform *platform = (CogWlPlatform *) cog_platform_get();

    // Destroy the still alive previous popup
    if (platform->popup)
        cog_wl_platform_popup_destroy();

    CogWlPopup *popup = cog_wl_popup_create(viewport, option_menu);
    platform->popup = popup;
}

void
cog_wl_platform_popup_destroy(void)
{
    CogWlPlatform *platform = (CogWlPlatform *) cog_platform_get();

    g_assert(platform->popup);
    g_clear_pointer(&platform->popup, cog_wl_popup_destroy);
}

void
cog_wl_platform_popup_update(void)
{
    CogWlPlatform *platform = (CogWlPlatform *) cog_platform_get();

    g_assert(platform->popup);
    cog_wl_popup_update(platform->popup);
}

static void
cog_wl_platform_viewport_created(CogPlatform *platform, CogViewport *viewport)
{
    CogWlPlatform *self = COG_WL_PLATFORM(platform);

    g_assert(!g_ptr_array_find(self->viewports, viewport, NULL));
    g_ptr_array_add(self->viewports, viewport);

    // TODO: Move into CogWlViewport.constructed or somewhere more suitable.
    cog_wl_viewport_create_window(COG_WL_VIEWPORT(viewport), NULL);

    g_signal_connect_object(viewport, "notify::visible-view", G_CALLBACK(cog_wl_platform_on_notify_visible_view),
                            platform, G_CONNECT_AFTER | G_CONNECT_SWAPPED);
}

static void
cog_wl_platform_viewport_disposed(CogPlatform *platform, CogViewport *viewport)
{
    CogWlPlatform *self = COG_WL_PLATFORM(platform);

    gboolean removed G_GNUC_UNUSED = g_ptr_array_remove_fast(self->viewports, viewport);
    g_assert(removed);
}

static void
cog_wl_platform_class_init(CogWlPlatformClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = cog_wl_platform_finalize;

    CogPlatformClass *platform_class = COG_PLATFORM_CLASS(klass);
    platform_class->is_supported = cog_wl_platform_is_supported;
    platform_class->get_view_type = cog_wl_view_get_type;
    platform_class->get_viewport_type = cog_wl_viewport_get_type;
    platform_class->setup = cog_wl_platform_setup;
    platform_class->create_im_context = cog_wl_platform_create_im_context;
    platform_class->viewport_created = cog_wl_platform_viewport_created;
    platform_class->viewport_disposed = cog_wl_platform_viewport_disposed;
}

static void
cog_wl_platform_class_finalize(CogWlPlatformClass *klass)
{
}

static void
cog_wl_platform_init(CogWlPlatform *self)
{
    self->viewports = g_ptr_array_sized_new(3);
}

G_MODULE_EXPORT void
g_io_cogplatform_wl_load(GIOModule *module)
{
    GTypeModule *type_module = G_TYPE_MODULE(module);
    cog_wl_platform_register_type(type_module);
    cog_wl_view_register_type_exported(type_module);
    cog_wl_viewport_register_type_exported(type_module);
}

G_MODULE_EXPORT void
g_io_cogplatform_wl_unload(GIOModule *module G_GNUC_UNUSED)
{
}
