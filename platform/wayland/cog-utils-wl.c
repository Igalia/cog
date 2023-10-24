/*
 * cog-utils-wl.c
 * Copyright (C) 2023 Pablo Saavedra <psaavedra@igalia.com>
 * Copyright (C) 2023 Adrian Perez de Castro <aperez@igalia.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../core/cog.h"

#include <wayland-client.h>

#include "cog-utils-wl.h"

#include "fullscreen-shell-unstable-v1-client.h"
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

CogWlDisplay *cog_wl_display_create(const char *name, GError **error)
{
    struct wl_display *wl_display = wl_display_connect(name);
    if (!wl_display) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Could not open Wayland display");
        return FALSE;
    }

    CogWlDisplay *display = g_slice_new0(CogWlDisplay);
    display->display = wl_display;

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

    wl_registry_destroy(display->registry);
    wl_display_flush(display->display);
    wl_display_disconnect(display->display);

    g_slice_free(CogWlDisplay, display);
}
