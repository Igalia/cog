/*
 * cog-utils-wl.c
 * Copyright (C) 2023 Pablo Saavedra <psaavedra@igalia.com>
 * Copyright (C) 2023 Adrian Perez de Castro <aperez@igalia.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../core/cog.h"

#include <errno.h>
#include <locale.h>
#include <wayland-client.h>
#ifdef COG_USE_WAYLAND_CURSOR
#    include <wayland-cursor.h>
#endif
#include <xkbcommon/xkbcommon-compose.h>
#include <xkbcommon/xkbcommon.h>

#include "../common/cursors.h"

#include "cog-im-context-wl-v1.h"
#include "cog-im-context-wl.h"
#include "cog-platform-wl.h"
#include "cog-utils-wl.h"

#include "fullscreen-shell-unstable-v1-client.h"
#include "text-input-unstable-v1-client.h"
#include "text-input-unstable-v3-client.h"
#include "xdg-foreign-unstable-v2-client.h"
#include "xdg-shell-client.h"

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

void
cog_wl_display_add_seat(CogWlDisplay *display, CogWlSeat *seat)
{
    g_assert(display);

    seat->display = display;
    if (!display->seat_default)
        display->seat_default = seat;
    wl_list_insert(&display->seats, &seat->link);
}

CogWlDisplay *
cog_wl_display_create(const char *name, GError **error)
{
    struct wl_display *wl_display = wl_display_connect(name);
    if (!wl_display) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Could not open Wayland display");
        return FALSE;
    }

    CogWlDisplay *display = g_slice_new0(CogWlDisplay);
    display->display = wl_display;

    wl_list_init(&display->seats);

    if (!display->event_src) {
        display->event_src = setup_wayland_event_source(g_main_context_get_thread_default(), display->display);
    }

    return display;
}

void
cog_wl_display_destroy(CogWlDisplay *display)
{
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

    display->seat_default = NULL;

    CogWlSeat *item, *tmp;
    wl_list_for_each_safe(item, tmp, &display->seats, link) {
        cog_wl_seat_destroy(item);
    }

    wl_registry_destroy(display->registry);
    wl_display_flush(display->display);
    wl_display_disconnect(display->display);

    g_slice_free(CogWlDisplay, display);
}

CogWlSeat *
cog_wl_seat_create(struct wl_seat *wl_seat, uint32_t name)
{
    CogWlSeat *seat = g_slice_new0(CogWlSeat);
    seat->seat = wl_seat;
    seat->seat_name = name;
    wl_list_init(&seat->link);

    if (!(seat->xkb.context = xkb_context_new(XKB_CONTEXT_NO_FLAGS))) {
        g_error("Could not initialize XKB context");
        return NULL;
    }

    seat->xkb.compose_table =
        xkb_compose_table_new_from_locale(seat->xkb.context, setlocale(LC_CTYPE, NULL), XKB_COMPOSE_COMPILE_NO_FLAGS);

    if (seat->xkb.compose_table != NULL) {
        seat->xkb.compose_state = xkb_compose_state_new(seat->xkb.compose_table, XKB_COMPOSE_STATE_NO_FLAGS);
        g_clear_pointer(&seat->xkb.compose_table, xkb_compose_table_unref);
    }
    return seat;
}

void
cog_wl_seat_destroy(CogWlSeat *seat)
{
    g_assert(seat != NULL);

    g_debug("%s: Destroying @ %p", G_STRFUNC, seat);
    g_clear_pointer(&seat->keyboard_obj, wl_keyboard_destroy);
    g_clear_pointer(&seat->pointer_obj, wl_pointer_destroy);
    g_clear_pointer(&seat->touch_obj, wl_touch_destroy);
    g_clear_pointer(&seat->seat, wl_seat_destroy);

    g_clear_pointer(&seat->xkb.state, xkb_state_unref);
    g_clear_pointer(&seat->xkb.compose_state, xkb_compose_state_unref);
    g_clear_pointer(&seat->xkb.keymap, xkb_keymap_unref);
    g_clear_pointer(&seat->xkb.context, xkb_context_unref);

    wl_list_remove(&seat->link);
    g_slice_free(CogWlSeat, seat);
}

uint32_t
cog_wl_seat_get_serial(CogWlSeat *seat)
{
    uint32_t serial = seat->pointer.serial;

    if (seat->keyboard.serial > serial)
        serial = seat->keyboard.serial;

    if (seat->touch.serial > serial)
        serial = seat->touch.serial;

    return serial;
}

#ifdef COG_USE_WAYLAND_CURSOR
void
cog_wl_seat_set_cursor(CogWlSeat *seat, enum cursor_type type)
{
    CogWlDisplay *display = seat->display;

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
    wl_pointer_set_cursor(seat->pointer_obj,
                          seat->pointer.serial,
                          display->cursor_surface,
                          image->hotspot_x,
                          image->hotspot_y);
    wl_surface_attach(display->cursor_surface, buffer, 0, 0);
    wl_surface_damage(display->cursor_surface, 0, 0, image->width, image->height);
    wl_surface_commit(display->cursor_surface);
}
#endif /* COG_USE_WAYLAND_CURSOR */

void
cog_wl_text_input_clear(CogWlPlatform *platform)
{
    CogWlDisplay *display = platform->display;
    cog_im_context_wl_set_text_input(NULL);
    g_clear_pointer(&display->text_input_manager, zwp_text_input_manager_v3_destroy);
    cog_im_context_wl_v1_set_text_input(NULL, NULL, NULL);
    g_clear_pointer(&display->text_input_manager_v1, zwp_text_input_manager_v1_destroy);
}

void
cog_wl_text_input_set(CogWlPlatform *platform, CogWlSeat *seat)
{
    CogWlDisplay *display = platform->display;
    if (display->text_input_manager != NULL) {
        struct zwp_text_input_v3 *text_input =
            zwp_text_input_manager_v3_get_text_input(display->text_input_manager, seat->seat);
        cog_im_context_wl_set_text_input(text_input);
    } else if (display->text_input_manager_v1 != NULL) {
        struct zwp_text_input_v1 *text_input =
            zwp_text_input_manager_v1_create_text_input(display->text_input_manager_v1);
        cog_im_context_wl_v1_set_text_input(text_input, seat->seat, platform->window.wl_surface);
    }
}
